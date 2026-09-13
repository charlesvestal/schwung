#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>

// =====================================================================
// RateBridge
// =====================================================================

void RateBridge::prepare (double hostSampleRate, int maxBlock)
{
    ratio = (double) SCHWUNG_RATE / (hostSampleRate > 0 ? hostSampleRate : SCHWUNG_RATE);
    passthrough = std::abs (hostSampleRate - (double) SCHWUNG_RATE) < 1.0e-6;

    /* Room for the largest pull plus a whole Schwung block plus the
     * interpolator's own look-ahead, so renderOneBlock never has to grow the
     * buffer from the audio thread. */
    const int capacity = (int) std::ceil (maxBlock * ratio) + SCHWUNG_BLOCK * 4 + 16;
    fifoL.assign ((size_t) capacity, 0.0f);
    fifoR.assign ((size_t) capacity, 0.0f);

    /* A Lagrange interpolator needs samples on both sides of its read point.
     * At the host's rate that is a handful of frames; reported so the DAW can
     * line this track up with the rest of the set. Zero when no conversion is
     * happening at all. */
    latency = passthrough ? 0 : (int) std::ceil (4.0 / ratio);

    reset();
}

void RateBridge::reset()
{
    std::fill (fifoL.begin(), fifoL.end(), 0.0f);
    std::fill (fifoR.begin(), fifoR.end(), 0.0f);
    readPos = writePos = 0;
    interpL.reset();
    interpR.reset();
}

void RateBridge::renderOneBlock (schwung_desktop_t* sd)
{
    schwung_desktop_render (sd, blockBuf.data());

    if (writePos + SCHWUNG_BLOCK > (int) fifoL.size())
    {
        /* Compact rather than grow: the capacity was sized in prepare() and
         * allocating here would be an allocation on the audio thread. */
        const int live = writePos - readPos;
        if (live > 0)
        {
            std::memmove (fifoL.data(), fifoL.data() + readPos, sizeof (float) * (size_t) live);
            std::memmove (fifoR.data(), fifoR.data() + readPos, sizeof (float) * (size_t) live);
        }
        readPos = 0;
        writePos = juce::jmax (0, live);
    }

    for (int i = 0; i < SCHWUNG_BLOCK; ++i)
    {
        fifoL[(size_t) (writePos + i)] = blockBuf[(size_t) (i * 2)]     / 32768.0f;
        fifoR[(size_t) (writePos + i)] = blockBuf[(size_t) (i * 2 + 1)] / 32768.0f;
    }
    writePos += SCHWUNG_BLOCK;
}

void RateBridge::pull (schwung_desktop_t* sd, float* outL, float* outR, int numSamples)
{
    if (sd == nullptr || numSamples <= 0)
    {
        if (outL) juce::FloatVectorOperations::clear (outL, numSamples);
        if (outR) juce::FloatVectorOperations::clear (outR, numSamples);
        return;
    }

    /* One extra whole block of slack: the interpolator may consume a sample or
     * two beyond the arithmetic minimum, and running the FIFO dry mid-pull
     * would splice a discontinuity into the output. */
    const int needed = (int) std::ceil (numSamples * ratio) + SCHWUNG_BLOCK;
    while (writePos - readPos < needed)
        renderOneBlock (sd);

    if (passthrough)
    {
        std::memcpy (outL, fifoL.data() + readPos, sizeof (float) * (size_t) numSamples);
        std::memcpy (outR, fifoR.data() + readPos, sizeof (float) * (size_t) numSamples);
        readPos += numSamples;
        return;
    }

    const int usedL = interpL.process (ratio, fifoL.data() + readPos, outL, numSamples);
    const int usedR = interpR.process (ratio, fifoR.data() + readPos, outR, numSamples);

    /* The two channels run the same ratio through the same number of output
     * samples, so they must consume the same input. If they ever disagree the
     * channels have drifted apart and the larger is the honest advance. */
    jassert (usedL == usedR);
    readPos += juce::jmax (usedL, usedR);
}

