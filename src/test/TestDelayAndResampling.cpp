/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    Rubber Band Library
    An audio time-stretching and pitch-shifting library.
    Copyright 2007-2024 Particular Programs Ltd.

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.

    Alternatively, if you have a valid commercial licence for the
    Rubber Band Library obtained by agreement with the copyright
    holders, you may redistribute and/or modify it under the terms
    described in that licence.

    If you wish to distribute code using the Rubber Band Library
    under terms other than those of the GNU General Public License,
    you must obtain a valid commercial licence before doing so.
*/

// Tests for the delay/latency conventions around the initial padding
// and the algorithmic delay, and for the way those interact with the
// resampler placement (before or after the time-stretch stage).
//
// All audio used here is generated locally as small sinusoid /
// impulse signals, so the tests have no dependency on any external
// audio files.

#ifndef BOOST_TEST_DYN_LINK
#define BOOST_TEST_DYN_LINK
#endif
#include <boost/test/unit_test.hpp>

#include "../../rubberband/RubberBandStretcher.h"
#include "../../rubberband/RubberBandLiveShifter.h"

#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <string>

using namespace RubberBand;

using std::vector;
using std::cerr;
using std::endl;
using std::string;

BOOST_AUTO_TEST_SUITE(TestDelayAndResampling)

static const int rate = 44100;

// A small locally generated signal: a sine wave with a couple of
// unit impulses embedded so that both periodic content and
// transients are present.

static
vector<vector<float> >
generateSignal(int channels, int n, float freq)
{
    // Both channels carry the same waveform with a per-channel
    // constant level and phase, so channel synchronisation can be
    // checked exactly while the channels remain distinguishable.
    vector<vector<float> > sig(channels, vector<float>(n, 0.f));
    const float levels[] = { 0.6f, -0.6f };
    for (int c = 0; c < channels; ++c) {
        float level = levels[c % 2];
        for (int i = 0; i < n; ++i) {
            sig[c][i] = level *
                sinf(float(i) * freq * M_PI * 2.f / float(rate));
        }
        // Identically positioned impulse pairs give a common timing
        // reference with channel-specific polarity.
        int p0 = 600;
        int p1 = n - 800;
        sig[c][p0] = (c == 0 ? 1.0f : -1.0f);
        sig[c][p0 + 1] = (c == 0 ? -1.0f : 1.0f);
        sig[c][p1] = (c == 0 ? 0.9f : -0.9f);
        sig[c][p1 + 1] = (c == 0 ? -0.9f : 0.9f);
    }
    return sig;
}

static
vector<vector<float> >
generateSine(int channels, int n, float freq, float scale = 0.5f)
{
    vector<vector<float> > sig(channels, vector<float>(n, 0.f));
    for (int c = 0; c < channels; ++c) {
        float phase = c * 0.7f;
        for (int i = 0; i < n; ++i) {
            sig[c][i] = scale *
                sinf(float(i) * freq * M_PI * 2.f / float(rate) + phase);
        }
    }
    return sig;
}

// Run an entire offline pass and return every output sample frame.

static
vector<vector<float> >
runOffline(RubberBandStretcher &s,
           const vector<vector<float> > &in,
           int nOut)
{
    int channels = in.size();
    int n = int(in[0].size());

    vector<const float *> inp(channels, nullptr);
    for (int c = 0; c < channels; ++c) inp[c] = in[c].data();

    s.setMaxProcessSize(n);
    s.setExpectedInputDuration(n);

    s.study(inp.data(), n, true);
    s.process(inp.data(), n, true);

    BOOST_TEST(s.available() == nOut);

    vector<vector<float> > out(channels, vector<float>(nOut, 0.f));
    vector<float *> outp(channels, nullptr);
    for (int c = 0; c < channels; ++c) outp[c] = out[c].data();

    size_t got = s.retrieve(outp.data(), nOut);
    BOOST_TEST(got == size_t(nOut));
    BOOST_TEST(s.available() == -1);
    return out;
}

// Simulate block-by-block realtime use, including the documented
// start-pad / start-delay compensation, and return the compensated
// output stream. The control flow mirrors the process_realtime
// helper used by TestStretcher.cpp.

