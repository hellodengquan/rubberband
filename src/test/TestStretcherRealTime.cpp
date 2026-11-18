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

#ifndef BOOST_TEST_DYN_LINK
#define BOOST_TEST_DYN_LINK
#endif
#include <boost/test/unit_test.hpp>

#include "../../rubberband/RubberBandStretcher.h"

#include <iostream>
#include <functional>

#include <cmath>

using namespace RubberBand;

using std::vector;
using std::cerr;
using std::endl;

namespace tt = boost::test_tools;

BOOST_AUTO_TEST_SUITE(TestStretcherRealTime)

// These tests exercise the real-time (streaming) mode of
// RubberBandStretcher, in which both the time ratio and the pitch
// scale may be changed at any point during processing. They cover:
//
//  * changing time ratio and pitch scale together mid-stream, with
//    the output remaining continuous and time-mapped according to the
//    ratio history;
//  * input and output block sizes that differ from one another,
//    including the requirement that the retrieve() chunking has no
//    effect on the output stream;
//  * draining the stretcher at the end of input (final = true) until
//    available() reports -1;
//  * preservation of left/right channel identity in stereo material;
//  * the documented handling of calls that are not meaningful in
//    real-time mode (study, setKeyFrameMap, process after final) or
//    not supported by an engine (formant scale in R2), and the
//    locking of ratios in offline mode once studying has begun.

// Drive the stretcher in real-time mode: prime with
// getPreferredStartPad() silent frames, discard getStartDelay()
// output frames, feed input in blocks of inBlock frames, and retrieve
// output in chunks of at most outBlock frames (or all available
// frames at once if outBlock is 0). After the final process() call,
// keep retrieving until available() returns -1. The beforeProcess
// hook, if given, is called with the current input offset before each
// process() call, allowing tests to change ratios mid-stream.
static vector<vector<float>> run_realtime(RubberBandStretcher &stretcher,
                                          const vector<vector<float>> &in,
                                          int inBlock,
                                          int outBlock,
                                          std::function<void(int)> beforeProcess = {})
{
    int channels = int(in.size());
    int n = int(in[0].size());

    vector<vector<float>> out(channels);

    stretcher.setMaxProcessSize(inBlock);

    // Prime the start with silence, as documented for real-time mode
    int pad = int(stretcher.getPreferredStartPad());
    if (pad > 0) {
        vector<float> silence(pad, 0.f);
        vector<const float *> src(channels);
        for (int c = 0; c < channels; ++c) src[c] = silence.data();
        stretcher.process(src.data(), pad, false);
    }

    int toSkip = int(stretcher.getStartDelay());

    int rbufSize = (outBlock > 0 ? outBlock : 65536);
    vector<vector<float>> rbuf(channels, vector<float>(rbufSize, 0.f));
    vector<float *> rptr(channels);
    for (int c = 0; c < channels; ++c) rptr[c] = rbuf[c].data();

    vector<const float *> src(channels);

    int inOffset = 0;
    bool finalSent = false;
    bool finished = false;

    while (!finished) {

        if (!finalSent) {
            int chunk = std::min(inBlock, n - inOffset);
            bool final = (inOffset + chunk >= n);
            if (beforeProcess) beforeProcess(inOffset);
            for (int c = 0; c < channels; ++c) {
                src[c] = in[c].data() + inOffset;
            }
            stretcher.process(src.data(), chunk, final);
            inOffset += chunk;
            finalSent = final;
        }

        int avail = stretcher.available();

        if (avail < 0) { // -1: fully processed and all output read
            finished = true;

        } else if (avail == 0) {
            // No output pending. Before the final chunk this simply
            // means more input is needed; afterwards the stretcher
            // must eventually report -1 rather than stalling at 0
            BOOST_TEST(!finalSent);
            if (finalSent) break;

        } else {
            int toGet = avail;
            if (outBlock > 0) toGet = std::min(outBlock, avail);
            if (toGet > rbufSize) {
                rbufSize = toGet;
                for (int c = 0; c < channels; ++c) {
                    rbuf[c].resize(rbufSize, 0.f);
                    rptr[c] = rbuf[c].data();
                }
            }
            size_t got = stretcher.retrieve(rptr.data(), toGet);
            BOOST_TEST(got == size_t(toGet));
            for (int i = 0; i < int(got); ++i) {
                if (toSkip > 0) {
                    --toSkip;
                    continue;
                }
                for (int c = 0; c < channels; ++c) {
                    out[c].push_back(rbuf[c][i]);
                }
            }
        }
    }

    return out;
}

