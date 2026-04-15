#include "AnalysisEngine.h"
#include "../DebugSessionLog.h"
#include <algorithm>
#include <cmath>
#include <ebur128.h>
#include <juce_dsp/juce_dsp.h>
#include <limits>

namespace are
{
namespace
{
void configureEbur128Channels (ebur128_state* st, unsigned n)
{
    if (st == nullptr || n == 0)
        return;

    if (n == 1)
    {
        ebur128_set_channel (st, 0, EBUR128_LEFT);
        return;
    }

    if (n == 2)
        return;

    if (n == 3)
    {
        ebur128_set_channel (st, 0, EBUR128_LEFT);
        ebur128_set_channel (st, 1, EBUR128_RIGHT);
        ebur128_set_channel (st, 2, EBUR128_CENTER);
        return;
    }

    if (n >= 6)
    {
        ebur128_set_channel (st, 0, EBUR128_LEFT);
        ebur128_set_channel (st, 1, EBUR128_RIGHT);
        ebur128_set_channel (st, 2, EBUR128_CENTER);
        ebur128_set_channel (st, 3, EBUR128_UNUSED);
        ebur128_set_channel (st, 4, EBUR128_LEFT_SURROUND);
        ebur128_set_channel (st, 5, EBUR128_RIGHT_SURROUND);
        for (unsigned c = 6; c < n; ++c)
            ebur128_set_channel (st, c, EBUR128_UNUSED);

        return;
    }

    for (unsigned c = 0; c < n; ++c)
    {
        switch (c)
        {
            case 0:
                ebur128_set_channel (st, c, EBUR128_LEFT);
                break;
            case 1:
                ebur128_set_channel (st, c, EBUR128_RIGHT);
                break;
            case 2:
                ebur128_set_channel (st, c, EBUR128_CENTER);
                break;
            case 3:
                ebur128_set_channel (st, c, EBUR128_UNUSED);
                break;
            case 4:
                ebur128_set_channel (st, c, EBUR128_LEFT_SURROUND);
                break;
            default:
                ebur128_set_channel (st, c, EBUR128_RIGHT_SURROUND);
                break;
        }
    }
}

int chooseWelchSegmentLength (int numMonoSamples)
{
    if (numMonoSamples < 32)
        return 0;

    int n = juce::jmin (4096, numMonoSamples);
    int p = 1;
    while (p * 2 <= n)
        p *= 2;

    if (p < 32 && numMonoSamples >= 32)
        p = 32;

    if (p > numMonoSamples)
        p = numMonoSamples;

    return p;
}
} // namespace

void AnalysisEngine::sanitizePlanar (juce::AudioBuffer<float>& buffer, int numSamples)
{
    const int numCh = buffer.getNumChannels();
    if (numSamples <= 0 || numCh <= 0)
        return;

    for (int c = 0; c < numCh; ++c)
        for (int i = 0; i < numSamples; ++i)
        {
            const float s = buffer.getSample (c, i);
            if (! std::isfinite ((double) s))
                buffer.setSample (c, i, 0.0f);
        }
}

void AnalysisEngine::downmixMonoMean (const juce::AudioBuffer<float>& buffer,
                                      int numSamples,
                                      std::vector<float>& monoOut)
{
    const int ch = buffer.getNumChannels();
    if (ch <= 0 || numSamples <= 0)
    {
        monoOut.clear();
        return;
    }

    try
    {
        monoOut.resize ((size_t) numSamples);
    }
    catch (const std::bad_alloc&)
    {
        monoOut.clear();
        return;
    }

    const float inv = 1.0f / (float) ch;
    for (int i = 0; i < numSamples; ++i)
    {
        float s = 0;
        for (int c = 0; c < ch; ++c)
            s += buffer.getSample (c, i) * inv;

        monoOut[(size_t) i] = s;
    }
}

void AnalysisEngine::decimateWaveformPeaks (const std::vector<float>& mono,
                                            int numSamples,
                                            std::vector<float>& peaksOut)
{
    peaksOut.assign ((size_t) waveformBuckets, 0.0f);
    if (numSamples <= 0 || mono.empty())
        return;

    for (int b = 0; b < waveformBuckets; ++b)
    {
        const int i0 = b * numSamples / waveformBuckets;
        const int i1 = (b + 1) * numSamples / waveformBuckets;
        float pk = 0.0f;
        for (int i = i0; i < i1 && i < numSamples; ++i)
            pk = juce::jmax (pk, std::abs (mono[(size_t) i]));

        peaksOut[(size_t) b] = juce::jlimit (0.0f, 1.0f, pk);
    }
}

void AnalysisEngine::measureVu (const std::vector<float>& mono,
                                double sampleRate,
                                bool& outValid,
                                double& outAvg,
                                double& outMax)
{
    outValid = false;
    outAvg = 0;
    outMax = 0;

    if (mono.empty() || sampleRate < 1.0)
        return;

    const int winLen = juce::jmax (1, (int) std::llround (0.3 * sampleRate));
    const double refLin = std::pow (10.0, vuRefDbfs / 20.0);
    constexpr double eps = 1e-12;

    double sumSq = 0;
    juce::int64 vuCount = 0;
    double sumVu = 0;
    double maxVu = -std::numeric_limits<double>::infinity();

    for (size_t i = 0; i < mono.size(); ++i)
    {
        const double v = (double) mono[i];
        sumSq += v * v;

        if ((int) i >= winLen)
        {
            const double lv = (double) mono[i - (size_t) winLen];
            sumSq -= lv * lv;
        }

        if ((int) i >= winLen - 1)
        {
            const double rms = std::sqrt (juce::jmax (sumSq / (double) winLen, 0.0));
            const double vu = 20.0 * std::log10 (juce::jmax (rms / (refLin + eps), eps));
            if (std::isfinite (vu))
            {
                sumVu += vu;
                ++vuCount;
                maxVu = std::max (maxVu, vu);
            }
        }
    }

    if (vuCount > 0)
    {
        outValid = true;
        outAvg = sumVu / (double) vuCount;
        outMax = maxVu;
        return;
    }

    double sumSqAll = 0;
    for (float v : mono)
        sumSqAll += (double) v * (double) v;

    const double rms = std::sqrt (juce::jmax (sumSqAll / (double) mono.size(), 0.0));
    outValid = true;
    outAvg = outMax = 20.0 * std::log10 (juce::jmax (rms / (refLin + eps), eps));
}

void AnalysisEngine::measureWelchSpectrum (const std::vector<float>& mono,
                                           double sampleRate,
                                           std::vector<float>& freqOut,
                                           std::vector<float>& dbPerHzOut)
{
    freqOut.clear();
    dbPerHzOut.clear();

    const int m = (int) mono.size();
    const int nperseg = chooseWelchSegmentLength (m);
    if (nperseg <= 0 || sampleRate < 1.0)
        return;

    const int hop = juce::jmax (1, nperseg / 2);
    const int fftOrder = (int) std::round (std::log2 ((double) nperseg));
    if (fftOrder <= 0 || (1 << fftOrder) != nperseg)
        return;

    juce::dsp::FFT fft (fftOrder);
    const int nBins = fft.getSize() / 2 + 1;

    std::vector<float> window ((size_t) nperseg);
    for (int i = 0; i < nperseg; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) juce::jmax (1, nperseg - 1));

