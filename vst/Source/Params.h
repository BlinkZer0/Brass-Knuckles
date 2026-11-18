#pragma once
// ============================================================================
//  Parameter table. Mirrors vst/ui/js/params.js (IDs, ranges, defaults, skew,
//  choice lists, ORDER). `BrassKnucklesTests --dump-params` prints this table
//  and tests/check-params.ps1 fails on any difference from params.js.
//
//  Stability rules (same as E-Piano):
//   - the order below is the storage order; new parameters are only APPENDED
//   - IDs never change; ParameterID version is 1
//   - parameters appended after v1.0 must default to a neutral value
// ============================================================================
#include <juce_audio_processors/juce_audio_processors.h>
#include <cmath>
#include <vector>

namespace bk
{
constexpr int kNumOscParam    = 3;
constexpr int kNumBands       = 3;   // additive layer: low / mid / high
constexpr int kNumLfoParam    = 2;
constexpr int kNumModSlotsParam = 12;
constexpr int kModSlotFields  = 5;   // On, Src, Via, Dst, Amt
constexpr int kLfoFields      = 9;   // On, Wave, Rate, Sync, Div, Mode, Trig, Phase, Amount
constexpr int kBandFields     = 5;   // Atk, Dec, Sus, Rel, Lvl

// Per-oscillator fields: param index = osc1On + osc * kOscFields + field
enum OscField
{
    oOn, oShape, oPW, oPitch, oFine, oLevel, oCut, oRes,
    // amplitude envelope (DAHDSR + curves + velocity / key scaling)
    oAmpDly, oAmpAtk, oAmpHld, oAmpDec, oAmpSus, oAmpRel, oAmpAtkCrv, oAmpDecCrv, oAmpRelCrv,
    oAmpVel, oAmpVelTime, oAmpKeyTime, oAmpKeyAmt,
    // pitch envelope (scoop + blip)
    oPEnvAmt, oPEnvTime, oPEnvCrv, oBlipAmt, oBlipTime, oPEnvVel, oPEnvKeyTime,
    // timbre envelope (brightness)
    oTEnvDly, oTEnvAtk, oTEnvDec, oTEnvSus, oTEnvRel, oTEnvCrv, oTEnvCut, oTEnvShape, oTEnvVel, oTEnvKeyTime,
    kOscFields
};

enum ParamIndex
{
    // ---- global -------------------------------------------------------------
    instrument, engineMode, ampLevel, master, tune, transpose, polyphony,
    voiceMode, glideOn, glideMode, glideTime, glideCurve, slurDip, bendRange, mpeOn, mpeBend, oversample,
    // ---- oscillators (3 x kOscFields) ----------------------------------------
    osc1On,
    oscLast = osc1On + kNumOscParam * kOscFields - 1,
    // ---- additive harmonic layer ---------------------------------------------
    addOn, addLevel, addPartials, addTilt, addOddEven, addSpecDly, addDynSpread, addCurve,
    addBand1, // band b field f = addBand1 + b * kBandFields + f
    addBandLast = addBand1 + kNumBands * kBandFields - 1,
    // ---- dynamics -----------------------------------------------------------
    dynBreath, dynExpr, dynAT, dynMPE, dynVel, dynBreathCC, dynCurve, dynRange,
    dynBright, dynBrCurve, dynBrass, dynAttack, dynSmooth,
    // ---- voice filter ---------------------------------------------------------
    fltOn, fltType, fltCutoff, fltReso, fltKey, fltDyn, fltEnvAmt, fltAtk, fltDec, fltSus, fltRel, fltVel,
    // ---- brassiness (nonlinear stage) ----------------------------------------
    brsAmt, brsType, brsTone,
    // ---- transients + noise ---------------------------------------------------
    lipAmt, lipTone, lipDecay, lipPitch, brNoise, brTone, brDyn, keyClick, chiff, tongue,
    // ---- body / bell -----------------------------------------------------------
    bodyMix, bodyShift, bodyReso,
    // ---- mutes ------------------------------------------------------------------
    muteType, muteMix, mutePlunger, muteSrc, muteCC,
    // ---- vibrato / growl / flutter ------------------------------------------------
    vibDepth, vibAmp, vibRate, vibDelay, vibFade, vibDrift, vibWheel,
    growlAmt, growlRate, growlRough, growlDyn, flutAmt, flutRate,
    // ---- articulations ---------------------------------------------------------
    artKs, artKsBase, artFallRng, artFallTime, artDoitRng, artDoitTime, artRipRng, artRipTime,
    artShakeRate, artShakeRng, artScoopRng, artScoopTime, artAuto, artAutoGap, artAutoLo, artAutoHi,
    // ---- section ---------------------------------------------------------------
    secPlayers, secDetune, secTiming, secVibDesync, secTimbre, secWidth,
    // ---- analog brass engine ------------------------------------------------------
    anaUnison, anaDetune, anaSpread, anaBlat,
    // ---- LFOs + mod matrix ---------------------------------------------------------
    lfo1On,
    lfoLast = lfo1On + kNumLfoParam * kLfoFields - 1,
    mod1On,
    modLast = mod1On + kNumModSlotsParam * kModSlotFields - 1,
    // ---- FX ------------------------------------------------------------------------
    fxDrvOn, fxDrvType, fxDrvAmt, fxDrvTone, fxDrvMix,
    eqOn, eqLoGain, eqLoFreq, eqMidGain, eqMidFreq, eqMidQ, eqHiGain, eqHiFreq,
    chOn, chRate, chDepth, chMix,
    rvOn, rvType, rvDecay, rvPre, rvDamp, rvMix,
    stWidth,
    kNumParams
};

inline int oscParam (int osc, int field)    { return osc1On + osc * kOscFields + field; }
inline int bandParam (int band, int field)  { return addBand1 + band * kBandFields + field; }
inline int lfoParam (int lfo, int field)    { return lfo1On + lfo * kLfoFields + field; }
inline int modParam (int slot, int field)   { return mod1On + slot * kModSlotFields + field; }

enum class Kind { Float, Bool, Choice, Int };
enum class Unit { None, Seconds, Hz, LfoHz, Semitones, Cents, Percent, BipolarPercent, Decibels, Degrees,
                  Octaves, Harmonic, Ms, Shape, Note, DbPerOct, Count };

struct ParamDef
{
    juce::String id;
    juce::String hostName;
    Kind kind;
    float min, max, def;
    Unit unit;
    bool logSkew;
    juce::StringArray choices; // Kind::Choice only
    bool noPreset = false;     // not stored in presets (master, UI-only settings)
};

// Choice lists: the order is the stored value; must match params.js and the engine enums.
inline juce::StringArray instrumentChoices() { return { "Trumpet", "Flugelhorn", "Trombone", "French Horn", "Tuba",
                                                        "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Synth" }; }
inline juce::StringArray engineChoices()     { return { "Modeled", "Analog" }; }
inline juce::StringArray voiceModeChoices()  { return { "Poly", "Mono", "Legato" }; }
inline juce::StringArray glideModeChoices()  { return { "Always", "Legato" }; }
inline juce::StringArray oversampleChoices() { return { "1x", "2x", "4x" }; }
inline juce::StringArray fltTypeChoices()    { return { "LP12", "LP24", "BP" }; }
inline juce::StringArray brsTypeChoices()    { return { "Brass", "Reed", "Edge" }; }
inline juce::StringArray tongueChoices()     { return { "Soft", "Normal", "Hard" }; }
inline juce::StringArray muteTypeChoices()   { return { "Open", "Straight", "Cup", "Harmon", "Plunger" }; }
inline juce::StringArray muteSrcChoices()    { return { "Knob", "Mod Wheel", "Breath", "Expression", "Aftertouch", "CC" }; }
inline juce::StringArray lfoWaveChoices()    { return { "Sine", "Triangle", "Square", "Saw Up", "Saw Down", "S&H" }; }
inline juce::StringArray lfoDivChoices()     { return { "1/1", "1/2D", "1/2", "1/2T", "1/4D", "1/4", "1/4T", "1/8D", "1/8", "1/8T",
                                                        "1/16D", "1/16", "1/16T", "1/32", "1/32T" }; }
inline juce::StringArray lfoModeChoices()    { return { "Global", "Voice" }; }
inline juce::StringArray lfoTrigChoices()    { return { "Free", "Note" }; }
inline juce::StringArray modSrcChoices()
{
    return { "None", "LFO 1", "LFO 2",
             "Osc 1 Amp Env", "Osc 2 Amp Env", "Osc 3 Amp Env",
             "Osc 1 Pitch Env", "Osc 2 Pitch Env", "Osc 3 Pitch Env",
             "Osc 1 Timbre Env", "Osc 2 Timbre Env", "Osc 3 Timbre Env",
             "Harm Low Env", "Harm Mid Env", "Harm High Env", "Filter Env",
             "Velocity", "Dynamics", "Breath", "Expression", "Aftertouch",
             "MPE Pressure", "MPE Slide", "MPE Bend", "Mod Wheel", "Pitch Bend", "Key", "Random", "Vibrato" };
}
inline juce::StringArray modDstChoices()
{
    return { "None", "Pitch (All)", "Osc 1 Pitch", "Osc 2 Pitch", "Osc 3 Pitch",
             "Osc 1 Level", "Osc 2 Level", "Osc 3 Level",
             "Osc 1 Shape", "Osc 2 Shape", "Osc 3 Shape",
             "Osc 1 PW", "Osc 2 PW", "Osc 3 PW",
             "Osc 1 Bright", "Osc 2 Bright", "Osc 3 Bright",
             "Harm Level", "Harm Tilt", "Filter Cutoff", "Filter Reso",
             "Dynamics", "Brightness", "Brassiness", "Breath Noise", "Lip Buzz",
             "Vibrato Depth", "Vibrato Rate", "Growl", "Flutter", "Amplitude", "Pan",
             "LFO 1 Rate", "LFO 2 Rate", "Plunger", "Body Shift", "FX Drive", "Chorus Mix", "Reverb Mix" };
}
inline juce::StringArray fxDrvTypeChoices()  { return { "Tube", "Tape" }; }
inline juce::StringArray rvTypeChoices()     { return { "Plate", "Hall" }; }

inline const std::vector<ParamDef>& paramDefs()
{
    static const std::vector<ParamDef> defs = []
    {
        std::vector<ParamDef> d;
        auto add = [&d] (juce::String id, juce::String name, Kind kind, float mn, float mx, float def,
                         Unit unit = Unit::None, bool log = false)
        {
            d.push_back ({ id, name, kind, mn, mx, def, unit, log, {}, false });
        };
        auto flt  = [&add] (juce::String id, juce::String name, float mn, float mx, float def, Unit u = Unit::None)
                    { add (id, name, Kind::Float, mn, mx, def, u, false); };
        auto logf = [&add] (juce::String id, juce::String name, float mn, float mx, float def, Unit u)
                    { add (id, name, Kind::Float, mn, mx, def, u, true); };
        auto intp = [&add] (juce::String id, juce::String name, float mn, float mx, float def, Unit u = Unit::Count)
                    { add (id, name, Kind::Int, mn, mx, def, u, false); };
        auto bln  = [&add] (juce::String id, juce::String name, bool def)
                    { add (id, name, Kind::Bool, 0, 1, def ? 1.0f : 0.0f); };
        auto choice = [&d] (juce::String id, juce::String name, juce::StringArray c, int def)
        {
            d.push_back ({ id, name, Kind::Choice, 0.0f, (float) (c.size() - 1), (float) def, Unit::None, false, c, false });
        };

        // ---- global
        choice ("instrument", "Instrument", instrumentChoices(), 0);
        choice ("engineMode", "Engine", engineChoices(), 0);
        flt  ("ampLevel", "Patch Level", -30, 6, 0, Unit::Decibels);
        flt  ("master", "Master", 0, 1, 0.7f, Unit::Percent);
        d.back().noPreset = true;
        intp ("tune", "Fine Tune", -100, 100, 0, Unit::Cents);
        intp ("transpose", "Transpose", -24, 24, 0, Unit::Semitones);
        intp ("polyphony", "Polyphony", 1, 64, 32);
        choice ("voiceMode", "Voice Mode", voiceModeChoices(), 0);
        bln  ("glideOn", "Glide On", false);
        choice ("glideMode", "Glide Mode", glideModeChoices(), 1);
        logf ("glideTime", "Glide Time", 0.001f, 2.0f, 0.08f, Unit::Seconds);
        flt  ("glideCurve", "Glide Curve", -1, 1, 0, Unit::BipolarPercent);
        flt  ("slurDip", "Lip Slur Dip", 0, 1, 0, Unit::Percent);
        intp ("bendRange", "Bend Range", 0, 24, 2, Unit::Semitones);
        bln  ("mpeOn", "MPE", false);
        intp ("mpeBend", "MPE Bend Range", 1, 96, 48, Unit::Semitones);
        choice ("oversample", "Oversampling", oversampleChoices(), 1);

        // ---- oscillators
        for (int n = 1; n <= kNumOscParam; ++n)
        {
            const juce::String o = "osc" + juce::String (n), on = "Osc " + juce::String (n) + " ";
            bln  (o + "On",     on + "On", n == 1);
            flt  (o + "Shape",  on + "Shape", 0, 3, 2, Unit::Shape);
            flt  (o + "PW",     on + "Pulse Width", 0.05f, 0.95f, 0.5f, Unit::Percent);
            intp (o + "Pitch",  on + "Pitch", -36, 36, 0, Unit::Semitones);
            intp (o + "Fine",   on + "Fine", -100, 100, 0, Unit::Cents);
            flt  (o + "Level",  on + "Level", 0, 1, 0.7f, Unit::Percent);
            logf (o + "Cut",    on + "Brightness", 1, 64, 12, Unit::Harmonic);
            flt  (o + "Res",    on + "Resonance", 0, 1, 0.1f, Unit::Percent);

            logf (o + "AmpDly", on + "Amp Delay",   0.001f, 4,  0.001f, Unit::Seconds);
            logf (o + "AmpAtk", on + "Amp Attack",  0.001f, 10, 0.03f,  Unit::Seconds);
            logf (o + "AmpHld", on + "Amp Hold",    0.001f, 4,  0.001f, Unit::Seconds);
            logf (o + "AmpDec", on + "Amp Decay",   0.005f, 10, 0.5f,   Unit::Seconds);
            flt  (o + "AmpSus", on + "Amp Sustain", 0, 1, 0.8f, Unit::Percent);
            logf (o + "AmpRel", on + "Amp Release", 0.005f, 10, 0.15f,  Unit::Seconds);
            flt  (o + "AmpAtkCrv", on + "Amp Attack Curve",  -1, 1, 0.3f, Unit::BipolarPercent);
            flt  (o + "AmpDecCrv", on + "Amp Decay Curve",   -1, 1, 0.5f, Unit::BipolarPercent);
            flt  (o + "AmpRelCrv", on + "Amp Release Curve", -1, 1, 0.5f, Unit::BipolarPercent);
            flt  (o + "AmpVel",     on + "Amp Vel Amount", 0, 1, 0, Unit::Percent);
            flt  (o + "AmpVelTime", on + "Amp Vel Time", -1, 1, 0, Unit::BipolarPercent);
            flt  (o + "AmpKeyTime", on + "Amp Key Time", -1, 1, 0, Unit::BipolarPercent);
            flt  (o + "AmpKeyAmt",  on + "Amp Key Amount", -1, 1, 0, Unit::BipolarPercent);

            flt  (o + "PEnvAmt",  on + "Pitch Env Amount", -1200, 1200, 0, Unit::Cents);
            logf (o + "PEnvTime", on + "Pitch Env Time", 0.001f, 2, 0.06f, Unit::Seconds);
            flt  (o + "PEnvCrv",  on + "Pitch Env Curve", -1, 1, 0.5f, Unit::BipolarPercent);
            flt  (o + "BlipAmt",  on + "Blip Amount", -200, 200, 0, Unit::Cents);
            logf (o + "BlipTime", on + "Blip Time", 0.001f, 0.5f, 0.03f, Unit::Seconds);
            flt  (o + "PEnvVel",  on + "Pitch Env Vel", -1, 1, 0, Unit::BipolarPercent);
            flt  (o + "PEnvKeyTime", on + "Pitch Env Key Time", -1, 1, 0, Unit::BipolarPercent);

            logf (o + "TEnvDly", on + "Timbre Delay",   0.001f, 4,  0.001f, Unit::Seconds);
            logf (o + "TEnvAtk", on + "Timbre Attack",  0.001f, 10, 0.05f,  Unit::Seconds);
            logf (o + "TEnvDec", on + "Timbre Decay",   0.005f, 10, 0.4f,   Unit::Seconds);
            flt  (o + "TEnvSus", on + "Timbre Sustain", 0, 1, 0.6f, Unit::Percent);
            logf (o + "TEnvRel", on + "Timbre Release", 0.005f, 10, 0.2f,   Unit::Seconds);
            flt  (o + "TEnvCrv", on + "Timbre Curve", -1, 1, 0.3f, Unit::BipolarPercent);
            flt  (o + "TEnvCut", on + "Timbre > Bright", -6, 6, 0, Unit::Octaves);
            flt  (o + "TEnvShape", on + "Timbre > Shape", -3, 3, 0, Unit::None);
            flt  (o + "TEnvVel", on + "Timbre Vel", -1, 1, 0, Unit::BipolarPercent);
            flt  (o + "TEnvKeyTime", on + "Timbre Key Time", -1, 1, 0, Unit::BipolarPercent);
        }

        // ---- additive harmonic layer
        bln  ("addOn", "Harmonics On", false);
        flt  ("addLevel", "Harmonics Level", 0, 1, 0.6f, Unit::Percent);
        intp ("addPartials", "Harmonics Partials", 4, 32, 24);
        flt  ("addTilt", "Harmonics Tilt", -12, 12, 0, Unit::DbPerOct);
        flt  ("addOddEven", "Harmonics Odd/Even", -1, 1, 0, Unit::BipolarPercent);
        flt  ("addSpecDly", "Spectral Delay", 0, 1, 0.3f, Unit::Percent);
        flt  ("addDynSpread", "Harmonics Dyn Spread", 0, 1, 0.5f, Unit::Percent);
        flt  ("addCurve", "Harmonics Env Curve", -1, 1, 0.3f, Unit::BipolarPercent);
        {
            const char* bandIds[]   { "Lo", "Mid", "Hi" };
            const char* bandNames[] { "Low", "Mid", "High" };
            const float atk[] { 0.02f, 0.04f, 0.07f }, dec[] { 0.3f, 0.3f, 0.25f },
                        sus[] { 0.9f, 0.7f, 0.5f }, rel[] { 0.2f, 0.15f, 0.1f }, lvl[] { 1.0f, 0.8f, 0.6f };
            for (int b = 0; b < kNumBands; ++b)
            {
                const juce::String p = juce::String ("add") + bandIds[b], nm = juce::String ("Harm ") + bandNames[b] + " ";
                logf (p + "Atk", nm + "Attack",  0.001f, 10, atk[b], Unit::Seconds);
                logf (p + "Dec", nm + "Decay",   0.005f, 10, dec[b], Unit::Seconds);
                flt  (p + "Sus", nm + "Sustain", 0, 1, sus[b], Unit::Percent);
                logf (p + "Rel", nm + "Release", 0.005f, 10, rel[b], Unit::Seconds);
                flt  (p + "Lvl", nm + "Level",   0, 1, lvl[b], Unit::Percent);
            }
        }

        // ---- dynamics
        flt  ("dynBreath", "Dyn Breath Weight", 0, 1, 1, Unit::Percent);
        flt  ("dynExpr", "Dyn Expression Weight", 0, 1, 0, Unit::Percent);
        flt  ("dynAT", "Dyn Aftertouch Weight", 0, 1, 0, Unit::Percent);
        flt  ("dynMPE", "Dyn MPE Pressure Weight", 0, 1, 1, Unit::Percent);
        flt  ("dynVel", "Dyn Velocity w/ Controller", 0, 1, 0.3f, Unit::Percent);
        intp ("dynBreathCC", "Breath CC", 1, 119, 2);
        flt  ("dynCurve", "Dyn Curve", -1, 1, 0, Unit::BipolarPercent);
        flt  ("dynRange", "Dyn Loudness Range", 0, 48, 24, Unit::Decibels);
        flt  ("dynBright", "Dyn Brightness", 0, 8, 3, Unit::Octaves);
        flt  ("dynBrCurve", "Dyn Brightness Curve", -1, 1, 0.4f, Unit::BipolarPercent);
        flt  ("dynBrass", "Dyn Brassiness", 0, 1, 0.5f, Unit::Percent);
        flt  ("dynAttack", "Dyn Attack Speed", 0, 1, 0.5f, Unit::Percent);
        logf ("dynSmooth", "Dyn Smoothing", 0.002f, 0.3f, 0.02f, Unit::Seconds);

        // ---- voice filter
        bln  ("fltOn", "Filter On", false);
        choice ("fltType", "Filter Type", fltTypeChoices(), 0);
        logf ("fltCutoff", "Filter Cutoff", 20, 20000, 8000, Unit::Hz);
        flt  ("fltReso", "Filter Reso", 0, 1, 0.1f, Unit::Percent);
        flt  ("fltKey", "Filter Key Track", 0, 1, 0.5f, Unit::Percent);
        flt  ("fltDyn", "Filter Dynamics", 0, 8, 0, Unit::Octaves);
        flt  ("fltEnvAmt", "Filter Env Amount", -8, 8, 0, Unit::Octaves);
        logf ("fltAtk", "Filter Attack", 0.001f, 10, 0.01f, Unit::Seconds);
        logf ("fltDec", "Filter Decay", 0.005f, 10, 0.3f, Unit::Seconds);
        flt  ("fltSus", "Filter Sustain", 0, 1, 0, Unit::Percent);
        logf ("fltRel", "Filter Release", 0.005f, 10, 0.3f, Unit::Seconds);
        flt  ("fltVel", "Filter Env Vel", 0, 1, 0.5f, Unit::Percent);

        // ---- brassiness
        flt  ("brsAmt", "Brassiness", 0, 1, 0.2f, Unit::Percent);
        choice ("brsType", "Brassiness Type", brsTypeChoices(), 0);
        flt  ("brsTone", "Brassiness Tone", 0, 1, 0.8f, Unit::Percent);

        // ---- transients + noise
        flt  ("lipAmt", "Lip Buzz", 0, 1, 0.3f, Unit::Percent);
        flt  ("lipTone", "Lip Tone", 0, 1, 0.5f, Unit::Percent);
        logf ("lipDecay", "Lip Decay", 0.002f, 0.3f, 0.03f, Unit::Seconds);
        flt  ("lipPitch", "Lip Pitched", 0, 1, 0.6f, Unit::Percent);
        flt  ("brNoise", "Breath Noise", 0, 1, 0.1f, Unit::Percent);
        flt  ("brTone", "Breath Tone", 0, 1, 0.5f, Unit::Percent);
        flt  ("brDyn", "Breath Follows Dyn", 0, 1, 0.7f, Unit::Percent);
        flt  ("keyClick", "Key Click", 0, 1, 0, Unit::Percent);
        flt  ("chiff", "Reed Chiff", 0, 1, 0, Unit::Percent);
        choice ("tongue", "Tongue", tongueChoices(), 1);

        // ---- body
        flt  ("bodyMix", "Body", 0, 1, 0.6f, Unit::Percent);
        flt  ("bodyShift", "Body Shift", -12, 12, 0, Unit::Semitones);
        flt  ("bodyReso", "Body Resonance", 0, 1, 0.5f, Unit::Percent);

        // ---- mutes
        choice ("muteType", "Mute", muteTypeChoices(), 0);
        flt  ("muteMix", "Mute Amount", 0, 1, 1, Unit::Percent);
        flt  ("mutePlunger", "Plunger", 0, 1, 0.5f, Unit::Percent);
        choice ("muteSrc", "Plunger Source", muteSrcChoices(), 1);
        intp ("muteCC", "Plunger CC", 0, 127, 4);

        // ---- vibrato / growl / flutter
        flt  ("vibDepth", "Vibrato Depth", 0, 100, 15, Unit::Cents);
        flt  ("vibAmp", "Vibrato Amp", 0, 1, 0.1f, Unit::Percent);
        flt  ("vibRate", "Vibrato Rate", 0.5f, 12, 5.5f, Unit::LfoHz);
        logf ("vibDelay", "Vibrato Delay", 0.001f, 3, 0.3f, Unit::Seconds);
        logf ("vibFade", "Vibrato Fade-in", 0.001f, 4, 0.5f, Unit::Seconds);
        flt  ("vibDrift", "Vibrato Drift", 0, 1, 0.3f, Unit::Percent);
        flt  ("vibWheel", "Vibrato on Wheel", 0, 1, 0, Unit::Percent);
        flt  ("growlAmt", "Growl", 0, 1, 0, Unit::Percent);
        flt  ("growlRate", "Growl Rate", 10, 80, 28, Unit::LfoHz);
        flt  ("growlRough", "Growl Roughness", 0, 1, 0.5f, Unit::Percent);
        flt  ("growlDyn", "Growl Follows Dyn", 0, 1, 0.3f, Unit::Percent);
        flt  ("flutAmt", "Flutter Tongue", 0, 1, 0, Unit::Percent);
        flt  ("flutRate", "Flutter Rate", 10, 35, 22, Unit::LfoHz);

        // ---- articulations
        bln  ("artKs", "Keyswitches", true);
        intp ("artKsBase", "Keyswitch Base", 0, 108, 12, Unit::Note);
        flt  ("artFallRng", "Fall Range", 1, 24, 7, Unit::Semitones);
        logf ("artFallTime", "Fall Time", 0.05f, 2, 0.35f, Unit::Seconds);
        flt  ("artDoitRng", "Doit Range", 1, 24, 5, Unit::Semitones);
        logf ("artDoitTime", "Doit Time", 0.05f, 2, 0.3f, Unit::Seconds);
        flt  ("artRipRng", "Rip Range", 1, 24, 7, Unit::Semitones);
        logf ("artRipTime", "Rip Time", 0.02f, 1, 0.12f, Unit::Seconds);
        flt  ("artShakeRate", "Shake Rate", 3, 12, 6.5f, Unit::LfoHz);
        flt  ("artShakeRng", "Shake Range", 1, 5, 3, Unit::Semitones);
        flt  ("artScoopRng", "Scoop Range", 0, 12, 3, Unit::Semitones);
        logf ("artScoopTime", "Scoop Time", 0.02f, 1, 0.12f, Unit::Seconds);
        bln  ("artAuto", "Auto Articulations", false);
        logf ("artAutoGap", "Auto Gap", 0.05f, 2, 0.4f, Unit::Seconds);
        intp ("artAutoLo", "Auto Soft Velocity", 1, 127, 45);
        intp ("artAutoHi", "Auto Hard Velocity", 1, 127, 115);

        // ---- section
        intp ("secPlayers", "Section Players", 1, 8, 1);
        flt  ("secDetune", "Section Detune", 0, 40, 8, Unit::Cents);
        flt  ("secTiming", "Section Timing", 0, 80, 20, Unit::Ms);
        flt  ("secVibDesync", "Section Vib Desync", 0, 1, 0.7f, Unit::Percent);
        flt  ("secTimbre", "Section Timbre Spread", 0, 1, 0.3f, Unit::Percent);
        flt  ("secWidth", "Section Width", 0, 1, 0.7f, Unit::Percent);

        // ---- analog brass
        intp ("anaUnison", "Analog Unison", 1, 7, 1);
        flt  ("anaDetune", "Analog Detune", 0, 50, 12, Unit::Cents);
        flt  ("anaSpread", "Analog Spread", 0, 1, 0.5f, Unit::Percent);
        flt  ("anaBlat", "Analog Blat", 0, 1, 0.3f, Unit::Percent);

        // ---- LFOs
        for (int n = 1; n <= kNumLfoParam; ++n)
        {
            const juce::String l = "lfo" + juce::String (n), ln = "LFO " + juce::String (n) + " ";
            bln  (l + "On", ln + "On", false);
            choice (l + "Wave", ln + "Wave", lfoWaveChoices(), 0);
            logf (l + "Rate", ln + "Rate", 0.02f, 40, 2, Unit::LfoHz);
            bln  (l + "Sync", ln + "Sync", false);
            choice (l + "Div", ln + "Division", lfoDivChoices(), 8);
            choice (l + "Mode", ln + "Mode", lfoModeChoices(), 0);
            choice (l + "Trig", ln + "Trigger", lfoTrigChoices(), 0);
            flt  (l + "Phase", ln + "Phase", 0, 360, 0, Unit::Degrees);
            flt  (l + "Amount", ln + "Amount", 0, 1, 1, Unit::Percent);
        }
        // ---- mod matrix
        for (int n = 1; n <= kNumModSlotsParam; ++n)
        {
            const juce::String m = "mod" + juce::String (n), mn = "Mod " + juce::String (n) + " ";
            bln  (m + "On", mn + "On", true);
            choice (m + "Src", mn + "Source", modSrcChoices(), 0);
            choice (m + "Via", mn + "Via", modSrcChoices(), 0);
            choice (m + "Dst", mn + "Destination", modDstChoices(), 0);
            flt  (m + "Amt", mn + "Amount", -1, 1, 0, Unit::BipolarPercent);
        }

        // ---- FX
        bln  ("fxDrvOn", "Drive On", false);
        choice ("fxDrvType", "Drive Type", fxDrvTypeChoices(), 0);
        flt  ("fxDrvAmt", "Drive", 0, 1, 0.3f, Unit::Percent);
        flt  ("fxDrvTone", "Drive Tone", 0, 1, 0.7f, Unit::Percent);
        flt  ("fxDrvMix", "Drive Mix", 0, 1, 1, Unit::Percent);
        bln  ("eqOn", "EQ On", false);
        flt  ("eqLoGain", "EQ Low", -15, 15, 0, Unit::Decibels);
        logf ("eqLoFreq", "EQ Low Freq", 40, 800, 200, Unit::Hz);
        flt  ("eqMidGain", "EQ Mid", -15, 15, 0, Unit::Decibels);
        logf ("eqMidFreq", "EQ Mid Freq", 200, 8000, 1200, Unit::Hz);
        logf ("eqMidQ", "EQ Mid Q", 0.3f, 6, 1, Unit::None);
        flt  ("eqHiGain", "EQ High", -15, 15, 0, Unit::Decibels);
        logf ("eqHiFreq", "EQ High Freq", 1500, 16000, 6000, Unit::Hz);
        bln  ("chOn", "Chorus On", false);
        logf ("chRate", "Chorus Rate", 0.05f, 5, 0.6f, Unit::LfoHz);
        flt  ("chDepth", "Chorus Depth", 0, 1, 0.4f, Unit::Percent);
        flt  ("chMix", "Chorus Mix", 0, 1, 0.35f, Unit::Percent);
        bln  ("rvOn", "Reverb On", false);
        choice ("rvType", "Reverb Type", rvTypeChoices(), 1);
        logf ("rvDecay", "Reverb Decay", 0.3f, 12, 1.8f, Unit::Seconds);
        flt  ("rvPre", "Reverb Pre-delay", 0, 200, 20, Unit::Ms);
        flt  ("rvDamp", "Reverb Damping", 0, 1, 0.5f, Unit::Percent);
        flt  ("rvMix", "Reverb Mix", 0, 1, 0.2f, Unit::Percent);
        flt  ("stWidth", "Stereo Width", 0, 2, 1, Unit::Percent);

        jassert ((int) d.size() == kNumParams);
        return d;
    }();
    return defs;
}

inline juce::String noteName (int n)
{
    static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    n = juce::jlimit (0, 127, n);
    return juce::String (names[n % 12]) + juce::String (n / 12 - 1);
}

inline juce::String formatValue (Unit unit, float v)
{
    auto signedNum = [] (float x, int decimals) { return (x > 0 ? "+" : "") + juce::String (x, decimals); };
    switch (unit)
    {
        case Unit::Seconds:   return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms"
                                              : juce::String (v, 2) + " s";
        case Unit::Hz:        return v >= 1000.0f ? juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz"
                                                  : juce::String (juce::roundToInt (v)) + " Hz";
        case Unit::LfoHz:     return juce::String (v, v < 1.0f ? 3 : (v < 10.0f ? 2 : 1)) + " Hz";
        case Unit::Semitones: return std::abs (v - std::round (v)) > 0.05f ? signedNum (v, 1) + " st"
                                                                            : (v > 0 ? "+" : "") + juce::String (juce::roundToInt (v)) + " st";
        case Unit::Cents:     return (v > 0 ? "+" : "") + juce::String (juce::roundToInt (v)) + " ct";
        case Unit::Percent:   return juce::String (juce::roundToInt (v * 100.0f)) + "%";
        case Unit::BipolarPercent: return (v > 0.004f ? "+" : "") + juce::String (juce::roundToInt (v * 100.0f)) + "%";
        case Unit::Decibels:  return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " dB";
        case Unit::Degrees:   return juce::String (juce::roundToInt (v)) + juce::String (juce::CharPointer_UTF8 ("\xc2\xb0"));
        case Unit::Octaves:   return (v > 0.005f ? "+" : "") + juce::String (v, 1) + " oct";
        case Unit::Harmonic:  return juce::String (juce::CharPointer_UTF8 ("\xc3\x97")) + juce::String (v, v < 10.0f ? 1 : 0);
        case Unit::Ms:        return juce::String (juce::roundToInt (v)) + " ms";
        case Unit::DbPerOct:  return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " dB/oct";
        case Unit::Count:     return juce::String (juce::roundToInt (v));
        case Unit::Note:      return noteName (juce::roundToInt (v));
        case Unit::Shape:
        {
            static const char* names[] { "Sine", "Tri", "Saw", "Pulse" };
            const int i = juce::jlimit (0, 3, (int) std::floor (v));
            const float f = v - (float) i;
            if (f < 0.01f || i == 3) return names[i];
            return juce::String (names[i]) + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xba")) + names[i + 1]
                   + " " + juce::String (juce::roundToInt (f * 100.0f)) + "%";
        }
        case Unit::None:
        default:              return juce::String (v, 2);
    }
}

// Same log mapping as paramToNorm/paramFromNorm in params.js
inline juce::NormalisableRange<float> logRange (float lo, float hi)
{
    return { lo, hi,
             [] (float start, float end, float n) { return start * std::pow (end / start, n); },
             [] (float start, float end, float v) { return std::log (v / start) / std::log (end / start); },
             [] (float start, float end, float v) { return juce::jlimit (start, end, v); } };
}

inline juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    for (const auto& d : paramDefs())
    {
        const juce::ParameterID pid { d.id, 1 };
        const auto unit = d.unit;

        switch (d.kind)
        {
            case Kind::Bool:
                layout.add (std::make_unique<juce::AudioParameterBool> (pid, d.hostName, d.def >= 0.5f));
                break;

            case Kind::Choice:
                layout.add (std::make_unique<juce::AudioParameterChoice> (pid, d.hostName, d.choices, (int) d.def));
                break;

            case Kind::Int:
                layout.add (std::make_unique<juce::AudioParameterInt> (
                    pid, d.hostName, (int) d.min, (int) d.max, (int) d.def,
                    juce::AudioParameterIntAttributes().withStringFromValueFunction (
                        [unit] (int v, int) { return formatValue (unit, (float) v); })));
                break;

            case Kind::Float:
            default:
                layout.add (std::make_unique<juce::AudioParameterFloat> (
                    pid, d.hostName,
                    d.logSkew ? logRange (d.min, d.max) : juce::NormalisableRange<float> (d.min, d.max),
                    d.def,
                    juce::AudioParameterFloatAttributes().withStringFromValueFunction (
                        [unit] (float v, int) { return formatValue (unit, v); })));
                break;
        }
    }
    return layout;
}
} // namespace bk
