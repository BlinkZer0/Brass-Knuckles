#pragma once
// ============================================================================
//  Shared DSP building blocks. Header-only, no JUCE, no allocation.
//  Everything here has a line-for-line twin in vst/webhost/dsp-worklet.js, so
//  keep it plain: doubles, std:: maths only, no SIMD or fast-math tricks.
// ============================================================================
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace bk
{
constexpr double kPi     = 3.141592653589793;
constexpr double kTwoPi  = 6.283185307179586;
constexpr double kSqrt2  = 1.4142135623730951;
constexpr double kLn2    = 0.6931471805599453;
constexpr double kSilence = 1e-4; // -80 dB

inline double clampd (double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double dbToGain (double db) { return std::pow (10.0, db / 20.0); }
inline double midiToHz (double note) { return 440.0 * std::pow (2.0, (note - 69.0) / 12.0); }

// Deterministic RNG (identical sequence in the JS engine: Math.imul LCG)
struct Rng
{
    uint32_t s = 0x2545F491u;
    void seed (uint32_t v) { s = v; }
    uint32_t next() { s = s * 1664525u + 1013904223u; return s; }
    double uni() { return (double) (next() >> 8) * (1.0 / 16777216.0); } // [0, 1)
    double bi()  { return uni() * 2.0 - 1.0; }                            // [-1, 1)
};

// Segment curve, p in [0,1] -> [0,1].
// c > 0: fast start, slow finish ("exponential", like an RC charge/discharge)
// c = 0: linear,  c < 0: slow start, fast finish.
inline double curveShape (double p, double c)
{
    if (c > -1e-3 && c < 1e-3) return p;
    const double k = 5.0 * c;
    return (1.0 - std::exp (-k * p)) / (1.0 - std::exp (-k));
}

// Response curve for 0..1 controller values. c > 0 needs harder playing (exponent > 1).
inline double responseCurve (double x, double c)
{
    x = clampd (x, 0.0, 1.0);
    if (c > -1e-3 && c < 1e-3) return x;
    return std::pow (x, std::pow (2.0, 2.0 * c));
}

inline double polyBlep (double t, double dt)
{
    if (t < dt)     { t /= dt;           return t + t - t * t - 1.0; }
    if (t > 1 - dt) { t = (t - 1) / dt;  return t * t + t + t + 1.0; }
    return 0.0;
}

//==============================================================================
// TPT state-variable filter (Zavalishin). g/k are passed per sample so cutoff
// and resonance can be ramped smoothly; process() returns the low-pass output
// and leaves the band-pass in `bp`.
struct SVF
{
    double ic1 = 0, ic2 = 0, bp = 0;

    void reset() { ic1 = ic2 = bp = 0; }

    static double gFor (double fc, double sr)
    {
        fc = std::min (std::max (fc, 20.0), sr * 0.45);
        return std::tan (kPi * fc / sr);
    }
    static double kFor (double reso) { return 2.0 - 1.9 * reso; } // Q 0.5 .. 10

    double process (double x, double g, double k)
    {
        const double a1 = 1 / (1 + g * (g + k));
        const double a2 = g * a1;
        const double a3 = g * a2;
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        bp = v1;
        return v2;
    }
};
} // namespace bk