static
vector<vector<float> >
runRealtime(RubberBandStretcher &s,
            const vector<vector<float> > &in,
            int nOut,
            int blocksize)
{
    int channels = in.size();
    int n = int(in[0].size());

    s.setMaxProcessSize(blocksize);

    // 1. Prime with the preferred number of silent frames.
    {
        vector<vector<float> > pad(channels,
                                   vector<float>(s.getPreferredStartPad(),
                                                 0.f));
        vector<float *> padp(channels, nullptr);
        for (int c = 0; c < channels; ++c) padp[c] = pad[c].data();
        if (pad[0].size() > 0) {
            s.process((const float *const *)padp.data(), pad[0].size(),
                      false);
        }
    }

    // 2. Discard the reported start delay from the output.
    int toSkip = int(s.getStartDelay());

    vector<vector<float> > out(channels, vector<float>(nOut, 0.f));
    int inOffset = 0, outOffset = 0;

    while (outOffset < nOut) {
        int needed = std::min(blocksize, nOut - outOffset);
        int obtained = 0;

        while (obtained < needed) {
            int available = s.available();

            if (available < 0) {
                // Finished with no more output; any remaining frames
                // stay silent.
                outOffset = nOut;
                break;

            } else if (available == 0) {
                // Provide another block of input.
                int required = s.getSamplesRequired();
                int toProcess = required;
                bool finalBlock = false;
                if (toProcess >= n - inOffset) {
                    toProcess = n - inOffset;
                    finalBlock = true;
                }
                vector<const float *> bp(channels, nullptr);
                for (int c = 0; c < channels; ++c) {
                    bp[c] = in[c].data() + inOffset;
                }
                s.process(bp.data(), toProcess, finalBlock);
                inOffset += toProcess;

            } else if (toSkip > 0) {
                int take = std::min(toSkip, available);
                vector<vector<float> > block(channels,
                                             vector<float>(take, 0.f));
                vector<float *> bp(channels, nullptr);
                for (int c = 0; c < channels; ++c) {
                    bp[c] = block[c].data();
                }
                int got = int(s.retrieve(bp.data(), take));
                BOOST_TEST(got == take);
                toSkip -= got;

            } else {
                int take = std::min(needed - obtained, available);
                vector<vector<float> > block(channels,
                                             vector<float>(take, 0.f));
                vector<float *> bp(channels, nullptr);
                for (int c = 0; c < channels; ++c) {
                    bp[c] = block[c].data();
                }
                int got = int(s.retrieve(bp.data(), take));
                BOOST_TEST(got == take);
                for (int c = 0; c < channels; ++c) {
                    for (int i = 0; i < got; ++i) {
                        out[c][outOffset + obtained + i] = block[c][i];
                    }
                }
                obtained += got;
            }
        }
        outOffset += obtained;
    }

    return out;
}

// Index of the largest absolute sample within a range.

static
int peakIndex(const vector<float> &v, int i0, int i1)
{
    i1 = std::min(i1, int(v.size()));
    int best = i0;
    float bestMag = -1.f;
    for (int i = i0; i < i1; ++i) {
        float m = fabsf(v[i]);
        if (m > bestMag) { bestMag = m; best = i; }
    }
    return best;
}

// Count positive-going zero crossings, used to estimate frequency.

static
int zeroCrossings(const vector<float> &v, int i0, int i1)
{
    i1 = std::min(i1, int(v.size()));
    int count = 0;
    for (int i = i0 + 1; i < i1; ++i) {
        if (v[i - 1] <= 0.f && v[i] > 0.f) ++count;
    }
    return count;
}

static
double rms(const vector<float> &v, int i0, int i1)
{
    i1 = std::min(i1, int(v.size()));
    double acc = 0.0;
    for (int i = i0; i < i1; ++i) acc += double(v[i]) * double(v[i]);
    return sqrt(acc / std::max(1, i1 - i0));
}

// --------------------------------------------------------------------
// 1. Delay conventions: offline performs padding/compensation
// internally, so both values are zero. In realtime mode both are
// positive and their values depend on whether the resampler runs
// before or after the stretch stage.
// --------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(offline_has_zero_pad_and_delay)
{
    const double pitchScales[] = { 0.5, 1.0, 1.5, 2.0 };
    const RubberBandStretcher::Options engines[] = {
        RubberBandStretcher::OptionEngineFaster,
        RubberBandStretcher::OptionEngineFiner
    };

    for (auto engine : engines) {
        for (double p : pitchScales) {
            RubberBandStretcher s(rate, 2, engine, 1.0, p);
            BOOST_TEST(s.getPreferredStartPad() == 0u);
            BOOST_TEST(s.getStartDelay() == 0u);
        }
    }
}

