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
    constexpr int kHeadH  = 208;
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
    slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 62, 20);
    /* Three places, not JUCE's default. At six the value did not fit the box
     * and rendered as "0.00000..." -- a number with its digits cut off is
     * worse than a rounder one. */
    slider.setNumDecimalPlacesToDisplay (3);
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

    /* Prefer the module's OWN name for the parameter. "Timbre" is what its
     * pages call it and what Live now shows in the automation list; the raw
     * key is already visible in the field to the left. */
    range.setText (b.label.isNotEmpty() ? b.label + "   " + b.status : b.status,
                   juce::dontSendNotification);
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


// --------------------------------------------------------------- MacroList
//
// Only the interesting slots get a row: everything bound, plus a few spare at
// the end so a key can still be typed by hand. With kMacroCount at 512 the
// alternative is ~2000 widgets for a window that shows a dozen.

void MacroList::rebuild()
{
    rows.clear();

    int lastBound = -1;
    for (int i = 0; i < kMacroCount; ++i)
        if (proc.getBinding (i).key.isNotEmpty())
            lastBound = i;

    const int spare = 4;
    const int shown = juce::jmin (kMacroCount, lastBound + 1 + spare);

    for (int i = 0; i < shown; ++i)
        addAndMakeVisible (rows.add (new MacroRow (proc, i)));

    setSize (getWidth() > 0 ? getWidth() : 600, preferredHeight());
    resized();
}

void MacroList::refresh()
{
    for (auto* r : rows) r->refresh();
}

int MacroList::preferredHeight() const
{
    return juce::jmax (1, rows.size()) * kRowH;
}

void MacroList::resized()
{
    auto r = getLocalBounds();
    for (auto* row : rows)
        row->setBounds (r.removeFromTop (kRowH));
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
    initLabel (midiFxLabel, "MIDI FX   1 - 8");
    initLabel (fxLabel, "AUDIO FX   1 - 8");

    auto style = [] (juce::ComboBox& b)
    {
        b.setColour (juce::ComboBox::backgroundColourId, kSurface);
        b.setColour (juce::ComboBox::textColourId, kInk);
        b.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff2a3134));
    };

    /* Item id 1 is always "(none)", so the module list starts at 2 and a
     * module's index is recoverable from the id. The chain spells an empty
     * position "None"; the editor spells it "(none)" and the processor
     * translates -- writing the display string through would load a module
     * called "(none)". */
    auto fill = [] (juce::ComboBox& b, const juce::StringArray& items, const juce::String& current)
    {
        b.addItem ("(none)", 1);
        for (int i = 0; i < items.size(); ++i)
            b.addItem (items[i], i + 2);
        b.setText (current.isEmpty() ? "(none)" : current, juce::dontSendNotification);
    };

    style (synthBox);
    fill (synthBox, proc.getAvailableSynths(), proc.getSynth());
    addAndMakeVisible (synthBox);
    synthBox.onChange = [this]
    {
        const auto t = synthBox.getText();
        proc.setSynth (t == "(none)" ? juce::String() : t);
        reloadRows();
    };

    for (int i = 0; i < kMidiFxSlots; ++i)
    {
        auto* b = midiFxBoxes.add (new juce::ComboBox());
        style (*b);
        fill (*b, proc.getAvailableMidiFx(), proc.getMidiFx (i));
        addAndMakeVisible (b);
        b->onChange = [this, i, b]
        {
            const auto t = b->getText();
            proc.setMidiFx (i, t == "(none)" ? juce::String() : t);
            reloadRows();
        };
    }

    for (int i = 0; i < kFxSlots; ++i)
    {
        auto* b = fxBoxes.add (new juce::ComboBox());
        style (*b);
        fill (*b, proc.getAvailableFx(), proc.getFx (i));
        addAndMakeVisible (b);
        b->onChange = [this, i, b]
        {
            const auto t = b->getText();
            proc.setFx (i, t == "(none)" ? juce::String() : t);
            reloadRows();
        };
    }

    statusLabel.setFont (juce::FontOptions (11.0f));
    statusLabel.setColour (juce::Label::textColourId, kInkFaint);
    addAndMakeVisible (statusLabel);

    countLabel.setFont (juce::FontOptions (10.0f));
    countLabel.setColour (juce::Label::textColourId, kInkFaint);
    countLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (countLabel);

    viewport.setViewedComponent (&list, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setColour (juce::ScrollBar::thumbColourId, juce::Colour (0xff3a4449));
    addAndMakeVisible (viewport);

    reloadRows();

    setSize (720, 620);
    setResizable (true, true);
    setResizeLimits (620, 380, 1400, 1600);
    startTimerHz (4);
}

void SchwungAudioProcessorEditor::reloadRows()
{
    list.rebuild();

    int bound = 0;
    for (int i = 0; i < kMacroCount; ++i)
        if (proc.getBinding (i).resolved) ++bound;

    countLabel.setText (juce::String (bound) + " of " + juce::String (kMacroCount) + " macros bound",
                        juce::dontSendNotification);
    resized();
}

void SchwungAudioProcessorEditor::timerCallback()
{
    statusLabel.setText (proc.getStatus(), juce::dontSendNotification);
}

void SchwungAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (kGround);

    g.setColour (juce::Colour (0xff2a3134));
    g.drawHorizontalLine (kHeadH - 6, (float) kPad, (float) (getWidth() - kPad));

    g.setColour (kInkFaint);
    g.setFont (juce::FontOptions (10.0f));
    /* ASCII only. A raw "\xe2\x80\x94" in a narrow literal reaches
     * juce::String through the char* constructor, which does not treat it as
     * UTF-8 -- the em-dash rendered as "a EUR" mojibake. Anything non-ASCII
     * here needs CharPointer_UTF8; a hyphen needs nothing. */
    g.drawText ("MACROS - bound to the loaded module, automatable in the host",
                kPad, kHeadH - 4, getWidth() - kPad * 2 - 170, 14,
                juce::Justification::centredLeft);
}

void SchwungAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (kPad);

    auto head = r.removeFromTop (kHeadH - kPad - 6);
    title.setBounds (head.removeFromTop (26));
    head.removeFromTop (4);

    synthLabel.setBounds (head.removeFromTop (14));
    synthBox.setBounds (head.removeFromTop (24));
    head.removeFromTop (8);

    /* Both FX rows are eight boxes across the full width. The chain's order is
     * left to right and that is the order they are laid out in, so the strip
     * reads as the signal path it is. */
    auto strip = [&head] (juce::Label& label, juce::OwnedArray<juce::ComboBox>& boxes)
    {
        label.setBounds (head.removeFromTop (14));
        auto row = head.removeFromTop (22);
        const int gap = 3;
        const int w = (row.getWidth() - gap * (boxes.size() - 1)) / boxes.size();
        for (int i = 0; i < boxes.size(); ++i)
        {
            boxes[i]->setBounds (row.removeFromLeft (w));
            if (i + 1 < boxes.size()) row.removeFromLeft (gap);
        }
        head.removeFromTop (6);
    };
    strip (midiFxLabel, midiFxBoxes);
    strip (fxLabel, fxBoxes);

    statusLabel.setBounds (head.removeFromTop (16));
    countLabel.setBounds (getWidth() - kPad - 170, kHeadH - 4, 170, 14);

    r.removeFromTop (18);
    viewport.setBounds (r);
    list.setSize (viewport.getMaximumVisibleWidth(), list.preferredHeight());
}
