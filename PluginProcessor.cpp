#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DebugSessionLog.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <limits>

AREPluginProcessor::AREPluginProcessor()
    : AudioProcessor (getBusLayout())
{
    liveCaptureMono.reserve ((size_t) liveCaptureMaxSamples);
}

AREPluginProcessor::~AREPluginProcessor()
{
    cancelPendingUpdate();

    if (timelineAnalysisFuture.has_value())
    {
        timelineAnalysisFuture->wait();
        timelineAnalysisFuture.reset();
    }

    if (referenceAnalysisFuture.has_value())
    {
        referenceAnalysisFuture->wait();
        referenceAnalysisFuture.reset();
    }
}

void AREPluginProcessor::handleAsyncUpdate()
{
    // #region agent log
    areDbgLog ("H2", "handleAsyncUpdate", "entry", 0, 0);
    // #endregion
    sendChangeMessage();
}

juce::AudioProcessor::BusesProperties AREPluginProcessor::getBusLayout()
{
    return BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true);
}

void AREPluginProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    hostSampleRate = sampleRate;
    prepareToPlayForARA (sampleRate, samplesPerBlock, getMainBusNumOutputChannels(), getProcessingPrecision());
}

void AREPluginProcessor::releaseResources()
{
    releaseResourcesForARA();
}

bool AREPluginProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mainOut = layouts.getMainOutputChannelSet();
    const auto mainIn = layouts.getMainInputChannelSet();
    return (mainOut == juce::AudioChannelSet::mono() || mainOut == juce::AudioChannelSet::stereo())
           && mainIn == mainOut;
}

void AREPluginProcessor::appendLiveCapture (const juce::AudioBuffer<float>& buffer)
{
    const int n = buffer.getNumSamples();
    const int ch = buffer.getNumChannels();
    if (n <= 0 || ch <= 0)
    {
        areDbgLog ("H2", "appendLiveCapture", "empty_buffer", n, ch);
        return;
    }

    // Check if audio is actually silent - if all samples are near zero, skip
    bool hasSilence = true;
    for (int c = 0; c < ch && hasSilence; ++c)
    {
        for (int i = 0; i < n && hasSilence; ++i)
        {
            if (std::abs (buffer.getSample (c, i)) > 1e-6f)
                hasSilence = false;
        }
    }

    // Only accumulate if there's actual audio content
    if (hasSilence)
        return;

    const juce::ScopedLock sl (dataLock);
    const float inv = 1.0f / (float) ch;
    const size_t oldSize = liveCaptureMono.size();
    for (int i = 0; i < n; ++i)
    {
        float s = 0;
        for (int c = 0; c < ch; ++c)
            s += buffer.getSample (c, i) * inv;

        liveCaptureMono.push_back (s);
        if ((juce::int64) liveCaptureMono.size() > liveCaptureMaxSamples)
            liveCaptureMono.erase (liveCaptureMono.begin(),
                                   liveCaptureMono.begin() + (liveCaptureMono.size() - (size_t) liveCaptureMaxSamples));
    }
    const size_t newSize = liveCaptureMono.size();
    areDbgLog ("H2", "appendLiveCapture", "appended", (int) oldSize, (int) newSize);
}

void AREPluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    if (! processBlockForARA (buffer, isRealtime(), getPlayHead()))
        processBlockBypassed (buffer, midi);

    // Append rolling live capture when not in ARA mode, or when realtime
    // monitoring is enabled (so user can analyze what's currently playing).
    const bool enabled = realtimeMonitoringEnabled.load();
    if (! isBoundToARA())
    {
        areDbgLog ("H2", "processBlock", "not_ara_append", buffer.getNumSamples(), buffer.getNumChannels());
        appendLiveCapture (buffer);
    }
    else if (enabled)
    {
        areDbgLog ("H2", "processBlock", "realtime_append_enabled", buffer.getNumSamples(), buffer.getNumChannels());
        appendLiveCapture (buffer);
    }
    else
    {
        areDbgLog ("H2", "processBlock", "ara_bound_realtime_disabled", buffer.getNumSamples(), buffer.getNumChannels());
    }
}

double AREPluginProcessor::getTailLengthSeconds() const
{
    double tail = 0.0;
    if (getTailLengthSecondsForARA (tail))
        return tail;

    return 0.0;
}

int AREPluginProcessor::getNumPrograms() { return 1; }
int AREPluginProcessor::getCurrentProgram() { return 0; }
void AREPluginProcessor::setCurrentProgram (int) {}
const juce::String AREPluginProcessor::getProgramName (int) { return {}; }
void AREPluginProcessor::changeProgramName (int, const juce::String&) {}

void AREPluginProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::MemoryOutputStream mos (destData, true);
    const juce::ScopedLock sl (dataLock);
    mos.writeString (referenceFilePath);
}

void AREPluginProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::MemoryInputStream mis (data, (size_t) sizeInBytes, false);
    const juce::String path = mis.readString();
    if (path.isNotEmpty())
        beginReferenceAnalysis (juce::File (path));
}

bool AREPluginProcessor::peekAraTimelineStats (juce::ARAEditorView* editorView,
                                                juce::int64& outLengthSamples,
                                                int& outNumChannels,
                                                double& outSampleRate) const noexcept
{
    outLengthSamples = 0;
    outNumChannels = 0;
    outSampleRate = 0;

    try
    {
        if (editorView == nullptr)
            return false;

        auto* dc = editorView->getDocumentController();
        if (dc == nullptr)
            return false;

        auto* spec = AREDocumentController::fromDocumentController (dc);
        if (spec == nullptr)
            return false;

        juce::ScopedTryReadLock modelLock (spec->getProcessLock());
        if (! modelLock.isLocked())
            return false;

        auto* doc = dc->getDocument<juce::ARADocument>();
        if (doc == nullptr || doc->getAudioSources().empty())
            return false;

        auto* audioSource = doc->getAudioSources().front();
        juce::ARAAudioSourceReader reader (audioSource);

        if (! reader.isValid())
            return false;

        outLengthSamples = reader.lengthInSamples;
        outNumChannels = juce::jmax (1, (int) reader.numChannels);
        outSampleRate = reader.sampleRate;
        return outLengthSamples > 0 && outSampleRate > 1.0;
    }
    catch (...)
    {
        return false;
    }
}

bool AREPluginProcessor::hasTimelineMediaChangedSinceAnalysis (juce::ARAEditorView* editorView) const noexcept
{
    if (! isBoundToARA() || editorView == nullptr)
        return false;

    juce::int64 len = 0;
    int ch = 0;
    double sr = 0;

    if (! peekAraTimelineStats (editorView, len, ch, sr))
        return false;

    const juce::ScopedLock sl (dataLock);
    if (! timelineSnapshot.valid)
        return false;

    if (len != timelineSnapshot.numFrames)
        return true;

    if (ch != timelineSnapshot.numChannels)
        return true;

    if (std::abs (sr - timelineSnapshot.sampleRate) > 0.5)
        return true;

    return false;
}