    double winSumSq = 0;
    for (float w : window)
        winSumSq += (double) w * (double) w;

    std::vector<double> acc ((size_t) nBins, 0.0);
    int segCount = 0;

    std::vector<float> fftData ((size_t) (2 * fft.getSize()), 0.f);

    for (int start = 0; start + nperseg <= m; start += hop)
    {
        ++segCount;
        for (int i = 0; i < nperseg; ++i)
            fftData[(size_t) i] = mono[(size_t) (start + i)] * window[(size_t) i];

        for (int i = nperseg; i < 2 * fft.getSize(); ++i)
            fftData[(size_t) i] = 0.f;

        fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

        for (int b = 0; b < nBins; ++b)
        {
            const double mag = (double) fftData[(size_t) b];
            acc[(size_t) b] += mag * mag;
        }
    }

    if (segCount <= 0 || winSumSq <= 0.0)
        return;

    const double scale = 1.0 / ((double) segCount * sampleRate * winSumSq);

    try
    {
        freqOut.resize ((size_t) nBins);
        dbPerHzOut.resize ((size_t) nBins);
    }
    catch (const std::bad_alloc&)
    {
        freqOut.clear();
        dbPerHzOut.clear();
        return;
    }

    constexpr double eps = 1e-20;

    for (int b = 0; b < nBins; ++b)
    {
        freqOut[(size_t) b] = (float) ((double) b * sampleRate / (double) nperseg);
        const double psd = acc[(size_t) b] * scale;
        dbPerHzOut[(size_t) b] = (float) (10.0 * std::log10 (juce::jmax (psd, eps)));
    }
}

