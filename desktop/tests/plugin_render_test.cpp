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

    std::printf ("PASS: the plugin renders Schwung audio through the rate bridge\n");
    return 0;
}