bool AREPluginProcessor::copyTimelineAudioUnderLock (juce::ARAEditorView* editorView,
                                                     juce::AudioBuffer<float>& audioOut,
                                                     double& sampleRateOut,
                                                     juce::String& errorOut)
{
    audioOut.setSize (0, 0, false, false, true);
    sampleRateOut = 0;
    errorOut = {};

    if (editorView == nullptr)
    {
        errorOut = "No ARA editor view.";
        areDbgLog ("H1", "copyTimeline", "editorView_null", 0, 0);
        return false;
    }

    auto* dc = editorView->getDocumentController();
    if (dc == nullptr)
    {
        errorOut = "No ARA document controller.";
        areDbgLog ("H1", "copyTimeline", "doc_controller_null", 0, 0);
        return false;
    }

    auto* spec = AREDocumentController::fromDocumentController (dc);
    if (spec == nullptr)
    {
        errorOut = "Internal document controller.";
        areDbgLog ("H1", "copyTimeline", "spec_null", 0, 0);
        return false;
    }

    areDbgLog ("H1", "copyTimeline", "spec_obtained", 0, 0);

    juce::ScopedTryReadLock modelLock (spec->getProcessLock());
    if (! modelLock.isLocked())
    {
        errorOut = "Host is editing the ARA document. Try again.";
        areDbgLog ("H1", "copyTimeline", "lock_failed", 0, 0);
        return false;
    }

    areDbgLog ("H1", "copyTimeline", "lock_acquired", 0, 0);

    auto* doc = dc->getDocument<juce::ARADocument>();
    if (doc == nullptr)
    {
        errorOut = "No ARA document.";
        areDbgLog ("H1", "copyTimeline", "doc_null", 0, 0);
        return false;
    }

    const auto& sources = doc->getAudioSources();
    if (sources.empty())
    {
        errorOut = "No ARA audio source on this track.";
        areDbgLog ("H1", "copyTimeline", "no_audio_sources", (int) sources.size(), 0);
        return false;
    }

    areDbgLog ("H1", "copyTimeline", "audio_sources_found", (int) sources.size(), 0);

    // Select the first valid ARA audio source for analysis. Summing lengths of
    // all sources caused inflated totalFrames when a track contained many
    // clipped items; use the first source (same as peekAraTimelineStats) so
    // the reported duration matches the active audio source.
    juce::int64 totalFrames = 0;
    int maxChannels = 0;
    double commonSampleRate = 0;

    for (auto* source : sources)
    {
        if (source == nullptr) continue;
        juce::ARAAudioSourceReader reader (source);
        if (! reader.isValid()) continue;

        // Use this first valid source and stop — this mirrors peekAraTimelineStats
        commonSampleRate = reader.sampleRate;
        totalFrames = reader.lengthInSamples;
        maxChannels = juce::jmax (maxChannels, (int) reader.numChannels);
        break;
    }

    if (totalFrames <= 0 || commonSampleRate <= 1.0 || maxChannels <= 0)
    {
        errorOut = "Empty or invalid timeline.";
        return false;
    }

    const juce::int64 capByMem = are::kMaxAnalysisTotalFloats / (juce::int64) maxChannels;
    const juce::int64 usableFrames = juce::jmin (totalFrames, capByMem);
    const int numSamples = (int) usableFrames;

    if (numSamples <= 0)
    {
        errorOut = "Timeline too large to load.";
        return false;
    }

    try
    {
        audioOut.setSize (maxChannels, numSamples, false, false, true);
        audioOut.clear();
        sampleRateOut = commonSampleRate;

        areDbgLog ("H1", "copyTimeline", "buffer_allocated", numSamples, maxChannels);

        juce::int64 writePos = 0;
        // Read only the chosen source (do not concatenate all sources). This
        // ensures the analysis corresponds to the current editor view / source
        // rather than mixing multiple items which produced inflated durations.
        juce::ARAAudioSource* chosenSource = nullptr;
        for (auto* src : sources)
        {
            if (src == nullptr) continue;
            juce::ARAAudioSourceReader tempReader (src);
            if (! tempReader.isValid()) continue;
            chosenSource = src;
            break;
        }

        if (chosenSource == nullptr)
        {
            errorOut = "No valid ARA audio source to read.";
            return false;
        }

        juce::ARAAudioSourceReader reader (chosenSource);
        const juce::int64 framesToRead = juce::jmin (reader.lengthInSamples, usableFrames - writePos);
        juce::int64 sourceReadPos = 0;

        while (sourceReadPos < framesToRead)
        {
            const int n = (int) juce::jmin ((juce::int64) 65536, framesToRead - sourceReadPos);
            if (! reader.read (&audioOut, (int) (writePos + sourceReadPos), n, sourceReadPos, true, true))
            {
                errorOut = "Failed while reading ARA audio source.";
                return false;
            }
            sourceReadPos += n;
        }
        writePos += framesToRead;

        areDbgLog ("H1", "copyTimeline", "read_complete", (int) sources.size(), (int) writePos);
        return true;
    }
    catch (const std::exception&)
    {
        errorOut = "Exception while reading timeline audio.";
        audioOut.setSize (0, 0, false, false, true);
        areDbgLog ("H1", "copyTimeline", "exception_caught", 0, 0);
        return false;
    }
    catch (...)
    {
        errorOut = "Unknown exception while reading timeline audio.";
        audioOut.setSize (0, 0, false, false, true);
        areDbgLog ("H1", "copyTimeline", "unknown_exception", 0, 0);
        return false;
    }
}

void AREPluginProcessor::beginAnalyzeAsync (juce::AudioBuffer<float> buffer,
                                            int numSamples,
                                            double sampleRate,
                                            const juce::String& label,
                                            bool isTimeline)
{
    juce::ignoreUnused (isTimeline);

    if (timelineAnalysisBusy.load())
        return;

    timelineAnalysisBusy = true;

    try
    {
        timelineAnalysisFuture = std::async (std::launch::async,
                                             [this,
                                              buffer = std::move (buffer),
                                              numSamples,
                                              sampleRate,
                                              label]() mutable
                                             {
                                               struct ClearBusy
                                               {
                                                   std::atomic<bool>& b;
                                                   ~ClearBusy() { b = false; }
                                               } clearBusy { timelineAnalysisBusy };

                                               AnalysisSnapshot result;

                                               try
                                               {
                                                   are::AnalysisEngine::sanitizePlanar (buffer, numSamples);
                                                   result = are::AnalysisEngine::analyzePlanar (buffer, numSamples, sampleRate, label);
                                               }
                                               catch (...)
                                               {
                                                   result = {};
                                               }

                                               {
                                                   const juce::ScopedLock sl (dataLock);
                                                   timelineSnapshot = result;
                                               }

                                               triggerAsyncUpdate();
                                             });
    }
    catch (...)
    {
        timelineAnalysisBusy = false;
    }
}

