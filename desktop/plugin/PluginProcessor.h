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
 * keys that depend on which module is loaded. Publishing "Macro 1..N" and
 * letting the user bind each to a key is the standard answer, and it keeps a
 * saved Live set valid when the bound module changes underneath it. */
static constexpr int kMacroCount = 8;

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
    float        min = 0.0f;
    float        max = 1.0f;
    bool         resolved = false;
    juce::String status;      // why it is not resolved, for the editor to show
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

    juce::String getFx() const { return currentFx; }
    void setFx (const juce::String& id);

    const MacroBinding& getBinding (int i) const { return bindings[(size_t) i]; }
    void setBinding (int i, const juce::String& key);

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::String resolveModuleRoot();
    void scanModules();
    void bringUpChain();
    bool resolveRange (const juce::String& key, float& lo, float& hi, juce::String& why);
    void pushMacroWrite (int index, float normalised);

    juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();

    schwung_desktop_t* sd = nullptr;
    juce::String moduleRoot, status, currentSynth, currentFx;
    juce::StringArray availableSynths, availableFx;

    std::array<MacroBinding, kMacroCount> bindings;
    std::array<std::atomic<float>, kMacroCount> lastSent {};
    std::array<juce::RangedAudioParameter*, kMacroCount> macroParams {};

    RateBridge bridge;

    /* Guards every call into the chain. Taken with try_lock on the audio
     * thread, which then outputs silence rather than blocking -- a module
     * load can take hundreds of milliseconds and waiting for it on the audio
     * thread is the dropout this design exists to avoid. */
    std::mutex chainLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SchwungAudioProcessor)
};