void AnalysisEngine::measureRmsEnvelope (const std::vector<float>& mono,
                                         double sampleRate,
                                         std::vector<float>& timeOut,
                                         std::vector<float>& dbOut)
{
    timeOut.clear();
    dbOut.clear();

    if (mono.empty() || sampleRate < 1.0)
        return;

    const int hop = juce::jmax (1, (int) std::llround (0.05 * sampleRate));
    const int nBlocks = (int) (mono.size() / (size_t) hop);
    if (nBlocks <= 0)
        return;

    try
    {
        timeOut.resize ((size_t) nBlocks);
        dbOut.resize ((size_t) nBlocks);
    }
    catch (const std::bad_alloc&)
    {
        timeOut.clear();
        dbOut.clear();
        return;
    }

    constexpr double eps = 1e-12;

    for (int i = 0; i < nBlocks; ++i)
    {
        double sumSq = 0;
        for (int k = 0; k < hop; ++k)
        {
            const float v = mono[(size_t) (i * hop + k)];
            sumSq += (double) v * (double) v;
        }

        const double rms = std::sqrt (juce::jmax (sumSq / (double) hop, 0.0));
        timeOut[(size_t) i] = (float) (((double) i * hop + (double) hop * 0.5) / sampleRate);
        dbOut[(size_t) i] = (float) (20.0 * std::log10 (juce::jmax (rms, eps)));
    }
}