// =====================================================================
// TransportClock
// =====================================================================

void TransportClock::advance (bool playing, double ppqStart, double ppqEnd,
                              const std::function<void (uint8_t)>& send)
{
    if (playing && ! wasPlaying)
    {
        send (0xFA);                                   // Start
        /* Phase to the playhead, not to zero. Starting the count here is what
         * puts a tick on the beat when playback begins mid-bar. */
        nextTick = std::ceil (ppqStart * kPPQN);
        wasPlaying = true;
    }
    else if (! playing && wasPlaying)
    {
        send (0xFC);                                   // Stop
        wasPlaying = false;
        return;
    }

    if (! playing) return;

    const double endTick = ppqEnd * kPPQN;

    /* A backwards jump (loop wrap, locate) means the old count is meaningless.
     * Re-phase rather than emitting nothing until the playhead catches up. */
    if (endTick + 1.0 < nextTick)
        nextTick = std::ceil (ppqStart * kPPQN);

    int emitted = 0;
    while (nextTick <= endTick && emitted < kMaxTicksPerBlock)
    {
        send (0xF8);                                   // Timing Clock
        nextTick += 1.0;
        ++emitted;
    }

    if (emitted >= kMaxTicksPerBlock)
        nextTick = std::ceil (endTick);                // gave up catching up
}

// =====================================================================
// Processor
// =====================================================================

juce::AudioProcessorValueTreeState::ParameterLayout SchwungAudioProcessor::makeLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (int i = 0; i < kMacroCount; ++i)
        layout.add (std::make_unique<MacroParameter> ("macro" + juce::String (i + 1),
                                                      "Macro " + juce::String (i + 1)));
    return layout;
}

SchwungAudioProcessor::SchwungAudioProcessor()
    : juce::AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "SCHWUNG", makeLayout())
{
    for (int i = 0; i < kMacroCount; ++i)
    {
        macroParams[(size_t) i] = dynamic_cast<MacroParameter*> (apvts.getParameter ("macro" + juce::String (i + 1)));
        lastSent[(size_t) i].store (-1.0f);
    }

    moduleRoot = resolveModuleRoot();
    scanModules();
    bringUpChain();
}

SchwungAudioProcessor::~SchwungAudioProcessor()
{
    const std::lock_guard<std::mutex> lock (chainLock);
    if (sd != nullptr) { schwung_desktop_destroy (sd); sd = nullptr; }
}

/*
 * Where the module tree lives.
 *
 * Checked in order, and the first that exists wins:
 *   1. $SCHWUNG_MODULES            -- explicit, for development
 *   2. ~/Library/Application Support/Schwung/modules   (or ~/.schwung/modules)
 *   3. nothing; the plugin comes up with no chain and SAYS SO
 *
 * The third case is deliberately not a silent failure. A plugin that loads
 * with no modules and no message is indistinguishable from one whose synth
 * simply is not making sound.
 */
juce::String SchwungAudioProcessor::resolveModuleRoot()
{
    if (auto env = juce::SystemStats::getEnvironmentVariable ("SCHWUNG_MODULES", {}); env.isNotEmpty())
        if (juce::File (env).isDirectory())
            return env;

    /* userApplicationDataDirectory is NOT the same folder on every platform:
     * on macOS it is ~/Library, not ~/Library/Application Support, so the
     * obvious one-liner looks in a directory nobody installs to and reports
     * "no modules found" with the modules sitting right there. */
    auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    appData = appData.getChildFile ("Application Support");
   #endif
    appData = appData.getChildFile ("Schwung").getChildFile ("modules");
    if (appData.isDirectory())
        return appData.getFullPathName();

    return {};
}