void AREPluginProcessor::beginTimelineAnalysis (juce::ARAEditorView* editorView)
{
    areDbgLog ("H1", "beginTimeline", "entry", editorView != nullptr ? 1 : 0, isBoundToARA() ? 1 : 0);

    if (timelineAnalysisBusy.load())
    {
        areDbgLog ("H1", "beginTimeline", "already_busy", 0, 0);
        return;
    }

    // If realtime monitoring is enabled, prefer the rolling live capture
    // regardless of ARA binding — this lets the user analyze what's
    // currently playing (e.g. master bus) even in an ARA host.
    if (realtimeMonitoringEnabled.load())
    {
        areDbgLog ("H1", "beginTimeline", "realtime_monitoring_active", 0, 0);
        std::vector<float> copy;
        double sr = hostSampleRate;
        {
            const juce::ScopedLock sl (dataLock);
            copy = liveCaptureMono;
        }

        if (copy.empty())
        {
            areDbgLog ("H1", "beginTimeline", "live_capture_empty_realtime", (int) copy.size(), 0);
            triggerAsyncUpdate();
            return;
        }

        areDbgLog ("H1", "beginTimeline", "live_capture_ready_realtime", (int) copy.size(), (int) sr);
        juce::AudioBuffer<float> live (1, (int) copy.size());
        live.copyFrom (0, 0, copy.data(), (int) copy.size());
        beginAnalyzeAsync (std::move (live), live.getNumSamples(), sr, "Realtime monitor", true);
        return;
    }

    if (! isBoundToARA())
    {
        areDbgLog ("H1", "beginTimeline", "not_ara_mode", 0, 0);
        std::vector<float> copy;
        double sr = hostSampleRate;
        {
            const juce::ScopedLock sl (dataLock);
            copy = liveCaptureMono;
        }

        if (copy.empty())
        {
            areDbgLog ("H1", "beginTimeline", "live_capture_empty", (int) copy.size(), 0);
            triggerAsyncUpdate();
            return;
        }

        areDbgLog ("H1", "beginTimeline", "live_capture_ready", (int) copy.size(), (int) sr);
        juce::AudioBuffer<float> live (1, (int) copy.size());
        live.copyFrom (0, 0, copy.data(), (int) copy.size());
        beginAnalyzeAsync (std::move (live), live.getNumSamples(), sr, "Live input", true);
        return;
    }

    // ARA mode - launch fully async to avoid UI hang during reader.read
    areDbgLog ("H1", "beginTimeline", "ara_mode_async_launch", editorView != nullptr ? 1 : 0, 0);

    if (editorView == nullptr)
    {
        areDbgLog ("H1", "beginTimeline", "ara_editorview_null", 0, 0);
        triggerAsyncUpdate();
        return;
    }

    timelineAnalysisBusy = true;

    try
    {
        timelineAnalysisFuture = std::async (std::launch::async,
                                             [this, editorView]() mutable
                                             {
                                               struct ClearBusy
                                               {
                                                   std::atomic<bool>& b;
                                                   ~ClearBusy() { b = false; }
                                               } clearBusy { timelineAnalysisBusy };

                                               juce::AudioBuffer<float> timelineAudio;
                                               double sr = 0;
                                               juce::String err;

                                               areDbgLog ("H1", "timelineWorker", "calling_copyTimeline", 0, 0);
                                               if (! copyTimelineAudioUnderLock (editorView, timelineAudio, sr, err))
                                               {
                                                   areDbgLog ("H1", "timelineWorker", "copyTimeline_failed", 0, 0);
                                                   triggerAsyncUpdate();
                                                   return;
                                               }

                                               const int ns = timelineAudio.getNumSamples();
                                               areDbgLog ("H1", "timelineWorker", "analysis_start", ns, (int) sr);

                                               AnalysisSnapshot result;
                                               try
                                               {
                                                   are::AnalysisEngine::sanitizePlanar (timelineAudio, ns);
                                                   result = are::AnalysisEngine::analyzePlanar (timelineAudio, ns, sr, "Timeline");
                                               }
                                               catch (...)
                                               {
                                                   result = {};
                                               }

                                               {
                                                   const juce::ScopedLock sl (dataLock);
                                                   timelineSnapshot = result;
                                               }

                                               areDbgLog ("H1", "timelineWorker", "analysis_done", result.valid ? 1 : 0, ns);
                                               triggerAsyncUpdate();
                                             });
    }
    catch (...)
    {
        timelineAnalysisBusy = false;
        areDbgLog ("H1", "beginTimeline", "async_launch_failed", 0, 0);
    }
}

