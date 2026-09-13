/*
 * PluginEditor.h — the plugin's window.
 *
 * This is NOT the Move OLED. Blitting the real 128x64 shadow UI means porting
 * shadow_ui.c's process and its shared-memory protocol, which is task 0.8 of
 * the port plan and a much larger piece of work. What this window does is the
 * part the OLED cannot do in a DAW anyway: name the chain, and let a macro be
 * bound to a parameter key so Live can automate it.
 *
 * The row layout is the information: each macro shows its key, its slider, and
 * the RANGE that key declared -- or why the binding was refused. A refusal has
 * to be visible, because an unbound macro that silently does nothing is the
 * failure this whole surface exists to prevent.
 */
#pragma once

#include "PluginProcessor.h"

class MacroRow : public juce::Component
{
public:
    MacroRow (SchwungAudioProcessor& p, int index);
    void resized() override;
    void refresh();

private:
    SchwungAudioProcessor& proc;
    int idx;

    juce::Label       number;
    juce::TextEditor  keyField;
    juce::Slider      slider;
    juce::Label       range;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

class SchwungAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    private juce::Timer
{
public:
    explicit SchwungAudioProcessorEditor (SchwungAudioProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    SchwungAudioProcessor& proc;

    juce::ComboBox synthBox, fxBox;
    juce::Label    synthLabel, fxLabel, statusLabel, title;
    juce::OwnedArray<MacroRow> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SchwungAudioProcessorEditor)
};
