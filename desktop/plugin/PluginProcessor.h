/*
 * PluginProcessor.h — Schwung as a VST3 / AU plugin.
 *
 * One plugin instance is ONE CHAIN (MIDI FX -> synth -> 8 FX), not the whole
 * four-slot device. See docs/plans/2026-09-13-schwung-vst-port.md for why:
 * Live's own tracks and returns do what slots and sends do, and per-track
 * automation only makes sense if a track owns one chain.
 *
 * THREE RULES THIS CLASS EXISTS TO ENFORCE.
 *
 * 1. The chain runs at 44100 Hz in 128-frame blocks, always. Live does not,
 *    so RateBridge converts. Nothing below this class ever sees the host's
 *    rate or block size.
 *
 * 2. Automation writes go through a lock-free FIFO drained at the top of
 *    processBlock. A DAW moves parameters from its own threads, and
 *    set_param on the chain is not thread-safe against render_block.
 *
 * 3. Anything that LOADS -- a synth or FX change -- suspends processing for
 *    its duration. On the device those calls are the SPI callback and the
 *    fleet's ~150 realtime violations are someone's dropout; here they happen
 *    on the message thread with the audio stopped, which is the freedom the
 *    desktop host has and the device does not.
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "schwung_desktop.h"
}

/* How many automatable macros the plugin publishes.
 *
 * A FIXED BANK IS FORCED BY THE FORMATS. VST3 and AU want the parameter list
 * at instantiation, while Schwung's parameters are runtime-discovered string
 * keys that depend on which module is loaded. So the bank has to be big enough
 * for the LARGEST module in the fleet, not for a comfortable-looking screen.
 *
 * MEASURED, from tests/fixtures/module-contracts.json (100 modules captured
 * off a device):
 *
 *     minijv  433      surge 303      forge 250      mrdrums 231
 *     median   19      >=64: 22       >=128: 13      >=256: 2
 *
 * 512 covers minijv with headroom. It is deliberately not the median: a bank
 * sized for the typical module silently truncates the big ones, and the
 * parameters that go missing are the ones at the end of the list, which is
 * exactly where a module puts its least-used and therefore least-noticed
 * controls. Most instances bind a couple of dozen of these and leave the rest
 * idle -- an unbound macro costs one float compare per block and nothing else.
 *
 * This does NOT cover a chain whose synth is minijv AND whose eight FX are all
 * large. Nothing fixed can; the formats do not allow a list that grows. */
static constexpr int kMacroCount = 512;

/* The chain has always run eight audio FX and eight MIDI FX. The plugin
 * exposed one of each, which is not a smaller feature -- it is a chain the
 * user cannot build. MAX_AUDIO_FX / MAX_MIDI_FX in chain_internal.h. */
static constexpr int kFxSlots = 8;
static constexpr int kMidiFxSlots = 8;

/* A macro's binding: the chain key it writes, plus the range read from that
 * module's own chain_params.
 *
 * THE RANGE IS READ, NEVER GUESSED. A module declares min/max in dB, in Hz or
 * in per-cent, and assuming 0..1 writes a silently wrong value into anything
 * that is not. A key whose range cannot be found is REFUSED rather than
 * bound -- the same rule the chain's voice sends already follow. */
struct MacroBinding
{
    juce::String key;         // e.g. "synth:timbre"; empty means unbound
    juce::String label;       // the module's own name for it, e.g. "Timbre"
    float        min = 0.0f;
    float        max = 1.0f;
    bool         resolved = false;
    bool         userSet = false;   // typed by hand; auto-bind must not clobber it
    juce::String status;      // why it is not resolved, for the editor to show
};

/*
 * A macro whose NAME follows whatever it is bound to.
 *
 * The formats fix the parameter LIST at instantiation, but not every parameter
 * ATTRIBUTE: getName() is virtual, and both VST3 and AU can be told the titles
 * changed. So the bank stays eight slots for the host's whole life while
 * "Macro 1" reads as "Timbre" in Live's automation list once a module is
 * loaded. Without this the eight lanes are indistinguishable in the one place
 * you actually pick them.
 */
class MacroParameter : public juce::AudioParameterFloat
{
public:
    MacroParameter (const juce::String& pid, const juce::String& fallback)
        : juce::AudioParameterFloat (juce::ParameterID { pid, 1 }, fallback,
                                     juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f),
          fallbackName (fallback) {}