void SchwungAudioProcessor::scanModules()
{
    availableSynths.clear();
    availableFx.clear();
    if (moduleRoot.isEmpty()) return;

    auto add = [] (const juce::File& dir, juce::StringArray& into)
    {
        if (! dir.isDirectory()) return;
        for (const auto& e : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findDirectories))
            into.add (e.getFile().getFileName());
        into.sort (true);
    };

    add (juce::File (moduleRoot).getChildFile ("sound_generators"), availableSynths);
    add (juce::File (moduleRoot).getChildFile ("audio_fx"), availableFx);
    add (juce::File (moduleRoot).getChildFile ("midi_fx"), availableMidiFx);
}

void SchwungAudioProcessor::bringUpChain()
{
    const std::lock_guard<std::mutex> lock (chainLock);

    if (sd != nullptr) { schwung_desktop_destroy (sd); sd = nullptr; }

    if (moduleRoot.isEmpty())
    {
        status = "No module tree found. Set SCHWUNG_MODULES, or install modules to "
                 "~/Library/Application Support/Schwung/modules.";
        return;
    }

    sd = schwung_desktop_create (moduleRoot.toRawUTF8(), nullptr);
    status = (sd != nullptr) ? "Chain ready." : "Chain failed to load from " + moduleRoot;
}

/*
 * Load a module into one position.
 *
 * Suspended rather than locked-and-hoped: a create_instance can read samples
 * off disk for hundreds of milliseconds, and waiting for that on the audio
 * thread is the dropout this exists to avoid.
 *
 * An EMPTY id means "empty this position", and the chain spells that "None" --
 * writing "" is not the same thing and leaves whatever was there running.
 */
void SchwungAudioProcessor::loadModuleAt (const juce::String& writeKey,
                                          const juce::String& id,
                                          juce::String& slotStore)
{
    suspendProcessing (true);
    {
        const std::lock_guard<std::mutex> lock (chainLock);
        if (sd != nullptr)
        {
            schwung_desktop_set_param (sd, writeKey.toRawUTF8(),
                                       id.isEmpty() ? "None" : id.toRawUTF8());
            slotStore = id;
        }
    }
    suspendProcessing (false);
}

/* Re-resolve every binding. Ranges belong to the module that just loaded --
 * the same key means per-cent in one module and dB in the next -- so a load
 * invalidates all of them, bound by hand or not. */
void SchwungAudioProcessor::rebindAll (bool adopt)
{
    const juce::ScopedValueSetter<bool> scope (restoring, ! adopt);
    for (int i = 0; i < kMacroCount; ++i)
        setBinding (i, bindings[(size_t) i].key, bindings[(size_t) i].userSet);
}

void SchwungAudioProcessor::setSynth (const juce::String& id)
{
    loadModuleAt ("synth:module", id, currentSynth);
    rebindAll (! restoring);
    if (! restoring) autoBindMacros();
}

void SchwungAudioProcessor::setFx (int pos, const juce::String& id)
{
    if (! juce::isPositiveAndBelow (pos, kFxSlots)) return;
    loadModuleAt ("fx" + juce::String (pos + 1) + ":module", id, fxModules[(size_t) pos]);
    rebindAll (! restoring);
}

void SchwungAudioProcessor::setMidiFx (int pos, const juce::String& id)
{
    if (! juce::isPositiveAndBelow (pos, kMidiFxSlots)) return;
    loadModuleAt ("midi_fx" + juce::String (pos + 1) + ":module", id, midiFxModules[(size_t) pos]);
    rebindAll (! restoring);
}

/* ---- the opaque state blob -------------------------------------------- */

juce::String SchwungAudioProcessor::readState (const juce::String& prefix)
{
    if (sd == nullptr) return {};
    std::vector<char> buf ((size_t) 262144);
    int n;
    {
        const std::lock_guard<std::mutex> lock (chainLock);
        n = schwung_desktop_get_param (sd, (prefix + ":state").toRawUTF8(),
                                       buf.data(), (int) buf.size());
    }
    /* n < 0 is a read that did not complete and n == 0 is a module with no
     * state. Neither is an error, and neither may be saved as "" -- writing an
     * empty blob back on restore is what would wipe a module that simply
     * failed to answer this once. */
    return n > 0 ? juce::String::fromUTF8 (buf.data(), n) : juce::String();
}