void AREPluginProcessor::beginReferenceAnalysis (const juce::File& file)
{
    if (referenceAnalysisBusy.load())
    {
        areDbgLog ("H1", "beginRef", "already_busy", 0, 0);
        return;
    }

    if (! file.existsAsFile())
    {
        areDbgLog ("H1", "beginRef", "file_not_exists", 0, 0);
        return;
    }

    const juce::String filePath = file.getFullPathName();
    const juce::String fileName = file.getFileName();

    if (filePath.isEmpty() || fileName.isEmpty())
    {
        areDbgLog ("H1", "beginRef", "empty_path_or_name", 0, 0);
        return;
    }

    areDbgLog ("H1", "beginRef", "starting_ref_analysis_async", 0, 0);
    referenceFilePath = filePath;
    referenceAnalysisBusy = true;

    try
    {
        referenceAnalysisFuture = std::async (std::launch::async,
                                              [this, file, fileName]() mutable
                                              {
                                                struct ClearBusy
                                                {
                                                    std::atomic<bool>& b;
                                                    ~ClearBusy() { b = false; }
                                                } clearBusy { referenceAnalysisBusy };

                                                AnalysisSnapshot snap;

                                                try
                                                {
                                                    juce::AudioFormatManager fm;
                                                    fm.registerBasicFormats();

                                                    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
                                                    if (reader == nullptr)
                                                    {
                                                        areDbgLog ("H1", "refWorker", "reader_creation_failed", 0, 0);
                                                        return;
                                                    }

                                                    const int numChannels = reader->numChannels;
                                                    const double sampleRate = reader->sampleRate;
                                                    const juce::int64 totalFrames = reader->lengthInSamples;

                                                    if (totalFrames <= 0 || numChannels <= 0)
                                                    {
                                                        areDbgLog ("H1", "refWorker", "invalid_source_stats", numChannels, (int) totalFrames);
                                                        return;
                                                    }

                                                    const juce::int64 capByMem = are::kMaxAnalysisTotalFloats / (juce::int64) numChannels;
                                                    const int numFrames = (int) juce::jmin (totalFrames, capByMem);

                                                    if (numFrames <= 0)
                                                    {
                                                        areDbgLog ("H1", "refWorker", "numFrames_zero_after_cap", 0, 0);
                                                        return;
                                                    }

                                                    juce::AudioBuffer<float> fileBuffer (numChannels, numFrames);
                                                    if (! reader->read (&fileBuffer, 0, numFrames, 0, true, true))
                                                    {
                                                        areDbgLog ("H1", "refWorker", "read_failed", 0, 0);
                                                        return;
                                                    }

                                                    areDbgLog ("H1", "refWorker", "analysis_start", numChannels, numFrames);
                                                    snap = are::AnalysisEngine::analyzePlanar (fileBuffer, numFrames, sampleRate, fileName);
                                                    areDbgLog ("H1", "refWorker", "analysis_done", snap.valid ? 1 : 0, (int) snap.numFrames);
                                                }
                                                catch (const std::bad_alloc&)
                                                {
                                                    areDbgLog ("H1", "refWorker", "bad_alloc", 0, 0);
                                                    snap = {};
                                                }
                                                catch (const std::exception&)
                                                {
                                                    areDbgLog ("H1", "refWorker", "exception", 0, 0);
                                                    snap = {};
                                                }
                                                catch (...)
                                                {
                                                    areDbgLog ("H1", "refWorker", "unknown_exception", 0, 0);
                                                    snap = {};
                                                }

                                                {
                                                    const juce::ScopedLock sl (dataLock);
                                                    referenceSnapshot = snap;
                                                }

                                                triggerAsyncUpdate();
                                              });
    }
    catch (...)
    {
        referenceAnalysisBusy = false;
        areDbgLog ("H1", "beginRef", "async_launch_failed", 0, 0);
    }
}

void AREPluginProcessor::clearReference()
{
    const juce::ScopedLock sl (dataLock);
    referenceSnapshot = {};
    referenceFilePath = {};
}

void AREPluginProcessor::copySnapshots (AnalysisSnapshot& timeline,
                                        AnalysisSnapshot& reference,
                                        juce::String& refPath) const
{
    const juce::ScopedLock sl (dataLock);
    timeline = timelineSnapshot;
    reference = referenceSnapshot;
    refPath = referenceFilePath;
}

juce::AudioProcessorEditor* AREPluginProcessor::createEditor()
{
    return new AREPluginEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AREPluginProcessor();
}