    juce::String getName (int maxLen) const override
    {
        const auto n = displayName.isNotEmpty() ? displayName : fallbackName;
        return n.substring (0, maxLen);
    }

    void setDisplayName (const juce::String& n) { displayName = n; }

private:
    juce::String fallbackName, displayName;
};

/* 44100 Hz -> the host's rate, and 128-frame blocks -> the host's block size.
 *
 * Output only. A synth chain has no input to convert, and when phase 1 feeds
 * line-input modules from the plugin's input bus that path gets its own
 * conversion rather than sharing this one. */
class RateBridge
{
public:
    void prepare (double hostSampleRate, int maxBlock);
    void reset();

    /* Pull `numSamples` frames at the host rate, rendering as many 128-frame
     * Schwung blocks as that needs. */
    void pull (schwung_desktop_t* sd, float* outL, float* outR, int numSamples);

    /* Frames of latency this conversion adds, for the host's delay
     * compensation. Reporting it is not cosmetic: an uncompensated plugin
     * drifts against every other track in the set. */
    int getLatencySamples() const noexcept { return latency; }

private:
    void renderOneBlock (schwung_desktop_t* sd);

    double ratio = 1.0;          // Schwung frames consumed per output frame
    int    latency = 0;
    bool   passthrough = true;   // host is already at 44100

    std::vector<float> fifoL, fifoR;
    int readPos = 0, writePos = 0;

    juce::LagrangeInterpolator interpL, interpR;
    std::array<int16_t, SCHWUNG_BLOCK * 2> blockBuf {};
};

class SchwungAudioProcessor : public juce::AudioProcessor
{
public:
    SchwungAudioProcessor();
    ~SchwungAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Schwung"; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- message-thread API, used by the editor -------------------------

    /** Module ids found under <root>/sound_generators. */
    juce::StringArray getAvailableSynths() const { return availableSynths; }
    juce::StringArray getAvailableFx() const     { return availableFx; }

    juce::String getModuleRoot() const { return moduleRoot; }
    juce::String getStatus() const     { return status; }

    juce::String getSynth() const { return currentSynth; }
    void setSynth (const juce::String& id);

    juce::StringArray getAvailableMidiFx() const { return availableMidiFx; }

    juce::String getFx (int pos) const      { return fxModules[(size_t) pos]; }
    juce::String getMidiFx (int pos) const  { return midiFxModules[(size_t) pos]; }
    void setFx (int pos, const juce::String& id);
    void setMidiFx (int pos, const juce::String& id);

    /* The opaque per-component blob. "synth:state" and "fx<N>:state" answer a
     * module's WHOLE parameter set as JSON -- 294 bytes for braids -- and
     * writing it back restores it. It is the only thing carrying a module's
     * samples, preset and internal config, none of which a macro can reach.
     * Public so a test can diff what was saved against what came back. */
    juce::String readState (const juce::String& prefix);
    void writeState (const juce::String& prefix, const juce::String& blob);

    const MacroBinding& getBinding (int i) const { return bindings[(size_t) i]; }
    void setBinding (int i, const juce::String& key);
    void setBinding (int i, const juce::String& key, bool byUser);

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::String resolveModuleRoot();
    void scanModules();
    void bringUpChain();
    bool resolveRange (const juce::String& key, float& lo, float& hi, juce::String& why);
    void autoBindMacros();
    void loadModuleAt (const juce::String& writeKey, const juce::String& id,
                       juce::String& slotStore);
    void rebindAll (bool adopt);



    juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();

    schwung_desktop_t* sd = nullptr;
    juce::String moduleRoot, status, currentSynth;
    std::array<juce::String, kFxSlots> fxModules;
    std::array<juce::String, kMidiFxSlots> midiFxModules;
    juce::StringArray availableSynths, availableFx, availableMidiFx;

    std::array<MacroBinding, kMacroCount> bindings;
    std::array<std::atomic<float>, kMacroCount> lastSent {};
    std::array<MacroParameter*, kMacroCount> macroParams {};

    RateBridge bridge;

    /* True only while setStateInformation runs. A binding made during a
     * restore resolves its range but does NOT adopt the module's current
     * value -- see setBinding. */
    bool restoring = false;

    /* Guards every call into the chain. Taken with try_lock on the audio
     * thread, which then outputs silence rather than blocking -- a module
     * load can take hundreds of milliseconds and waiting for it on the audio
     * thread is the dropout this design exists to avoid. */
    std::mutex chainLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SchwungAudioProcessor)
};