BOOST_AUTO_TEST_CASE(realtime_pad_and_delay_are_positive)
{
    const double pitchScales[] = { 0.5, 1.0, 1.5, 2.0 };
    const RubberBandStretcher::Options engines[] = {
        RubberBandStretcher::OptionEngineFaster,
        RubberBandStretcher::OptionEngineFiner
    };

    for (auto engine : engines) {
        for (double p : pitchScales) {
            RubberBandStretcher::Options opts =
                engine | RubberBandStretcher::OptionProcessRealTime;
            RubberBandStretcher s(rate, 2, opts, 1.0, p);
            size_t pad = s.getPreferredStartPad();
            size_t delay = s.getStartDelay();

            // Unity pitch needs no resampler, but the windowing
            // algorithm still has a start delay in realtime mode.
            BOOST_TEST(pad > 0u);
            BOOST_TEST(delay > 0u);

            // Both are reported in output sample frames and must be
            // small relative to even a short signal.
            BOOST_TEST(pad < size_t(rate / 2));
            BOOST_TEST(delay < size_t(rate / 2));
        }
    }
}

// R3 resampler placement rules (see R3Stretcher::areWeResampling):
//  - HighConsistency: always resample AFTER
//  - HighSpeed (default): pitch > 1 resamples BEFORE,
//                         pitch < 1 resamples AFTER
//  - HighQuality:        pitch > 1 resamples AFTER,
//                         pitch < 1 resamples BEFORE
// When resampling happens BEFORE, the input pad is scaled by the
// pitch ratio while the output delay is the plain window half-size;
// when AFTER, the pad is unscaled and the delay is divided by the
// pitch ratio.

static
void checkR3ResamplerPlacement(RubberBandStretcher::Options pitchOpt,
                               double pitch,
                               bool expectBefore)
{
    RubberBandStretcher::Options opts =
        RubberBandStretcher::OptionEngineFiner |
        RubberBandStretcher::OptionProcessRealTime | pitchOpt;
    RubberBandStretcher s(rate, 1, opts, 1.0, pitch);

    // Unity-pitch reference: no resampler at all, giving the base
    // window half-size for both pad and delay.
    RubberBandStretcher ref(rate, 1,
                            RubberBandStretcher::OptionEngineFiner |
                            RubberBandStretcher::OptionProcessRealTime,
                            1.0, 1.0);
    size_t base = ref.getStartDelay();
    BOOST_TEST(base > 0u);

    size_t pad = s.getPreferredStartPad();
    size_t delay = s.getStartDelay();

    if (expectBefore) {
        size_t expectedPad = size_t(ceil(double(base) * pitch));
        BOOST_TEST(pad == expectedPad);
        BOOST_TEST(delay == base);
    } else {
        size_t expectedDelay = size_t(ceil(double(base) / pitch));
        BOOST_TEST(pad == base);
        BOOST_TEST(delay == expectedDelay);
    }
}

BOOST_AUTO_TEST_CASE(r3_resampler_before_after_delay_conventions)
{
    using O = RubberBandStretcher;

    // HighConsistency: always after
    checkR3ResamplerPlacement(O::OptionPitchHighConsistency, 2.0, false);
    checkR3ResamplerPlacement(O::OptionPitchHighConsistency, 0.5, false);

    // HighSpeed: up-shift before, down-shift after
    checkR3ResamplerPlacement(O::OptionPitchHighSpeed, 2.0, true);
    checkR3ResamplerPlacement(O::OptionPitchHighSpeed, 0.5, false);

    // HighQuality: up-shift after, down-shift before
    checkR3ResamplerPlacement(O::OptionPitchHighQuality, 2.0, false);
    checkR3ResamplerPlacement(O::OptionPitchHighQuality, 0.5, true);
}

// --------------------------------------------------------------------
// 2. Resampling with speed changes: resample-before and resample-after
// configurations must both produce the expected output duration,
// output pitch, and correctly aligned transients, in both processing
// modes.
// --------------------------------------------------------------------