static vector<float> sinusoid(int n, int rate, float freq, float amplitude)
{
    vector<float> v(n);
    for (int i = 0; i < n; ++i) {
        v[i] = amplitude * sinf(float(i) * freq * M_PI * 2.f / float(rate));
    }
    return v;
}

static int count_positive_crossings(const vector<float> &v, int i0, int i1)
{
    int crossings = 0;
    for (int i = i0; i + 1 < i1 && i + 1 < int(v.size()); ++i) {
        if (v[i] <= 0.f && v[i+1] > 0.f) ++crossings;
    }
    return crossings;
}

static double rms(const vector<float> &v, int i0, int i1)
{
    if (i1 > int(v.size())) i1 = int(v.size());
    if (i0 >= i1) return 0.0;
    double acc = 0.0;
    for (int i = i0; i < i1; ++i) acc += double(v[i]) * double(v[i]);
    return sqrt(acc / double(i1 - i0));
}

static float max_abs_jump(const vector<float> &v)
{
    float worst = 0.f;
    for (int i = 0; i + 1 < int(v.size()); ++i) {
        worst = std::max(worst, fabsf(v[i+1] - v[i]));
    }
    return worst;
}

static bool all_finite(const vector<float> &v)
{
    for (float f : v) {
        if (!std::isfinite(f)) return false;
    }
    return true;
}

// Change both the time ratio and the pitch scale at the midpoint of
// the input, and verify that the output remains finite and
// continuous, that its total duration reflects the ratio history,
// and that the pitch in the second segment reflects the new scale.
static void time_and_pitch_change_midstream(RubberBandStretcher::Options options)
{
    const int rate = 44100;
    const int n = 44100;
    const float freq = 440.f;
    const double ratio1 = 1.5, ratio2 = 0.75;
    const double pitch1 = 1.0, pitch2 = 2.0;
    const int inBlock = 512;

    RubberBandStretcher stretcher(rate, 1, options, ratio1, pitch1);
    BOOST_TEST(stretcher.getTimeRatio() == ratio1);
    BOOST_TEST(stretcher.getPitchScale() == pitch1);

    vector<vector<float>> in { sinusoid(n, rate, freq, 0.5f) };

    int switchAt = -1;
    auto hook = [&](int inOffset) {
        // Change both ratios together, once, as the midpoint of the
        // input is reached
        if (switchAt < 0 && inOffset + inBlock > n / 2) {
            stretcher.setTimeRatio(ratio2);
            stretcher.setPitchScale(pitch2);
            switchAt = inOffset;
        }
    };

    vector<vector<float>> out = run_realtime(stretcher, in, inBlock, 0, hook);

    BOOST_TEST(switchAt > 0);

    // The new values must be reported back immediately
    BOOST_TEST(stretcher.getTimeRatio() == ratio2);
    BOOST_TEST(stretcher.getPitchScale() == pitch2);

    const vector<float> &o = out[0];
    int nOut = int(o.size());

    // Time semantics: the total duration must reflect the ratio
    // history. Real-time mode guarantees no exact figure (input
    // already buffered when the ratios change is processed with the
    // new ratios, and the engines flush differing tails at the end),
    // so bracket the result empirically: the same input run entirely
    // at the slower ratio must produce more output, and entirely at
    // the faster ratio less output, than the mid-stream change
    {
        RubberBandStretcher slower(rate, 1, options, ratio1, pitch1);
        RubberBandStretcher faster(rate, 1, options, ratio2, pitch1);
        int nSlower = int(run_realtime(slower, in, inBlock, 0)[0].size());
        int nFaster = int(run_realtime(faster, in, inBlock, 0)[0].size());
        BOOST_TEST(nOut < nSlower);
        BOOST_TEST(nOut > nFaster);
    }

    // The output must be finite throughout and free of gross
    // discontinuities. The signal is a sinusoid of amplitude 0.5
    // whose steepest legitimate slope (at 880 Hz) is about 0.063 per
    // sample, so a jump of 0.5 indicates a click or dropout
    BOOST_TEST(all_finite(o));
    float jump = max_abs_jump(o);
    if (jump >= 0.5f) {
        cerr << "max consecutive-sample jump = " << jump << endl;
    }
    BOOST_TEST(jump < 0.5f);

    // Pitch semantics, in order: the early part of the output carries
    // the unshifted sinusoid, the late part the shifted one - i.e.
    // the pitch change took effect, in the right order, without
    // disturbing continuity
    {
        int i0 = int(nOut * 0.05), i1 = int(nOut * 0.25);
        int crossings = count_positive_crossings(o, i0, i1);
        double expected = freq * pitch1 * double(i1 - i0) / rate;
        BOOST_TEST(crossings > int(expected * 0.95));
        BOOST_TEST(crossings < int(expected * 1.05));
    }
    {
        int i0 = int(nOut * 0.85), i1 = int(nOut * 0.95);
        int crossings = count_positive_crossings(o, i0, i1);
        double expected = freq * pitch2 * double(i1 - i0) / rate;
        BOOST_TEST(crossings > int(expected * 0.95));
        BOOST_TEST(crossings < int(expected * 1.05));
    }
}

