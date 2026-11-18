#pragma once
// ============================================================================
//  Morphing oscillator: shape 0 = sine, 1 = triangle, 2 = saw, 3 = pulse
//  (with pulse width). Between whole numbers it crossfades the two neighbours.
//  Saw and pulse are anti-aliased with PolyBLEP.
// ============================================================================
#include "Common.h"

namespace bk
{
struct MorphOsc
{
    double phase = 0;

    void reset (double p) { phase = p; }

    static double wave (int w, double t, double dt, double pw)
    {
        switch (w)
        {
            case 1: // triangle, phase-shifted to start at 0
            {
                double t3 = t + 0.25; if (t3 >= 1) t3 -= 1;
                return 1 - 4 * std::abs (t3 - 0.5);
            }
            case 2: // saw
                return 2 * t - 1 - polyBlep (t, dt);
            case 3: // pulse, DC removed
            {
                double t2 = t + (1 - pw); if (t2 >= 1) t2 -= 1;
                return (t < pw ? 1.0 : -1.0) + polyBlep (t, dt) - polyBlep (t2, dt) - (2 * pw - 1);
            }
            default:
                return std::sin (kTwoPi * t);
        }
    }

    double next (double shape, double pw, double dt)
    {
        int i = (int) shape;
        if (i > 2) i = 2;
        if (i < 0) i = 0;
        const double f = shape - i;
        double y = wave (i, phase, dt, pw);
        if (f > 1e-6) y += (wave (i + 1, phase, dt, pw) - y) * f;
        double p = phase + dt; if (p >= 1) p -= 1;
        phase = p;
        return y;
    }
};
} // namespace bk
