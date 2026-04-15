#include "PluginEditor.h"
#include "DebugSessionLog.h"

namespace
{
juce::String formatDb (double v)
{
    if (! std::isfinite (v))
        return "---";

    return juce::String (v, 2);
}

juce::String formatLufsLine (const juce::String& title, double v)
{
    return title.paddedRight (' ', 22) + formatDb (v) + " LUFS\n";
}

juce::String formatVuLine (const juce::String& title, bool valid, double v)
{
    const juce::String val = valid ? (juce::String (v, 2) + " VU") : juce::String ("---");
    return title.paddedRight (' ', 22) + val + "\n";
}
} // namespace

//==============================================================================
class AREPluginEditor::ReportPage final : public juce::Component
{
public:
    explicit ReportPage (AREPluginEditor& o) : owner (o) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (findColour (juce::ResizableWindow::backgroundColourId).darker (0.2f));

        juce::String text;
        const auto& t = owner.cachedTimeline;
        const auto& r = owner.cachedReference;

        text << "TIMELINE (ARA or live capture)\n";
        text << "------------------------------\n";

        if (! t.valid)
            text << "No timeline analysis yet. Click \"Analyze timeline\".\n\n";
        else
        {
            if (t.durationSec > 0)
                text << juce::String ("Duration").paddedRight (' ', 22) + juce::String (t.durationSec, 2) + " s  |  "
                     << t.numChannels << " ch\n";
            text << formatLufsLine ("Integrated loudness", t.integratedLufs);
            text << formatLufsLine ("Short-term max", t.shortTermMaxLufs);
            text << formatLufsLine ("Momentary max", t.momentaryMaxLufs);
            text << juce::String (" ").paddedRight (' ', 22) + "(LUFS: same multichannel BS.1770 meter)\n";
            text << juce::String ("Loudness range (LRA)").paddedRight (' ', 22) + formatDb (t.lraLu) + " LU\n";
            text << juce::String ("True peak").paddedRight (' ', 22) + formatDb (t.truePeakDbTp) + " dBTP\n";
            text << juce::String ("Sample peak").paddedRight (' ', 22) + formatDb (t.samplePeakDbfs) + " dBFS\n";
            text << formatVuLine ("VU average", t.vuValid, t.vuAvg);
            text << formatVuLine ("VU maximum", t.vuValid, t.vuMax);
            text << juce::String (" ").paddedRight (' ', 22) + "(0 VU = -18 dBFS; 300 ms ballistics, else whole-file RMS)\n";
            text << "\n";
        }

        text << "REFERENCE FILE\n";
        text << "------------------------------\n";

        if (! r.valid)
            text << "No reference loaded.\n";
        else
        {
            if (r.durationSec > 0)
                text << juce::String ("Duration").paddedRight (' ', 22) + juce::String (r.durationSec, 2) + " s  |  "
                     << r.numChannels << " ch\n";
            text << formatLufsLine ("Integrated loudness", r.integratedLufs);
            text << formatLufsLine ("Short-term max", r.shortTermMaxLufs);
            text << formatLufsLine ("Momentary max", r.momentaryMaxLufs);
            text << juce::String ("Loudness range (LRA)").paddedRight (' ', 22) + formatDb (r.lraLu) + " LU\n";
            text << juce::String ("True peak").paddedRight (' ', 22) + formatDb (r.truePeakDbTp) + " dBTP\n";
            text << juce::String ("Sample peak").paddedRight (' ', 22) + formatDb (r.samplePeakDbfs) + " dBFS\n";
            text << formatVuLine ("VU average", r.vuValid, r.vuAvg);
            text << formatVuLine ("VU maximum", r.vuValid, r.vuMax);
            text << juce::String (" ").paddedRight (' ', 22) + "(0 VU = -18 dBFS; 300 ms ballistics, else whole-file RMS)\n";
            text << "\n";
        }

