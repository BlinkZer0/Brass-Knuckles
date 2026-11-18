#pragma once
// ============================================================================
//  Physical-model voice (Engine = Physical): a driven-valve waveguide.
//
//    mouth pressure pm (blowing envelope x dynamics, with turbulence)
//      -> VALVE: lips (outward: open more the harder you blow) or a reed
//         (inward: pressure closes it, a loud reed slaps shut each cycle),
//         moving at the played pitch, with a pulse shape set by lip tension /
//         reed stiffness and by the dynamics
//      -> FLOW: Bernoulli flow through the opening, solved each sample against
//         the pressure coming back from the bore (closed form, square roots only)
//      -> BORE: a delay line one period long; at the bell part of the wave is
//         reflected (losses, a low-pass "bell" that lets the highs escape, no
//         reflection of the mean flow) and comes back to the mouthpiece
//      -> radiated sound = the wave arriving at the bell (DC removed)
//
//  Why driven: a free-running lip / reed oscillator chooses its own pitch and
//  register (it overblows, drifts with pressure and lip tension). Real players
//  lock their lips onto the note; here the valve is locked to the played pitch
//  and everything else (flow, bore build-up, the resonance-shaped spectrum, the
//  pressure-dependent level and colour) comes out of the interaction. Every
//  note is in tune, glides / vibrato / bends work as on the other engines.
//
//  The valve is moved by a rotating phasor (no sin / cos per sample); the bore
//  stores float samples. Everything is +, -, *, / and sqrt, so the JS twin
//  (dsp-worklet.js) gives the same numbers.
// ============================================================================
#include "Common.h"

namespace bk
{
// Per-voice state. `buf` points into the engine's pool (allocated in prepare()).
struct PhysModelVoice
{
    float* buf = nullptr;
    int mask = 0, w = 0;
    double c = 1, s = 0;                       // valve phasor (cos, sin of the valve phase)
    double lp = 0, dcx = 0, dcy = 0;           // bell low-pass, reflection DC blocker
    double ox = 0, oy = 0;                     // output DC blocker
    double dt = 0, pm = 0, sh = 1, bell = 0.5; // ramp origins (normalised frequency, pressure, pulse shape, bell)

    void reset()
    {
        if (buf != nullptr) std::fill (buf, buf + mask + 1, 0.0f);
        w = 0; c = 1; s = 0;
        lp = dcx = dcy = ox = oy = 0;
        pm = 0;
    }
};

constexpr double kPmMinHz = 20.0;   // lowest bore (longer delays would need a bigger buffer)
constexpr double kPmGain  = 2.5;    // output scaling: about the level of the other engines

// Bore buffer size per voice for a sample rate (power of two, >= one period at kPmMinHz).
inline int physBufferSize (double sr)
{
    int size = 1;
    while (size < (int) (sr / kPmMinHz) + 8) size <<= 1;
    return size;
}

// Bell low-pass coefficient (one-pole) for a cutoff, and its phase delay at low frequencies.
inline double physBellCoef (double fc, double sr)
{
    fc = std::min (std::max (fc, 50.0), sr * 0.45);
    return 1.0 - std::exp (-kTwoPi * fc / sr);
}
} // namespace bk