BOOST_AUTO_TEST_CASE(time_and_pitch_change_midstream_finer)
{
    time_and_pitch_change_midstream(RubberBandStretcher::OptionEngineFiner |
                                    RubberBandStretcher::OptionProcessRealTime |
                                    RubberBandStretcher::OptionPitchHighConsistency);
}

BOOST_AUTO_TEST_CASE(time_and_pitch_change_midstream_faster)
{
    time_and_pitch_change_midstream(RubberBandStretcher::OptionEngineFaster |
                                    RubberBandStretcher::OptionProcessRealTime |
                                    RubberBandStretcher::OptionPitchHighConsistency);
}

// The chunking used for retrieve() calls must not affect the output
// stream: two runs with identical process() calls but different
// retrieve() block sizes must produce bit-identical results.
static void retrieve_chunking_neutral(RubberBandStretcher::Options options)
{
    const int rate = 44100;
    const int n = 20000;
    const double timeRatio = 1.5;
    const double pitchScale = 1.5;

    vector<vector<float>> in { sinusoid(n, rate, 440.f, 0.5f) };

    RubberBandStretcher s1(rate, 1, options, timeRatio, pitchScale);
    vector<vector<float>> outAll = run_realtime(s1, in, 512, 0);

    RubberBandStretcher s2(rate, 1, options, timeRatio, pitchScale);
    vector<vector<float>> outChunks = run_realtime(s2, in, 512, 97);

    BOOST_TEST(outAll[0].size() == outChunks[0].size());
    BOOST_TEST(outAll[0] == outChunks[0]);
}

BOOST_AUTO_TEST_CASE(retrieve_chunking_neutral_finer)
{
    retrieve_chunking_neutral(RubberBandStretcher::OptionEngineFiner |
                              RubberBandStretcher::OptionProcessRealTime);
}

BOOST_AUTO_TEST_CASE(retrieve_chunking_neutral_faster)
{
    retrieve_chunking_neutral(RubberBandStretcher::OptionEngineFaster |
                              RubberBandStretcher::OptionProcessRealTime);
}

// Input and output block sizes may differ from one another and from
// any internal block size. Whatever the combination, processing must
// run to completion (available() reaches -1) and produce a duration
// consistent with the time ratio.
static void mismatched_block_sizes(RubberBandStretcher::Options options,
                                   int inBlock, int outBlock)
{
    const int rate = 44100;
    const int n = 30000;
    const double timeRatio = 1.25;
    const double pitchScale = 0.75;

    vector<vector<float>> in { sinusoid(n, rate, 330.f, 0.5f) };

    RubberBandStretcher stretcher(rate, 1, options, timeRatio, pitchScale);
    vector<vector<float>> out = run_realtime(stretcher, in, inBlock, outBlock);

    int nOut = int(out[0].size());
    double expected = n * timeRatio;

    // Real-time mode guarantees no exact output duration: the
    // compensated output must deliver at least (roughly) the
    // stretched input, but the engines may flush a tail of buffered
    // samples at the end. So check against truncation tightly and
    // against overrun loosely
    BOOST_TEST(all_finite(out[0]));
    BOOST_TEST(nOut > int(expected * 0.9));
    BOOST_TEST(nOut < int(expected * 1.3));

    // The pitch scale must survive the block-size juggling: measure
    // the frequency in the steady-state middle of the output
    int i0 = nOut * 3 / 10;
    int i1 = nOut * 7 / 10;
    int crossings = count_positive_crossings(out[0], i0, i1);
    double expectedCrossings = 330.0 * pitchScale * double(i1 - i0) / rate;
    BOOST_TEST(crossings > int(expectedCrossings * 0.95));
    BOOST_TEST(crossings < int(expectedCrossings * 1.05));
}

