#pragma once
// ============================================================================
//  2x / 4x oversampling for the per-voice nonlinear stage, with linear-phase
//  half-band FIR filters (Kaiser-windowed sinc, designed once at start-up from
//  a formula, so the JS engine computes the same coefficients).
//
//  Stage A (base <-> 2x): 55 taps, about -75 dB alias rejection, flat to ~19 kHz at 48 kHz.
//  Stage B (2x <-> 4x):   15 taps (the band to protect is much narrower there).
//
//  The waveshapers are rational functions (no tanh / exp per sample): cheap,
//  smooth, and bit-identical between C++ and JS.
// ============================================================================
#include "Common.h"

namespace bk
{
// Zeroth-order modified Bessel function (Kaiser window).
inline double besselI0 (double x)
{
    double sum = 1, term = 1;
    for (int k = 1; k < 30; ++k)
    {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
    }
    return sum;
}

// Half-band filter of length 4M-1: centre tap 0.5, odd offsets g[i] (offset 2i+1), even offsets 0.
struct HalfbandDesign
{
    int M = 0;
    double g[16] {};
    HalfbandDesign (int m, double beta) : M (m)
    {
        const double half = 2 * M;           // centre index + 1
        double sum = 0;
        for (int i = 0; i < M; ++i)
        {
            const double d = 2 * i + 1;
            const double ideal = std::sin (kPi * d / 2) / (kPi * d);
            const double r = d / half;
            const double w = besselI0 (beta * std::sqrt (std::max (0.0, 1 - r * r))) / besselI0 (beta);
            g[i] = ideal * w;
            sum += g[i];
        }
        for (int i = 0; i < M; ++i) g[i] *= 0.25 / sum;   // DC gain exactly 1: 0.5 + 2*sum(g) = 1
    }
};

inline const HalfbandDesign& halfbandA() { static const HalfbandDesign d (14, 7.0); return d; }
inline const HalfbandDesign& halfbandB() { static const HalfbandDesign d (4, 6.0); return d; }

constexpr int kHbMaxHist = 64; // >= 4 * max M

// Upsampler: one input -> two outputs (polyphase; gain 2 restores the zero-stuffed level).
struct HalfbandUp
{
    double h[2 * kHbMaxHist] {};
    int p = 0;
    void reset() { std::fill (std::begin (h), std::end (h), 0.0); p = 0; }

    void process (double x, const HalfbandDesign& d, double& out0, double& out1)
    {
        const int N = 2 * d.M;
        p = p + 1 >= N ? 0 : p + 1;
        h[p] = x; h[p + N] = x;
        const double* base = h + p + N;   // base[-j] = x[n - j]
        double acc = 0;
        for (int i = 0; i < d.M; ++i)
            acc += d.g[i] * (base[-(d.M - 1 - i)] + base[-(d.M + i)]);
        out0 = 2 * acc;
        out1 = base[-(d.M - 1)];
    }
};

// Downsampler: two inputs -> one output.
struct HalfbandDown
{
    double h[2 * kHbMaxHist] {};
    int p = 0;
    void reset() { std::fill (std::begin (h), std::end (h), 0.0); p = 0; }

    void push (double v, int N)
    {
        p = p + 1 >= N ? 0 : p + 1;
        h[p] = v; h[p + N] = v;
    }

    double process (double v0, double v1, const HalfbandDesign& d)
    {
        const int N = 4 * d.M;
        push (v0, N);
        push (v1, N);
        const double* base = h + p + N;   // base[-j] = v[2n - j]
        double acc = 0.5 * base[-(2 * d.M - 1)];
        for (int i = 0; i < d.M; ++i)
            acc += d.g[i] * (base[-(2 * d.M - 2 - 2 * i)] + base[-(2 * d.M + 2 * i)]);
        return acc;
    }
};

//==============================================================================
enum ShaperType { shapeBrass, shapeReed, shapeEdge };

// Padé tanh: exact 1 at |x| = 3, monotonic, smooth.
inline double tanhRational (double x)
{
    x = clampd (x, -3.0, 3.0);
    const double x2 = x * x;
    return x * (27 + x2) / (27 + 9 * x2);
}

inline double shapeFn (int type, double x)
{
    switch (type)
    {
        case shapeReed:   // softer knee (x / (1 + |x|)) with a little asymmetry -> even harmonics
        {
            const double t = x / (1 + std::abs (x));
            return t + 0.12 * t * t;
        }
        case shapeEdge:   // harder knee: cubic soft clip
        {
            const double c = clampd (x, -1.0, 1.0);
            return 1.5 * c - 0.5 * c * c * c;
        }
        default:          // brass: tanh with asymmetry (the steepening, "brassy" edge)
        {
            const double t = tanhRational (x);
            return t + 0.18 * t * t;
        }
    }
}

// Runs shapeFn(type, x) at 1x, 2x or 4x.
struct Oversampler
{
    HalfbandUp upA, upB;
    HalfbandDown downA, downB;
    void reset() { upA.reset(); upB.reset(); downA.reset(); downB.reset(); }

    double process (double x, int factor, int type)
    {
        if (factor <= 1) return shapeFn (type, x);
        const auto& A = halfbandA();
        double a0, a1;
        upA.process (x, A, a0, a1);
        if (factor == 2)
            return downA.process (shapeFn (type, a0), shapeFn (type, a1), A);
        const auto& B = halfbandB();
        double b0, b1, b2, b3;
        upB.process (a0, B, b0, b1);
        upB.process (a1, B, b2, b3);
        const double c0 = downB.process (shapeFn (type, b0), shapeFn (type, b1), B);
        const double c1 = downB.process (shapeFn (type, b2), shapeFn (type, b3), B);
        return downA.process (c0, c1, A);
    }
};
} // namespace bk
