#pragma once
// ============================================================================
//  Multi-stage envelope: Delay, Attack, Hold, Decay, Sustain, Release, with a
//  curve per segment. Evaluated at control rate (once per 32-sample chunk) by
//  advancing it by the chunk's duration; the voice ramps the result per sample.
//
//  - gateOn() restarts from the current level (no click on retrigger)
//  - release starts from wherever the envelope is, and always lands on exactly
//    zero after the release time, so voices end deterministically
// ============================================================================
#include "Common.h"

namespace bk
{
struct EnvTimes
{
    double dly = 0, atk = 0.01, hld = 0, dec = 0.3, sus = 1, rel = 0.2;
    double atkC = 0, decC = 0, relC = 0;  // curveShape() amounts per segment
};

// Delay / hold at their minimum (1 ms) count as "off".
constexpr double kEnvMinStage = 0.0011;

struct Envelope
{
    enum Stage { Idle = 0, Delay, Attack, Hold, Decay, Sustain, Release };
    int stage = Idle;
    double pos = 0;    // 0..1 within the current segment
    double level = 0;
    double from = 0;   // level at the start of attack / release

    bool active() const { return stage != Idle; }
    void gateOn()  { stage = Delay; pos = 0; from = level; }
    void gateOff()
    {
        if (stage == Idle || stage == Release) return;
        stage = Release; pos = 0; from = level;
    }
    void kill() { stage = Idle; level = 0; pos = 0; from = 0; }

    // Advance by dt seconds; returns the level at the end of that time.
    double advance (const EnvTimes& e, double dt)
    {
        for (;;)
        {
            switch (stage)
            {
                case Delay:
                    if (! step (e.dly > kEnvMinStage ? e.dly : 0.0, dt)) { level = from; return level; }
                    stage = Attack; pos = 0;
                    break;
                case Attack:
                    if (! step (e.atk, dt)) { level = from + (1.0 - from) * curveShape (pos, e.atkC); return level; }
                    level = 1; stage = Hold; pos = 0;
                    break;
                case Hold:
                    if (! step (e.hld > kEnvMinStage ? e.hld : 0.0, dt)) { level = 1; return level; }
                    stage = Decay; pos = 0;
                    break;
                case Decay:
                    if (! step (e.dec, dt)) { level = 1.0 + (e.sus - 1.0) * curveShape (pos, e.decC); return level; }
                    level = e.sus;
                    if (e.sus < kSilence) { kill(); return 0; }
                    stage = Sustain; pos = 0;
                    break;
                case Sustain:
                    level = e.sus;
                    if (e.sus < kSilence) kill();
                    return level;
                case Release:
                    if (! step (e.rel, dt)) { level = from * (1.0 - curveShape (pos, e.relC)); return level; }
                    kill();
                    return 0;
                default:
                    return level;
            }
        }
    }

private:
    // Consumes time from dt for a segment of length T. True when the segment finished.
    bool step (double T, double& dt)
    {
        if (T <= 0) { pos = 1; return true; }
        const double rem = (1.0 - pos) * T;
        if (dt < rem) { pos += dt / T; dt = 0; return false; }
        dt -= rem;
        pos = 1;
        return true;
    }
};
} // namespace bk