void SchwungAudioProcessor::writeState (const juce::String& prefix, const juce::String& blob)
{
    if (sd == nullptr || blob.isEmpty()) return;
    suspendProcessing (true);
    {
        const std::lock_guard<std::mutex> lock (chainLock);
        schwung_desktop_set_param (sd, (prefix + ":state").toRawUTF8(), blob.toRawUTF8());
    }
    suspendProcessing (false);
}

/*
 * Find a key's declared range in the module's own chain_params.
 *
 * A FAILURE HERE IS A REFUSAL, NOT A DEFAULT. Falling back to 0..1 would write
 * a plausible-looking wrong number into a dB or Hz parameter, which is worse
 * than not binding at all because it makes a sound and so reads as working.
 */
bool SchwungAudioProcessor::resolveRange (const juce::String& key, float& lo, float& hi, juce::String& why)
{
    if (sd == nullptr) { why = "no chain"; return false; }

    /* Which component's contract to ask for. The chain namespaces its keys, so
     * "synth:timbre" is parameter "timbre" of the synth's chain_params. */
    const int colon = key.indexOfChar (':');
    if (colon <= 0) { why = "key needs a component prefix, e.g. synth:timbre"; return false; }
    const auto component = key.substring (0, colon);
    const auto leaf = key.substring (colon + 1);

    std::vector<char> buf ((size_t) 262144);
    int n;
    {
        const std::lock_guard<std::mutex> lock (chainLock);
        n = schwung_desktop_get_param (sd, (component + ":chain_params").toRawUTF8(),
                                       buf.data(), (int) buf.size());
    }

    /* THREE ANSWERS, NOT TWO. n < 0 is a read that did not complete; an empty
     * string is a read that completed and produced nothing. Only the second
     * means "this component has no parameters", and neither may become a
     * guessed range. */
    if (n < 0)  { why = "contract read did not complete"; return false; }
    if (n == 0) { why = "component declares no chain_params"; return false; }

    auto parsed = juce::JSON::parse (juce::String::fromUTF8 (buf.data(), n));
    if (auto* arr = parsed.getArray())
    {
        for (const auto& item : *arr)
        {
            if (item.getProperty ("key", {}).toString() != leaf) continue;

            const auto type = item.getProperty ("type", {}).toString();
            if (type == "enum")
            {
                if (auto* opts = item.getProperty ("options", {}).getArray())
                {
                    lo = 0.0f;
                    hi = (float) juce::jmax (0, opts->size() - 1);
                    return true;
                }
                why = "enum declares no options";
                return false;
            }

            const auto minV = item.getProperty ("min", {});
            const auto maxV = item.getProperty ("max", {});
            if (minV.isVoid() || maxV.isVoid()) { why = "parameter declares no min/max"; return false; }
            lo = (float) (double) minV;
            hi = (float) (double) maxV;
            return true;
        }
        why = "no such parameter in " + component + "'s contract";
        return false;
    }

    why = "contract did not parse as an array";
    return false;
}

/*
 * Fill the macro bank from whatever module is loaded.
 *
 * Without this the bank is eight empty slots and the only way to reach a
 * parameter is to know its key and type it, which is not a user interface.
 * The module already publishes everything needed -- key, display name and
 * range -- in its own chain_params; this just takes the first kMacroCount of
 * them, in the order the module declares, which is the order its own pages use.
 *
 * A macro the user bound BY HAND is left alone. Auto-binding is a starting
 * point, not a policy, and silently overwriting a deliberate binding on every
 * module change would make the manual field useless.
 */