BOOST_AUTO_TEST_CASE(mismatched_blocks_small_in_large_out_finer)
{
    mismatched_block_sizes(RubberBandStretcher::OptionEngineFiner |
                           RubberBandStretcher::OptionProcessRealTime,
                           300, 1024);
}

BOOST_AUTO_TEST_CASE(mismatched_blocks_large_in_small_out_finer)
{
    mismatched_block_sizes(RubberBandStretcher::OptionEngineFiner |
                           RubberBandStretcher::OptionProcessRealTime,
                           1024, 100);
}

BOOST_AUTO_TEST_CASE(mismatched_blocks_small_in_large_out_faster)
{
    mismatched_block_sizes(RubberBandStretcher::OptionEngineFaster |
                           RubberBandStretcher::OptionProcessRealTime,
                           300, 1024);
}

BOOST_AUTO_TEST_CASE(mismatched_blocks_large_in_small_out_faster)
{
    mismatched_block_sizes(RubberBandStretcher::OptionEngineFaster |
                           RubberBandStretcher::OptionProcessRealTime,
                           1024, 100);
}

// End-of-stream draining: content at the very end of the input must
// appear at the end of the drained output, and available() must
// return -1 once everything has been retrieved.
static void final_drain_flushes_tail(RubberBandStretcher::Options options)
{
    const int rate = 44100;
    const int n = 20000;
    const double timeRatio = 2.0;
    const double pitchScale = 1.0;

    // Silence except for a sinusoid burst in the last 2000 samples:
    // if draining works, the output ends with the stretched burst;
    // if the tail were dropped, the end of the output would be silent
    vector<float> burst(n, 0.f);
    for (int i = n - 2000; i < n; ++i) {
        burst[i] = 0.5f * sinf(float(i) * 440.f * M_PI * 2.f / float(rate));
    }
    vector<vector<float>> in { burst };

    RubberBandStretcher stretcher(rate, 1, options, timeRatio, pitchScale);

    // Small retrieve chunks force the drain to take many iterations
    vector<vector<float>> out = run_realtime(stretcher, in, 700, 64);
    const vector<float> &o = out[0];
    int nOut = int(o.size());

    // Draining completed: run_realtime only returns once available()
    // has returned -1
    BOOST_TEST(stretcher.available() == -1);

    // Duration is consistent with the time ratio
    BOOST_TEST(nOut > int(n * timeRatio * 0.9));

    // The silent prefix is silent in the output
    BOOST_TEST(rms(o, 0, nOut / 4) < 0.01);

    // The burst has been flushed to the end of the output, not lost
    BOOST_TEST(rms(o, nOut * 3 / 4, nOut) > 0.1);
}

BOOST_AUTO_TEST_CASE(final_drain_flushes_tail_finer)
{
    final_drain_flushes_tail(RubberBandStretcher::OptionEngineFiner |
                             RubberBandStretcher::OptionProcessRealTime);
}

BOOST_AUTO_TEST_CASE(final_drain_flushes_tail_faster)
{
    final_drain_flushes_tail(RubberBandStretcher::OptionEngineFaster |
                             RubberBandStretcher::OptionProcessRealTime);
}

// Stereo channel correspondence: with a different frequency in each
// channel, each output channel must contain the shifted version of
// its own input channel - channels must not be swapped or mixed.
static void stereo_channel_identity(RubberBandStretcher::Options options)
{
    const int rate = 44100;
    const int n = 30000;
    const double timeRatio = 1.25;
    const double pitchScale = 1.5;
    const float freq0 = 440.f, freq1 = 660.f;

    vector<vector<float>> in { sinusoid(n, rate, freq0, 0.4f),
                               sinusoid(n, rate, freq1, 0.4f) };

    RubberBandStretcher stretcher(rate, 2, options, timeRatio, pitchScale);
    BOOST_TEST(stretcher.getChannelCount() == 2u);

    vector<vector<float>> out = run_realtime(stretcher, in, 512, 0);

    BOOST_TEST(out[0].size() == out[1].size());
    int nOut = int(out[0].size());
    int i0 = nOut * 3 / 10;
    int i1 = nOut * 7 / 10;

    int c0 = count_positive_crossings(out[0], i0, i1);
    int c1 = count_positive_crossings(out[1], i0, i1);

    double e0 = freq0 * pitchScale * double(i1 - i0) / rate;
    double e1 = freq1 * pitchScale * double(i1 - i0) / rate;

    BOOST_TEST(c0 > int(e0 * 0.95));
    BOOST_TEST(c0 < int(e0 * 1.05));
    BOOST_TEST(c1 > int(e1 * 0.95));
    BOOST_TEST(c1 < int(e1 * 1.05));

    // Explicit anti-swap check: channel 1 must have more cycles than
    // channel 0, as in the input
    BOOST_TEST(c1 > c0);
}

