#include "PluginUiComponents.h"
#include "../PluginEditor.h"
#include <cmath>

namespace areui
{

ARELookAndFeel::ARELookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, juce::Colour (0xff1a1d23));
    setColour (juce::Label::textColourId, juce::Colour (0xffe8eaed));
    setColour (juce::TextButton::buttonColourId, juce::Colour (0xff343b45));
    setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff454d5a));
    setColour (juce::TextButton::textColourOffId, juce::Colour (0xffe8eaed));
    setColour (juce::TabbedComponent::backgroundColourId, juce::Colour (0xff1e2229));
    setColour (juce::TabbedButtonBar::tabOutlineColourId, juce::Colour (0xff3d4450));
    setColour (juce::TabbedButtonBar::frontOutlineColourId, juce::Colour (0xff5c6370));
    setColour (juce::ScrollBar::thumbColourId, juce::Colour (0xff4a5363));
    setColour (juce::ScrollBar::trackColourId, juce::Colour (0xff252a32));
    setColour (juce::ScrollBar::backgroundColourId, juce::Colour (0xff1e2229));
}

void ARELookAndFeel::drawTabButton (juce::TabBarButton& button, juce::Graphics& g, bool isMouseOver, bool isMouseDown)
{
    juce::ignoreUnused (isMouseDown);

    auto bounds = button.getActiveArea().toFloat().reduced (1.0f, 0.0f);
    const bool selected = button.getToggleState();

    auto bg = selected ? juce::Colour (0xff2d333b) : juce::Colour (0xff23272f);
    if (isMouseOver && ! selected)
        bg = bg.brighter (0.08f);

    g.setColour (bg);
    g.fillRoundedRectangle (bounds, 5.0f);

    g.setColour (juce::Colour (0xff4a5363));
    g.drawRoundedRectangle (bounds, 5.0f, 1.0f);

    auto textColour = selected ? juce::Colour (0xfff0f2f5) : juce::Colour (0xffb8c0cc);
    g.setColour (textColour);
    g.setFont (juce::FontOptions().withHeight (13.0f));
    g.drawFittedText (button.getButtonText().unquoted(),
                      button.getActiveArea().reduced (8, 0),
                      juce::Justification::centred,
                      1);
}

//==============================================================================
WaveformStrip::WaveformStrip (AREPluginEditor& editor)
    : owner (editor)
{
}

void WaveformStrip::drawOneWaveform (juce::Graphics& g,
                                     juce::Rectangle<float> area,
                                     const std::vector<float>& peaks,
                                     juce::Colour strokeColour)
{
    if (peaks.empty())
    {
        g.setColour (juce::Colour (0xff5c6370));
        g.setFont (juce::FontOptions().withHeight (12.0f));
        g.drawText ("No data - click Analyze timeline (or load reference)", area.toNearestInt(), juce::Justification::centred);
        return;
    }

    const float mid = area.getCentreY();
    const float h2 = area.getHeight() * 0.45f;
    const float denom = peaks.size() > 1 ? (float) (peaks.size() - 1) : 1.0f;

    juce::Path p;
    bool pathStarted = false;
    for (size_t i = 0; i < peaks.size(); ++i)
    {
        const float pk = std::isfinite ((double) peaks[i]) ? juce::jlimit (-1.0f, 1.0f, peaks[i]) : 0.0f;
        const float x = area.getX() + (float) i / denom * area.getWidth();
        const float y = mid - pk * h2;
        if (! std::isfinite ((double) x) || ! std::isfinite ((double) y))
            continue;

        if (! pathStarted)
        {
            p.startNewSubPath (x, y);
            pathStarted = true;
        }
        else
        {
            p.lineTo (x, y);
        }
    }

    if (! pathStarted)
        return;

    juce::Path fill = p;
    fill.lineTo (area.getRight(), mid);
    fill.lineTo (area.getX(), mid);
    fill.closeSubPath();
    g.setColour (strokeColour.withAlpha (0.22f));
    g.fillPath (fill);

    g.setColour (strokeColour);
    g.strokePath (p, juce::PathStrokeType (1.35f));

    g.setColour (strokeColour.withAlpha (0.35f));
    g.drawLine (area.getX(), mid, area.getRight(), mid, 0.8f);
}