void SchwungAudioProcessor::autoBindMacros()
{
    if (sd == nullptr || currentSynth.isEmpty()) return;

    std::vector<char> buf ((size_t) 262144);
    int n;
    {
        const std::lock_guard<std::mutex> lock (chainLock);
        n = schwung_desktop_get_param (sd, "synth:chain_params", buf.data(), (int) buf.size());
    }
    if (n <= 0) return;   // a failed or unserved read binds nothing, and says nothing

    auto parsed = juce::JSON::parse (juce::String::fromUTF8 (buf.data(), n));
    auto* arr = parsed.getArray();
    if (arr == nullptr) return;

    int slot = 0;
    for (const auto& item : *arr)
    {
        if (slot >= kMacroCount) break;
        while (slot < kMacroCount && bindings[(size_t) slot].userSet) ++slot;
        if (slot >= kMacroCount) break;

        const auto key = item.getProperty ("key", {}).toString();
        if (key.isEmpty()) continue;

        setBinding (slot, "synth:" + key, /*byUser=*/false);
        bindings[(size_t) slot].label = item.getProperty ("name", key).toString();

        if (auto* p = macroParams[(size_t) slot])
            p->setDisplayName (bindings[(size_t) slot].label);
        ++slot;
    }

    /* Clear any trailing auto-bindings left over from a module with more
     * parameters than this one. */
    for (; slot < kMacroCount; ++slot)
        if (! bindings[(size_t) slot].userSet)
        {
            setBinding (slot, {}, false);
            if (auto* p = macroParams[(size_t) slot]) p->setDisplayName ({});
        }

    /* Tell the host the titles moved. Without this Live keeps showing
     * "Macro 1..8" in its automation list until the plugin is reloaded. */
    updateHostDisplay (juce::AudioProcessor::ChangeDetails{}.withParameterInfoChanged (true));
}

void SchwungAudioProcessor::setBinding (int i, const juce::String& key)
{
    setBinding (i, key, /*byUser=*/true);
}

void SchwungAudioProcessor::setBinding (int i, const juce::String& key, bool byUser)
{
    if (! juce::isPositiveAndBelow (i, kMacroCount)) return;

    auto& b = bindings[(size_t) i];
    b.key = key;
    b.resolved = false;
    b.min = 0.0f;
    b.max = 1.0f;
    b.status = {};
    b.label = {};
    if (byUser) b.userSet = key.isNotEmpty();

    if (key.isEmpty()) { b.status = "unbound"; return; }

    juce::String why;
    if (resolveRange (key, b.min, b.max, why))
    {
        b.resolved = true;
        b.status = juce::String (b.min, 2) + " .. " + juce::String (b.max, 2);
    }
    else
    {
        b.status = "refused: " + why;
    }

    /*
     * ADOPT THE MODULE'S CURRENT VALUE. DO NOT PUSH THE MACRO'S.
     *
     * A macro defaults to 0.0, and binding it used to mark the value dirty so
     * the next processBlock wrote it out. With auto-binding that meant every
     * parameter of a freshly loaded module was slammed to its MINIMUM -- level,
     * sustain, decay and all -- the instant it loaded. braids went from
     * -21 dBFS to silence, and the module looked broken rather than overwritten.
     *
     * So the binding reads what the module already has and moves the macro
     * there, then records it as sent. Nothing is written until a human or an
     * automation lane actually moves the control.
     */
    if (b.resolved && sd != nullptr && ! restoring)
    {
        char cur[128];
        int n;
        {
            const std::lock_guard<std::mutex> lock (chainLock);
            n = schwung_desktop_get_param (sd, b.key.toRawUTF8(), cur, (int) sizeof (cur));
        }

        if (n > 0)
        {
            const float span = b.max - b.min;
            const float norm = (span > 0.0f)
                             ? juce::jlimit (0.0f, 1.0f, ((float) juce::String (cur).getDoubleValue() - b.min) / span)
                             : 0.0f;
            if (auto* p = macroParams[(size_t) i])
                p->setValueNotifyingHost (norm);
            lastSent[(size_t) i].store (norm);
        }
        else
        {
            /* A read that did not complete is not a value. Leave the macro
             * where it is and mark it sent, so an unknown current value is
             * never "corrected" to the macro's default. */
            if (auto* p = macroParams[(size_t) i])
                lastSent[(size_t) i].store (p->convertTo0to1 (p->get()));
        }
    }
    else
    {
        /* -1 marks the macro dirty, so processBlock writes it out.
         *
         * THIS IS THE RESTORE PATH, AND IT MUST NOT ADOPT. Reopening a Live
         * set replaces the parameter state first and loads the module second;
         * if binding then adopted the module's freshly-constructed DEFAULTS it
         * would overwrite every value the set just restored, and the project
         * would reopen sounding like a new instance. Saved values win, and are
         * pushed INTO the module instead. */
        lastSent[(size_t) i].store (-1.0f);
    }
}

void SchwungAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    bridge.prepare (sampleRate, samplesPerBlock);
    clock.reset();
    setLatencySamples (bridge.getLatencySamples());
    for (auto& v : lastSent) v.store (-1.0f);
}

void SchwungAudioProcessor::releaseResources()
{
    bridge.reset();
}

bool SchwungAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void SchwungAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    /* try_lock, never lock. The message thread holds this across a module
     * load; waiting for that here is exactly the dropout the suspend is meant
     * to prevent, and a suspended host may still call us once on the way in. */
    std::unique_lock<std::mutex> lock (chainLock, std::try_to_lock);
    if (! lock.owns_lock() || sd == nullptr)
        return;

    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            const double bpm     = pos->getBpm().orFallback (120.0);
            const double beat    = pos->getPpqPosition().orFallback (-1.0);
            const bool   playing = pos->getIsPlaying();

            schwung_desktop_set_transport (sd, bpm, beat, playing ? 1 : 0);

            /* And the realtime clock, which is a SEPARATE channel the chain
             * answers get_clock_status from -- see TransportClock. Emitted
             * before this block's MIDI so a module that starts on 0xFA is
             * running by the time the first note arrives. */
            if (beat >= 0.0)
            {
                const double blockBeats = (double) buffer.getNumSamples()
                                        / juce::jmax (1.0, getSampleRate()) * bpm / 60.0;
                clock.advance (playing, beat, beat + blockBeats,
                               [this] (uint8_t status)
                               {
                                   const uint8_t msg[1] = { status };
                                   schwung_desktop_midi (sd, msg, 1);
                               });
            }
        }
    }

    /* Macros first, so a value automated for this block is in effect for it.
     * Only changes are written: set_param on the chain can be expensive, and
     * a DAW re-sends every parameter every block whether or not it moved. */
    for (int i = 0; i < kMacroCount; ++i)
    {
        const auto& b = bindings[(size_t) i];
        if (! b.resolved) continue;

        const float norm = macroParams[(size_t) i]->convertTo0to1 (macroParams[(size_t) i]->get());
        if (std::abs (norm - lastSent[(size_t) i].load()) < 1.0e-6f) continue;
        lastSent[(size_t) i].store (norm);

        const float scaled = b.min + norm * (b.max - b.min);
        char val[32];
        std::snprintf (val, sizeof (val), "%.6f", scaled);
        schwung_desktop_set_param (sd, b.key.toRawUTF8(), val);
    }

    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (msg.getRawDataSize() >= 1 && msg.getRawDataSize() <= 3)
            schwung_desktop_midi (sd, msg.getRawData(), msg.getRawDataSize());
    }

    bridge.pull (sd, buffer.getWritePointer (0), buffer.getWritePointer (1), buffer.getNumSamples());
}

juce::AudioProcessorEditor* SchwungAudioProcessor::createEditor()
{
    return new SchwungAudioProcessorEditor (*this);
}

