#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "UI/PluginUiComponents.h"

class AREPluginEditor final : public juce::AudioProcessorEditor,
                              public juce::AudioProcessorEditorARAExtension,
                              public juce::ChangeListener,
                              public juce::Button::Listener,
                              private juce::Timer
{
public:
    explicit AREPluginEditor (AREPluginProcessor&);
    ~AREPluginEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void buttonClicked (juce::Button*) override;
    void timerCallback() override;

    AnalysisSnapshot cachedTimeline;
    AnalysisSnapshot cachedReference;
    juce::String cachedRefPath;

    AREPluginProcessor& getProcessor() noexcept { return audioProcessor; }
    void browseForReferenceFile();
    void refreshUi();

private:
    AREPluginProcessor& audioProcessor;
    areui::ARELookAndFeel areLookAndFeel;

    juce::Label bannerLabel;
    juce::TextButton analyzeButton { "Analyze timeline" };
    juce::ToggleButton realtimeToggle { "Realtime monitoring" };
    juce::Label statusLabel;

    std::unique_ptr<areui::ReferenceImportPanel> referencePanel;
    std::unique_ptr<areui::WaveformStrip> waveformStrip;

    class ReportPage;
    std::unique_ptr<ReportPage> reportPage;
    std::unique_ptr<areui::SpectrumPlotPanel> spectrumPanel;
    std::unique_ptr<areui::DynamicsPlotPanel> dynamicsPanel;

    std::unique_ptr<juce::Viewport> metricsViewport;
    juce::Label buildInfoLabel;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AREPluginEditor)
};