void WaveformStrip::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1e2229));

    auto bounds = getLocalBounds().toFloat().reduced (8.0f, 6.0f);
    g.setColour (juce::Colour (0xff3d4450));
    g.drawRoundedRectangle (bounds, 6.0f, 1.0f);

    const auto& t = owner.cachedTimeline;
    const auto& r = owner.cachedReference;

    auto topRow = bounds.removeFromTop (bounds.getHeight() * 0.48f);
    auto botRow = bounds;

    g.setColour (juce::Colour (0xff8b939e));
    g.setFont (juce::FontOptions().withHeight (11.0f));
    g.drawText ("Your timeline / capture - peak outline vs time", topRow.removeFromTop (14.0f).toNearestInt(), juce::Justification::left);
    drawOneWaveform (g, topRow.reduced (4.0f, 2.0f), t.waveformPeaks, timelineTraceColour());

    g.drawText ("Reference file - peak outline vs time", botRow.removeFromTop (14.0f).toNearestInt(), juce::Justification::left);
    drawOneWaveform (g, botRow.reduced (4.0f, 2.0f), r.waveformPeaks, referenceTraceColour());
}

//==============================================================================
SpectrumPlotPanel::SpectrumPlotPanel (AREPluginEditor& editor)
    : owner (editor)
{
}

void SpectrumPlotPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1a1d23));

    auto outer = getLocalBounds().toFloat().reduced (10.0f, 8.0f);
    g.setColour (juce::Colour (0xffe8eaed));
    g.setFont (juce::FontOptions().withHeight (11.0f));
    g.drawText ("Average spectral balance (Welch PSD, dB/Hz) - same idea as the Python plot", outer.removeFromTop (18.0f).toNearestInt(), juce::Justification::centredLeft);

    auto bounds = outer;
    g.setColour (juce::Colour (0xff2a3038));
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (juce::Colour (0xff3d4450));
    g.drawRoundedRectangle (bounds, 6.0f, 1.0f);

    auto plot = bounds.reduced (44.0f, 28.0f);

    const auto& t = owner.cachedTimeline;
    const auto& r = owner.cachedReference;

    if (! t.valid && ! r.valid)
    {
        g.setColour (juce::Colour (0xff8b939e));
        g.setFont (juce::FontOptions().withHeight (13.0f));
        g.drawText ("Run analysis and/or load a reference - graphs update automatically here (no extra tab).",
                    bounds.toNearestInt(),
                    juce::Justification::centred);
        return;
    }

    float ymin = 1e9f, ymax = -1e9f;
    auto acc = [&] (const AnalysisSnapshot& s)
    {
        if (! s.valid)
            return;
        for (float v : s.spectrumDbPerHz)
        {
            if (! std::isfinite ((double) v))
                continue;
            ymin = juce::jmin (ymin, v);
            ymax = juce::jmax (ymax, v);
        }
    };
    acc (t);
    acc (r);
    if (ymin > ymax || ! std::isfinite ((double) ymin) || ! std::isfinite ((double) ymax))
        ymin = -90.0f, ymax = -20.0f;

    const float lo = ymin - 8.0f;
    const float hi = ymax + 8.0f;
    const float x0 = plot.getX();
    const float x1 = plot.getRight();
    const float y0 = plot.getY();
    const float y1 = plot.getBottom();

    g.setColour (juce::Colour (0xff2f3540));
    for (int db = -100; db <= 0; db += 20)
    {
        const float y = y0 + (hi - (float) db) / juce::jmax (hi - lo, 1.0f) * (y1 - y0);
        g.drawLine (x0, y, x1, y, 0.7f);
    }

    g.setColour (juce::Colour (0xff8b939e));
    g.setFont (juce::FontOptions().withHeight (10.0f));
    g.drawText (juce::String (hi, 0) + " dB/Hz", juce::Rectangle<int> ((int) x0 - 40, (int) y0 - 2, 38, 14), juce::Justification::centredRight);
    g.drawText (juce::String (lo, 0) + " dB/Hz", juce::Rectangle<int> ((int) x0 - 40, (int) y1 - 12, 38, 14), juce::Justification::centredRight);

    for (float hz : { 100.0f, 1000.0f, 10000.0f })
    {
        const float lf = std::log10 (hz);
        const float l0 = std::log10 (20.0f);
        const float l1 = std::log10 (20000.0f);
        const float x = x0 + (lf - l0) / (l1 - l0) * (x1 - x0);
        g.setColour (juce::Colour (0xff2f3540));
        g.drawVerticalLine (juce::roundToInt (x), y0, y1);
        g.setColour (juce::Colour (0xff8b939e));
        juce::String lab = hz >= 1000.0f ? juce::String (hz / 1000.0f, (hz >= 10000.0f ? 0 : 1)) + " kHz"
                                         : juce::String ((int) hz) + " Hz";
        g.drawText (lab, juce::Rectangle<int> ((int) x - 24, (int) y1 + 2, 48, 14), juce::Justification::centred);
    }

    auto xForFreq = [x0, x1] (float hz)
    {
        const float lf = std::log10 (juce::jmax (hz, 1.0f));
        const float l0 = std::log10 (20.0f);
        const float l1 = std::log10 (20000.0f);
        return x0 + (lf - l0) / (l1 - l0) * (x1 - x0);
    };

    auto yForDb = [y0, y1, lo, hi] (float db)
    {
        return y0 + (hi - db) / juce::jmax (hi - lo, 1.0f) * (y1 - y0);
    };

    auto drawTrace = [&] (const AnalysisSnapshot& snap, juce::Colour col)
    {
        if (! snap.valid || snap.spectrumFreqHz.size() < 2)
            return;

        juce::Path path;
        bool started = false;
        float firstX = 0, lastX = 0;
        for (size_t i = 0; i < snap.spectrumFreqHz.size(); ++i)
        {
            const float f = snap.spectrumFreqHz[i];
            if (f < 20.0f || f > 20000.0f || ! std::isfinite ((double) f))
                continue;

            const float db = i < snap.spectrumDbPerHz.size() ? snap.spectrumDbPerHz[i] : 0.0f;
            if (! std::isfinite ((double) db))
                continue;

            const float x = xForFreq (f);
            const float y = yForDb (db);
            if (! std::isfinite ((double) x) || ! std::isfinite ((double) y))
                continue;

            if (! started)
            {
                path.startNewSubPath (x, y);
                firstX = x;
                started = true;
            }
            else
            {
                path.lineTo (x, y);
            }

            lastX = x;
        }

        if (! started)
            return;

        {
            juce::Path fill = path;
            fill.lineTo (lastX, y1);
            fill.lineTo (firstX, y1);
            fill.closeSubPath();
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (plot.toNearestInt());
            g.setColour (col.withAlpha (0.2f));
            g.fillPath (fill);
        }

        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (plot.toNearestInt());
        g.setColour (col);
        g.strokePath (path, juce::PathStrokeType (1.65f));
    };

    drawTrace (t, timelineTraceColour());
    drawTrace (r, referenceTraceColour());

    {
        auto leg = bounds.removeFromRight (130.0f).removeFromTop (44.0f).reduced (6.0f, 4.0f);
        g.setColour (timelineTraceColour());
        g.fillEllipse (leg.getX(), leg.getY() + 4, 8, 8);
        g.setColour (juce::Colour (0xffc8d0dc));
        g.setFont (juce::FontOptions().withHeight (11.0f));
        g.drawText ("Timeline", leg.withTrimmedLeft (14).toNearestInt(), juce::Justification::centredLeft);
        leg = leg.translated (0, 16);
        g.setColour (referenceTraceColour());
        g.fillEllipse (leg.getX(), leg.getY() + 4, 8, 8);
        g.setColour (juce::Colour (0xffc8d0dc));
        g.drawText ("Reference", leg.withTrimmedLeft (14).toNearestInt(), juce::Justification::centredLeft);
    }
}