/*
 * WHAT A SAVED LIVE SET HAS TO CARRY.
 *
 * Module ids and macro bindings are not enough, and saving only those is the
 * quietest kind of data loss: the project reopens, the right modules load, the
 * chain works -- and it sounds like a new instance, because every parameter a
 * macro does not happen to cover came back at its default. Worse for a sampler
 * or a drum module, where the sample and the kit are not parameters at all.
 *
 * The opaque "<prefix>:state" blob is the module's WHOLE configuration and is
 * the only thing that carries those. It is saved per position.
 */
void SchwungAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    auto chain = state.getOrCreateChildWithName ("chain", nullptr);

    chain.setProperty ("synth", currentSynth, nullptr);
    chain.setProperty ("synthState", readState ("synth"), nullptr);

    for (int i = 0; i < kFxSlots; ++i)
    {
        const auto n = juce::String (i + 1);
        chain.setProperty ("fx" + n, fxModules[(size_t) i], nullptr);
        if (fxModules[(size_t) i].isNotEmpty())
            chain.setProperty ("fx" + n + "State", readState ("fx" + n), nullptr);
    }

    for (int i = 0; i < kMidiFxSlots; ++i)
    {
        const auto n = juce::String (i + 1);
        chain.setProperty ("mfx" + n, midiFxModules[(size_t) i], nullptr);
        if (midiFxModules[(size_t) i].isNotEmpty())
            chain.setProperty ("mfx" + n + "State", readState ("midi_fx" + n), nullptr);
    }

    /* Only bound macros are written. 512 properties per instance, almost all
     * empty, would bloat every Live set that ever loaded this plugin. */
    for (int i = 0; i < kMacroCount; ++i)
        if (bindings[(size_t) i].key.isNotEmpty())
            chain.setProperty ("bind" + juce::String (i), bindings[(size_t) i].key, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void SchwungAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr) return;

    auto state = juce::ValueTree::fromXml (*xml);
    if (! state.isValid()) return;
    apvts.replaceState (state);

    auto chain = state.getChildWithName ("chain");
    if (! chain.isValid()) return;

    /*
     * ORDER IS THE WHOLE THING HERE.
     *
     *   1. load the modules            -- a state blob is meaningless until the
     *                                     module that understands it exists
     *   2. write the state blobs       -- this is what restores the SOUND
     *   3. re-read the bindings        -- ranges belong to the loaded module
     *   4. adopt, LAST                 -- so the macros show the restored
     *                                     values rather than overwriting them
     *
     * `restoring` suppresses adoption for steps 1-3. Doing it any earlier
     * pushes a freshly constructed module's defaults over the file: the
     * project reopens sounding wrong, with nothing logged and a perfectly
     * valid set on disk. That failure shipped for about an hour.
     */
    {
        const juce::ScopedValueSetter<bool> scope (restoring, true);

        for (int i = 0; i < kMacroCount; ++i)
            setBinding (i, chain.getProperty ("bind" + juce::String (i), "").toString(), true);

        setSynth (chain.getProperty ("synth", "").toString());

        for (int i = 0; i < kFxSlots; ++i)
            setFx (i, chain.getProperty ("fx" + juce::String (i + 1), "").toString());

        for (int i = 0; i < kMidiFxSlots; ++i)
            setMidiFx (i, chain.getProperty ("mfx" + juce::String (i + 1), "").toString());

        writeState ("synth", chain.getProperty ("synthState", "").toString());

        for (int i = 0; i < kFxSlots; ++i)
        {
            const auto n = juce::String (i + 1);
            writeState ("fx" + n, chain.getProperty ("fx" + n + "State", "").toString());
        }
        for (int i = 0; i < kMidiFxSlots; ++i)
        {
            const auto n = juce::String (i + 1);
            writeState ("midi_fx" + n, chain.getProperty ("mfx" + n + "State", "").toString());
        }
    }

    autoBindMacros();
    rebindAll (/*adopt=*/true);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SchwungAudioProcessor();
}
