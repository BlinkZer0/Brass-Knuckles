#pragma once
// ============================================================================
//  Physical-model voice: INTERFACE STUB (phase 7).
//
//  The plan is a lip-reed (brass) / single-reed (sax) waveguide:
//    mouth pressure (from dynamics) -> nonlinear valve (lip: outward-striking
//    mass-spring; reed: pressure-controlled stiffness) -> bore delay line with
//    a bell / tone-hole reflection filter -> radiation filter -> output
//  It will become a third "Engine" choice (appended to the parameter's choice
//  list, so existing projects and presets are unaffected) and plug into the
//  same voice framework: dynamics, envelopes for blowing pressure, vibrato,
//  articulations, section players, body formants, mutes and FX all apply.
//
//  Nothing in v1.0 calls this. It exists so the voice code has a fixed shape
//  to aim at and the web build has a matching file to port later.
// ============================================================================
#include "Common.h"

namespace bk
{
struct PhysModelSettings
{
    int instrument = 0;        // same models as Instruments.h
    double boreLength = 1;     // relative (pitch comes from the note)
    double lipMass = 1;        // or reed stiffness for saxes
    double damping = 0.5;
    double bellBrightness = 0.5;
};

class PhysModelVoice
{
public:
    void prepare (double sampleRate) { sr = sampleRate; }
    void configure (const PhysModelSettings& s) { settings = s; }
    void start (double /*hz*/, double /*pressure*/) {}
    void setPressure (double /*pressure*/) {}   // from dynamics, every chunk
    void setFrequency (double /*hz*/) {}        // glide, vibrato, articulations
    void release() {}
    bool active() const { return false; }

    // Adds n samples of output to out. The stub is silent.
    void render (double* /*out*/, int /*n*/) {}

private:
    double sr = 48000;
    PhysModelSettings settings;
};
} // namespace bk
