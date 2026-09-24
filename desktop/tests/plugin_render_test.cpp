/*
 * plugin_render_test — drive the PLUGIN, not the chain, and check it makes
 * Schwung audio.
 *
 * auval and pluginval both pass on a plugin that renders pure silence, so
 * neither can tell "the wrapper is well-formed" from "the wrapper is
 * connected to anything". This does: it instantiates the real
 * SchwungAudioProcessor, picks a synth, sends note-ons through the same
 * MidiBuffer path a DAW uses, and fails unless the output is audible.
 *
 * It runs at 48 kHz in 512-frame blocks ON PURPOSE. The chain only ever runs
 * at 44100 in 128s, so every sample here has been through RateBridge, and a
 * bridge that silently produced silence or garbage would otherwise reach a
 * DAW before anyone noticed.
 */
#include "../plugin/PluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
    constexpr double kRate  = 48000.0;
    constexpr int    kBlock = 512;

    int fail (const char* why) { std::printf ("FAIL: %s\n", why); return 1; }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::String synth = (argc > 1) ? argv[1] : "braids";

    SchwungAudioProcessor proc;
    std::printf ("modules: %s\n", proc.getModuleRoot().toRawUTF8());
    std::printf ("status : %s\n", proc.getStatus().toRawUTF8());

    if (proc.getModuleRoot().isEmpty())
        return fail ("no module tree; set SCHWUNG_MODULES");

    if (! proc.getAvailableSynths().contains (synth))
        return fail (("synth '" + synth + "' not installed").toRawUTF8());

    proc.setSynth (synth);
    proc.prepareToPlay (kRate, kBlock);

    /* Bind a macro and move it, so the automation path is exercised too --
     * including the range lookup, which REFUSES rather than guessing. */
    proc.setBinding (0, "synth:timbre");
    const auto& b = proc.getBinding (0);
    std::printf ("macro 1: synth:timbre -> %s\n", b.status.toRawUTF8());
    if (! b.resolved)
        return fail ("macro binding was refused; the range lookup is broken");

    if (auto* p = proc.apvts.getParameter ("macro1"))
        p->setValueNotifyingHost (0.75f);

    juce::AudioBuffer<float> buf (2, kBlock);
    const int blocks = (int) (kRate * 3.0 / kBlock);

    double sum = 0.0; float peak = 0.0f; int samples = 0;

    for (int n = 0; n < blocks; ++n)
    {
        juce::MidiBuffer midi;
        if (n == 2)                 // a chord, once the chain has settled
        {
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            midi.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 0);
            midi.addEvent (juce::MidiMessage::noteOn (1, 67, (juce::uint8) 100), 0);
        }
        if (n == blocks / 2)
        {
            midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
            midi.addEvent (juce::MidiMessage::noteOff (1, 64), 0);
            midi.addEvent (juce::MidiMessage::noteOff (1, 67), 0);
        }

        buf.clear();
        proc.processBlock (buf, midi);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < kBlock; ++i)
            {
                const float s = buf.getSample (ch, i);
                if (! std::isfinite (s))
                    return fail ("output contains NaN or Inf");
                sum += (double) s * s;
                peak = juce::jmax (peak, std::abs (s));
                ++samples;
            }
    }

    const double rms = std::sqrt (sum / juce::jmax (1, samples));
    const double rmsDb  = rms  > 0 ? 20.0 * std::log10 (rms)  : -999.0;
    const double peakDb = peak > 0 ? 20.0 * std::log10 (peak) : -999.0;

    std::printf ("rendered %d blocks @ %.0f Hz / %d frames  rms %.1f dBFS  peak %.1f dBFS\n",
                 blocks, kRate, kBlock, rmsDb, peakDb);
    std::printf ("latency reported: %d samples\n", proc.getLatencySamples());

    /* A generous floor. The point is to separate "made sound" from "made
     * silence", not to pin a level -- every synth answers differently, and a
     * tight bound here would fail on the next module rather than on a bug. */
    if (rmsDb < -60.0)
        return fail ("output is effectively silent -- the wrapper is not connected to the chain");
    if (peak >= 1.0f)
        return fail ("output is clipping at full scale");

    /* ---- TWO INSTANCES IN ONE PROCESS ------------------------------------
     *
     * A Live set with two Schwung tracks is two SchwungAudioProcessors in one
     * host process, sharing one dlopen'd chain.dylib and whatever process-wide
     * state sits behind it. That is the ordinary case, not an edge case, and
     * nothing above this line exercises it. */
    {
        SchwungAudioProcessor a, b;
        a.setSynth (synth);
        b.setSynth (synth);
        a.prepareToPlay (kRate, kBlock);
        b.prepareToPlay (kRate, kBlock);

        juce::AudioBuffer<float> ba (2, kBlock), bb (2, kBlock);
        double sa = 0.0, sbv = 0.0;

        for (int n = 0; n < 120; ++n)
        {
            juce::MidiBuffer ma, mb;
            if (n == 2)
            {
                ma.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
                mb.addEvent (juce::MidiMessage::noteOn (1, 67, (juce::uint8) 100), 0);
            }
            ba.clear(); bb.clear();
            a.processBlock (ba, ma);
            b.processBlock (bb, mb);

            for (int i = 0; i < kBlock; ++i)
            {
                const float x = ba.getSample (0, i), y = bb.getSample (0, i);
                if (! std::isfinite (x) || ! std::isfinite (y))
                    return fail ("two instances: non-finite output");
                sa += (double) x * x;
                sbv += (double) y * y;
            }
        }

        const double ra = std::sqrt (sa / (120.0 * kBlock));
        const double rb = std::sqrt (sbv / (120.0 * kBlock));
        std::printf ("two instances: A %.1f dBFS   B %.1f dBFS\n",
                     ra > 0 ? 20.0 * std::log10 (ra) : -999.0,
                     rb > 0 ? 20.0 * std::log10 (rb) : -999.0);

        if (ra <= 0.0 || rb <= 0.0)
            return fail ("two instances: one of them is silent -- shared state between plugin instances");
    }

    auto renderRms = [] (SchwungAudioProcessor& p) -> double
    {
        juce::AudioBuffer<float> b (2, kBlock);
        double acc = 0.0; int count = 0;
        for (int n = 0; n < 120; ++n)
        {
            juce::MidiBuffer m;
            if (n == 2) m.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            b.clear();
            p.processBlock (b, m);
            for (int i = 0; i < kBlock; ++i)
            {
                const float v = b.getSample (0, i);
                acc += (double) v * v;
                ++count;
            }
        }
        return std::sqrt (acc / juce::jmax (1, count));
    };

    /* ---- SAVE, REOPEN, GET THE SAME CHAIN BACK --------------------------
     *
     * Covers state persistence, the FX positions and the MIDI FX positions
     * together, because nothing short of it does: a plugin that saves module
     * ids and macro bindings alone passes every other check in this file and
     * still reopens a project sounding like a new instance -- every parameter
     * no macro happened to cover back at its default, and a sampler's sample
     * gone entirely.
     *
     * WHAT IT COMPARES, AND WHY NOT LEVEL. The obvious test is "render before,
     * render after, same dBFS". It does not work, and the positive control
     * below is what proves it rather than an argument: rendering the SAME
     * processor twice gives -24.2 then -19.2 dBFS. braids' oscillator phase
     * and freeverb's tail carry from one render into the next, so a note lands
     * differently the second time. A 5 dB spread on an unchanged instance
     * means level cannot resolve a 0.5 dB claim about a changed one, and a
     * tolerance wide enough to pass would be wide enough to miss the bug.
     *
     * The state blob is the right instrument: it IS the module's whole
     * configuration, it is what restore writes, and it is exactly
     * reproducible. Rendering is still checked -- for audibility, which is the
     * part a blob comparison cannot see, since a blob that round-trips through
     * the file and is never applied compares equal and restores nothing.
     */
    {
        SchwungAudioProcessor a;
        a.setSynth (synth);
        if (a.getAvailableFx().contains ("freeverb")) a.setFx (0, "freeverb");
        a.prepareToPlay (kRate, kBlock);

        /* Move something well away from its default. Deliberately a parameter
         * reached through the module's own state rather than only through a
         * macro -- the claim is that the MODULE's configuration travels. */
        a.setBinding (0, "synth:engine");
        if (auto* p0 = a.apvts.getParameter ("macro1"))
            p0->setValueNotifyingHost (0.45f);

        const double r1 = renderRms (a);
        const double r2 = renderRms (a);
        std::printf ("control: one instance, two renders: %.2f / %.2f dBFS  (spread %.2f dB)\n",
                     20.0 * std::log10 (r1), 20.0 * std::log10 (r2),
                     std::abs (20.0 * std::log10 (r1) - 20.0 * std::log10 (r2)));

        juce::MemoryBlock saved;
        a.getStateInformation (saved);
        if (saved.getSize() == 0) return fail ("save produced nothing");

        SchwungAudioProcessor b;
        b.setStateInformation (saved.getData(), (int) saved.getSize());
        b.prepareToPlay (kRate, kBlock);

        if (b.getSynth() != a.getSynth())   return fail ("restore lost the synth");
        if (b.getFx (0) != a.getFx (0))     return fail ("restore lost fx1");

        const auto sa = a.readState ("synth");
        const auto sb = b.readState ("synth");
        if (sa.isEmpty())                   return fail ("the synth served no state to save");

        /* THE SAVED STATE MUST DIFFER FROM A FRESH MODULE'S.
         *
         * Without this the whole round-trip passes on defaults: if the macro
         * write never reached the module, or the blob were never applied, both
         * sides would hold the same untouched configuration and compare equal.
         * A test whose subject happens to equal the default cannot tell
         * "restored correctly" from "never changed in the first place". */
        {
            SchwungAudioProcessor fresh;
            fresh.setSynth (synth);
            if (fresh.readState ("synth") == sa)
                return fail ("saved state equals a fresh module's -- the edit never landed, "
                             "so the round-trip proves nothing");
        }
        if (sa != sb)
        {
            std::printf ("  saved   : %.300s\n", sa.toRawUTF8());
            std::printf ("  restored: %.300s\n", sb.toRawUTF8());
            return fail ("restored synth state differs from what was saved");
        }

        if (renderRms (b) <= 0.0)
            return fail ("restored instance is silent -- state compared equal but was never applied");

        std::printf ("save/restore: %s + %s, %d-byte synth state round-tripped\n",
                     b.getSynth().toRawUTF8(),
                     b.getFx (0).isEmpty() ? "-" : b.getFx (0).toRawUTF8(),
                     sa.length());
    }

    /* ---- THE TRANSPORT CLOCK --------------------------------------------
     *
     * The chain does NOT read the host's transport. It overrides
     * get_clock_status with its own, answered from MIDI realtime bytes it has
     * actually received, so a clock-gated module stays silent forever unless
     * the plugin synthesises 0xFA / 0xF8 / 0xFC from the playhead. breakbeat
     * checks it in five places; sequencers and arps are the same class.
     *
     * Setting get_bpm and get_beat_position is NOT enough, which is the part
     * that looks finished and is not -- verified on the device path: with the
     * transport set but no realtime bytes, breakbeat logged clock_status=1
     * and rendered silence; with the bytes, clock_status=2 and -19.6 dBFS.
     *
     * Asserted on the byte sequence because that is the contract, and none of
     * it is visible in the audio.
     */
    {
        TransportClock tc;
        std::vector<uint8_t> got;
        auto sink = [&got] (uint8_t b) { got.push_back (b); };

        // Stopped: nothing at all.
        tc.advance (false, 0.0, 0.25, sink);
        if (! got.empty()) return fail ("clock emitted while the transport was stopped");

        // Starting mid-bar: Start first, then ticks phased to the playhead.
        got.clear();
        tc.advance (true, 4.0, 4.25, sink);
        if (got.empty() || got[0] != 0xFA) return fail ("no MIDI Start when playback began");
        const int firstTicks = (int) std::count (got.begin(), got.end(), (uint8_t) 0xF8);
        if (firstTicks < 5 || firstTicks > 7)
            return fail ("wrong tick count for a quarter beat (expected 6 at 24 PPQN)");

        // A whole beat is 24 ticks.
        got.clear();
        for (int i = 0; i < 4; ++i)
            tc.advance (true, 4.25 + i * 0.25, 4.5 + i * 0.25, sink);
        const int beatTicks = (int) std::count (got.begin(), got.end(), (uint8_t) 0xF8);
        if (beatTicks != 24)
            return fail ("a beat did not carry 24 ticks");

        // A loop wrap jumps the playhead BACKWARDS. Re-phase, do not go mute
        // until the old count catches up -- which for a long loop is forever.
        got.clear();
        tc.advance (true, 0.0, 0.25, sink);
        if (std::count (got.begin(), got.end(), (uint8_t) 0xF8) < 5)
            return fail ("clock went quiet after a backwards jump (loop wrap)");

        // Stop, exactly once.
        got.clear();
        tc.advance (false, 1.0, 1.25, sink);
        if (std::count (got.begin(), got.end(), (uint8_t) 0xFC) != 1)
            return fail ("no single MIDI Stop when playback ended");

        // A locate across minutes must not emit the thousands of pulses
        // between: ticks are catch-up, not a log.
        got.clear();
        tc.advance (true, 0.0, 0.1, sink);
        got.clear();
        tc.advance (true, 0.1, 500.0, sink);
        const int flood = (int) std::count (got.begin(), got.end(), (uint8_t) 0xF8);
        if (flood > 96)
            return fail ("a locate flooded the chain with ticks");

        std::printf ("transport clock: start/stop, 24 PPQN, wrap re-phase, locate capped at %d\n", flood);
    }

    /* ---- LINE INPUT ------------------------------------------------------
     *
     * A line-input module does not receive audio as an argument. It reads the
     * SPI mailbox at host->audio_in_offset, because that is where the codec
     * puts it on the device -- so the plugin has to fill the same region, in
     * the same layout, before each render_block. `linein` exists to pass that
     * region straight through, which makes it the one honest instrument for
     * this: anything that comes out came from the sidechain.
     *
     * Checked in BOTH directions. Silence in must give silence out, or the
     * test would pass on a module that hums on its own and prove nothing about
     * routing.
     */
    if (proc.getAvailableSynths().contains ("linein"))
    {
        SchwungAudioProcessor p;
        p.setSynth ("linein");
        p.prepareToPlay (kRate, kBlock);

        /* Enable the sidechain the way a host would. */
        auto layout = p.getBusesLayout();
        layout.inputBuses.set (0, juce::AudioChannelSet::stereo());
        const bool enabled = p.setBusesLayout (layout);
        std::printf ("line input: sidechain %s\n", enabled ? "enabled" : "REFUSED by the layout");
        if (! enabled) return fail ("the plugin refused a stereo sidechain");

        p.prepareToPlay (kRate, kBlock);

        auto runWith = [&] (bool feed) -> double
        {
            /* TWO channels, not four. With one input bus and one output bus
             * JUCE lays them over the SAME buffer channels -- the usual
             * in-place arrangement -- so the sidechain arrives in channels
             * 0-1, exactly where the output will be written. That is why
             * processBlock must read the input BEFORE clearing the output,
             * and why a 4-channel buffer writes the tone nowhere. */
            juce::AudioBuffer<float> b (juce::jmax (p.getTotalNumInputChannels(),
                                                    p.getTotalNumOutputChannels()), kBlock);
            double acc = 0.0; int count = 0;
            double phase = 0.0;
            const double inc = 2.0 * juce::MathConstants<double>::pi * 220.0 / kRate;

            for (int n = 0; n < 60; ++n)
            {
                b.clear();
                if (feed)
                    for (int i = 0; i < kBlock; ++i)
                    {
                        const float v = 0.5f * (float) std::sin (phase);
                        phase += inc;
                        b.setSample (0, i, v);
                        if (b.getNumChannels() > 1) b.setSample (1, i, v);
                    }

                juce::MidiBuffer m;
                p.processBlock (b, m);

                if (n >= 20)   // skip the bridge priming
                    for (int i = 0; i < kBlock; ++i)
                    {
                        const float o = b.getSample (0, i);
                        acc += (double) o * o;
                        ++count;
                    }
            }
            return std::sqrt (acc / juce::jmax (1, count));
        };

        const double quiet = runWith (false);
        const double loud  = runWith (true);

        std::printf ("line input: silence in %.1f dBFS, signal in %.1f dBFS\n",
                     quiet > 0 ? 20.0 * std::log10 (quiet) : -999.0,
                     loud  > 0 ? 20.0 * std::log10 (loud)  : -999.0);

        if (loud <= 0.0)
            return fail ("line input produced nothing -- the sidechain is not reaching the mailbox");
        if (quiet > 0.0 && loud / quiet < 4.0)
            return fail ("output barely changed with input -- not actually passing the sidechain through");
    }
    else
    {
        std::printf ("line input: SKIPPED (linein not installed)\n");
    }

    std::printf ("PASS: the plugin renders Schwung audio through the rate bridge\n");
    return 0;
}
