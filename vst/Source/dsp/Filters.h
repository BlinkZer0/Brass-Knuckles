#pragma once
// ============================================================================
//  Biquads (RBJ cookbook), transposed direct form II. Used for the body /
//  bell formants and the mutes, which sit on the stereo bus and change
//  slowly (coefficients are recomputed once per 32-sample chunk).
// ============================================================================
#include "Common.h"

namespace bk
{
enum BiquadType { bqPeak, bqLowShelf, bqHighShelf, bqLowPass, bqHighPass, bqBandPass };

struct BiquadCoefs { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };

inline BiquadCoefs designBiquad (int type, double fc, double q, double db, double sr)
{
    fc = clampd (fc, 10.0, sr * 0.45);
    q = std::max (q, 0.05);
    const double A = std::pow (10.0, db / 40.0);
    const double w0 = kTwoPi * fc / sr;
    const double cw = std::cos (w0), sw = std::sin (w0);
    const double alpha = sw / (2 * q);
    const double sa = 2 * std::sqrt (A) * alpha;
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (type)
    {
        case bqLowShelf:
            b0 = A * ((A + 1) - (A - 1) * cw + sa);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - sa);
            a0 = (A + 1) + (A - 1) * cw + sa;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - sa;
            break;
        case bqHighShelf:
            b0 = A * ((A + 1) + (A - 1) * cw + sa);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - sa);
            a0 = (A + 1) - (A - 1) * cw + sa;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - sa;
            break;
        case bqLowPass:
            b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
            a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        case bqHighPass:
            b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
            a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        case bqBandPass:
            b0 = alpha; b1 = 0; b2 = -alpha;
            a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        default: // peak
            b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
            a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
            break;
    }
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}

struct Biquad
{
    double z1 = 0, z2 = 0;
    void reset() { z1 = z2 = 0; }
    double process (double x, const BiquadCoefs& c)
    {
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
};
} // namespace bk