static
void checkResampledStream(const vector<vector<float> > &out,
                          int inputLength,
                          double timeRatio,
                          double pitchScale,
                          int impulseAt,
                          float baseFreq)
{
    int channels = int(out.size());
    int nOut = int(out[0].size());

    BOOST_TEST(nOut == int(round(inputLength * timeRatio)));

    for (int c = 0; c < channels; ++c) {

        // Duration: the number of output frames follows the time
        // ratio (pitch shift alone must not change duration).
        BOOST_TEST(int(out[c].size()) == nOut);

        // Pitch: zero-crossing density in the steady-state region
        // scales by the pitch ratio, independent of time ratio.
        int i0 = nOut / 4;
        int i1 = (nOut * 3) / 4;
        int crossings = zeroCrossings(out[c], i0, i1);
        int span = i1 - i0;
        // Each positive-going zero crossing marks one full cycle.
        double measuredFreq =
            double(crossings) * double(rate) / double(span);
        double expectedFreq = double(baseFreq) * pitchScale;
        double cents = 1200.0 *
            log(measuredFreq / expectedFreq) / log(2.0);
        if (fabs(cents) >= 60.0) {
            cerr << "pitch check: channel " << c
                 << ", measured " << measuredFreq
                 << " Hz, expected " << expectedFreq
                 << " Hz (" << cents << " cents), crossings "
                 << crossings << " in " << span << " frames"
                 << endl;
        }
        // Windowing at the region edges costs a crossing or two;
        // allow a generous 60 cents, the steady-state estimate is
        // much tighter in practice.
        BOOST_TEST(fabs(cents) < 60.0);

        // Alignment: the input impulse appears at the time-ratio
        // scaled position. Give the transient detector some slack.
        int expectedPeak = int(round(impulseAt * timeRatio));
        int peak = peakIndex(out[c],
                             std::max(0, expectedPeak - 400),
                             expectedPeak + 400);
        BOOST_TEST(abs(peak - expectedPeak) < 400);
    }
}

BOOST_AUTO_TEST_CASE(resample_before_and_after_offline)
{
    using O = RubberBandStretcher;
    const int n = 4096;
    auto in = generateSignal(2, n, 440.f);
    const double timeRatio = 1.5;
    const double pitch = 2.0;
    const int nOut = int(round(n * timeRatio));

    struct Case {
        O::Options opts;
        string name;
    } cases[] = {
        { O::OptionEngineFiner | O::OptionPitchHighSpeed,
          "offline-highspeed" },
        { O::OptionEngineFiner | O::OptionPitchHighQuality,
          "offline-highquality" },
        { O::OptionEngineFiner | O::OptionPitchHighConsistency,
          "offline-highconsistency" }
    };

    for (auto &tc : cases) {
        RubberBandStretcher s(rate, 2, tc.opts, timeRatio, pitch);
        auto out = runOffline(s, in, nOut);
        checkResampledStream(out, n, timeRatio, pitch, 600, 440.f);
    }
}

BOOST_AUTO_TEST_CASE(resample_before_and_after_realtime)
{
    using O = RubberBandStretcher;
    const int n = 8192;
    auto in = generateSignal(2, n, 440.f);
    const double timeRatio = 1.5;
    const double pitch = 2.0;
    const int nOut = int(round(n * timeRatio));

    struct Case {
        O::Options opts;
        string name;
    } cases[] = {
        { O::OptionEngineFiner | O::OptionProcessRealTime |
          O::OptionPitchHighSpeed, "rt-highspeed-before" },
        { O::OptionEngineFiner | O::OptionProcessRealTime |
          O::OptionPitchHighQuality, "rt-highquality-mixed" },
        { O::OptionEngineFiner | O::OptionProcessRealTime |
          O::OptionPitchHighConsistency, "rt-highconsistency-after" }
    };

    for (auto &tc : cases) {
        RubberBandStretcher s(rate, 2, tc.opts, timeRatio, pitch);
        size_t pad = s.getPreferredStartPad();
        size_t delay = s.getStartDelay();
        BOOST_TEST(pad > 0u);
        BOOST_TEST(delay > 0u);

        auto out = runRealtime(s, in, nOut, 512);
        checkResampledStream(out, n, timeRatio, pitch, 600, 440.f);
    }
}

// Speed-up (time ratio < 1) as well as slow-down, again both modes.

BOOST_AUTO_TEST_CASE(resample_speed_change_both_modes)
{
    using O = RubberBandStretcher;
    const int n = 8192;
    auto in = generateSignal(2, n, 330.f);

    const double ratios[] = { 0.75, 1.5 };
    const double pitches[] = { 0.75, 1.5 };

    for (double timeRatio : ratios) {
        for (double pitch : pitches) {
            int nOut = int(round(n * timeRatio));

            {
                RubberBandStretcher off(rate, 2,
                    O::OptionEngineFiner, timeRatio, pitch);
                auto out = runOffline(off, in, nOut);
                checkResampledStream(out, n, timeRatio, pitch, 600,
                                     330.f);
            }
            {
                RubberBandStretcher rt(rate, 2,
                    O::OptionEngineFiner | O::OptionProcessRealTime |
                    O::OptionPitchHighConsistency,
                    timeRatio, pitch);
                auto out = runRealtime(rt, in, nOut, 512);
                checkResampledStream(out, n, timeRatio, pitch, 600,
                                     330.f);
            }
        }
    }
}