        if (t.valid && r.valid)
        {
            text << "DELTA (timeline - reference)\n";
            text << "------------------------------\n";
            text << formatLufsLine ("Delta integrated", t.integratedLufs - r.integratedLufs);
            text << juce::String ("Delta true peak").paddedRight (' ', 22)
                 + formatDb (t.truePeakDbTp - r.truePeakDbTp) + " dB\n";
        }

        g.setColour (findColour (juce::Label::textColourId));
        g.setFont (juce::FontOptions().withName (juce::Font::getDefaultSansSerifFontName()).withHeight (13.0f));
        g.drawMultiLineText (text, 16, 24, getWidth() - 32);
    }

private:
    AREPluginEditor& owner;
};

//==============================================================================
AREPluginEditor::AREPluginEditor (AREPluginProcessor& p)
    : AudioProcessorEditor (&p)
    , AudioProcessorEditorARAExtension (&p)
    , audioProcessor (p)
{
    setLookAndFeel (&areLookAndFeel);

    addAndMakeVisible (bannerLabel);
    bannerLabel.setJustificationType (juce::Justification::topLeft);
    bannerLabel.setFont (juce::FontOptions().withName (juce::Font::getDefaultSansSerifFontName()).withHeight (13.0f));
    bannerLabel.setMinimumHorizontalScale (0.78f);

    addAndMakeVisible (analyzeButton);
    analyzeButton.addListener (this);

    addAndMakeVisible (statusLabel);
    statusLabel.setJustificationType (juce::Justification::centredRight);
    statusLabel.setFont (juce::FontOptions().withHeight (12.0f));
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa0a6));

    referencePanel = std::make_unique<areui::ReferenceImportPanel> (*this);
    addAndMakeVisible (*referencePanel);

    waveformStrip = std::make_unique<areui::WaveformStrip> (*this);
    addAndMakeVisible (*waveformStrip);

    reportPage = std::make_unique<ReportPage> (*this);
    spectrumPanel = std::make_unique<areui::SpectrumPlotPanel> (*this);
    dynamicsPanel = std::make_unique<areui::DynamicsPlotPanel> (*this);

    addAndMakeVisible (*spectrumPanel);
    addAndMakeVisible (*dynamicsPanel);

    metricsViewport = std::make_unique<juce::Viewport>();
    metricsViewport->setScrollBarsShown (true, false);
    addAndMakeVisible (*metricsViewport);
    metricsViewport->setViewedComponent (reportPage.get(), false);

    addAndMakeVisible (buildInfoLabel);
    buildInfoLabel.setJustificationType (juce::Justification::centredLeft);
    buildInfoLabel.setFont (juce::FontOptions().withHeight (11.0f));
    buildInfoLabel.setColour (juce::Label::textColourId, juce::Colour (0xff6d7580));
    {
        juce::String s;
        s << "VST3 v" << JucePlugin_VersionString << " | " << __DATE__ << " (not Reaper JS)";
        buildInfoLabel.setText (s, juce::dontSendNotification);
    }

    audioProcessor.addChangeListener (this);
    setSize (880, 920);
    setResizeLimits (760, 720, 4000, 3200);
    refreshUi();
    startTimer (2000);
}