//==============================================================================
DynamicsPlotPanel::DynamicsPlotPanel (AREPluginEditor& editor)
    : owner (editor)
{
}

void DynamicsPlotPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1a1d23));

    auto outer = getLocalBounds().toFloat().reduced (10.0f, 8.0f);
    g.setColour (juce::Colour (0xffe8eaed));
    g.setFont (juce::FontOptions().withHeight (11.0f));
    g.drawText ("RMS loudness envelope (dBFS) vs time - like the Python fill_between plot", outer.removeFromTop (20.0f).toNearestInt(), juce::Justification::centredLeft);

    auto bounds = outer;
    g.setColour (juce::Colour (0xff2a3038));
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (juce::Colour (0xff3d4450));
    g.drawRoundedRectangle (bounds, 6.0f, 1.0f);

    auto plot = bounds.reduced (44.0f, 32.0f);

    const auto& t = owner.cachedTimeline;
    const auto& r = owner.cachedReference;

    if (! t.valid && ! r.valid)
    {
        g.setColour (juce::Colour (0xff8b939e));
        g.setFont (juce::FontOptions().withHeight (13.0f));
        g.drawText ("Run analysis to plot RMS envelope vs time.",
                    bounds.toNearestInt(),
                    juce::Justification::centred);
        return;
    }

    double tMax = 0.001;
    if (t.valid && ! t.rmsEnvelopeTimeSec.empty())
    {
        const double tb = (double) t.rmsEnvelopeTimeSec.back();
        if (std::isfinite (tb))
            tMax = juce::jmax (tMax, tb);
    }
    if (r.valid && ! r.rmsEnvelopeTimeSec.empty())
    {
        const double tb = (double) r.rmsEnvelopeTimeSec.back();
        if (std::isfinite (tb))
            tMax = juce::jmax (tMax, tb);
    }
    if (! std::isfinite (tMax) || tMax <= 0.0)
        tMax = 0.001;

    constexpr float dbMin = -60.0f;
    constexpr float dbMax = 0.0f;
    const float x0 = plot.getX();
    const float x1 = plot.getRight();
    const float y0 = plot.getY();
    const float y1 = plot.getBottom();

    g.setColour (juce::Colour (0xff2f3540));
    for (int db = -60; db <= 0; db += 10)
    {
        const float y = y0 + (dbMax - (float) db) / (dbMax - dbMin) * (y1 - y0);
        g.drawLine (x0, y, x1, y, 0.7f);
    }

    g.setColour (juce::Colour (0xff8b939e));
    g.setFont (juce::FontOptions().withHeight (10.0f));
    g.drawText ("0 dBFS", juce::Rectangle<int> ((int) x0 - 44, (int) y0 - 2, 40, 12), juce::Justification::centredRight);
    g.drawText ("-60", juce::Rectangle<int> ((int) x0 - 44, (int) y1 - 12, 40, 12), juce::Justification::centredRight);

    auto drawEnv = [&] (const AnalysisSnapshot& snap, juce::Colour col)
    {
        if (! snap.valid || snap.rmsEnvelopeTimeSec.size() < 2)
            return;

        juce::Path path;
        bool started = false;
        float firstX = 0, lastX = 0;
        const size_t nPts = juce::jmin (snap.rmsEnvelopeTimeSec.size(), snap.rmsEnvelopeDbfs.size());
        for (size_t i = 0; i < nPts; ++i)
        {
            const float ti = snap.rmsEnvelopeTimeSec[i];
            const float db = snap.rmsEnvelopeDbfs[i];
            if (! std::isfinite ((double) ti) || ! std::isfinite ((double) db))
                continue;

            const float x = x0 + (float) (ti / tMax) * (x1 - x0);
            const float y = y0 + (dbMax - db) / (dbMax - dbMin) * (y1 - y0);
            if (! std::isfinite ((double) x) || ! std::isfinite ((double) y))
                continue;

            if (! started)
            {
                path.startNewSubPath (x, y);
                firstX = x;
                started = true;
            }
            else
            {
                path.lineTo (x, y);
            }

            lastX = x;
        }

        if (! started)
            return;

        {
            juce::Path fill = path;
            fill.lineTo (lastX, y1);
            fill.lineTo (firstX, y1);
            fill.closeSubPath();
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (plot.toNearestInt());
            g.setColour (col.withAlpha (0.28f));
            g.fillPath (fill);
        }

        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (plot.toNearestInt());
        g.setColour (col);
        g.strokePath (path, juce::PathStrokeType (1.45f));
    };

    drawEnv (t, timelineTraceColour());
    drawEnv (r, referenceTraceColour());

    juce::String tlab = "t = " + juce::String (tMax, 2) + " s";
    g.setColour (juce::Colour (0xff8b939e));
    g.drawText (tlab, juce::Rectangle<int> ((int) x1 - 100, (int) y1 + 4, 96, 14), juce::Justification::centredRight);
}

