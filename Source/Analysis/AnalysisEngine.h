#pragma once

#include <limits>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <vector>

/** One completed analysis (timeline or reference), safe to copy for UI. */
struct AnalysisSnapshot
{
    bool valid = false;
    juce::String label;
    double sampleRate = 0;
    int numChannels = 1;
    juce::int64 numFrames = 0;
    double durationSec = 0;

    double integratedLufs = -std::numeric_limits<double>::infinity();
    double shortTermMaxLufs = -std::numeric_limits<double>::infinity();
    double momentaryMaxLufs = -std::numeric_limits<double>::infinity();
    double lraLu = 0;
    double truePeakDbTp = -std::numeric_limits<double>::infinity();
    double samplePeakDbfs = -std::numeric_limits<double>::infinity();

    bool vuValid = false;
    double vuAvg = 0;
    double vuMax = 0;

    /** Peak envelope decimated for waveform strip (linear 0..1, relative to full scale). */
    std::vector<float> waveformPeaks;

    std::vector<float> spectrumFreqHz;
    std::vector<float> spectrumDbPerHz;
    std::vector<float> rmsEnvelopeTimeSec;
    std::vector<float> rmsEnvelopeDbfs;
};

namespace are
{
/** Hard cap on (numChannels * numFrames) floats decoded/analyzed at once — avoids multi-GB RAM and host OOM. */
inline constexpr juce::int64 kMaxAnalysisTotalFloats = 96ll * 1024 * 1024;

/** Full-file analysis: LUFS (libebur128 multichannel), peaks, VU, Welch spectrum, RMS envelope, waveform. */
class AnalysisEngine
{
public:
    static constexpr double vuRefDbfs = -18.0;
    static constexpr int waveformBuckets = 1024;

    /** Planar `buffer`: `numSamples` frames, `buffer.getNumChannels()` channels (ITU-style mapping into ebur128). */
    static AnalysisSnapshot analyzePlanar (const juce::AudioBuffer<float>& buffer,
                                           int numSamples,
                                           double sampleRate,
                                           const juce::String& label);

    /** Reads file into memory then `analyzePlanar` (bounded duration). */
    static AnalysisSnapshot analyzeReader (juce::AudioFormatReader& reader,
                                           const juce::String& label,
                                           juce::int64 maxSecondsToRead = 900);

    /** Single-channel convenience (e.g. live capture). */
    static AnalysisSnapshot analyzeMono (std::vector<float> mono,
                                         double sampleRate,
                                         const juce::String& label);

    /** Replace NaN/Inf samples with 0 (avoids unstable DSP and broken UI paths). */
    static void sanitizePlanar (juce::AudioBuffer<float>& buffer, int numSamples);

private:
    static void downmixMonoMean (const juce::AudioBuffer<float>& buffer,
                                 int numSamples,
                                 std::vector<float>& monoOut);

    static void decimateWaveformPeaks (const std::vector<float>& mono,
                                        int numSamples,
                                        std::vector<float>& peaksOut);

    static void measureVu (const std::vector<float>& mono,
                           double sampleRate,
                           bool& outValid,
                           double& outAvg,
                           double& outMax);

    static void measureWelchSpectrum (const std::vector<float>& mono,
                                      double sampleRate,
                                      std::vector<float>& freqOut,
                                      std::vector<float>& dbPerHzOut);

    static void measureRmsEnvelope (const std::vector<float>& mono,
                                    double sampleRate,
                                    std::vector<float>& timeOut,
                                    std::vector<float>& dbOut);
};

} // namespace are
