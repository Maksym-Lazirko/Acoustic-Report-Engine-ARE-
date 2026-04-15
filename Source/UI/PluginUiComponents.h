#pragma once

#include <JuceHeader.h>
#include "../Analysis/AnalysisEngine.h"

class AREPluginEditor;

namespace areui
{

/** Timeline trace - matplotlib default green (`#2ca02c`). */
inline juce::Colour timelineTraceColour() { return juce::Colour (0xff2ca02c); }
/** Reference trace - matplotlib default orange (`#ff7f0e`). */
inline juce::Colour referenceTraceColour() { return juce::Colour (0xffff7f0e); }

/** Neutral tab bar and controls - avoids host / default green accents. */
class ARELookAndFeel final : public juce::LookAndFeel_V4
{
public:
    ARELookAndFeel();

    void drawTabButton (juce::TabBarButton& button, juce::Graphics& g, bool isMouseOver, bool isMouseDown) override;
    int getTabButtonSpaceAroundImage() override { return 6; }
    int getTabButtonOverlap (int tabDepth) override { juce::ignoreUnused (tabDepth); return 0; }
};

class WaveformStrip final : public juce::Component
{
public:
    explicit WaveformStrip (AREPluginEditor& editor);

    void paint (juce::Graphics& g) override;

private:
    AREPluginEditor& owner;

    static void drawOneWaveform (juce::Graphics& g,
                                 juce::Rectangle<float> area,
                                 const std::vector<float>& peaks,
                                 juce::Colour strokeColour);
};

class SpectrumPlotPanel final : public juce::Component
{
public:
    explicit SpectrumPlotPanel (AREPluginEditor& editor);
    void paint (juce::Graphics& g) override;

private:
    AREPluginEditor& owner;
};

class DynamicsPlotPanel final : public juce::Component
{
public:
    explicit DynamicsPlotPanel (AREPluginEditor& editor);
    void paint (juce::Graphics& g) override;

private:
    AREPluginEditor& owner;
};

/** Reference import: disk only (not cloud). Drop zone + actions. */
class ReferenceImportPanel final : public juce::Component,
                                    public juce::FileDragAndDropTarget,
                                    public juce::Button::Listener
{
public:
    explicit ReferenceImportPanel (AREPluginEditor& editor);
    ~ReferenceImportPanel() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    void buttonClicked (juce::Button* b) override;

    void refreshFromEditor();

private:
    AREPluginEditor& owner;
    bool dragHighlight = false;
    juce::Label titleLabel;
    juce::Label hintLabel;
    juce::Label fileLabel;
    juce::TextButton loadButton { "Choose audio file..." };
    juce::TextButton clearButton { "Clear" };
    juce::TextButton revealButton { "Show in folder" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReferenceImportPanel)
};

} // namespace areui