// --------------------------------------------------------------------
// 3. First and last samples: offline mode returns exactly the
// ratio-scaled count and keeps the start of the signal aligned
// (internal padding); realtime mode only aligns after the caller
// applies pad + start-delay, and must flush the tail through after
// the final input block rather than truncating it.
// --------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(offline_start_and_end_sample_counts)
{
    using O = RubberBandStretcher;
    const int n = 4096;
    auto in = generateSignal(2, n, 440.f);

    const double timeRatio = 2.0;
    int nOut = int(round(n * timeRatio));

    RubberBandStretcher s(rate, 2, O::OptionEngineFiner,
                          timeRatio, 1.5);
    auto out = runOffline(s, in, nOut);

    // Exactly the expected frame count on both channels: nothing is
    // lost at either end.
    BOOST_TEST(out[0].size() == size_t(nOut));
    BOOST_TEST(out[1].size() == size_t(nOut));

    // The first transient survives and stays at its original scaled
    // position, i.e. offline padding did not shift the head.
    int peak0 = peakIndex(out[0], 0, nOut / 4);
    BOOST_TEST(abs(peak0 - 600 * int(timeRatio)) < 300);

    // The tail impulse survives as well.
    int tailAt = int(round((n - 800) * timeRatio));
    int peak1 = peakIndex(out[0], tailAt - 400, tailAt + 400);
    BOOST_TEST(abs(peak1 - tailAt) < 400);
}

BOOST_AUTO_TEST_CASE(realtime_without_compensation_loses_head_samples)
{
    // Without pad/delay handling the first samples are attenuated:
    // document that the compensation is necessary by showing the
    // uncompensated head region is much quieter than the compensated
    // one.
    using O = RubberBandStretcher;
    const int n = 8192;
    auto in = generateSine(1, n, 440.f);
    const int nOut = n;
    const int bs = 512;

    auto processRaw = [&]() {
        RubberBandStretcher s(rate, 1,
            O::OptionEngineFiner | O::OptionProcessRealTime);
        s.setMaxProcessSize(bs);
        vector<float> got;
        int inOffset = 0;
        while (int(got.size()) < nOut + 4096) {
            int avail = s.available();
            if (avail < 0) break;
            if (avail == 0) {
                int req = s.getSamplesRequired();
                if (req <= 0) req = bs;
                int take = std::min(req, n - inOffset);
                bool finalBlock = (inOffset + take >= n);
                const float *p = in[0].data() + inOffset;
                s.process(&p, take, finalBlock);
                inOffset += take;
            } else {
                vector<float> block(avail);
                float *p = block.data();
                size_t g = s.retrieve(&p, avail);
                for (size_t i = 0; i < g; ++i) got.push_back(block[i]);
            }
        }
        return got;
    };

    vector<float> raw = processRaw();
    RubberBandStretcher s(rate, 1,
        O::OptionEngineFiner | O::OptionProcessRealTime);
    auto comp = runRealtime(s, in, nOut, bs);

    int probe = 1024;
    double rawHead = rms(raw, 0, probe);
    double compHead = rms(comp[0], 0, probe);

    // The compensated stream reaches full signal level at the head;
    // the raw stream is quieter there because of fill-in zeros and
    // window ramp-up.
    BOOST_TEST(compHead > 0.1);
    BOOST_TEST(rawHead < compHead * 0.5);
}

BOOST_AUTO_TEST_CASE(realtime_tail_is_flushed_after_final_block)
{
    using O = RubberBandStretcher;
    const int n = 8192;
    auto in = generateSignal(2, n, 440.f);
    const int nOut = n;

    RubberBandStretcher s(rate, 2,
        O::OptionEngineFiner | O::OptionProcessRealTime |
        O::OptionPitchHighConsistency);

    auto out = runRealtime(s, in, nOut, 512);

    // The tail impulse must appear in the flushed output rather than
    // being dropped with the final input block.
    int tailAt = n - 800;
    for (int c = 0; c < 2; ++c) {
        int peak = peakIndex(out[c], tailAt - 600, tailAt + 600);
        BOOST_TEST(abs(peak - tailAt) < 600);
        // And there must be non-trivial energy right up to the end,
        // i.e. the stream was not truncated early.
        double tailLevel = rms(out[c], nOut - 1024, nOut);
        BOOST_TEST(tailLevel > 0.02);
    }
}

