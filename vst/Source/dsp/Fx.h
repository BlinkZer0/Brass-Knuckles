#pragma once
// ============================================================================
//  FX chain on the stereo bus:  drive -> 3-band EQ -> chorus -> reverb -> width
//  Buffers are allocated in prepare() only (never on the audio thread).
//  Feedback paths flush values that decay below 1e-18 to exactly zero, so a
//  reverb tail never turns into a stream of denormals.
// ============================================================================
#include <vector>
#include "Filters.h"
#include "Oversampler.h"

namespace bk
{
// Anti-denormal: tiny values become exactly 0 (a compare, so fast-math can't optimise it away;
// identical in JS).
inline double flushTiny (double x) { return (x < 1e-30 && x > -1e-30) ? 0.0 : x; }

//==============================================================================
// Stereo chorus: two modulated delay lines (12 ms +- 6 ms x depth), LFOs 90 degrees apart.
struct Chorus
{
    std::vector<double> bufL, bufR;
    int mask = 0, w = 0;
    double phase = 0, dL = 0, dR = 0;   // delays (samples) at the start of the chunk

    void prepare (double sr)
    {
        int size = 1;
        while (size < (int) (0.05 * sr) + 4) size <<= 1;
        bufL.assign ((size_t) size, 0.0);
        bufR.assign ((size_t) size, 0.0);
        mask = size - 1;
        w = 0; phase = 0;
        dL = dR = 0.012 * sr;
    }
    void reset() { std::fill (bufL.begin(), bufL.end(), 0.0); std::fill (bufR.begin(), bufR.end(), 0.0); }

    double read (const std::vector<double>& b, double d) const
    {
        const double pos = w - d;
        const int i = (int) std::floor (pos);
        const double f = pos - i;
        const double a = b[(size_t) (i & mask)], c = b[(size_t) ((i + 1) & mask)];
        return a + (c - a) * f;
    }
};

//==============================================================================
// Reverb: 8-line feedback delay network (Hadamard mixing), input diffusion,
// pre-delay, damping in the loop. Plate: short, dense, bright line lengths.
// Hall: longer lines, more diffusion.
constexpr int kRvLines = 8;
constexpr double kPlateMs[kRvLines] = { 11.3, 13.7, 17.2, 19.9, 23.1, 27.4, 30.3, 35.7 };
constexpr double kHallMs[kRvLines]  = { 29.7, 37.1, 41.1, 43.7, 53.0, 59.9, 67.1, 73.3 };
constexpr double kDiffMs[4]         = { 2.96, 2.23, 7.9, 5.77 };

struct Reverb
{
    std::vector<double> line[kRvLines], diff[4], pre;
    int len[kRvLines] {}, pos[kRvLines] {}, dlen[4] {}, dpos[4] {};
    double lp[kRvLines] {}, g[kRvLines] {};
    int preMask = 0, preW = 0, type = -1;
    double sr = 48000;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        for (int i = 0; i < kRvLines; ++i) line[i].assign ((size_t) (std::max (kPlateMs[i], kHallMs[i]) * 0.001 * sr) + 2, 0.0);
        for (int i = 0; i < 4; ++i)
        {
            dlen[i] = std::max (1, (int) std::lround (kDiffMs[i] * 0.001 * sr));
            diff[i].assign ((size_t) dlen[i], 0.0);
            dpos[i] = 0;
        }
        int size = 1;
        while (size < (int) (0.25 * sr) + 4) size <<= 1;
        pre.assign ((size_t) size, 0.0);
        preMask = size - 1;
        preW = 0;
        type = -1;
    }

    void reset()
    {
        for (auto& l : line) std::fill (l.begin(), l.end(), 0.0);
        for (auto& d : diff) std::fill (d.begin(), d.end(), 0.0);
        std::fill (pre.begin(), pre.end(), 0.0);
        for (int i = 0; i < kRvLines; ++i) { lp[i] = 0; pos[i] = 0; }
    }

    void setType (int t)
    {
        if (t == type) return;
        type = t;
        const double* ms = t == 0 ? kPlateMs : kHallMs;
        for (int i = 0; i < kRvLines; ++i) { len[i] = (int) std::lround (ms[i] * 0.001 * sr); pos[i] = 0; lp[i] = 0; }
        for (auto& l : line) std::fill (l.begin(), l.end(), 0.0);
    }

    // feedback gain per line for a decay (T60) in seconds
    void setDecay (double t60)
    {
        for (int i = 0; i < kRvLines; ++i) g[i] = std::pow (10.0, -3.0 * len[i] / (std::max (t60, 0.05) * sr));
    }

    // One sample. in = mono input; preDelay in samples; damp = loop low-pass coefficient (0..1, 1 = open).
    void process (double in, int preDelay, double damp, double& outL, double& outR)
    {
        pre[(size_t) preW] = in;
        double x = pre[(size_t) ((preW - preDelay) & preMask)];
        preW = (preW + 1) & preMask;
        for (int d = 0; d < 4; ++d)                   // input diffusion: series all-passes
        {
            auto& b = diff[d];
            const double z = b[(size_t) dpos[d]];
            const double v = x + 0.6 * z;
            b[(size_t) dpos[d]] = flushTiny (v);
            x = z - 0.6 * v;
            dpos[d] = dpos[d] + 1 >= dlen[d] ? 0 : dpos[d] + 1;
        }
        double o[kRvLines];
        for (int i = 0; i < kRvLines; ++i)
        {
            const double y = line[i][(size_t) pos[i]];
            lp[i] = flushTiny (lp[i] + (y - lp[i]) * damp);
            o[i] = lp[i] * g[i];
        }
        // fast Walsh-Hadamard transform, normalised (energy preserving)
        for (int h = 1; h < kRvLines; h <<= 1)
            for (int i = 0; i < kRvLines; i += h << 1)
                for (int j = i; j < i + h; ++j) { const double a = o[j], b = o[j + h]; o[j] = a + b; o[j + h] = a - b; }
        const double norm = 0.35355339059327373;     // 1 / sqrt(8)
        double l = 0, r = 0;
        for (int i = 0; i < kRvLines; ++i)
        {
            const double y = line[i][(size_t) pos[i]];
            if (i & 1) r += y; else l += y;
            line[i][(size_t) pos[i]] = flushTiny (o[i] * norm + ((i & 2) ? -x : x));
            pos[i] = pos[i] + 1 >= len[i] ? 0 : pos[i] + 1;
        }
        outL = l * 0.5; outR = r * 0.5;
    }
};
} // namespace bk