AREPluginEditor::~AREPluginEditor()
{
    stopTimer();
    audioProcessor.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void AREPluginEditor::paint (juce::Graphics& g)
{
    g.fillAll (findColour (juce::ResizableWindow::backgroundColourId));
}

void AREPluginEditor::resized()
{
    auto r = getLocalBounds().reduced (12);
    bannerLabel.setBounds (r.removeFromTop (54));
    r.removeFromTop (4);

    auto row = r.removeFromTop (34);
    analyzeButton.setBounds (row.removeFromLeft (168).reduced (0, 3));
    row.removeFromLeft (12);
    statusLabel.setBounds (row);

    r.removeFromTop (6);
    referencePanel->setBounds (r.removeFromTop (142));
    r.removeFromTop (6);
    waveformStrip->setBounds (r.removeFromTop (108));
    r.removeFromTop (6);

    const int graphH = juce::jmax (248, (int) ((float) r.getHeight() * 0.44f));
    auto graphRow = r.removeFromTop (graphH);
    const int halfW = graphRow.getWidth() / 2;
    spectrumPanel->setBounds (graphRow.removeFromLeft (halfW).reduced (4, 0));
    dynamicsPanel->setBounds (graphRow.reduced (4, 0));

    r.removeFromTop (4);
    buildInfoLabel.setBounds (r.removeFromBottom (22));
    r.removeFromTop (2);
    metricsViewport->setBounds (r);

    const int vw = juce::jmax (40, metricsViewport->getWidth() - metricsViewport->getScrollBarThickness());
    reportPage->setBounds (0, 0, vw, 580);
}

void AREPluginEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    juce::Component::SafePointer<AREPluginEditor> safe (this);
    juce::MessageManager::callAsync ([safe]()
                                     {
                                         // #region agent log
                                         areDbgLog ("H2", "callAsync", "before_refreshUi", safe != nullptr ? 1 : 0, 0);
                                         // #endregion
                                         if (safe != nullptr)
                                             safe->refreshUi();
                                     });
}

void AREPluginEditor::timerCallback()
{
    if (! audioProcessor.isAraHost())
        return;

    try
    {
        if (audioProcessor.hasTimelineMediaChangedSinceAnalysis (getARAEditorView()))
            refreshUi();
    }
    catch (...)
    {
    }
}

void AREPluginEditor::buttonClicked (juce::Button* b)
{
    if (b == &analyzeButton)
    {
        audioProcessor.beginTimelineAnalysis (getARAEditorView());
        refreshUi();
    }
}

void AREPluginEditor::browseForReferenceFile()
{
    juce::Component::SafePointer<AREPluginEditor> safe (this);

    fileChooser = std::make_unique<juce::FileChooser> ("Select reference audio",
                                                         juce::File(),
                                                         "*.wav;*.flac;*.aif;*.aiff;*.ogg;*.mp3;*.m4a;*.aac");

    auto browserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    fileChooser->launchAsync (browserFlags, [safe] (const juce::FileChooser& fc)
                              {
                                  if (safe == nullptr)
                                      return;

                                  const auto f = fc.getResult();
                                  if (f.existsAsFile())
                                      safe->getProcessor().beginReferenceAnalysis (f);

                                  safe->refreshUi();
                              });
}

void AREPluginEditor::refreshUi()
{
    audioProcessor.copySnapshots (cachedTimeline, cachedReference, cachedRefPath);

    juce::String banner;

    if (audioProcessor.isAraHost())
    {
        banner = "ARA host (e.g. REAPER): graphs use the audio on this item/timeline. Integrated loudness: ITU-R BS.1770 / EBU R128 (libebur128, multichannel).";

        bool timelineDirty = false;
        try
        {
            timelineDirty = audioProcessor.hasTimelineMediaChangedSinceAnalysis (getARAEditorView());
        }
        catch (...)
        {
            timelineDirty = false;
        }

        if (timelineDirty)
            banner << "\n\nTimeline media changed - click \"Analyze timeline\" to refresh graphs and metrics.";
    }
    else
    {
        banner = "Non-ARA host: analysis uses a rolling capture of the live input (not the full song). For full-track graphs, use an ARA host such as REAPER, Cubase, or Studio One.";
    }

    bannerLabel.setText (banner, juce::dontSendNotification);

    if (audioProcessor.isAnalysisRunning())
        statusLabel.setText ("Analyzing...", juce::dontSendNotification);
    else if (cachedTimeline.valid || cachedReference.valid)
        statusLabel.setText ("Ready", juce::dontSendNotification);
    else
        statusLabel.setText ("Idle", juce::dontSendNotification);

    referencePanel->refreshFromEditor();

    reportPage->repaint();
    spectrumPanel->repaint();
    dynamicsPanel->repaint();
    waveformStrip->repaint();
}