// --------------------------------------------------------------------
// 4. Stereo synchronisation: the two channels must stay sample-frame
// aligned through all of this. Test both channel handling modes
// (apart and together) and both processing modes, plus the live
// shifter.
// --------------------------------------------------------------------

static
void checkStereoAligned(const vector<vector<float> > &out,
                        int tolerance)
{
    BOOST_REQUIRE(out.size() == 2u);
    int n = int(out[0].size());
    BOOST_TEST(int(out[1].size()) == n);

    // Impulse peaks land on the same frame in both channels.
    int quarter = n / 4;
    int p0 = peakIndex(out[0], 0, quarter);
    int p1 = peakIndex(out[1], 0, quarter);
    if (abs(p0 - p1) > tolerance) {
        cerr << "stereo head peak mismatch: " << p0 << " vs " << p1
             << endl;
    }
    BOOST_TEST(abs(p0 - p1) <= tolerance);

    int threeQ = (n * 3) / 4;
    int q0 = peakIndex(out[0], threeQ, n);
    int q1 = peakIndex(out[1], threeQ, n);
    if (abs(q0 - q1) > tolerance) {
        cerr << "stereo tail peak mismatch: " << q0 << " vs " << q1
             << endl;
    }
    BOOST_TEST(abs(q0 - q1) <= tolerance);

    // Correlation check across the steady-state region: any constant
    // channel offset would show up as a reduced best-lag correlation.
    // Use absolute correlation because the channels may have
    // opposite polarity (the test signals use signed levels).
    double bestAtZero = -1.0, bestElsewhere = -1.0;
    for (int lag = -8; lag <= 8; ++lag) {
        double acc = 0.0;
        int count = 0;
        for (int i = quarter; i < threeQ; ++i) {
            int j = i + lag;
            if (j < quarter || j >= threeQ) continue;
            acc += double(out[0][i]) * double(out[1][j]);
            ++count;
        }
        if (count > 0) acc = fabs(acc / count);
        if (lag == 0) bestAtZero = acc;
        else bestElsewhere = std::max(bestElsewhere, acc);
    }
    if (!(bestAtZero >= bestElsewhere * 0.98)) {
        cerr << "stereo correlation: lag-zero " << bestAtZero
             << " best other-lag " << bestElsewhere << endl;
    }
    BOOST_TEST(bestAtZero >= bestElsewhere * 0.98);
}

BOOST_AUTO_TEST_CASE(stereo_sync_offline)
{
    using O = RubberBandStretcher;
    const int n = 4096;
    auto in = generateSignal(2, n, 440.f);
    const double timeRatio = 1.5;
    const double pitch = 1.5;
    int nOut = int(round(n * timeRatio));

    O::Options channelModes[] = {
        O::OptionChannelsApart,
        O::OptionChannelsTogether
    };
    for (auto cm : channelModes) {
        RubberBandStretcher s(rate, 2,
            O::OptionEngineFiner | cm, timeRatio, pitch);
        auto out = runOffline(s, in, nOut);
        checkStereoAligned(out, 2);
    }
}

BOOST_AUTO_TEST_CASE(stereo_sync_realtime)
{
    using O = RubberBandStretcher;
    const int n = 8192;
    auto in = generateSignal(2, n, 440.f);
    const double timeRatio = 1.25;
    const double pitch = 0.8;
    int nOut = int(round(n * timeRatio));

    O::Options channelModes[] = {
        O::OptionChannelsApart,
        O::OptionChannelsTogether
    };
    for (auto cm : channelModes) {
        RubberBandStretcher s(rate, 2,
            O::OptionEngineFiner | O::OptionProcessRealTime |
            O::OptionPitchHighConsistency | cm,
            timeRatio, pitch);
        auto out = runRealtime(s, in, nOut, 512);
        checkStereoAligned(out, 3);
    }
}

