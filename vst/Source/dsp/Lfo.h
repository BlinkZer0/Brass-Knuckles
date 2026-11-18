#pragma once
// ============================================================================
//  LFOs (same design as E-Piano): sine, triangle, square, saw up / down, S&H;
//  free rate or tempo-synced divisions; global (one phase for all voices) or
//  per voice; free-running or retriggered by notes. Square and S&H steps get
//  ~2 ms of smoothing so they don't click. Evaluated once per 32-sample chunk.
// ============================================================================
#include "Common.h"

namespace bk
{
enum LfoWave { lfoSine, lfoTriangle, lfoSquare, lfoSawUp, lfoSawDown, lfoSampleHold };

// Synced divisions, in quarter-note beats (order = the "Division" choice list)
constexpr double kLfoDivBeats[] = { 4.0, 3.0, 2.0, 4.0 / 3.0, 1.5, 1.0, 2.0 / 3.0, 0.75, 0.5, 1.0 / 3.0,
                                    0.375, 0.25, 1.0 / 6.0, 0.125, 1.0 / 12.0 };
constexpr int kNumLfoDivs = 15;

struct Lfo
{
    double phase = 0, held = 0, out = 0;
    bool primed = false;

    void start (double p, Rng& rng)     { phase = p; held = rng.bi(); primed = false; } // fresh voice
    void retrigger (double p, Rng& rng) { phase = p; held = rng.bi(); }                 // keeps its smoothing

    double shape (int wave, double offset) const
    {
        double t = phase + offset;
        t -= std::floor (t);
        switch (wave)
        {
            case lfoTriangle:   { double t3 = t + 0.25; if (t3 >= 1) t3 -= 1; return 1 - 4 * std::abs (t3 - 0.5); }
            case lfoSquare:     return t < 0.5 ? 1.0 : -1.0;
            case lfoSawUp:      return 2 * t - 1;
            case lfoSawDown:    return 1 - 2 * t;
            case lfoSampleHold: return held;
            default:            return std::sin (kTwoPi * t);
        }
    }

    void advance (double inc, Rng& rng)
    {
        phase += inc;
        if (phase >= 1) { phase -= std::floor (phase); held = rng.bi(); }
    }

    double smooth (double target, double coef)
    {
        if (! primed) { out = target; primed = true; }
        else out += (target - out) * coef;
        return out;
    }
};
} // namespace bk