AnalysisSnapshot AnalysisEngine::analyzePlanar (const juce::AudioBuffer<float>& buffer,
                                                int numSamples,
                                                double sampleRate,
                                                const juce::String& label)
{
    AnalysisSnapshot snap;
    snap.label = label;
    snap.sampleRate = sampleRate;
    snap.numChannels = buffer.getNumChannels();
    snap.numFrames = numSamples;
    snap.durationSec = sampleRate > 0 ? (double) numSamples / sampleRate : 0;

    if (numSamples <= 0 || snap.numChannels <= 0 || sampleRate < 1.0)
        return snap;

    // Validate buffer size matches numSamples
    if (buffer.getNumSamples() < numSamples)
        return snap;

    try
    {
        // #region agent log
        areDbgLog ("H1", "analyzePlanar", "try_begin", numSamples, snap.numChannels);
        // #endregion

        std::vector<float> mono;
        downmixMonoMean (buffer, numSamples, mono);

        if (mono.empty())
        {
            areDbgLog ("H1", "analyzePlanar", "mono_empty", 0, 0);
            return snap;  // Return with valid=false
        }

        snap.valid = true;  // Only set valid=true AFTER we have mono data

        decimateWaveformPeaks (mono, numSamples, snap.waveformPeaks);

        const auto srUl = (unsigned long) sampleRate;
        const unsigned nCh = (unsigned) snap.numChannels;

        const int eburModeBlock = EBUR128_MODE_I | EBUR128_MODE_LRA | EBUR128_MODE_TRUE_PEAK;
        const int eburModeHist = eburModeBlock | EBUR128_MODE_HISTOGRAM;

        ebur128_state* ebRaw = nullptr;
        try
        {
            ebRaw = ebur128_init (nCh, srUl, eburModeBlock);
        }
        catch (...)
        {
            ebRaw = nullptr;
        }

        if (ebRaw == nullptr)
        {
            try
            {
                ebRaw = ebur128_init (nCh, srUl, eburModeHist);
            }
            catch (...)
            {
                ebRaw = nullptr;
            }
        }

        struct EbHolder
        {
            ebur128_state* st = nullptr;
            ~EbHolder()
            {
                if (st != nullptr)
                    ebur128_destroy (&st);
            }
        } eb { ebRaw };

        if (eb.st != nullptr)
        {
            configureEbur128Channels (eb.st, nCh);

            // Feed libebur128 from planar buffer in small interleaved chunks (no full-file duplicate).
            constexpr int kMaxEburChunkFrames = 65536;
            std::vector<float> interleavedChunk;

            try
            {
                interleavedChunk.resize ((size_t) kMaxEburChunkFrames * (size_t) nCh);
            }
            catch (const std::bad_alloc&)
            {
                areDbgLog ("H1", "analyzePlanar", "interleavedChunk_alloc_failed", 0, 0);
                interleavedChunk.clear();
                return snap;
            }

            snap.momentaryMaxLufs = -std::numeric_limits<double>::infinity();
            snap.shortTermMaxLufs = -std::numeric_limits<double>::infinity();

            const int chunkFrames = juce::jmax (1, (int) std::llround (0.05 * sampleRate));
            const int needMomentary = juce::jmax (1, (int) std::llround (0.4 * sampleRate));
            const int needShort = juce::jmax (1, (int) std::llround (3.0 * sampleRate));
            int fed = 0;

            for (int pos = 0; pos < numSamples;)
            {
                const int n = juce::jmin (juce::jmin (chunkFrames, kMaxEburChunkFrames), numSamples - pos);

                for (int i = 0; i < n; ++i)
                    for (unsigned c = 0; c < nCh; ++c)
                        interleavedChunk[(size_t) i * nCh + c] = buffer.getSample ((int) c, pos + i);

                ebur128_add_frames_float (eb.st, interleavedChunk.data(), (size_t) n);
                fed += n;
                pos += n;

                if (fed >= needMomentary)
                {
                    double m = 0;
                    if (ebur128_loudness_momentary (eb.st, &m) == EBUR128_SUCCESS && std::isfinite (m))
                        snap.momentaryMaxLufs = std::max (snap.momentaryMaxLufs, m);
                }

                if (fed >= needShort)
                {
                    double s = 0;
                    if (ebur128_loudness_shortterm (eb.st, &s) == EBUR128_SUCCESS && std::isfinite (s))
                        snap.shortTermMaxLufs = std::max (snap.shortTermMaxLufs, s);
                }
            }

            {
                double m = 0;
                if (ebur128_loudness_momentary (eb.st, &m) == EBUR128_SUCCESS && std::isfinite (m))
                    snap.momentaryMaxLufs = std::max (snap.momentaryMaxLufs, m);

                double s = 0;
                if (ebur128_loudness_shortterm (eb.st, &s) == EBUR128_SUCCESS && std::isfinite (s))
                    snap.shortTermMaxLufs = std::max (snap.shortTermMaxLufs, s);
            }

            double ig = -std::numeric_limits<double>::infinity();
            if (ebur128_loudness_global (eb.st, &ig) == EBUR128_SUCCESS)
                snap.integratedLufs = ig;

            double lra = 0;
            if (ebur128_loudness_range (eb.st, &lra) == EBUR128_SUCCESS)
                snap.lraLu = lra;

            double maxTpLin = 0;
            double maxSpLin = 0;
            for (unsigned c = 0; c < nCh; ++c)
            {
                double tp = 0, sp = 0;
                if (ebur128_true_peak (eb.st, c, &tp) == EBUR128_SUCCESS)
                    maxTpLin = juce::jmax (maxTpLin, tp);
                if (ebur128_sample_peak (eb.st, c, &sp) == EBUR128_SUCCESS)
                    maxSpLin = juce::jmax (maxSpLin, sp);
            }

            if (maxTpLin > 0)
                snap.truePeakDbTp = 20.0 * std::log10 (maxTpLin);
            if (maxSpLin > 0)
                snap.samplePeakDbfs = 20.0 * std::log10 (maxSpLin);
        }

        measureVu (mono, sampleRate, snap.vuValid, snap.vuAvg, snap.vuMax);
        measureWelchSpectrum (mono, sampleRate, snap.spectrumFreqHz, snap.spectrumDbPerHz);
        measureRmsEnvelope (mono, sampleRate, snap.rmsEnvelopeTimeSec, snap.rmsEnvelopeDbfs);

        // #region agent log
        areDbgLog ("H1", "analyzePlanar", "try_done", snap.valid ? 1 : 0, (int) juce::jmin ((juce::int64) 2000000000, snap.numFrames));
        // #endregion

        return snap;
    }
    catch (const std::bad_alloc&)
    {
        areDbgLog ("H1", "analyzePlanar", "bad_alloc_caught", 0, 0);
        return {};
    }
    catch (...)
    {
        areDbgLog ("H1", "analyzePlanar", "exception_caught", 0, 0);
        return {};
    }
}

