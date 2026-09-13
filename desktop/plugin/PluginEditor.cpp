#include "PluginEditor.h"

namespace
{
    const juce::Colour kGround   { 0xff16191c };
    const juce::Colour kSurface  { 0xff1f2428 };
    const juce::Colour kInk      { 0xffe3e8e9 };
    const juce::Colour kInkFaint { 0xff8a949b };
    const juce::Colour kSignal   { 0xff4ec2d3 };
    const juce::Colour kRefused  { 0xffd5776b };

    constexpr int kRowH   = 34;
    constexpr int kPad    = 12;
    constexpr int kHeadH  = 108;
}

// ---------------------------------------------------------------- MacroRow

MacroRow::MacroRow (SchwungAudioProcessor& p, int index) : proc (p), idx (index)
{
    number.setText (juce::String (index + 1), juce::dontSendNotification);
    number.setColour (juce::Label::textColourId, kInkFaint);
    number.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (number);

    keyField.setTextToShowWhenEmpty ("synth:timbre", kInkFaint);
    keyField.setColour (juce::TextEditor::backgroundColourId, kSurface);
    keyField.setColour (juce::TextEditor::textColourId, kInk);
    keyField.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff2a3134));
    keyField.onReturnKey = [this] { proc.setBinding (idx, keyField.getText().trim()); refresh(); };
    keyField.onFocusLost = [this] { proc.setBinding (idx, keyField.getText().trim()); refresh(); };
    addAndMakeVisible (keyField);

    slider.setSliderStyle (juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 20);
    slider.setColour (juce::Slider::trackColourId, kSignal);
    slider.setColour (juce::Slider::thumbColourId, kSignal);
    slider.setColour (juce::Slider::textBoxTextColourId, kInk);
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (slider);

    range.setColour (juce::Label::textColourId, kInkFaint);
    range.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (range);

    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        proc.apvts, "macro" + juce::String (index + 1), slider);

    refresh();
}

void MacroRow::refresh()
{
    const auto& b = proc.getBinding (idx);
    if (keyField.getText() != b.key)
        keyField.setText (b.key, juce::dontSendNotification);
    range.setText (b.status, juce::dontSendNotification);
    range.setColour (juce::Label::textColourId,
                     b.status.startsWith ("refused") ? kRefused : kInkFaint);
}

void MacroRow::resized()
{
    auto r = getLocalBounds().reduced (0, 3);
    number.setBounds (r.removeFromLeft (22));
    r.removeFromLeft (6);
    keyField.setBounds (r.removeFromLeft (150));
    r.removeFromLeft (8);
    range.setBounds (r.removeFromRight (130));
    r.removeFromRight (8);
    slider.setBounds (r);
}

// ------------------------------------------------------------------ Editor

SchwungAudioProcessorEditor::SchwungAudioProcessorEditor (SchwungAudioProcessor& p)
    : juce::AudioProcessorEditor (&p), proc (p)
{
    title.setText ("Schwung", juce::dontSendNotification);
    title.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, kInk);
    addAndMakeVisible (title);

    auto initLabel = [this] (juce::Label& l, const juce::String& t)
    {
        l.setText (t, juce::dontSendNotification);
        l.setFont (juce::FontOptions (11.0f));
        l.setColour (juce::Label::textColourId, kInkFaint);
        addAndMakeVisible (l);
    };
    initLabel (synthLabel, "SYNTH");
    initLabel (fxLabel, "FX 1");

    auto initBox = [this] (juce::ComboBox& b, const juce::StringArray& items, const juce::String& current)
    {
        b.setColour (juce::ComboBox::backgroundColourId, kSurface);
        b.setColour (juce::ComboBox::textColourId, kInk);
        b.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff2a3134));
        b.addItem ("(none)", 1);
        for (int i = 0; i < items.size(); ++i)
            b.addItem (items[i], i + 2);
        b.setText (current.isEmpty() ? "(none)" : current, juce::dontSendNotification);
        addAndMakeVisible (b);
    };
    initBox (synthBox, proc.getAvailableSynths(), proc.getSynth());
    initBox (fxBox, proc.getAvailableFx(), proc.getFx());

    synthBox.onChange = [this]
    {
        const auto t = synthBox.getText();
        proc.setSynth (t == "(none)" ? juce::String() : t);
        for (auto* r : rows) r->refresh();
    };
    fxBox.onChange = [this]
    {
        const auto t = fxBox.getText();
        proc.setFx (t == "(none)" ? juce::String() : t);
        for (auto* r : rows) r->refresh();
    };

    statusLabel.setFont (juce::FontOptions (11.0f));
    statusLabel.setColour (juce::Label::textColourId, kInkFaint);
    addAndMakeVisible (statusLabel);

    for (int i = 0; i < kMacroCount; ++i)
        addAndMakeVisible (rows.add (new MacroRow (proc, i)));

    setSize (620, kHeadH + kMacroCount * kRowH + kPad * 2);
    startTimerHz (4);
}

void SchwungAudioProcessorEditor::timerCallback()
{
    statusLabel.setText (proc.getStatus(), juce::dontSendNotification);
}

void SchwungAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (kGround);

    /* One hairline under the header, so the chain selectors read as a
     * different kind of thing from the macro rows below them. */
    g.setColour (juce::Colour (0xff2a3134));
    g.drawHorizontalLine (kHeadH - 6, (float) kPad, (float) (getWidth() - kPad));

    g.setColour (kInkFaint);
    g.setFont (juce::FontOptions (10.0f));
    g.drawText ("MACROS  \xe2\x80\x94  bind a chain key, then automate",
                kPad, kHeadH - 2, getWidth() - kPad * 2, 14,
                juce::Justification::centredLeft);
}

void SchwungAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (kPad);

    auto head = r.removeFromTop (kHeadH - kPad - 6);
    title.setBounds (head.removeFromTop (26));
    head.removeFromTop (6);

    auto pickers = head.removeFromTop (44);
    auto left = pickers.removeFromLeft (pickers.getWidth() / 2 - 6);
    synthLabel.setBounds (left.removeFromTop (14));
    synthBox.setBounds (left.removeFromTop (24));

    pickers.removeFromLeft (12);
    fxLabel.setBounds (pickers.removeFromTop (14));
    fxBox.setBounds (pickers.removeFromTop (24));

    statusLabel.setBounds (head.removeFromTop (16));

    r.removeFromTop (18);
    for (auto* row : rows)
        row->setBounds (r.removeFromTop (kRowH));
}