BOOST_AUTO_TEST_CASE(stereo_sync_live_shifter)
{
    using O = RubberBandLiveShifter;
    const int n = 8192;
    auto in = generateSignal(2, n, 440.f);

    O::Options channelModes[] = {
        O::OptionChannelsApart,
        O::OptionChannelsTogether
    };
    const double pitches[] = { 0.5, 1.0, 2.0 };

    for (auto cm : channelModes) {
        for (double pitch : pitches) {
            RubberBandLiveShifter shifter(rate, 2, cm);
            shifter.setPitchScale(pitch);

            int bs = int(shifter.getBlockSize());
            BOOST_TEST(bs == 512);
            int blocks = n / bs;
            int total = blocks * bs;

            vector<vector<float> > out(2, vector<float>(total, 0.f));
            for (int b = 0; b < blocks; ++b) {
                const float *inp[2] = {
                    in[0].data() + b * bs,
                    in[1].data() + b * bs
                };
                float *outp[2] = {
                    out[0].data() + b * bs,
                    out[1].data() + b * bs
                };
                shifter.shift(inp, outp);
            }

            // shift() is fixed-size and returns a block every call,
            // so both channels always have identical, complete
            // lengths.
            BOOST_TEST(out[0].size() == out[1].size());

            // Channel peaks remain frame-aligned despite the start
            // delay. Search the window around the first input impulse
            // (at frame 600) so that ordinary sine-wave extrema do
            // not dominate the peak search; the reported delay puts
            // the impulse at delay + 600 in the output.
            int delay = int(shifter.getStartDelay());
            BOOST_TEST(delay > 0);
            BOOST_TEST(delay < total);

            int centre = delay + 600;
            int first = peakIndex(out[0], centre - 256, centre + 256);
            int second = peakIndex(out[1], centre - 256, centre + 256);
            if (abs(first - second) > 2) {
                cerr << "live shifter stereo peak mismatch: "
                     << first << " vs " << second
                     << " (centre " << centre << ")" << endl;
            }
            BOOST_TEST(abs(first - second) <= 2);

            // Lag-0 cross-correlation remains maximal over the
            // steady-state region on both channels. Absolute value
            // because the test channels use opposite polarity. A
            // small relative slack covers rounding in the two
            // independently processed channels.
            double bestAtZero = -1.0, bestElsewhere = -1.0;
            int i0 = total / 4, i1 = (total * 3) / 4;
            for (int lag = -8; lag <= 8; ++lag) {
                double acc = 0.0;
                int count = 0;
                for (int i = i0; i < i1; ++i) {
                    int j = i + lag;
                    if (j < i0 || j >= i1) continue;
                    acc += double(out[0][i]) * double(out[1][j]);
                    ++count;
                }
                if (count > 0) acc = fabs(acc / count);
                if (lag == 0) bestAtZero = acc;
                else bestElsewhere = std::max(bestElsewhere, acc);
            }
            if (!(bestAtZero >= bestElsewhere * 0.98)) {
                cerr << "live shifter correlation: lag-zero "
                     << bestAtZero << " best other-lag "
                     << bestElsewhere << endl;
            }
            BOOST_TEST(bestAtZero >= bestElsewhere * 0.98);
        }
    }
}

// The live shifter reports its own start delay, which changes with
// the pitch ratio because of the resampler fill-in, and that delay is
// exactly the number of head frames that need discarding for
// alignment. Verify that after discarding them a unity-pitch sine
// comes back in alignment with the input.

BOOST_AUTO_TEST_CASE(live_shifter_start_delay_alignment)
{
    const int n = 16384;
    auto in = generateSine(1, n, 440.f);

    RubberBandLiveShifter shifter(rate, 1,
                                  RubberBandLiveShifter::DefaultOptions);
    shifter.setPitchScale(1.0);

    int bs = int(shifter.getBlockSize());
    int blocks = n / bs;
    vector<float> out(blocks * bs, 0.f);
    for (int b = 0; b < blocks; ++b) {
        const float *inp = in[0].data() + b * bs;
        float *outp = out.data() + b * bs;
        shifter.shift(&inp, &outp);
    }

    int delay = int(shifter.getStartDelay());

    // Steady-state region, well clear of both head fill-in and the
    // final blocks.
    int i0 = delay + 2048;
    int i1 = n - 2048;
    double err = 0.0;
    int count = 0;
    for (int i = i0; i < i1; ++i) {
        double d = double(out[i]) - double(in[0][i - delay]);
        err += d * d;
        ++count;
    }
    err = sqrt(err / count);
    BOOST_TEST(err < 0.05);
}