AnalysisSnapshot AnalysisEngine::analyzeMono (std::vector<float> mono,
                                            double sampleRate,
                                            const juce::String& label)
{
    if (mono.empty())
        return {};

    juce::AudioBuffer<float> buf (1, (int) mono.size());
    buf.copyFrom (0, 0, mono.data(), (int) mono.size());
    sanitizePlanar (buf, (int) mono.size());
    return analyzePlanar (buf, (int) mono.size(), sampleRate, label);
}

AnalysisSnapshot AnalysisEngine::analyzeReader (juce::AudioFormatReader& reader,
                                                const juce::String& label,
                                                juce::int64 maxSecondsToRead)
{
    constexpr juce::int64 kMaxFramesInt = (juce::int64) std::numeric_limits<int>::max() - 256;

    const int nCh = reader.numChannels;
    if (nCh <= 0)
    {
        areDbgLog ("H1", "analyzeReader", "invalid_numChannels", nCh, 0);
        return {};
    }

    const double sr = reader.sampleRate;
    if (! std::isfinite (sr) || sr < 1.0 || sr > 1.0e7)
    {
        areDbgLog ("H1", "analyzeReader", "invalid_sampleRate", 0, 0);
        return {};
    }

    const double maxDur = juce::jmax (1.0, (double) maxSecondsToRead);
    const double maxFramesD = maxDur * sr;
    if (! std::isfinite (maxFramesD) || maxFramesD <= 0.0)
    {
        areDbgLog ("H1", "analyzeReader", "invalid_maxFrames", 0, 0);
        return {};
    }

    const juce::int64 maxByDuration = (juce::int64) std::llround (maxFramesD);
    juce::int64 usableFrames = juce::jmin (reader.lengthInSamples, maxByDuration);
    usableFrames = juce::jmax ((juce::int64) 0, usableFrames);

    if (usableFrames <= 0)
    {
        areDbgLog ("H1", "analyzeReader", "usableFrames_zero", 0, 0);
        return {};
    }

    usableFrames = juce::jmin (usableFrames, kMaxFramesInt);
    const juce::int64 capByMem = kMaxAnalysisTotalFloats / (juce::int64) nCh;
    usableFrames = juce::jmin (usableFrames, juce::jmax ((juce::int64) 0, capByMem));

    const int numSamples = (int) usableFrames;
    if (numSamples <= 0)
    {
        areDbgLog ("H1", "analyzeReader", "numSamples_zero_after_cap", 0, 0);
        return {};
    }

    try
    {
        juce::AudioBuffer<float> full (nCh, numSamples);
        juce::int64 readPos = 0;

        areDbgLog ("H1", "analyzeReader", "starting_read_loop", numSamples, nCh);

        while (readPos < usableFrames)
        {
            const int n = (int) juce::jmin ((juce::int64) 65536, usableFrames - readPos);

            try
            {
                if (! reader.read (&full, (int) readPos, n, readPos, true, true))
                {
                    areDbgLog ("H1", "analyzeReader", "reader_read_failed", (int) readPos, n);
                    return {};
                }
            }
            catch (const std::exception& e)
            {
                areDbgLog ("H1", "analyzeReader", "read_exception", (int) readPos, 0);
                return {};
            }
            catch (...)
            {
                areDbgLog ("H1", "analyzeReader", "read_unknown_exception", (int) readPos, 0);
                return {};
            }

            readPos += n;
        }

        areDbgLog ("H1", "analyzeReader", "read_complete", numSamples, 0);

        sanitizePlanar (full, numSamples);

        areDbgLog ("H1", "analyzeReader", "calling_analyzePlanar", numSamples, nCh);

        return analyzePlanar (full, numSamples, reader.sampleRate, label);
    }
    catch (const std::bad_alloc& e)
    {
        areDbgLog ("H1", "analyzeReader", "bad_alloc_exception", 0, 0);
        return {};
    }
    catch (const std::exception& e)
    {
        areDbgLog ("H1", "analyzeReader", "exception_caught", 0, 0);
        return {};
    }
    catch (...)
    {
        areDbgLog ("H1", "analyzeReader", "unknown_exception", 0, 0);
        return {};
    }
}

} // namespace are