//==============================================================================
ReferenceImportPanel::ReferenceImportPanel (AREPluginEditor& editor)
    : owner (editor)
{
    addAndMakeVisible (titleLabel);
    titleLabel.setText ("Load reference audio to compare (from disk, not uploaded)", juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions().withHeight (14.0f));
    titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe8eaed));

    addAndMakeVisible (hintLabel);
    hintLabel.setText ("WAV, AIFF, FLAC, MP3, OGG - drag a file here or use the button.",
                        juce::dontSendNotification);
    hintLabel.setFont (juce::FontOptions().withHeight (12.0f));
    hintLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9aa0a6));

    addAndMakeVisible (fileLabel);
    fileLabel.setJustificationType (juce::Justification::centredLeft);
    fileLabel.setFont (juce::FontOptions().withHeight (12.0f));
    fileLabel.setColour (juce::Label::textColourId, juce::Colour (0xffc8d0dc));

    addAndMakeVisible (loadButton);
    loadButton.addListener (this);
    addAndMakeVisible (clearButton);
    clearButton.addListener (this);
    addAndMakeVisible (revealButton);
    revealButton.addListener (this);
}

ReferenceImportPanel::~ReferenceImportPanel()
{
    loadButton.removeListener (this);
    clearButton.removeListener (this);
    revealButton.removeListener (this);
}

void ReferenceImportPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff22262e));

    auto inner = getLocalBounds().toFloat().reduced (6.0f);
    juce::Path rounded;
    rounded.addRoundedRectangle (inner, 8.0f);
    juce::Path dashed;
    const float pattern[] = { 6.0f, 5.0f };
    juce::PathStrokeType (1.5f).createDashedStroke (dashed, rounded, pattern, 2);
    g.setColour (dragHighlight ? juce::Colour (0xff6eb5ff) : juce::Colour (0xff3d4a5c));
    g.strokePath (dashed, juce::PathStrokeType (1.5f));
}

void ReferenceImportPanel::fileDragEnter (const juce::StringArray& files, int, int)
{
    dragHighlight = isInterestedInFileDrag (files);
    repaint();
}

void ReferenceImportPanel::fileDragExit (const juce::StringArray&)
{
    dragHighlight = false;
    repaint();
}

void ReferenceImportPanel::resized()
{
    auto r = getLocalBounds().reduced (14, 10);
    titleLabel.setBounds (r.removeFromTop (22));
    r.removeFromTop (4);
    hintLabel.setBounds (r.removeFromTop (36));
    r.removeFromTop (8);

    auto row = r.removeFromTop (30);
    loadButton.setBounds (row.removeFromLeft (200).reduced (0, 2));
    row.removeFromLeft (8);
    clearButton.setBounds (row.removeFromLeft (72).reduced (0, 2));
    row.removeFromLeft (8);
    revealButton.setBounds (row.removeFromLeft (120).reduced (0, 2));

    r.removeFromTop (8);
    fileLabel.setBounds (r.removeFromTop (22));
}

bool ReferenceImportPanel::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (f.endsWithIgnoreCase (".wav") || f.endsWithIgnoreCase (".flac") || f.endsWithIgnoreCase (".aif")
            || f.endsWithIgnoreCase (".aiff") || f.endsWithIgnoreCase (".ogg") || f.endsWithIgnoreCase (".mp3")
            || f.endsWithIgnoreCase (".wave") || f.endsWithIgnoreCase (".m4a") || f.endsWithIgnoreCase (".aac")
            || f.endsWithIgnoreCase (".wma"))
            return true;

    return false;
}

void ReferenceImportPanel::filesDropped (const juce::StringArray& files, int, int)
{
    dragHighlight = false;
    repaint();

    if (files.isEmpty())
        return;

    const juce::File f (files[0]);
    juce::Component::SafePointer<AREPluginEditor> sp (&owner);

    juce::MessageManager::callAsync ([sp, f]()
                                     {
                                         if (sp == nullptr)
                                             return;

                                         sp->getProcessor().beginReferenceAnalysis (f);
                                         sp->refreshUi();
                                     });
}

void ReferenceImportPanel::buttonClicked (juce::Button* b)
{
    if (b == &loadButton)
        owner.browseForReferenceFile();
    else if (b == &clearButton)
    {
        owner.getProcessor().clearReference();
        refreshFromEditor();
    }
    else if (b == &revealButton)
    {
        if (owner.cachedRefPath.isNotEmpty())
            juce::File (owner.cachedRefPath).revealToUser();
    }
}

void ReferenceImportPanel::refreshFromEditor()
{
    if (owner.cachedRefPath.isNotEmpty())
        fileLabel.setText (juce::File (owner.cachedRefPath).getFullPathName(), juce::dontSendNotification);
    else
        fileLabel.setText ("No file loaded", juce::dontSendNotification);

    const bool busy = owner.getProcessor().isAnalysisRunning();
    loadButton.setEnabled (! busy);
    clearButton.setEnabled (! busy);
    revealButton.setEnabled (owner.cachedRefPath.isNotEmpty());
}

} // namespace areui