BOOST_AUTO_TEST_CASE(stereo_channel_identity_apart_finer)
{
    stereo_channel_identity(RubberBandStretcher::OptionEngineFiner |
                            RubberBandStretcher::OptionProcessRealTime |
                            RubberBandStretcher::OptionChannelsApart);
}

BOOST_AUTO_TEST_CASE(stereo_channel_identity_together_finer)
{
    stereo_channel_identity(RubberBandStretcher::OptionEngineFiner |
                            RubberBandStretcher::OptionProcessRealTime |
                            RubberBandStretcher::OptionChannelsTogether);
}

BOOST_AUTO_TEST_CASE(stereo_channel_identity_apart_faster)
{
    stereo_channel_identity(RubberBandStretcher::OptionEngineFaster |
                            RubberBandStretcher::OptionProcessRealTime |
                            RubberBandStretcher::OptionChannelsApart);
}

BOOST_AUTO_TEST_CASE(stereo_channel_identity_together_faster)
{
    stereo_channel_identity(RubberBandStretcher::OptionEngineFaster |
                            RubberBandStretcher::OptionProcessRealTime |
                            RubberBandStretcher::OptionChannelsTogether);
}

// A silent input channel must remain (essentially) silent in the
// output while the other channel is processed normally: no
// cross-channel leakage.
static void silent_channel_stays_silent(RubberBandStretcher::Options options)
{
    const int rate = 44100;
    const int n = 20000;

    vector<vector<float>> in { sinusoid(n, rate, 440.f, 0.5f),
                               vector<float>(n, 0.f) };

    RubberBandStretcher stretcher(rate, 2, options, 1.5, 2.0);
    vector<vector<float>> out = run_realtime(stretcher, in, 512, 0);

    int nOut = int(out[0].size());
    int i0 = nOut / 4, i1 = nOut * 3 / 4;

    BOOST_TEST(rms(out[0], i0, i1) > 0.2);
    BOOST_TEST(rms(out[1], i0, i1) < 0.02);
}

BOOST_AUTO_TEST_CASE(silent_channel_stays_silent_finer)
{
    silent_channel_stays_silent(RubberBandStretcher::OptionEngineFiner |
                                RubberBandStretcher::OptionProcessRealTime |
                                RubberBandStretcher::OptionChannelsApart);
}

BOOST_AUTO_TEST_CASE(silent_channel_stays_silent_faster)
{
    silent_channel_stays_silent(RubberBandStretcher::OptionEngineFaster |
                                RubberBandStretcher::OptionProcessRealTime |
                                RubberBandStretcher::OptionChannelsApart);
}

// Calls that are not meaningful in real-time mode must be rejected
// cleanly (logged and ignored) without disturbing processing:
// study(), setKeyFrameMap(), an excessive setMaxProcessSize(), and
// process() after the final chunk. The output of a stretcher
// subjected to these calls must be identical to that of an untouched
// reference stretcher.
static void illegal_calls_ignored(RubberBandStretcher::Options options)
{
    const int rate = 44100;
    const int n = 20000;
    const double timeRatio = 1.25;
    const double pitchScale = 1.5;

    vector<vector<float>> in { sinusoid(n, rate, 440.f, 0.5f) };

    RubberBandStretcher reference(rate, 1, options, timeRatio, pitchScale);
    vector<vector<float>> outRef = run_realtime(reference, in, 512, 0);

    RubberBandStretcher stretcher(rate, 1, options, timeRatio, pitchScale);

    // study() is only meaningful in offline mode
    vector<float> junk(256, 0.25f);
    const float *junkp = junk.data();
    stretcher.study(&junkp, 256, true);

    // setKeyFrameMap() cannot be used in real-time mode
    std::map<size_t, size_t> keyframes;
    keyframes[0] = 0;
    keyframes[1000] = 2000;
    stretcher.setKeyFrameMap(keyframes);

    // Requests beyond the process size limit are clamped, not fatal
    stretcher.setMaxProcessSize(stretcher.getProcessSizeLimit() + 1);

    // None of the above may change the ratios
    BOOST_TEST(stretcher.getTimeRatio() == timeRatio);
    BOOST_TEST(stretcher.getPitchScale() == pitchScale);

    vector<vector<float>> out = run_realtime(stretcher, in, 512, 0);

    BOOST_TEST(out[0].size() == outRef[0].size());
    BOOST_TEST(out[0] == outRef[0]);

    // Processing again after the final chunk is rejected and ignored
    BOOST_TEST(stretcher.available() == -1);
    stretcher.process(&junkp, 256, true);
    BOOST_TEST(stretcher.available() == -1);
}