// --------------------------------------------------------------------
// 5. Sample-rate coverage: the delay conventions and alignment must
// hold at 48 kHz as well, including with the R2 (faster) engine.
// --------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(delay_conventions_and_alignment_at_48k)
{
    using O = RubberBandStretcher;
    const int localRate = 48000;
    const int n = 4096;
    const float freq = 260.f;

    // Locally generated 48 kHz stereo signal.
    vector<vector<float> > in(2, vector<float>(n, 0.f));
    for (int c = 0; c < 2; ++c) {
        float level = (c == 0 ? 0.6f : -0.6f);
        for (int i = 0; i < n; ++i) {
            in[c][i] = level *
                sinf(float(i) * freq * M_PI * 2.f / float(localRate));
        }
        in[c][600] = (c == 0 ? 1.0f : -1.0f);
        in[c][601] = (c == 0 ? -1.0f : 1.0f);
    }

    // Offline: zero pad/delay, exact ratio-scaled length. The R2
    // engine's fixed window at 48 kHz leaves too little output from
    // this short a signal for an exact-count check, so the offline
    // leg exercises R3 while R2 is covered in realtime below and
    // elsewhere in the suite at 44.1 kHz.
    {
        const double timeRatio = 1.25;
        int nOut = int(round(n * timeRatio));
        RubberBandStretcher s(localRate, 2, O::OptionEngineFiner,
                              timeRatio, 1.0);
        BOOST_TEST(s.getPreferredStartPad() == 0u);
        BOOST_TEST(s.getStartDelay() == 0u);
        auto out = runOffline(s, in, nOut);
        BOOST_TEST(int(out[0].size()) == nOut);
        BOOST_TEST(int(out[1].size()) == nOut);
    }

    O::Options engines[] = { O::OptionEngineFaster,
                             O::OptionEngineFiner };
    for (auto engine : engines) {
        // Realtime: positive pad/delay, compensated stream aligned.
        RubberBandStretcher s(localRate, 2,
            engine | O::OptionProcessRealTime |
            O::OptionPitchHighConsistency, 1.0, 1.5);
        size_t pad = s.getPreferredStartPad();
        size_t delay = s.getStartDelay();
        BOOST_TEST(pad > 0u);
        BOOST_TEST(delay > 0u);

        int nOut = n;
        auto out = runRealtime(s, in, nOut, 512);
        checkStereoAligned(out, 3);
    }
}

// --------------------------------------------------------------------
// 6. Reset: after reset() the realtime delay convention still applies
// and a fresh run reproduces the same aligned output; the live
// shifter likewise keeps its block-size and start-delay contract.
// --------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(realtime_reset_restores_delay_behaviour)
{
    using O = RubberBandStretcher;
    const int n = 8192;
    auto in = generateSignal(2, n, 440.f);
    const int nOut = int(round(n * 1.25));

    O::Options opts =
        O::OptionEngineFiner | O::OptionProcessRealTime |
        O::OptionPitchHighConsistency;

    RubberBandStretcher s(rate, 2, opts, 1.25, 0.8);
    auto first = runRealtime(s, in, nOut, 512);

    size_t padBefore = s.getPreferredStartPad();
    size_t delayBefore = s.getStartDelay();

    s.reset();

    // Reset must leave the reported delay convention intact.
    BOOST_TEST(s.getPreferredStartPad() == padBefore);
    BOOST_TEST(s.getStartDelay() == delayBefore);

    auto second = runRealtime(s, in, nOut, 512);
    BOOST_REQUIRE(first.size() == second.size());
    for (size_t c = 0; c < first.size(); ++c) {
        BOOST_TEST(second[c].size() == first[c].size());
        // A reset run is deterministic and reproduces the first run.
        int len = int(std::min(first[c].size(), second[c].size()));
        for (int i = 0; i < len; ++i) {
            BOOST_TEST(fabsf(second[c][i] - first[c][i]) < 1e-5f);
        }
    }

    RubberBandLiveShifter shifter(rate, 2,
                                  RubberBandLiveShifter::DefaultOptions);
    shifter.setPitchScale(0.8);
    size_t liveDelayBefore = shifter.getStartDelay();
    int blocksize = int(shifter.getBlockSize());
    shifter.reset();
    BOOST_TEST(shifter.getBlockSize() == size_t(blocksize));
    BOOST_TEST(shifter.getStartDelay() == liveDelayBefore);
}

BOOST_AUTO_TEST_SUITE_END()
