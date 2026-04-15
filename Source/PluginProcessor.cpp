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
        return;

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
}

void AREPluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    if (! processBlockForARA (buffer, isRealtime(), getPlayHead()))
        processBlockBypassed (buffer, midi);

    if (! isBoundToARA())
        appendLiveCapture (buffer);
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

    auto* audioSource = sources.front();
    if (audioSource == nullptr)
    {
        errorOut = "Audio source pointer is null.";
        areDbgLog ("H1", "copyTimeline", "audio_source_null", 0, 0);
        return false;
    }

    areDbgLog ("H1", "copyTimeline", "audio_source_valid", 0, 0);

    try
    {
        juce::ARAAudioSourceReader reader (audioSource);

        if (! reader.isValid())
        {
            errorOut = "ARA audio reader is not valid.";
            areDbgLog ("H1", "copyTimeline", "reader_invalid", 0, 0);
            return false;
        }

        areDbgLog ("H1", "copyTimeline", "reader_valid", (int) reader.numChannels, (int) reader.sampleRate);

        const auto total = reader.lengthInSamples;
        if (total <= 0)
        {
            errorOut = "Empty timeline.";
            areDbgLog ("H1", "copyTimeline", "empty_timeline", (int) total, 0);
            return false;
        }

        areDbgLog ("H1", "copyTimeline", "timeline_length", (int) total, (int) reader.sampleRate);

        const auto maxFrames = (juce::int64) std::llround (900.0 * reader.sampleRate);
        juce::int64 usable = juce::jmin (total, maxFrames);

        const int ch = juce::jmax (1, (int) reader.numChannels);
        const juce::int64 kMaxSamplesPerChannel =
            (juce::int64) (std::numeric_limits<int>::max() / juce::jmax (1, ch));
        usable = juce::jmin (usable, kMaxSamplesPerChannel);
        usable = juce::jmin (usable, are::kMaxAnalysisTotalFloats / (juce::int64) ch);

        sampleRateOut = reader.sampleRate;
        const int numSamples = (int) usable;

        if (numSamples <= 0)
        {
            errorOut = "Timeline too large to load.";
            areDbgLog ("H1", "copyTimeline", "timeline_too_large", (int) usable, (int) maxFrames);
            return false;
        }

        areDbgLog ("H1", "copyTimeline", "allocating_buffer", numSamples, ch);

        try
        {
            audioOut.setSize (ch, numSamples, false, false, true);
        }
        catch (...)
        {
            errorOut = "Out of memory loading timeline.";
            audioOut.setSize (0, 0, false, false, true);
            areDbgLog ("H1", "copyTimeline", "buffer_alloc_failed", numSamples, ch);
            return false;
        }

        areDbgLog ("H1", "copyTimeline", "buffer_allocated", numSamples, ch);

        juce::int64 readPos = 0;
        int readCount = 0;

        while (readPos < usable)
        {
            const int n = (int) juce::jmin ((juce::int64) 65536, usable - readPos);
            if (! reader.read (&audioOut, (int) readPos, n, readPos, true, true))
            {
                errorOut = "Failed while reading ARA audio.";
                areDbgLog ("H1", "copyTimeline", "reader_read_failed", (int) readPos, n);
                audioOut.setSize (0, 0, false, false, true);
                return false;
            }

            readPos += n;
            ++readCount;
        }

        areDbgLog ("H1", "copyTimeline", "read_complete", readCount, (int) readPos);
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

    if (timelineAnalysisFuture.has_value())
        timelineAnalysisFuture->wait();

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

    // ARA mode
    areDbgLog ("H1", "beginTimeline", "ara_mode_entry", editorView != nullptr ? 1 : 0, 0);

    if (editorView == nullptr)
    {
        areDbgLog ("H1", "beginTimeline", "ara_editorview_null", 0, 0);
        triggerAsyncUpdate();
        return;
    }

    try
    {
        juce::AudioBuffer<float> timelineAudio;
        double sr = 0;
        juce::String err;

        areDbgLog ("H1", "beginTimeline", "calling_copyTimeline", 0, 0);
        if (! copyTimelineAudioUnderLock (editorView, timelineAudio, sr, err))
        {
            areDbgLog ("H1", "beginTimeline", "copyTimeline_failed", 0, 0);
            juce::ignoreUnused (err);
            triggerAsyncUpdate();
            return;
        }

        const int ns = timelineAudio.getNumSamples();
        areDbgLog ("H1", "beginTimeline", "copyTimeline_success", ns, (int) sr);
        beginAnalyzeAsync (std::move (timelineAudio), ns, sr, "Timeline", true);
    }
    catch (const std::exception&)
    {
        areDbgLog ("H1", "beginTimeline", "exception_caught", 0, 0);
        triggerAsyncUpdate();
    }
    catch (...)
    {
        areDbgLog ("H1", "beginTimeline", "unknown_exception", 0, 0);
        triggerAsyncUpdate();
    }
}

