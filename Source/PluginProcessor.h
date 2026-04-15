#pragma once

#include <JuceHeader.h>
#include <future>
#include <optional>
#include "Analysis/AnalysisEngine.h"
#include "AREDocumentController.h"

class AREPluginProcessor final : public juce::AudioProcessor,
                                 public juce::AudioProcessorARAExtension,
                                 public juce::ChangeBroadcaster,
                                 private juce::AsyncUpdater
{
public:
    AREPluginProcessor();
    ~AREPluginProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }

    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    void beginTimelineAnalysis (juce::ARAEditorView* editorView);

    void beginReferenceAnalysis (const juce::File& file);
    void clearReference();

    void copySnapshots (AnalysisSnapshot& timeline, AnalysisSnapshot& reference, juce::String& refPath) const;

    /** True while timeline or reference analysis is running (each can run independently). */
    bool isAnalysisRunning() const noexcept
    {
        return timelineAnalysisBusy.load() || referenceAnalysisBusy.load();
    }

    bool isAraHost() const noexcept { return isBoundToARA(); }

    /** For ARA hosts: true if the bound timeline media length/rate no longer matches the last analysis (e.g. new take in Reaper). */
    bool hasTimelineMediaChangedSinceAnalysis (juce::ARAEditorView* editorView) const noexcept;

private:
    void handleAsyncUpdate() override;

    static juce::AudioProcessor::BusesProperties getBusLayout();

    bool copyTimelineAudioUnderLock (juce::ARAEditorView* editorView,
                                     juce::AudioBuffer<float>& audioOut,
                                     double& sampleRateOut,
                                     juce::String& errorOut);

    /** Lightweight read of ARA source stats (no full buffer alloc). Returns false if unavailable. */
    bool peekAraTimelineStats (juce::ARAEditorView* editorView,
                               juce::int64& outLengthSamples,
                               int& outNumChannels,
                               double& outSampleRate) const noexcept;

    void appendLiveCapture (const juce::AudioBuffer<float>& buffer);

    void beginAnalyzeAsync (juce::AudioBuffer<float> buffer,
                            int numSamples,
                            double sampleRate,
                            const juce::String& label,
                            bool isTimeline);

    mutable juce::CriticalSection dataLock;
    AnalysisSnapshot timelineSnapshot;
    AnalysisSnapshot referenceSnapshot;
    juce::String referenceFilePath;

    std::atomic<bool> timelineAnalysisBusy { false };
    std::atomic<bool> referenceAnalysisBusy { false };

    /** Joined in destructor so background work never touches the processor after destroy. */
    std::optional<std::future<void>> timelineAnalysisFuture;
    std::optional<std::future<void>> referenceAnalysisFuture;

    double hostSampleRate = 48000.0;
    juce::int64 liveCaptureMaxSamples = 48000 * 90;
    std::vector<float> liveCaptureMono;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AREPluginProcessor)
};