BOOST_AUTO_TEST_CASE(illegal_calls_ignored_realtime_finer)
{
    illegal_calls_ignored(RubberBandStretcher::OptionEngineFiner |
                          RubberBandStretcher::OptionProcessRealTime);
}

BOOST_AUTO_TEST_CASE(illegal_calls_ignored_realtime_faster)
{
    illegal_calls_ignored(RubberBandStretcher::OptionEngineFaster |
                          RubberBandStretcher::OptionProcessRealTime);
}

// In offline mode the time and pitch ratios are locked as soon as
// studying (or processing) has begun: subsequent attempts to change
// them are rejected, leaving the previously set values in effect.
static void offline_ratio_lock(RubberBandStretcher::Options options)
{
    const int rate = 44100;
    const int n = 10000;

    RubberBandStretcher stretcher(rate, 1, options);

    stretcher.setTimeRatio(2.0);
    stretcher.setPitchScale(1.5);
    BOOST_TEST(stretcher.getTimeRatio() == 2.0);
    BOOST_TEST(stretcher.getPitchScale() == 1.5);

    stretcher.setMaxProcessSize(n);
    stretcher.setExpectedInputDuration(n);

    vector<float> in = sinusoid(n, rate, 440.f, 0.5f);
    const float *inp = in.data();

    stretcher.study(&inp, n, true);

    // Too late now: these calls must be rejected
    stretcher.setTimeRatio(3.0);
    stretcher.setPitchScale(0.5);
    BOOST_TEST(stretcher.getTimeRatio() == 2.0);
    BOOST_TEST(stretcher.getPitchScale() == 1.5);

    stretcher.process(&inp, n, true);

    // The output duration reflects the locked ratio of 2.0, not the
    // rejected 3.0
    BOOST_TEST(stretcher.available() == n * 2);

    vector<float> out(n * 2, 0.f);
    float *outp = out.data();
    size_t got = stretcher.retrieve(&outp, n * 2);
    BOOST_TEST(got == size_t(n) * 2);
    BOOST_TEST(stretcher.available() == -1);
}

BOOST_AUTO_TEST_CASE(offline_ratio_lock_finer)
{
    offline_ratio_lock(RubberBandStretcher::OptionEngineFiner);
}

BOOST_AUTO_TEST_CASE(offline_ratio_lock_faster)
{
    offline_ratio_lock(RubberBandStretcher::OptionEngineFaster);
}

// Formant scale is supported only by the R3 (Finer) engine. In R2 the
// setter has no effect and the getter always returns 0.0; in R3 the
// value is stored, with 0.0 meaning automatic scaling.
BOOST_AUTO_TEST_CASE(formant_scale_engine_contract)
{
    RubberBandStretcher r2(44100, 1,
                           RubberBandStretcher::OptionEngineFaster |
                           RubberBandStretcher::OptionProcessRealTime);
    r2.setFormantScale(1.5);
    BOOST_TEST(r2.getFormantScale() == 0.0);

    RubberBandStretcher r3(44100, 1,
                           RubberBandStretcher::OptionEngineFiner |
                           RubberBandStretcher::OptionProcessRealTime);
    BOOST_TEST(r3.getFormantScale() == 0.0); // automatic by default
    r3.setFormantScale(1.5);
    BOOST_TEST(r3.getFormantScale() == 1.5);
    r3.setFormantScale(0.0);
    BOOST_TEST(r3.getFormantScale() == 0.0);
}

BOOST_AUTO_TEST_SUITE_END()