void AREPluginProcessor::beginReferenceAnalysis (const juce::File& file)
{
    if (! file.existsAsFile())
    {
        areDbgLog ("H1", "beginRef", "file_not_exists", 0, 0);
        return;
    }

    juce::String filePath = file.getFullPathName();
    juce::String fileName = file.getFileName();

    if (filePath.isEmpty() || fileName.isEmpty())
    {
        areDbgLog ("H1", "beginRef", "empty_path_or_name", 0, 0);
        return;
    }

    areDbgLog ("H1", "beginRef", "starting_ref_analysis", 0, 0);
    referenceFilePath = filePath;
    referenceAnalysisBusy = true;

    // Read audio into a buffer on THIS thread (main thread) BEFORE launching async
    // This avoids the crash from reader accessing destroyed AudioFormatManager
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    areDbgLog ("H1", "beginRef", "formats_registered", 1, 0);

    juce::AudioFormatReader* readerPtr = fm.createReaderFor (filePath);
    if (readerPtr == nullptr)
    {
        areDbgLog ("H1", "beginRef", "reader_creation_failed", 0, 0);
        referenceAnalysisBusy = false;
        return;
    }

    areDbgLog ("H1", "beginRef", "reader_created", 1, (int) readerPtr->numChannels);

    // Load the entire audio file into memory while reader is valid
    std::unique_ptr<juce::AudioFormatReader> reader (readerPtr);
    const int numChannels = reader->numChannels;
    const int numFrames = (int) reader->lengthInSamples;
    const double sampleRate = reader->sampleRate;

    if (numFrames <= 0)
    {
        areDbgLog ("H1", "beginRef", "invalid_num_frames", numFrames, 0);
        referenceAnalysisBusy = false;
        return;
    }

    juce::AudioBuffer<float> fileBuffer (numChannels, numFrames);
    const int readSamples = reader->read (&fileBuffer, 0, numFrames, 0, true, true);
    areDbgLog ("H1", "beginRef", "file_read_complete", readSamples, numChannels);

    if (readSamples != numFrames)
    {
        areDbgLog ("H1", "beginRef", "incomplete_read", readSamples, numFrames);
    }

    // Reader is no longer needed, it will be destroyed along with fm
    reader.reset();

    // Now launch async analysis with the loaded audio buffer
    try
    {
        referenceAnalysisFuture = std::async (std::launch::async,
                                              [this, fileBuffer = std::move(fileBuffer), numFrames, fileName, sampleRate]() mutable
                                              {
                                                struct ClearBusy
                                                {
                                                    std::atomic<bool>& b;
                                                    ~ClearBusy() { b = false; }
                                                } clearBusy { referenceAnalysisBusy };

                                                AnalysisSnapshot snap;

                                                try
                                                {
                                                    areDbgLog ("H1", "refWorker", "analysis_start", fileBuffer.getNumChannels(), numFrames);
                                                    snap = are::AnalysisEngine::analyzePlanar (fileBuffer, numFrames, sampleRate, fileName);
                                                    areDbgLog ("H1", "refWorker", "analysis_done", snap.valid ? 1 : 0, (int) snap.numFrames);
                                                }
                                                catch (const std::exception&)
                                                {
                                                    areDbgLog ("H1", "refWorker", "analysis_exception", 0, 0);
                                                    snap = {};
                                                }
                                                catch (...)
                                                {
                                                    areDbgLog ("H1", "refWorker", "analysis_unknown_exception", 0, 0);
                                                    snap = {};
                                                }

                                                {
                                                    const juce::ScopedLock sl (dataLock);
                                                    referenceSnapshot = snap;
                                                }

                                                areDbgLog ("H2", "refWorker", "update_ui", snap.valid ? 1 : 0,
                                                           (int) juce::jmin ((juce::int64) 2000000000, snap.numFrames));

                                                triggerAsyncUpdate();
                                              });
    }
    catch (const std::exception&)
    {
        areDbgLog ("H1", "beginRef", "async_exception", 0, 0);
        referenceAnalysisBusy = false;
    }
    catch (...)
    {
        areDbgLog ("H1", "beginRef", "async_unknown_exception", 0, 0);
        referenceAnalysisBusy = false;
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
