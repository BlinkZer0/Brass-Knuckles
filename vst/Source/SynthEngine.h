#pragma once
// ============================================================================
//  Brass Knuckles DSP engine. Its JavaScript twin is vst/webhost/dsp-worklet.js
//  (checked by tests/run-parity.ps1); keep the two line-for-line equivalent.
//
//  Per voice (Modeled engine):
//    OSC 1-3 (morph sine/tri/saw/pulse) -> per-osc brightness SVF
//      each with its own Amp env (DAHDSR), Pitch env (scoop + blip), Timbre env
//    + additive harmonic layer (32 harmonics, 9 envelope nodes, spectral delay,
//      instrument spectrum whose slope follows the dynamics)
//    + lip buzz / reed onset transient
//    -> optional voice filter (LP12/LP24/BP + filter env)
//    -> brassiness waveshaper (drive grows with dynamics), 1x/2x/4x oversampled
//    + breath noise (instrument noise band, follows dynamics; reed chiff) + key click
//    -> x loudness x vibrato amp x growl x flutter x slur dip -> pan
//  Bus (stereo): body / bell formants -> mute (straight, cup, harmon, plunger wah)
//    -> patch level x master (smoothed) -> soft clip
//
//  Dynamics: one value per voice, from velocity and/or continuous controllers
//  (breath CC, expression, aftertouch, per-note pressure), updated every chunk.
//  Drives loudness, brightness (osc filters + harmonic slope + brassiness drive)
//  and attack speed (at note-on).
//
//  Control rate: every kChunk (32) samples envelopes, dynamics, vibrato, growl
//  and pitch are evaluated; per-sample values are ramped linearly in between.
//
//  Voices are allocated in note GROUPS (one group = one played note). A group
//  has 1 player now; section mode (phase 3) adds up to 8 players per group.
//
//  No allocation, no locks, no JUCE dependency.
// ============================================================================
#include "dsp/Common.h"
#include "dsp/Envelope.h"
#include "dsp/Oscillator.h"
#include "dsp/Filters.h"
#include "dsp/Oversampler.h"
#include "dsp/Instruments.h"
#include "dsp/Additive.h"
#include "dsp/PhysModel.h"   // phase 7: interface stub, not used yet
#include "dsp/Lfo.h"
#include "dsp/Fx.h"

namespace bk
{
constexpr int    kNumOsc      = 3;
constexpr int    kMaxPoly     = 64;
constexpr int    kMaxPlayers  = 8;
constexpr int    kPoolSize    = 288;   // 32 notes x 8 players + room for fading voices
constexpr int    kChunk       = 32;
constexpr double kVoiceGain   = 0.45;  // per-voice headroom (presets level-match with ampLevel)
constexpr double kKillTime    = 0.006; // fade-out of a stolen voice (s)
constexpr int    kNumChannels = 16;
constexpr double kLn10Over20  = 0.11512925464970229;

enum VoiceMode  { modePoly, modeMono, modeLegato };
enum FilterType { fltLP12, fltLP24, fltBP };
enum Tongue     { tongueSoft, tongueNormal, tongueHard };
enum PlungerSrc { plKnob, plWheel, plBreath, plExpression, plAftertouch, plCC };
enum EngineMode { engModeled, engAnalog };
// Keyswitch order (artKsBase + index). Fall / doit act on the release, rip / scoop on the
// attack, shake on the sustain.
enum Articulation { artNormal, artFall, artDoit, artRip, artShake, artScoop, kNumArticulations };

// Mod matrix (order = the choice lists in Params.h / params.js)
constexpr int kNumLfo = 2;
constexpr int kNumModSlots = 12;
enum ModSource
{
    srcNone, srcLfo1, srcLfo2, srcAmpEnv1, srcAmpEnv2, srcAmpEnv3, srcPitchEnv1, srcPitchEnv2, srcPitchEnv3,
    srcTmbEnv1, srcTmbEnv2, srcTmbEnv3, srcHarmLo, srcHarmMid, srcHarmHi, srcFltEnv, srcVelocity, srcDynamics,
    srcBreath, srcExpression, srcAftertouch, srcMpePressure, srcMpeSlide, srcMpeBend, srcModWheel, srcPitchBend,
    srcKey, srcRandom, srcVibrato, kNumModSources
};
enum ModDest
{
    dstNone, dstPitchAll, dstOsc1Pitch, dstOsc2Pitch, dstOsc3Pitch, dstOsc1Level, dstOsc2Level, dstOsc3Level,
    dstOsc1Shape, dstOsc2Shape, dstOsc3Shape, dstOsc1PW, dstOsc2PW, dstOsc3PW, dstOsc1Bright, dstOsc2Bright, dstOsc3Bright,
    dstHarmLevel, dstHarmTilt, dstFltCutoff, dstFltReso, dstDynamics, dstBrightness, dstBrassiness, dstBreathNoise,
    dstLipBuzz, dstVibDepth, dstVibRate, dstGrowl, dstFlutter, dstAmp, dstPan, dstLfo1Rate, dstLfo2Rate,
    // global destinations (the bus / FX): driven only by global sources (global LFOs, controllers)
    dstPlunger, dstBodyShift, dstFxDrive, dstChorusMix, dstReverbMix, kNumModDests
};
constexpr int kFirstGlobalDest = dstPlunger;

inline bool isBipolarSource (int s)
{
    return s == srcLfo1 || s == srcLfo2 || s == srcMpeBend || s == srcPitchBend || s == srcKey || s == srcRandom || s == srcVibrato;
}
inline bool isGlobalSource (int s)
{
    return s == srcBreath || s == srcExpression || s == srcAftertouch || s == srcModWheel || s == srcPitchBend
        || s == srcLfo1 || s == srcLfo2; // LFOs only when in Global mode (checked by the engine)
}

// Parameter snapshot, taken once per processBlock (see EngineParamsFromValues.h).
struct EngineParams
{
    struct Osc
    {
        bool on = false;
        double shape = 2, pw = 0.5, semis = 0, level = 0.7, cutHarm = 12, res = 0.1;
        EnvTimes amp;
        double ampVel = 0, ampVelTime = 0, ampKeyTime = 0, ampKeyAmt = 0;
        double pAmt = 0, pTime = 0.06, pCrv = 0.5, blipAmt = 0, blipTime = 0.03, pVel = 0, pKeyTime = 0;
        EnvTimes tmb;
        double tCut = 0, tShape = 0, tVel = 0, tKeyTime = 0;
    };
    std::array<Osc, kNumOsc> osc;

    int instrument = 0, engineMode = 0;
    double ampGain = 1, master = 0.7, tuneSemis = 0;
    int transpose = 0, polyphony = 32;
    int voiceMode = modePoly;
    bool glideOn = false, glideLegatoOnly = true;
    double glideTime = 0.08, glideCurve = 0, slurDip = 0;
    int bendRange = 2;
    bool mpeOn = false;
    int mpeBend = 48, oversample = 1;

    // additive harmonic layer
    struct Band { double atk = 0.03, dec = 0.3, sus = 0.8, rel = 0.15, lvl = 1; };
    bool addOn = false;
    double addLevel = 0.6;
    int addPartials = 24;
    double addTilt = 0, addOddEven = 0, addSpecDly = 0.3, addDynSpread = 0.5, addCurve = 0.3;
    std::array<Band, 3> band;

    // dynamics
    double wBreath = 1, wExpr = 0, wAT = 0, wMPE = 1, dynVel = 0.3;
    int breathCC = 2;
    double dynCurve = 0, dynRange = 24, dynBright = 3, dynBrCurve = 0.4, dynBrass = 0.5, dynAttack = 0.5, dynSmooth = 0.02;

    // voice filter
    bool fltOn = false;
    int fltType = fltLP12;
    double fltCutoff = 8000, fltReso = 0.1, fltKey = 0.5, fltDyn = 0, fltEnvAmt = 0, fltVel = 0.5;
    EnvTimes fenv;

    // brassiness
    double brsAmt = 0.2, brsTone = 0.8;
    int brsType = shapeBrass;

    // transients + noise
    double lipAmt = 0.3, lipTone = 0.5, lipDecay = 0.03, lipPitch = 0.6;
    double brNoise = 0.1, brTone = 0.5, brDyn = 0.7, keyClick = 0, chiff = 0;
    int tongue = tongueNormal;

    // body + mutes
    double bodyMix = 0.6, bodyShift = 0, bodyReso = 0.5;
    int muteType = muteOpen;
    double muteMix = 1, mutePlunger = 0.5;
    int muteSrc = plWheel, muteCC = 4;

    // vibrato / growl / flutter
    double vibDepth = 15, vibAmp = 0.1, vibRate = 5.5, vibDelay = 0.3, vibFade = 0.5, vibDrift = 0.3, vibWheel = 0;
    double growlAmt = 0, growlRate = 28, growlRough = 0.5, growlDyn = 0.3, flutAmt = 0, flutRate = 22;

    // articulations
    bool artKs = true, artAuto = false;
    int artKsBase = 12, autoLo = 45, autoHi = 115;
    double fallRng = 7, fallTime = 0.35, doitRng = 5, doitTime = 0.3, ripRng = 7, ripTime = 0.12;
    double shakeRate = 6.5, shakeRng = 3, scoopRng = 3, scoopTime = 0.12, autoGap = 0.4;

    // section / analog unison
    int secPlayers = 1, anaUnison = 1;
    double secDetune = 8, secTiming = 20, secVibDesync = 0.7, secTimbre = 0.3, secWidth = 0.7;
    double anaDetune = 12, anaSpread = 0.5, anaBlat = 0.3;

    // modulation
    struct LfoP
    {
        bool on = false; int wave = lfoSine; double rate = 2; bool sync = false; int div = 8;
        bool perVoice = false; bool retrig = false; double phase = 0; /* 0..1 */ double amount = 1;
    };
    struct ModSlot { bool on = true; int src = srcNone, via = srcNone, dst = dstNone; double amount = 0; };
    std::array<LfoP, kNumLfo> lfo;
    std::array<ModSlot, kNumModSlots> mod;

    // FX
    bool fxDrvOn = false, eqOn = false, chOn = false, rvOn = false;
    int fxDrvType = 0, rvType = 1;
    double fxDrvAmt = 0.3, fxDrvTone = 0.7, fxDrvMix = 1;
    double eqLoGain = 0, eqLoFreq = 200, eqMidGain = 0, eqMidFreq = 1200, eqMidQ = 1, eqHiGain = 0, eqHiFreq = 6000;
    double chRate = 0.6, chDepth = 0.4, chMix = 0.35;
    double rvDecay = 1.8, rvPre = 20, rvDamp = 0.5, rvMix = 0.2;
    double stWidth = 1;
};

//==============================================================================
struct OscVoice
{
    MorphOsc osc;
    SVF filt;
    Envelope amp, tmb;
    // note-on scaling (velocity / key tracking / dynamics attack speed / tongue)
    double ampScale = 1, ampTime = 1, ampAtkTime = 1, pScale = 1, pTime = 1, tScale = 1, tTime = 1, tAtkTime = 1;
    // values at the start of the current chunk (ramp origin)
    double dt = 0, lvl = 0, shape = 2, pw = 0.5, g = 0, k = 2;
};

struct Voice
{
    std::array<OscVoice, kNumOsc> o;
    AddVoice add;
    Envelope fenv;
    SVF f1, f2;
    Oversampler os;
    Rng nrng;                        // per-voice noise (deterministic, independent of other voices)
    SVF lipF, noiseF;
    bool on = false;
    int note = -1, channel = 0, group = 0, player = 0;
    double vel = 0, dyn = 0, dynOnset = 0, fltVelScale = 1, addAtkScale = 1;
    uint64_t age = 0;
    bool held = false, sustained = false, killing = false;
    double kill = 1;                 // fade-out gain while being stolen
    double tNote = 0, tVib = 0;      // seconds since the attack / since the last note change
    double tTrans = 0, tClick = 0;   // seconds since the last transient / key click
    double pitch = 60, glideFrom = 60, glideTo = 60, glidePos = 1;
    double slurT = 1e9, slurDur = 0.06;
    double vibPhase = 0, driftR = 0, driftRT = 0, driftD = 0, driftDT = 0;
    int driftLeft = 0;
    double growlPh = 0, growlN = 0, growlNT = 0, flutPh = 0;
    int growlLeft = 0;
    double pressure = 0;             // per-note pressure (MPE channel pressure or poly aftertouch)
    bool pressureSeen = false;
    // tongue (set at note-on)
    double tgLip = 1, tgDecay = 1, tgScoop = 1, tgBlip = 1;
    // transient peaks (set when triggered)
    double lipPeak = 0, chiffPeak = 0, clickPeak = 0;
    // ramp origins
    double gain = 0, fg = 0, fk = 2, panL = 1, panR = 1;
    double drv = 1, drvInv = 1, lipLvl = 0, lipG = 0.1, noiseLvl = 0, nG = 0.1, clickLvl = 0;
    double lipPhase = 0, dcX = 0, dcY = 0, toneZ = 0, nPrev = 0;
    bool snap = true;                // first chunk after a fresh start: no ramp from stale values
    // articulations
    int artAtk = artNormal;          // artRip / artScoop on this attack
    bool artShake = false;
    bool relPending = false;         // release deferred (fall / doit / section player timing)
    int relKind = artNormal;         // artFall / artDoit while pending, else a plain delay
    double relT = 0, relDur = 0, artSemis = 0;
    double noteStart = 0;            // engine time of the attack (s)
    // section player / unison voice (set when the group starts)
    double plDetune = 0, plDelay = 0, plRelDelay = 0, plBright = 0, plGain = 1, plVibRate = 1, plVibDelay = 1, plPan = 0;
    int plCount = 1;                 // players in this note
    bool panMod = false;             // pan was modulated last chunk
    double delayLeft = 0;            // onset delay still to wait (s)
    double slide = -1;               // MPE slide (CC74), -1 = not seen
    // modulation
    std::array<Lfo, kNumLfo> lfo;    // per-voice LFOs (used when an LFO is in Voice mode)
    double rnd = 0;                  // "Random" source: drawn per note, -1..1

    bool active() const { return on; }
};

//==============================================================================
class SynthEngine
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        masterSmooth = 1 - std::exp (-1 / (0.02 * sr));
        wheelSmooth  = 1 - std::exp (-kChunk / (0.010 * sr)); // ~10 ms
        driftSmooth  = 1 - std::exp (-kChunk / (0.30 * sr));
        busSmooth    = 1 - std::exp (-kChunk / (0.015 * sr)); // plunger, bus mixes
        artSmooth    = 1 - std::exp (-kChunk / (0.004 * sr)); // articulation pitch (rip steps)
        ctlSmooth    = 1 - std::exp (-kChunk / (0.005 * sr)); // mod amounts, FX settings
        lfoSmooth    = 1 - std::exp (-kChunk / (0.002 * sr)); // square / S&H edges
        chorus.prepare (sr);
        reverb.prepare (sr);
        for (auto& l : lfoG) l = Lfo{};
        slotAmt.fill (0);
        fxInit = true;
        rvDecaySet = -1; rvTypeSet = -1; eqKey = -1e300; widthCur = 1;
        drvWasOn = eqWasOn = chWasOn = rvWasOn = false;
        samplesIntoBlock = 0;
        now = 0;
        lastAllUp = -1e9;
        latchedArt = artNormal;
        dcR          = 1 - kTwoPi * 8.0 / sr;
        halfbandA(); halfbandB(); addTables();                 // build the static tables now, not on the audio thread
        rng.seed (0x2545F491u);
        firstParams = true;
        busKey = -1;
        resetControllers();
        panic();
    }

    void setParams (const EngineParams& p)
    {
        params = p;
        if (firstParams)
        {
            masterGain = p.master;
            ampCur = p.ampGain;
            bodyMixCur = p.bodyMix;
            muteMixCur = p.muteType == muteOpen ? 0.0 : p.muteMix;
            plungerCur = plungerTarget();
            firstParams = false;
        }
    }

    // Host transport (synced LFOs, phase 4). Without a host: 120 BPM, stopped.
    void setTransport (double bpmIn, double ppqIn, bool playingIn)
    {
        bpm = bpmIn > 0 ? bpmIn : 120.0;
        ppqAtBlock = ppqIn;
        playing = playingIn;
        samplesIntoBlock = 0;
    }

    //--------------------------------------------------------------------------
    void noteOn (int note, int vel, int ch = 0)
    {
        if (vel <= 0) { noteOff (note, ch); return; }
        ch &= 15;
        const auto& P = params;
        if (isKeyswitch (note))   // latch an articulation; keyswitches never sound
        {
            const int k = note - P.artKsBase;
            if (k < kNumArticulations) latchedArt = k;
            return;
        }
        const int heldBefore = keyCount;
        pushKey (note);
        const double velN = vel / 127.0;
        if (heldBefore == 0)   // a new phrase: note-triggered global LFOs restart
            for (int i = 0; i < kNumLfo; ++i)
                if (! P.lfo[(size_t) i].perVoice && P.lfo[(size_t) i].retrig) lfoG[(size_t) i].retrigger (0.0, rng);

        // attack articulation for the voices this note starts (keyswitch first, then auto)
        pendingAtk = artNormal;
        if (latchedArt == artRip || latchedArt == artScoop) pendingAtk = latchedArt;
        else if (P.artAuto && heldBefore == 0 && now - lastAllUp >= P.autoGap)
        {
            if (vel <= P.autoLo) pendingAtk = artScoop;
            else if (vel >= P.autoHi) pendingAtk = artRip;
        }

        if (P.voiceMode == modePoly)
        {
            int g = -1;
            for (auto& v : voices)
                if (v.on && ! v.killing && v.note == note && (! P.mpeOn || v.channel == ch)) { g = v.group; break; }
            if (g >= 0)   // same note again: retrigger its voices from where they are
            {
                for (auto& v : voices)
                    if (v.on && v.group == g) startVoice (v, note, velN, ch, g, v.player, (double) note);
            }
            else
            {
                // notes, not voices: with section players each note uses several voices
                const int limit = std::min (std::min (std::max (P.polyphony, 1), kMaxPoly), kPoolSize / playersPerNote());
                for (int groups = countGroups(); groups >= limit; --groups)
                    if (! stealOldestGroup()) break;
                const bool glide = P.glideOn && lastNote >= 0 && (! P.glideLegatoOnly || heldBefore > 0);
                newGroup (note, velN, ch, glide ? lastPitch : (double) note);
            }
        }
        else
        {
            Voice* lead = monoLead();
            const bool releasing = lead != nullptr && ! lead->held && ! lead->sustained;
            const bool overlapping = heldBefore > 0 && lead != nullptr && ! releasing;
            const bool glide = P.glideOn && lastNote >= 0 && (overlapping || ! P.glideLegatoOnly);
            if (P.voiceMode == modeLegato && overlapping)
            {
                for (auto& v : voices)
                {
                    if (! (v.on && v.group == monoGroup)) continue;
                    setVoicePitch (v, note, glide);
                    v.note = note;
                    v.held = true;
                    v.sustained = false;
                    legatoChange (v, glide);
                }
            }
            else if (lead != nullptr)
            {
                const double from = glide ? lead->pitch : (double) note;
                for (auto& v : voices)
                    if (v.on && v.group == monoGroup) startVoice (v, note, velN, ch, monoGroup, v.player, from);
            }
            else
            {
                monoGroup = newGroup (note, velN, ch, glide ? lastPitch : (double) note);
            }
        }
        lastNote = note;
        lastPitch = note;
    }

    void noteOff (int note, int ch = 0)
    {
        ch &= 15;
        const auto& P = params;
        if (isKeyswitch (note)) return;
        removeKey (note);
        if (keyCount == 0) lastAllUp = now;
        const bool mono = P.voiceMode != modePoly;
        for (auto& v : voices)   // poly voices (and poly leftovers while in mono)
        {
            if (! v.on || v.killing || ! v.held || v.note != note) continue;
            if (mono && v.group == monoGroup) continue;
            if (P.mpeOn && v.channel != ch) continue;
            if (sustainPedal) { v.held = false; v.sustained = true; }
            else releaseVoice (v);
        }
        if (! mono) return;

        Voice* lead = monoLead();
        if (lead == nullptr || ! lead->held || lead->note != note) return;
        if (keyCount > 0)
        {
            const int back = keys[(size_t) (keyCount - 1)];   // return to the last key still held
            for (auto& v : voices)
            {
                if (! (v.on && v.group == monoGroup)) continue;
                setVoicePitch (v, back, P.glideOn);
                v.note = back;
                legatoChange (v, P.glideOn);
            }
            lastNote = back;
            lastPitch = back;
        }
        else
        {
            for (auto& v : voices)
                if (v.on && v.group == monoGroup)
                {
                    if (sustainPedal) { v.held = false; v.sustained = true; }
                    else releaseVoice (v);
                }
        }
    }

    void setSustain (bool down)
    {
        sustainPedal = down;
        if (! down) for (auto& v : voices) if (v.on && v.sustained) releaseVoice (v);
    }

    // value -1..1. MPE: channel 1 (index 0) is the master channel and bends everything by
    // Bend Range; member channels bend only their own notes by MPE Bend Range.
    void setPitchBend (double v, int ch = 0)
    {
        ch &= 15;
        bendCh[(size_t) ch] = v;
        if (! params.mpeOn || ch == 0) bendGlobal = v;
    }

    // Latch an articulation from the UI (same as pressing its keyswitch).
    void setArticulation (int a) { latchedArt = std::min (std::max (a, 0), kNumArticulations - 1); }
    int articulation() const { return latchedArt; }
    void setModWheel (double v)               { wheelTarget = clampd (v, 0.0, 1.0); }

    // Any CC, value 0..1. Handles mod wheel, breath (configurable CC), expression and sustain.
    void setController (int cc, double value, int ch = 0)
    {
        ch &= 15;
        if (cc < 0 || cc > 127) return;
        value = clampd (value, 0.0, 1.0);
        if (params.mpeOn && cc == 74 && ch != 0)   // MPE slide (per note)
        {
            for (auto& v : voices) if (v.on && v.channel == ch) v.slide = value;
            return;
        }
        ccValue[(size_t) cc] = value;
        if (cc == 1)  setModWheel (value);
        if (cc == params.breathCC) { breath = value; seenBreath = true; }
        if (cc == 11) { expression = value; seenExpr = true; }
        if (cc == 64) setSustain (value >= 0.5);
        if (cc == 121) resetControllers();
    }

    // Back to velocity-only dynamics until the breath controller sends again.
    void forgetBreath() { breath = 0; seenBreath = false; }

    // Channel aftertouch. In MPE mode it is the pressure of the notes on that channel.
    void setChannelPressure (double value, int ch = 0)
    {
        value = clampd (value, 0.0, 1.0);
        if (params.mpeOn)
        {
            for (auto& v : voices)
                if (v.on && v.channel == (ch & 15)) { v.pressure = value; v.pressureSeen = true; }
            return;
        }
        aftertouch = value;
        seenAT = true;
    }

    // Polyphonic aftertouch: per-note pressure.
    void setPolyPressure (int note, double value, int ch = 0)
    {
        (void) ch;
        value = clampd (value, 0.0, 1.0);
        for (auto& v : voices)
            if (v.on && v.note == note) { v.pressure = value; v.pressureSeen = true; }
    }

    void allNotesOff()
    {
        sustainPedal = false;
        keyCount = 0;
        for (auto& v : voices) if (v.on) releaseVoice (v);
    }

    void panic()
    {
        sustainPedal = false;
        keyCount = 0;
        for (auto& v : voices) hardStop (v);
        for (int b = 0; b < kBodyBands; ++b) { bodyL[(size_t) b].reset(); bodyR[(size_t) b].reset(); muteL[(size_t) b].reset(); muteR[(size_t) b].reset(); }
        plL.reset(); plR.reset();
        chorus.reset(); reverb.reset();                       // PANIC means silence, tails included
        for (int b = 0; b < 3; ++b) { eqL[(size_t) b].reset(); eqR[(size_t) b].reset(); }
        fxOsL.reset(); fxOsR.reset();
    }

    //--------------------------------------------------------------------------
    // Writes (does not add) numSamples into L (and R if non-null; mono sums).
    void render (float* L, float* R, int numSamples)
    {
        int pos = 0;
        while (pos < numSamples)
        {
            const int n = std::min (kChunk, numSamples - pos);
            now += n / sr;
            ++chunkStamp;
            std::fill (mixL.begin(), mixL.begin() + n, 0.0);
            std::fill (mixR.begin(), mixR.begin() + n, 0.0);
            updateControl (n);

            int count = 0;
            for (auto& v : voices)
                if (v.on)
                {
                    renderVoice (v, n);
                    if (v.player == 0 && ! v.killing) ++count;
                }
            active = count;

            processBus (n);
            processFx (n);   // patch level -> drive -> EQ -> chorus -> reverb -> width

            const double target = params.master;
            double g = masterGain;
            for (int i = 0; i < n; ++i)
            {
                g += (target - g) * masterSmooth;
                if (R != nullptr)
                {
                    const float yl = (float) safetyClip (mixL[(size_t) i] * g);
                    const float yr = (float) safetyClip (mixR[(size_t) i] * g);
                    L[pos + i] = yl;
                    R[pos + i] = yr;
                    peak = std::max (peak, std::max (std::abs (yl), std::abs (yr)));
                }
                else
                {
                    const float y = (float) safetyClip (0.5 * (mixL[(size_t) i] + mixR[(size_t) i]) * g);
                    L[pos + i] = y;
                    peak = std::max (peak, std::abs (y));
                }
            }
            masterGain = g;
            pos += n;
            samplesIntoBlock += n;
        }
    }

    int activeVoices() const { return active; }
    bool sustainDown() const { return sustainPedal; }
    double modWheel() const { return wheelTarget; }
    double tempo() const { return bpm; }
    double breathValue() const { return seenBreath ? breath : -1.0; }      // -1 = no breath controller seen
    double expressionValue() const { return seenExpr ? expression : -1.0; }
    double aftertouchValue() const { return seenAT ? aftertouch : -1.0; }
    double lastDynamics() const { return lastDyn; }
    double plungerPosition() const { return plungerCur; }
    float takePeak() { const float p = peak; peak = 0; return p; }

private:
    // Output safety clipper: exactly linear up to 0.8 (-1.9 dBFS), then a tanh knee
    // that never exceeds 1. Below the knee it adds no distortion (and no aliasing).
    static double safetyClip (double x)
    {
        const double a = std::abs (x);
        if (a <= 0.8) return x;
        const double y = 0.8 + 0.2 * std::tanh ((a - 0.8) / 0.2);
        return x < 0 ? -y : y;
    }

    //--------------------------------------------------------------------------
    void resetControllers()
    {
        breath = 0; expression = 1; aftertouch = 0;
        seenBreath = seenExpr = seenAT = false;
        bendGlobal = 0;
        bendCh.fill (0);
        ccValue.fill (0);
        wheelTarget = 0;
    }

    void pushKey (int note)
    {
        removeKey (note);
        if (keyCount < 128) keys[(size_t) keyCount++] = note;
    }

    void removeKey (int note)
    {
        int w = 0;
        for (int r = 0; r < keyCount; ++r)
            if (keys[(size_t) r] != note) keys[(size_t) w++] = keys[(size_t) r];
        keyCount = w;
    }

    int countGroups() const
    {
        int c = 0;
        for (const auto& v : voices) if (v.on && ! v.killing && v.player == 0) ++c;
        return c;
    }

    // Fades out the oldest note group, preferring released ones. False if nothing to steal.
    bool stealOldestGroup()
    {
        const Voice* best = nullptr;
        for (const auto& v : voices)
        {
            if (! v.on || v.killing || v.player != 0) continue;
            if (best == nullptr) { best = &v; continue; }
            const bool vr = ! v.held && ! v.sustained, br = ! best->held && ! best->sustained;
            if ((vr && ! br) || (vr == br && v.age < best->age)) best = &v;
        }
        if (best == nullptr) return false;
        const int g = best->group;
        for (auto& v : voices) if (v.on && v.group == g) v.killing = true;
        return true;
    }

    Voice* monoLead()
    {
        if (monoGroup <= 0) return nullptr;
        for (auto& v : voices) if (v.on && ! v.killing && v.group == monoGroup && v.player == 0) return &v;
        return nullptr;
    }

    Voice& allocVoice()
    {
        for (auto& v : voices) if (! v.on) return v;
        Voice* best = nullptr;   // all busy: the oldest fading voice, else the oldest voice
        for (auto& v : voices)
        {
            if (best == nullptr) { best = &v; continue; }
            if ((v.killing && ! best->killing) || (v.killing == best->killing && v.age < best->age)) best = &v;
        }
        return *best;
    }

    bool isKeyswitch (int note) const
    {
        return params.artKs && note >= params.artKsBase && note < params.artKsBase + 12;
    }

    // Players per note: section players, times analog unison voices in the Analog engine.
    int playersPerNote() const
    {
        const auto& P = params;
        const int sec = std::min (std::max (P.secPlayers, 1), kMaxPlayers);
        if (P.engineMode == engAnalog) return std::min (std::max (sec * std::max (P.anaUnison, 1), 1), kMaxPlayers);
        return sec;
    }

    // Starts one note group: every player gets its own detune, onset / release timing,
    // vibrato rate and delay, brightness and level offsets, and a place in the stereo field.
    int newGroup (int note, double vel, int ch, double fromPitch)
    {
        const auto& P = params;
        const int g = ++groupCounter;
        const int N = playersPerNote();
        const bool analog = P.engineMode == engAnalog;
        const bool section = P.secPlayers > 1;
        for (int p = 0; p < N; ++p)
        {
            Voice& v = allocVoice();
            const double pos = N == 1 ? 0.0 : -1.0 + 2.0 * p / (N - 1);
            const double r1 = rng.bi(), r2 = rng.uni(), r3 = rng.bi(), r4 = rng.bi(), r5 = rng.bi(), r6 = rng.uni();
            double cents = 0;
            if (analog) cents += P.anaDetune * pos;
            if (section) cents += P.secDetune * (0.6 * pos + 0.4 * r1);
            v.plDetune   = cents / 100.0;
            v.plDelay    = section && p > 0 ? P.secTiming / 1000.0 * r2 : 0.0;
            v.plRelDelay = section && p > 0 ? P.secTiming / 1000.0 * 0.6 * r6 : 0.0;
            v.plBright   = section ? P.secTimbre * 0.6 * r3 : 0.0;
            v.plGain     = (section ? dbToGain (P.secTimbre * 1.5 * r4) : 1.0) / std::sqrt ((double) N);
            v.plVibRate  = 1 + (section ? P.secVibDesync * 0.1 * r5 : 0.0);
            v.plVibDelay = 1 + (section ? P.secVibDesync * 0.5 * r2 : 0.0);
            const double width = section ? std::max (P.secWidth, analog ? P.anaSpread : 0.0) : (analog ? P.anaSpread : 0.0);
            v.plPan = clampd (pos * width, -1.0, 1.0);
            v.plCount = N;
            v.panMod = false;
            const double ang = (v.plPan + 1) * kPi / 4;   // centre = unity in both channels
            v.panL = std::min (1.0, kSqrt2 * std::cos (ang));
            v.panR = std::min (1.0, kSqrt2 * std::sin (ang));
            startVoice (v, note, vel, ch, g, p, fromPitch);
        }
        return g;
    }

    void setVoicePitch (Voice& v, int note, bool glide)
    {
        v.glideFrom = v.pitch;
        v.glideTo = note;
        v.glidePos = glide ? 0.0 : 1.0;
        if (! glide) v.pitch = note;
    }

    // Legato note change: lip slur dip, vibrato restarts, keys click, a little lip transient.
    void legatoChange (Voice& v, bool glide)
    {
        const auto& P = params;
        v.slurT = 0;
        v.slurDur = std::max (glide ? P.glideTime : 0.0, 0.05);
        v.tVib = 0;
        v.tTrans = 0;
        v.tClick = 0;
        v.lipPeak = P.lipAmt * v.tgLip * 0.3 * (0.35 + 0.65 * v.dyn);
        v.chiffPeak = P.chiff * v.tgLip * 0.3 * (0.3 + 0.7 * v.dyn);
        v.clickPeak = P.keyClick * 0.7;
    }

    // Key released. A fall / doit (keyswitched, or auto at the end of a hard phrase) keeps the
    // note sounding while it bends and fades; section players release a little apart.
    void releaseVoice (Voice& v)
    {
        if (v.relPending) return;
        const auto& P = params;
        v.held = v.sustained = false;
        int kind = artNormal;
        if (latchedArt == artFall || latchedArt == artDoit) kind = latchedArt;
        else if (P.artAuto && keyCount == 0 && v.vel * 127 >= P.autoHi && now - v.noteStart >= 0.25) kind = artFall;
        const double dur = (kind == artFall ? P.fallTime : (kind == artDoit ? P.doitTime : 0.0)) + v.plRelDelay;
        if (dur > 0)
        {
            v.relPending = true;
            v.relKind = kind;
            v.relT = 0;
            v.relDur = dur;
            return;
        }
        gateOffAll (v);
    }

    static void gateOffAll (Voice& v)
    {
        for (auto& o : v.o) { o.amp.gateOff(); o.tmb.gateOff(); }
        for (auto& e : v.add.node) e.gateOff();
        v.fenv.gateOff();
    }

    void hardStop (Voice& v)
    {
        v.on = false;
        v.held = v.sustained = v.killing = false;
        for (auto& o : v.o) { o.amp.kill(); o.tmb.kill(); o.filt.reset(); o.lvl = 0; }
        v.add.kill();
        v.fenv.kill();
        v.f1.reset(); v.f2.reset();
        v.os.reset();
        v.lipF.reset(); v.noiseF.reset();
        v.gain = 0;
        v.lipLvl = v.noiseLvl = v.clickLvl = 0;
        v.dcX = v.dcY = v.toneZ = v.nPrev = 0;
        v.relPending = false;
        v.relKind = artNormal;
        v.delayLeft = 0;
    }

    double dynamicsTarget (const Voice& v) const
    {
        const auto& P = params;
        double num = 0, den = 0;
        if (seenBreath && P.wBreath > 0) { num += P.wBreath * breath;     den += P.wBreath; }
        if (seenExpr && P.wExpr > 0)     { num += P.wExpr * expression;   den += P.wExpr; }
        if (seenAT && P.wAT > 0)         { num += P.wAT * aftertouch;     den += P.wAT; }
        if (v.pressureSeen && P.wMPE > 0) { num += P.wMPE * v.pressure;   den += P.wMPE; }
        // with a controller: it sets the level, velocity only colours it (dynVel);
        // without one: velocity alone
        const double x = den > 0 ? (num / den) * (1 - P.dynVel + P.dynVel * v.vel) : v.vel;
        return responseCurve (x, P.dynCurve);
    }

    void startVoice (Voice& v, int note, double vel, int ch, int group, int player, double fromPitch)
    {
        const auto& P = params;
        const bool fresh = ! v.on;
        if (fresh)
        {
            for (int k = 0; k < kNumOsc; ++k)
            {
                auto& o = v.o[(size_t) k];
                // sine/tri start at phase 0 (clean attack), saw/pulse at a random phase
                o.osc.reset (P.osc[(size_t) k].shape >= 1.5 ? rng.uni() : 0.0);
                o.filt.reset();
                o.amp.kill();
                o.tmb.kill();
                o.lvl = 0;
            }
            v.add.kill();
            v.add.c = 1; v.add.s = 0;   // phase 0: the stack starts at a zero crossing
            v.fenv.kill();
            v.f1.reset(); v.f2.reset();
            v.os.reset();
            v.lipF.reset(); v.noiseF.reset();
            v.nrng.seed (rng.next());
            v.vibPhase = rng.uni();
            v.driftR = v.driftRT = rng.bi();
            v.driftD = v.driftDT = rng.bi();
            v.driftLeft = 0;
            v.growlPh = rng.uni();
            v.growlN = v.growlNT = 0;
            v.growlLeft = 0;
            v.flutPh = 0;
            v.lipPhase = 0;
            v.gain = 0;
            v.lipLvl = v.noiseLvl = v.clickLvl = 0;
            v.dcX = v.dcY = v.toneZ = v.nPrev = 0;
            v.artSemis = 0;
            v.slide = -1;
            for (int i = 0; i < kNumLfo; ++i)
                v.lfo[(size_t) i].start (P.lfo[(size_t) i].retrig ? 0.0 : rng.uni(), rng);
            v.snap = true;
        }
        else
        {
            for (int i = 0; i < kNumLfo; ++i)
                if (P.lfo[(size_t) i].retrig) v.lfo[(size_t) i].retrigger (0.0, rng);
        }
        v.rnd = rng.bi();
        v.artAtk = pendingAtk;
        v.artShake = latchedArt == artShake;
        v.relPending = false;
        v.relKind = artNormal;
        v.relT = 0;
        v.noteStart = now;
        v.delayLeft = v.plDelay;
        v.on = true;
        v.killing = false;
        v.kill = 1;
        v.note = note;
        v.channel = ch;
        v.group = group;
        v.player = player;
        v.vel = vel;
        v.age = ++ageCounter;
        v.held = true;
        v.sustained = false;
        v.tNote = 0;
        v.tVib = 0;
        v.tTrans = 0;
        v.tClick = 0;
        v.slurT = 1e9;
        v.pressure = 0;
        v.pressureSeen = false;
        v.glideFrom = fromPitch;
        v.glideTo = note;
        v.glidePos = fromPitch == (double) note ? 1.0 : 0.0;
        v.pitch = fromPitch;

        v.dyn = dynamicsTarget (v);
        v.dynOnset = v.dyn;

        // tongue: soft = gentle onset with more scoop, hard = sharp, bright onset, little scoop
        static const double tgLip[]   { 0.45, 1.0, 1.7 };
        static const double tgDecay[] { 1.6, 1.0, 0.6 };
        static const double tgScoop[] { 1.3, 1.0, 0.75 };
        static const double tgBlip[]  { 0.5, 1.0, 1.6 };
        static const double tgAtk[]   { 1.5, 1.0, 0.6 };
        const int tg = std::min (std::max (P.tongue, 0), 2);
        v.tgLip = tgLip[tg]; v.tgDecay = tgDecay[tg]; v.tgScoop = tgScoop[tg]; v.tgBlip = tgBlip[tg];

        // soft notes swell in slowly, hard notes bite: x4 slower .. x4 faster attacks
        const double atkSpeed = std::pow (2.0, P.dynAttack * (2.0 - 4.0 * v.dynOnset)) * tgAtk[tg];
        const double key = (note - 60) / 24.0;           // +1 per two octaves above C4
        const double velB = 2 * vel - 1;                 // -1..1

        for (int k = 0; k < kNumOsc; ++k)
        {
            const auto& O = P.osc[(size_t) k];
            auto& o = v.o[(size_t) k];
            o.ampScale   = (1 - O.ampVel * (1 - vel)) * std::pow (2.0, O.ampKeyAmt * key);
            o.ampTime    = std::pow (2.0, -O.ampKeyTime * key);
            o.ampAtkTime = o.ampTime * std::pow (2.0, -O.ampVelTime * velB) * atkSpeed;
            o.pScale     = std::max (0.0, 1 + O.pVel * velB);
            o.pTime      = std::pow (2.0, -O.pKeyTime * key);
            o.tScale     = std::max (0.0, 1 + O.tVel * velB);
            o.tTime      = std::pow (2.0, -O.tKeyTime * key);
            o.tAtkTime   = o.tTime * atkSpeed;
            o.amp.gateOn();
            o.tmb.gateOn();
        }
        v.addAtkScale = atkSpeed;
        for (auto& e : v.add.node) e.gateOn();
        v.fltVelScale = 1 - P.fltVel + P.fltVel * vel;
        v.fenv.gateOn();

        // attack transients
        v.lipPeak   = P.lipAmt * v.tgLip * (0.35 + 0.65 * v.dynOnset);
        v.chiffPeak = P.chiff * v.tgLip * (0.3 + 0.7 * v.dynOnset);
        v.clickPeak = P.keyClick * (0.5 + 0.5 * vel);
    }

    //--------------------------------------------------------------------------
    double plungerTarget() const
    {
        const auto& P = params;
        switch (P.muteSrc)
        {
            case plWheel:      return wheel;
            case plBreath:     return seenBreath ? breath : P.mutePlunger;
            case plExpression: return seenExpr ? expression : P.mutePlunger;
            case plAftertouch: return seenAT ? aftertouch : P.mutePlunger;
            case plCC:         return ccValue[(size_t) std::min (std::max (P.muteCC, 0), 127)];
            default:           return P.mutePlunger;
        }
    }

    void updateControl (int n)
    {
        const auto& P = params;
        wheel += (wheelTarget - wheel) * wheelSmooth;
        dynCoef = 1 - std::exp (-n / (std::max (P.dynSmooth, 0.001) * sr));

        // ---- global LFOs (one phase for all voices; synced + free + playing = locked to the song)
        for (int i = 0; i < kNumLfo; ++i)
        {
            const auto& lp = P.lfo[(size_t) i];
            lfoInc[(size_t) i] = lfoRateHz (lp) * n / sr;
            if (lp.perVoice) { lfoGlobal[(size_t) i] = 0; continue; }
            auto& g = lfoG[(size_t) i];
            const bool locked = lp.sync && playing && ! lp.retrig;
            if (locked)
            {
                const double beats = kLfoDivBeats[std::min (std::max (lp.div, 0), kNumLfoDivs - 1)];
                double ph = (ppqAtBlock + samplesIntoBlock / sr * bpm / 60.0) / beats;
                ph -= std::floor (ph);
                if (ph < g.phase - 0.5) g.held = rng.bi();   // wrapped
                g.phase = ph;
            }
            const double raw = lp.on ? g.shape (lp.wave, lp.phase) * lp.amount : 0.0;
            lfoGlobal[(size_t) i] = g.smooth (raw, lfoSmooth);
            if (! locked) g.advance (lfoInc[(size_t) i], rng);
        }

        // ---- mod matrix: smoothed amounts, the list of active routes
        numActive = 0;
        for (int s = 0; s < kNumModSlots; ++s)
        {
            const auto& m = P.mod[(size_t) s];
            const bool routed = m.src != srcNone && m.dst != dstNone;
            const double target = (m.on && routed) ? m.amount : 0.0;
            slotAmt[(size_t) s] += (target - slotAmt[(size_t) s]) * ctlSmooth;
            if (routed && std::abs (slotAmt[(size_t) s]) > 1e-6)
                activeSlots[(size_t) numActive++] = { m.src, m.via, m.dst, slotAmt[(size_t) s] };
        }
        // global destinations (bus / FX) take global sources only
        gDst.fill (0);
        for (int a = 0; a < numActive; ++a)
        {
            const auto& s = activeSlots[(size_t) a];
            if (s.dst < kFirstGlobalDest) continue;
            double m = globalSource (s.src) * s.amt;
            if (s.via != srcNone)
            {
                double w = globalSource (s.via);
                if (isBipolarSource (s.via)) w = 0.5 * (w + 1);
                m *= w;
            }
            gDst[(size_t) s.dst] += m;
        }

        brsToneCoef = P.brsTone >= 0.999 ? 1.0 : 1 - std::exp (-kTwoPi * 1500.0 * std::pow (2.0, P.brsTone * 3.75) / sr);

        // ---- bus: body formants + mute (coefficients only when the settings change)
        const double bodyShift = clampd (P.bodyShift + 12.0 * gDst[dstBodyShift], -24.0, 24.0);
        const double key = P.instrument * 1e6 + bodyShift * 1e3 + P.bodyReso * 10 + P.muteType + 0.5;
        if (key != busKey)
        {
            const auto& M = instrumentModel (P.instrument);
            const double shift = std::pow (2.0, bodyShift / 12.0);
            for (int b = 0; b < kBodyBands; ++b)
            {
                const auto& B = M.body[b];
                const double db = B.db * (0.5 + P.bodyReso);
                bodyC[(size_t) b] = designBiquad (B.type, B.hz * shift, B.q, db, sr);
            }
            const auto& MM = muteModel (P.muteType);
            if (P.muteType != muteKind)
            {
                for (int b = 0; b < kBodyBands; ++b) { muteL[(size_t) b].reset(); muteR[(size_t) b].reset(); }
                plL.reset(); plR.reset();
                muteKind = P.muteType;
            }
            for (int b = 0; b < 4; ++b)
                muteC[(size_t) b] = designBiquad (MM.bands[b].type, MM.bands[b].hz, MM.bands[b].q, MM.bands[b].db, sr);
            muteGain = dbToGain (MM.gainDb);
            busKey = key;
        }
        plungerFrom = plungerCur;
        plungerCur += (clampd (plungerTarget() + gDst[dstPlunger], 0.0, 1.0) - plungerCur) * busSmooth;
        bodyMixFrom = bodyMixCur;
        bodyMixCur += (P.bodyMix - bodyMixCur) * busSmooth;
        muteMixFrom = muteMixCur;
        muteMixCur += ((P.muteType == muteOpen ? 0.0 : P.muteMix) - muteMixCur) * busSmooth;
    }

    void processBus (int n)
    {
        const auto& P = params;
        const double inv = 1.0 / n;
        // body / bell formants (parallel blend)
        if (bodyMixCur > 1e-4 || bodyMixFrom > 1e-4)
        {
            double m = bodyMixFrom;
            const double dm = (bodyMixCur - bodyMixFrom) * inv;
            for (int i = 0; i < n; ++i)
            {
                m += dm;
                double l = mixL[(size_t) i], r = mixR[(size_t) i];
                double bl = l, br = r;
                for (int b = 0; b < kBodyBands; ++b)
                {
                    bl = bodyL[(size_t) b].process (bl, bodyC[(size_t) b]);
                    br = bodyR[(size_t) b].process (br, bodyC[(size_t) b]);
                }
                mixL[(size_t) i] = l + m * (bl - l);
                mixR[(size_t) i] = r + m * (br - r);
            }
        }
        // mute
        if (muteMixCur > 1e-4 || muteMixFrom > 1e-4)
        {
            const bool plunger = P.muteType == mutePlungerType;
            const auto cutFor = [] (double pos) { return 350.0 * std::pow (2.0, clampd (pos, 0.0, 1.0) * 4.3); };
            double pg = SVF::gFor (cutFor (plungerFrom), sr);
            const double dpg = (SVF::gFor (cutFor (plungerCur), sr) - pg) * inv;
            const double pk = SVF::kFor (0.35);
            double pgain = dbToGain (-6.0 * (1 - clampd (plungerFrom, 0.0, 1.0)));
            const double dpgain = (dbToGain (-6.0 * (1 - clampd (plungerCur, 0.0, 1.0))) - pgain) * inv;
            double m = muteMixFrom;
            const double dm = (muteMixCur - muteMixFrom) * inv;
            for (int i = 0; i < n; ++i)
            {
                m += dm; pg += dpg; pgain += dpgain;
                const double l = mixL[(size_t) i], r = mixR[(size_t) i];
                double ml = l, mr = r;
                for (int b = 0; b < 4; ++b)
                {
                    ml = muteL[(size_t) b].process (ml, muteC[(size_t) b]);
                    mr = muteR[(size_t) b].process (mr, muteC[(size_t) b]);
                }
                if (plunger)
                {
                    ml = plL.process (ml, pg, pk) * pgain;
                    mr = plR.process (mr, pg, pk) * pgain;
                }
                ml *= muteGain; mr *= muteGain;
                mixL[(size_t) i] = l + m * (ml - l);
                mixR[(size_t) i] = r + m * (mr - r);
            }
        }
    }

    double lfoRateHz (const EngineParams::LfoP& l) const
    {
        if (! l.sync) return l.rate;
        return (bpm / 60.0) / kLfoDivBeats[std::min (std::max (l.div, 0), kNumLfoDivs - 1)];
    }

    // Sources that exist outside any voice (for the bus / FX destinations).
    double globalSource (int s) const
    {
        switch (s)
        {
            case srcLfo1: case srcLfo2:
            {
                const int i = s - srcLfo1;
                return params.lfo[(size_t) i].perVoice ? 0.0 : lfoGlobal[(size_t) i];
            }
            case srcBreath:     return seenBreath ? breath : 0.0;
            case srcExpression: return seenExpr ? expression : 0.0;
            case srcAftertouch: return seenAT ? aftertouch : 0.0;
            case srcModWheel:   return wheel;
            case srcPitchBend:  return bendGlobal;
            default:            return 0.0;
        }
    }

    // patch level -> drive -> EQ -> chorus -> reverb -> width, on the stereo bus
    void processFx (int n)
    {
        const auto& P = params;
        const double inv = 1.0 / n;
        const double drvAmt = clampd (P.fxDrvAmt + gDst[dstFxDrive], 0.0, 1.0);
        const double chMix  = clampd (P.chMix + gDst[dstChorusMix], 0.0, 1.0);
        const double rvMix  = clampd (P.rvMix + gDst[dstReverbMix], 0.0, 1.0);

        // ---- patch level (before the FX, so drive and reverb see every preset at the same level)
        for (int i = 0; i < n; ++i)
        {
            ampCur += (P.ampGain - ampCur) * masterSmooth;
            mixL[(size_t) i] *= ampCur;
            mixR[(size_t) i] *= ampCur;
        }

        // ---- drive (tube / tape), oversampled like the brassiness stage. Level-matched by an
        //      automatic output gain that follows input vs. output power (~300 ms), so turning
        //      the drive up changes the colour, not the loudness, at any input level.
        if (P.fxDrvOn)
        {
            const int type = P.fxDrvType == 1 ? shapeTape : shapeTube;
            const double tg = dbToGain (drvAmt * 36.0);
            const double tmix = P.fxDrvMix;
            if (! drvWasOn || fxInit)
            {
                fxOsL.reset(); fxOsR.reset();
                drvLpL = drvLpR = drvDcXL = drvDcYL = drvDcXR = drvDcYR = 0;
                drvG = tg; drvMixCur = tmix;
                drvComp = 1.0 / tg;          // small-signal gain of the shaper is ~1: start level-matched
                drvPin = drvPout = 0;
            }
            const double toneC = P.fxDrvTone >= 0.999 ? 1.0 : 1 - std::exp (-kTwoPi * 2000.0 * std::pow (2.0, P.fxDrvTone * 3.3) / sr);
            const int factor = P.oversample >= 2 ? 4 : (P.oversample == 1 ? 2 : 1);
            double g = drvG, m = drvMixCur, c = drvComp;
            const double dg = (tg - g) * inv, dm = (tmix - m) * inv, dc = (drvCompTarget - c) * inv;
            double pin = 0, pout = 0;
            for (int i = 0; i < n; ++i)
            {
                g += dg; m += dm; c += dc;
                const double xl = mixL[(size_t) i], xr = mixR[(size_t) i];
                const double yl = fxOsL.process (xl * g, factor, type);
                const double yr = fxOsR.process (xr * g, factor, type);
                const double hl = yl - drvDcXL + dcR * drvDcYL; drvDcXL = yl; drvDcYL = hl;
                const double hr = yr - drvDcXR + dcR * drvDcYR; drvDcXR = yr; drvDcYR = hr;
                drvLpL += (hl - drvLpL) * toneC;
                drvLpR += (hr - drvLpR) * toneC;
                pin += xl * xl + xr * xr;
                pout += drvLpL * drvLpL + drvLpR * drvLpR;
                mixL[(size_t) i] = xl + m * (drvLpL * c - xl);
                mixR[(size_t) i] = xr + m * (drvLpR * c - xr);
            }
            // follow the powers; the gain that makes them equal becomes the next chunk's target
            const double a = 1 - std::exp (-n / (0.3 * sr));
            drvPin += (pin * inv - drvPin) * a;
            drvPout += (pout * inv - drvPout) * a;
            if (drvPout > 1e-12 && drvPin > 1e-12)
                drvCompTarget = clampd (std::sqrt (drvPin / drvPout), 0.01, 4.0);
            else
                drvCompTarget = drvComp;
            drvG = tg; drvMixCur = tmix; drvComp = c;
        }
        drvWasOn = P.fxDrvOn;

        // ---- 3-band EQ
        if (P.eqOn)
        {
            const double key = P.eqLoGain * 1e9 + P.eqLoFreq * 1e5 + P.eqMidGain * 1e3 + P.eqMidFreq + P.eqMidQ * 0.1
                               + P.eqHiGain * 1e-3 + P.eqHiFreq * 1e-7;
            if (key != eqKey)
            {
                eqC[0] = designBiquad (bqLowShelf, P.eqLoFreq, 0.7071, P.eqLoGain, sr);
                eqC[1] = designBiquad (bqPeak, P.eqMidFreq, P.eqMidQ, P.eqMidGain, sr);
                eqC[2] = designBiquad (bqHighShelf, P.eqHiFreq, 0.7071, P.eqHiGain, sr);
                eqKey = key;
            }
            if (! eqWasOn) for (int b = 0; b < 3; ++b) { eqL[(size_t) b].reset(); eqR[(size_t) b].reset(); }
            for (int i = 0; i < n; ++i)
            {
                double l = mixL[(size_t) i], r = mixR[(size_t) i];
                for (int b = 0; b < 3; ++b) { l = eqL[(size_t) b].process (l, eqC[(size_t) b]); r = eqR[(size_t) b].process (r, eqC[(size_t) b]); }
                mixL[(size_t) i] = l; mixR[(size_t) i] = r;
            }
        }
        eqWasOn = P.eqOn;

        // ---- chorus
        if (P.chOn)
        {
            if (! chWasOn || fxInit) { chorus.reset(); chMixCur = chMix; }
            chorus.phase += P.chRate * n / sr;
            chorus.phase -= std::floor (chorus.phase);
            const double tl = (0.012 + 0.006 * P.chDepth * std::sin (kTwoPi * chorus.phase)) * sr;
            const double tr = (0.012 + 0.006 * P.chDepth * std::sin (kTwoPi * (chorus.phase + 0.25))) * sr;
            double dl = chorus.dL, dr = chorus.dR, m = chMixCur;
            const double ddl = (tl - dl) * inv, ddr = (tr - dr) * inv, dm = (chMix - m) * inv;
            for (int i = 0; i < n; ++i)
            {
                dl += ddl; dr += ddr; m += dm;
                const double l = mixL[(size_t) i], r = mixR[(size_t) i];
                chorus.bufL[(size_t) chorus.w] = l;
                chorus.bufR[(size_t) chorus.w] = r;
                const double wl = chorus.read (chorus.bufL, dl), wr = chorus.read (chorus.bufR, dr);
                chorus.w = (chorus.w + 1) & chorus.mask;
                const double norm = 1.0 / (1.0 + 0.5 * m);   // dry + wet, about level-matched
                mixL[(size_t) i] = (l + m * wl) * norm;
                mixR[(size_t) i] = (r + m * wr) * norm;
            }
            chorus.dL = tl; chorus.dR = tr; chMixCur = chMix;
        }
        chWasOn = P.chOn;

        // ---- reverb (equal-power dry / wet blend)
        if (P.rvOn)
        {
            if (! rvWasOn || fxInit) { reverb.reset(); rvMixCur = rvMix; }
            reverb.setType (P.rvType == 0 ? 0 : 1);
            if (P.rvDecay != rvDecaySet || reverb.type != rvTypeSet)
            {
                reverb.setDecay (P.rvDecay);
                double gm = 0;
                for (int i = 0; i < kRvLines; ++i) gm += reverb.g[i];
                gm /= kRvLines;
                rvNorm = std::sqrt (std::max (1 - gm * gm, 1e-4)) * 2.2;   // longer decays -> more energy -> scale down
                rvDecaySet = P.rvDecay;
                rvTypeSet = reverb.type;
            }
            const int pre = (int) std::lround (P.rvPre * 0.001 * sr);
            const double damp = 1 - std::exp (-kTwoPi * 18000.0 * std::pow (2.0, -P.rvDamp * 5.0) / sr);
            double dry0 = std::cos (rvMixCur * kPi / 2), wet0 = std::sin (rvMixCur * kPi / 2) * rvNorm;
            const double dry1 = std::cos (rvMix * kPi / 2), wet1 = std::sin (rvMix * kPi / 2) * rvNorm;
            const double ddry = (dry1 - dry0) * inv, dwet = (wet1 - wet0) * inv;
            for (int i = 0; i < n; ++i)
            {
                dry0 += ddry; wet0 += dwet;
                const double l = mixL[(size_t) i], r = mixR[(size_t) i];
                double wl, wr;
                reverb.process (0.5 * (l + r), pre, damp, wl, wr);
                mixL[(size_t) i] = l * dry0 + wl * wet0;
                mixR[(size_t) i] = r * dry0 + wr * wet0;
            }
            rvMixCur = rvMix;
        }
        rvWasOn = P.rvOn;

        // ---- stereo width (mid / side)
        if (std::abs (P.stWidth - 1) > 1e-4 || std::abs (widthCur - 1) > 1e-4)
        {
            double w = widthCur;
            const double dw = (P.stWidth - w) * inv;
            for (int i = 0; i < n; ++i)
            {
                w += dw;
                const double mid = 0.5 * (mixL[(size_t) i] + mixR[(size_t) i]), side = 0.5 * (mixL[(size_t) i] - mixR[(size_t) i]) * w;
                mixL[(size_t) i] = mid + side;
                mixR[(size_t) i] = mid - side;
            }
            widthCur = P.stWidth;
        }
        fxInit = false;
    }

    static EnvTimes scaled (const EnvTimes& e, double t, double atk)
    {
        EnvTimes r = e;
        r.dly *= t; r.atk *= atk; r.hld *= t; r.dec *= t; r.rel *= t;
        return r;
    }

    void renderVoice (Voice& v, int n)
    {
        const auto& P = params;
        const auto& M = instrumentModel (P.instrument);
        const double dtSec = n / sr;
        if (v.delayLeft > 0) { v.delayLeft -= dtSec; return; }   // section player not in yet
        const bool analog = P.engineMode == engAnalog;
        v.tNote += dtSec;
        v.tVib += dtSec;
        v.slurT += dtSec;
        v.tTrans += dtSec;
        v.tClick += dtSec;

        // ---- dynamics (the matrix may offset it below) ---------------------------
        v.dyn += (dynamicsTarget (v) - v.dyn) * dynCoef;

        // ---- LFOs: per-voice ones run here, global ones were computed for the chunk ------
        double lfoVal[kNumLfo];
        for (int i = 0; i < kNumLfo; ++i)
        {
            const auto& lp = P.lfo[(size_t) i];
            lfoVal[i] = lp.perVoice
                ? v.lfo[(size_t) i].smooth (lp.on ? v.lfo[(size_t) i].shape (lp.wave, lp.phase) * lp.amount : 0.0, lfoSmooth)
                : lfoGlobal[(size_t) i];
        }

        // ---- vibrato signal (its depth and rate can be modulated) ----------------------
        if (--v.driftLeft <= 0)
        {
            v.driftRT = rng.bi();
            v.driftDT = rng.bi();
            v.driftLeft = (int) (0.25 * sr / kChunk);
        }
        v.driftR += (v.driftRT - v.driftR) * driftSmooth;
        v.driftD += (v.driftDT - v.driftD) * driftSmooth;
        const double vibDelay = (P.vibDelay > kEnvMinStage ? P.vibDelay : 0.0) * v.plVibDelay;
        const double vibEnv = v.tVib < vibDelay ? 0.0 : std::min (1.0, (v.tVib - vibDelay) / std::max (P.vibFade, 0.001));
        const double wheelScale = 1 - P.vibWheel + P.vibWheel * wheel;
        const double vs = std::sin (kTwoPi * v.vibPhase) * vibEnv * wheelScale;

        // ---- mod matrix (sources from this voice; envelopes as of the last chunk) ------------
        double dst[kNumModDests] = {};
        if (numActive > 0)
        {
            double src[kNumModSources];
            src[srcNone] = 0;
            src[srcLfo1] = lfoVal[0];
            src[srcLfo2] = lfoVal[1];
            for (int k = 0; k < kNumOsc; ++k)
            {
                const auto& O = P.osc[(size_t) k];
                const auto& o = v.o[(size_t) k];
                src[srcAmpEnv1 + k] = o.amp.level;
                src[srcPitchEnv1 + k] = 1 - curveShape (std::min (v.tNote / (O.pTime * o.pTime), 1.0), O.pCrv);
                src[srcTmbEnv1 + k] = o.tmb.level;
            }
            src[srcHarmLo] = v.add.node[0].level;
            src[srcHarmMid] = v.add.node[4].level;
            src[srcHarmHi] = v.add.node[7].level;
            src[srcFltEnv] = v.fenv.level;
            src[srcVelocity] = v.vel;
            src[srcDynamics] = v.dyn;
            src[srcBreath] = seenBreath ? breath : 0.0;
            src[srcExpression] = seenExpr ? expression : 0.0;
            src[srcAftertouch] = seenAT ? aftertouch : 0.0;
            src[srcMpePressure] = v.pressure;
            src[srcMpeSlide] = v.slide >= 0 ? v.slide : 0.0;
            src[srcMpeBend] = P.mpeOn && v.channel != 0 ? bendCh[(size_t) v.channel] : 0.0;
            src[srcModWheel] = wheel;
            src[srcPitchBend] = bendGlobal;
            src[srcKey] = (v.note - 60) / 60.0;
            src[srcRandom] = v.rnd;
            src[srcVibrato] = vs;
            for (int a = 0; a < numActive; ++a)
            {
                const auto& s = activeSlots[(size_t) a];
                if (s.dst >= kFirstGlobalDest) continue;
                double m = src[s.src] * s.amt;
                if (s.via != srcNone)
                {
                    double w = src[s.via];
                    if (isBipolarSource (s.via)) w = 0.5 * (w + 1);   // bipolar -> 0..1
                    m *= w;
                }
                dst[s.dst] += m;
            }
        }

        const double d = clampd (v.dyn + dst[dstDynamics], 0.0, 1.0);
        lastDyn = d;
        const double loud = (d < 0.05 ? d / 0.05 : 1.0) * dbToGain (-P.dynRange * (1 - d));
        const double brightCurve = responseCurve (d, P.dynBrCurve);
        // analog "blat": a fast overshoot of brightness (and a little pitch) on the attack
        const double blat = analog ? P.anaBlat * std::exp (-v.tNote / 0.06) * (0.4 + 0.6 * v.vel) : 0.0;
        double bright = P.dynBright * brightCurve + v.plBright + 3.0 * blat + 4.0 * dst[dstBrightness];
        if (P.mpeOn && v.slide >= 0) bright += (v.slide - 0.5) * 3.0;   // MPE slide: +-1.5 oct

        // ---- articulations ----------------------------------------------------------
        double artTarget = 0, artGain = 1;
        if (v.artAtk == artScoop && v.tNote < P.scoopTime)
        {
            const double x = v.tNote / P.scoopTime;
            artTarget -= P.scoopRng * (1 - x) * (1 - x);
        }
        if (v.artAtk == artRip && v.tNote < P.ripTime)   // up through the notes below, in steps
        {
            const double x = v.tNote / P.ripTime;
            artTarget -= std::floor (P.ripRng * (1 - x) + 0.5);
            artGain *= 0.55 + 0.45 * x;
        }
        if (v.artShake && v.tNote > 0.12)                // lip trill to the note above
        {
            const double x = v.tNote - 0.12;
            artTarget += P.shakeRng * std::min (1.0, x / 0.25) * (0.5 + 0.5 * std::tanh (3 * std::sin (kTwoPi * P.shakeRate * x)));
        }
        if (v.relPending)
        {
            v.relT += dtSec;
            const double x = std::min (1.0, v.relT / std::max (v.relDur, 1e-3));
            if (v.relKind == artFall)      { artTarget -= P.fallRng * x * x; artGain *= 1 - std::pow (x, 1.5); }
            else if (v.relKind == artDoit) { artTarget += P.doitRng * x * x; artGain *= 1 - std::pow (x, 1.5); }
            if (x >= 1) { v.relPending = false; gateOffAll (v); }
        }
        else if (v.relKind != artNormal)
            artGain = 0;                                  // fall / doit finished: stays silent through the release
        v.artSemis += (artTarget - v.artSemis) * artSmooth;

        // ---- vibrato (with slow humanised drift of rate and depth) --------------
        const double vibDepthNow = std::max (0.0, P.vibDepth + 100.0 * dst[dstVibDepth]);
        const double vibCents = vibDepthNow * vs * (1 + 0.3 * P.vibDrift * v.driftD);
        const double vibAmpMul = 1 + 0.5 * P.vibAmp * vs;
        const double vibRateMod = dst[dstVibRate] != 0 ? std::pow (2.0, 2.0 * dst[dstVibRate]) : 1.0;
        v.vibPhase += P.vibRate * vibRateMod * v.plVibRate * (1 + 0.12 * P.vibDrift * v.driftR) * dtSec;
        v.vibPhase -= std::floor (v.vibPhase);

        // ---- growl (rough AM + FM, hummed into the horn) and flutter tongue ---------
        double growlAM = 1, growlCents = 0;
        const double growl = clampd (P.growlAmt + dst[dstGrowl], 0.0, 1.0) * (1 - P.growlDyn + P.growlDyn * d);
        if (growl > 1e-4)
        {
            if (--v.growlLeft <= 0)
            {
                v.growlNT = v.nrng.bi();
                v.growlLeft = std::max (1, (int) (sr / (P.growlRate * 2) / kChunk));
            }
            v.growlN += (v.growlNT - v.growlN) * 0.3;
            const double gs = std::sin (kTwoPi * v.growlPh);
            const double gm = (1 - P.growlRough) * gs + P.growlRough * v.growlN;
            growlAM = 1 - 0.45 * growl * (1 + gm);
            growlCents = 18 * growl * gm;
            v.growlPh += P.growlRate * (1 + 0.15 * P.growlRough * v.growlN) * dtSec;
            v.growlPh -= std::floor (v.growlPh);
        }
        double flutAM = 1, flutNoise = 0;
        const double flutAmt = clampd (P.flutAmt + dst[dstFlutter], 0.0, 1.0);
        if (flutAmt > 1e-4)
        {
            const double fs = 0.5 + 0.5 * std::sin (kTwoPi * v.flutPh);
            const double f3 = fs * fs * fs;
            flutAM = 1 - 0.85 * flutAmt * f3;
            flutNoise = flutAmt * f3;
            v.flutPh += P.flutRate * dtSec;
            v.flutPh -= std::floor (v.flutPh);
        }

        // ---- lip slur dip on legato note changes -----------------------------------
        const double dip = v.slurT < v.slurDur ? P.slurDip * std::sin (kPi * v.slurT / v.slurDur) : 0.0;

        // ---- per-oscillator targets for the end of this chunk ------------------------
        const double bendSemis = P.mpeOn ? bendGlobal * P.bendRange + (v.channel != 0 ? bendCh[(size_t) v.channel] * P.mpeBend : 0.0)
                                         : bendGlobal * P.bendRange;
        const double base = v.pitch + P.transpose - 69 + P.tuneSemis + bendSemis + v.artSemis + v.plDetune
                            + (vibCents + growlCents) / 100.0 + 0.25 * blat + 12.0 * dst[dstPitchAll];
        double tdt[kNumOsc], tlvl[kNumOsc], tshape[kNumOsc], tpw[kNumOsc], tg[kNumOsc], tk[kNumOsc];
        bool alive = false;
        double envMain = 0, pe0 = 0, hz0 = 440;
        for (int k = 0; k < kNumOsc; ++k)
        {
            const auto& O = P.osc[(size_t) k];
            auto& o = v.o[(size_t) k];
            const double aLvl = o.amp.advance (scaled (O.amp, o.ampTime, o.ampAtkTime), dtSec);
            const double tLvl = o.tmb.advance (scaled (O.tmb, o.tTime, o.tAtkTime), dtSec);
            if (O.on && o.amp.active()) alive = true;
            if (O.on) envMain = std::max (envMain, aLvl);

            // pitch envelope: start offset gliding to 0 (scoop), plus a blip on the attack
            double pe = O.pAmt * o.pScale * v.tgScoop * (1 - curveShape (std::min (v.tNote / (O.pTime * o.pTime), 1.0), O.pCrv));
            const double bx = v.tNote / (O.blipTime * o.pTime);
            if (bx < 1) pe += O.blipAmt * o.pScale * v.tgBlip * std::sin (kPi * bx);

            const double semis = base + O.semis + pe / 100.0 + 12.0 * dst[dstOsc1Pitch + k];
            const double hz = 440.0 * std::pow (2.0, semis / 12.0);
            if (k == 0) { pe0 = pe; hz0 = hz; }
            tdt[k] = std::min (hz / sr, 0.45);
            const double cut = hz * O.cutHarm * std::pow (2.0, tLvl * O.tCut * o.tScale + bright - 2 * dip + 5.0 * dst[dstOsc1Bright + k]);
            tg[k] = SVF::gFor (cut, sr);
            tk[k] = SVF::kFor (O.res * 0.9);
            tshape[k] = clampd (O.shape + tLvl * O.tShape * o.tScale + 3.0 * dst[dstOsc1Shape + k], 0.0, 3.0);
            tpw[k] = clampd (O.pw + 0.45 * dst[dstOsc1PW + k], 0.05, 0.95);
            tlvl[k] = O.on ? O.level * aLvl * o.ampScale * clampd (1 + dst[dstOsc1Level + k], 0.0, 2.0) : 0.0;
        }

        // ---- additive harmonic layer: node envelopes + harmonic amplitudes -----------
        double tA[kMaxPartials];
        int addCount = 0;
        double tdtA = v.add.dt;
        const bool addOn = P.addOn && ! analog;   // the Analog engine is purely subtractive
        if (addOn)
        {
            const auto& T = addTables();
            double nodeLvl[kAddNodes];
            for (int j = 0; j < kAddNodes; ++j)
            {
                const double x = T.nodePos[j];
                const double w0 = T.nodeW[j][0], w1 = T.nodeW[j][1], w2 = T.nodeW[j][2];
                const auto& B0 = P.band[0]; const auto& B1 = P.band[1]; const auto& B2 = P.band[2];
                EnvTimes e;
                e.dly = P.addSpecDly * 0.08 * T.nodeDly[j];
                e.atk = (w0 * B0.atk + w1 * B1.atk + w2 * B2.atk) * (1 + 1.5 * P.addSpecDly * x) * v.addAtkScale;
                e.hld = 0;
                e.dec = w0 * B0.dec + w1 * B1.dec + w2 * B2.dec;
                e.sus = w0 * B0.sus + w1 * B1.sus + w2 * B2.sus;
                e.rel = (w0 * B0.rel + w1 * B1.rel + w2 * B2.rel) * (1 - 0.6 * P.addSpecDly * x);
                e.atkC = e.decC = e.relC = P.addCurve;
                const double lvl = v.add.node[j].advance (e, dtSec);
                nodeLvl[j] = lvl * (w0 * B0.lvl + w1 * B1.lvl + w2 * B2.lvl);
                if (v.add.node[j].active()) alive = true;
                if (j == 0) envMain = std::max (envMain, lvl);
            }
            const double hzA = 440.0 * std::pow (2.0, (base + pe0 / 100.0) / 12.0);
            tdtA = std::min (hzA / sr, 0.45);

            // the note's spectrum, shared by all its players this chunk
            auto& S = specCache[(size_t) (v.group & (kSpecCache - 1))];
            if (S.group != v.group || S.stamp != chunkStamp)
            {
                // brass: steep spectrum when soft, nearly flat when loud
                const double slope = 0.5 * (M.softSlope + M.loudSlope)
                                     + (1 - 2 * brightCurve) * P.addDynSpread * 0.5 * (M.softSlope - M.loudSlope);
                const int maxN = std::min (std::max (P.addPartials, 1), kMaxPartials);
                const double logHz = std::log (hzA);
                double power = 0;
                S.count = 0;
                for (int k = 0; k < maxN; ++k)
                {
                    const double f = hzA * (k + 1);
                    if (f >= 0.45 * sr) break;
                    const double db = spectralEnvelopeDbLog (P.instrument, logHz + T.logn[k]) + slope * k + P.addTilt * T.log2n[k];
                    double a = std::exp (db * kLn10Over20);
                    const bool even = ((k + 1) & 1) == 0;
                    if (P.addOddEven > 0 && even) a *= 1 - P.addOddEven;
                    else if (P.addOddEven < 0 && ! even && k > 0) a *= 1 + P.addOddEven;
                    if (f > 0.4 * sr) a *= (0.45 * sr - f) / (0.05 * sr);
                    S.lin[k] = a;
                    power += a * a;
                    S.count = k + 1;
                }
                const double norm = power > 0 ? P.addLevel / std::sqrt (power) : 0.0;
                for (int k = 0; k < S.count; ++k) S.lin[k] *= norm;
                S.group = v.group;
                S.stamp = chunkStamp;
            }
            // extra section players are cheaper: 12 harmonics are plenty under the lead player
            addCount = std::min (S.count, v.player > 0 ? 12 : kMaxPartials);
            const double lvlMod = clampd (1 + dst[dstHarmLevel], 0.0, 2.0);
            const double tiltMod = 12.0 * dst[dstHarmTilt];   // dB/oct, per voice (not in the shared spectrum)
            for (int k = 0; k < addCount; ++k)
            {
                const int j = T.lo[k];
                tA[k] = S.lin[k] * lvlMod * (nodeLvl[j] + (nodeLvl[j + 1 < kAddNodes ? j + 1 : j] - nodeLvl[j]) * T.w[k]);
            }
            if (tiltMod > 1e-4 || tiltMod < -1e-4)
            {
                // factor(n) = n^a is multiplicative: exp() only for the primes, products for the rest
                const double alpha = tiltMod * kLn10Over20 / kLn2;
                double f[kMaxPartials + 1];
                f[1] = 1;
                for (int nn = 2; nn <= addCount; ++nn)
                {
                    const int p = T.spf[nn];
                    f[nn] = p == nn ? std::exp (alpha * T.logn[nn - 1]) : f[p] * f[nn / p];
                }
                for (int k = 0; k < addCount; ++k) tA[k] *= f[k + 1];
            }
        }
        for (int k = addCount; k < kMaxPartials; ++k) tA[k] = 0;
        const int addRun = std::max (addCount, v.add.count);   // ramp vanished harmonics down to 0

        // ---- transients + noise ---------------------------------------------------------
        // Only the lead player of a note renders them (scaled by sqrt(players): the energy of
        // that many independent sources); the other players would just be masked copies.
        const bool extras = ! analog && v.player == 0;
        const double plScale = std::sqrt ((double) v.plCount);
        double tLip = 0, tClick = 0, tNoise = 0, tLipG = v.lipG, tNG = v.nG;
        if (extras)
        {
            const double tChiff = v.chiffPeak * std::exp (-v.tTrans / 0.035);
            tLip   = v.lipPeak * std::exp (-v.tTrans / std::max (P.lipDecay * v.tgDecay, 0.001))
                     * clampd (1 + dst[dstLipBuzz], 0.0, 3.0) * plScale;
            tClick = v.clickPeak * std::exp (-v.tClick / 0.005) * plScale;
            tNoise = (P.brNoise * (1 - P.brDyn + P.brDyn * d) * envMain * (1 + 2 * flutNoise) * clampd (1 + dst[dstBreathNoise], 0.0, 3.0)
                      + tChiff) * 2.0 * plScale;
            if (tLip > 1e-5 || v.lipLvl > 1e-5)
                tLipG = SVF::gFor (hz0 * std::pow (2.0, P.lipTone * 4.0), sr);
            if (tNoise > 1e-6 || v.noiseLvl > 1e-6)
                tNG = SVF::gFor (M.noiseHz * std::pow (2.0, (P.brTone - 0.5) * 3.0 + P.bodyShift / 12.0)
                                 * (1 + 1.5 * tChiff / (tChiff + 0.05)), sr);
        }
        const double nK     = 1.2 / M.noiseQ;
        const bool lipOn    = extras && (tLip > 1e-5 || v.lipLvl > 1e-5);
        const bool noiseOn  = extras && (tNoise > 1e-6 || v.noiseLvl > 1e-6);
        const bool clickOn  = extras && (tClick > 1e-6 || v.clickLvl > 1e-6);

        // ---- voice filter ------------------------------------------------------------
        const double fLvl = v.fenv.advance (P.fenv, dtSec);
        double tfg = v.fg, tfk = v.fk;
        if (P.fltOn)
        {
            const double oct = P.fltKey * (v.pitch + P.transpose - 60) / 12.0 + P.fltEnvAmt * fLvl * v.fltVelScale
                               + P.fltDyn * d - 2 * dip + 3.0 * blat + 5.0 * dst[dstFltCutoff];
            tfg = SVF::gFor (P.fltCutoff * std::pow (2.0, oct), sr);
            tfk = SVF::kFor (clampd (P.fltReso + dst[dstFltReso], 0.0, 1.0));
        }

        // ---- brassiness: drive grows with dynamics ------------------------------------------
        const bool shaper = P.brsAmt > 1e-3 || P.dynBrass > 1e-3 || dst[dstBrassiness] > 1e-3;
        const int shapeType = std::min (std::max (P.brsType, 0), 2);
        const double tDrv = dbToGain (clampd (P.brsAmt * 12.0 + P.dynBrass * brightCurve * 12.0 + 12.0 * dst[dstBrassiness], 0.0, 30.0));
        const double tDrvInv = 2.0 / (shapeFn (shapeType, tDrv) - shapeFn (shapeType, -tDrv)); // full scale in -> full scale out
        int osFactor = P.oversample >= 2 ? 4 : (P.oversample == 1 ? 2 : 1);
        if (v.player > 0) osFactor = std::max (1, osFactor / 2);   // extra section players: half the oversampling
        const double toneCoef = brsToneCoef;   // global setting: computed once per chunk

        // ---- voice gain ------------------------------------------------------------------
        if (v.killing) v.kill = std::max (0.0, v.kill - dtSec / kKillTime);
        const double tgain = kVoiceGain * loud * vibAmpMul * (1 - 0.5 * dip) * v.kill * growlAM * flutAM * artGain * v.plGain
                             * clampd (1 + dst[dstAmp], 0.0, 2.0);
        // pan: the player's place plus modulation (centre = unity in both channels)
        double tPanL = v.panL, tPanR = v.panR;
        if (dst[dstPan] != 0 || v.panMod)
        {
            const double panNow = clampd (v.plPan + dst[dstPan], -1.0, 1.0);
            const double panAng = (panNow + 1) * kPi / 4;
            tPanL = std::min (1.0, kSqrt2 * std::cos (panAng));
            tPanR = std::min (1.0, kSqrt2 * std::sin (panAng));
            v.panMod = dst[dstPan] != 0;   // one more update after the route goes quiet, back to the player's place
        }

        if (v.snap)   // fresh voice: pitch / filters start at their targets; levels ramp up from 0
        {
            for (int k = 0; k < kNumOsc; ++k)
            {
                auto& o = v.o[(size_t) k];
                o.dt = tdt[k]; o.shape = tshape[k]; o.pw = tpw[k]; o.g = tg[k]; o.k = tk[k];
            }
            v.add.dt = tdtA;
            v.fg = tfg; v.fk = tfk;
            v.drv = tDrv; v.drvInv = tDrvInv;
            v.lipG = tLipG; v.nG = tNG;
            v.snap = false;
        }

        // ---- per-sample rendering with linear ramps -------------------------------------
        const double inv = 1.0 / n;
        double dt[kNumOsc], ddt[kNumOsc], lv[kNumOsc], dlv[kNumOsc], sh[kNumOsc], dsh[kNumOsc],
               pw[kNumOsc], dpw[kNumOsc], g[kNumOsc], dg[kNumOsc], kk[kNumOsc], dkk[kNumOsc];
        bool run[kNumOsc];
        for (int k = 0; k < kNumOsc; ++k)
        {
            const auto& o = v.o[(size_t) k];
            dt[k] = o.dt;    ddt[k] = (tdt[k] - o.dt) * inv;
            lv[k] = o.lvl;   dlv[k] = (tlvl[k] - o.lvl) * inv;
            sh[k] = o.shape; dsh[k] = (tshape[k] - o.shape) * inv;
            pw[k] = o.pw;    dpw[k] = (tpw[k] - o.pw) * inv;
            g[k] = o.g;      dg[k] = (tg[k] - o.g) * inv;
            kk[k] = o.k;     dkk[k] = (tk[k] - o.k) * inv;
            run[k] = P.osc[(size_t) k].on || o.lvl > 0;
        }
        double aA[kMaxPartials], daA[kMaxPartials];
        for (int k = 0; k < addRun; ++k) { aA[k] = v.add.amp[k]; daA[k] = (tA[k] - aA[k]) * inv; }
        // one rotation step for the chunk, at the chunk's mean frequency
        const double rotC = std::cos (kPi * (v.add.dt + tdtA)), rotS = std::sin (kPi * (v.add.dt + tdtA));
        double pc = v.add.c, ps = v.add.s;
        double fg = v.fg, fk = v.fk, gain = v.gain, pl = v.panL, pr = v.panR;
        const double dfg = (tfg - fg) * inv, dfk = (tfk - fk) * inv, dgain = (tgain - gain) * inv;
        const double dpl = (tPanL - pl) * inv, dpr = (tPanR - pr) * inv;
        double drv = v.drv, drvInv = v.drvInv;
        const double ddrv = (tDrv - drv) * inv, ddrvInv = (tDrvInv - drvInv) * inv;
        double lipL = v.lipLvl, lipG = v.lipG, nL = v.noiseLvl, nG = v.nG, cL = v.clickLvl;
        const double dlipL = (tLip - lipL) * inv, dlipG = (tLipG - lipG) * inv, dnL = (tNoise - nL) * inv,
                     dnG = (tNG - nG) * inv, dcL = (tClick - cL) * inv;
        const bool flt = P.fltOn;
        const int ftype = P.fltType;
        const double lipPitch = P.lipPitch;

        for (int i = 0; i < n; ++i)
        {
            double s = 0;
            for (int k = 0; k < kNumOsc; ++k)
            {
                if (! run[k]) continue;
                dt[k] += ddt[k]; lv[k] += dlv[k]; sh[k] += dsh[k]; pw[k] += dpw[k]; g[k] += dg[k]; kk[k] += dkk[k];
                auto& o = v.o[(size_t) k];
                const double x = o.osc.next (sh[k], pw[k], dt[k]);
                s += o.filt.process (x, g[k], kk[k]) * lv[k];
            }
            if (addRun > 0)
            {
                const double nc = pc * rotC - ps * rotS;   // advance the phasor first (same order as before:
                ps = ps * rotC + pc * rotS;                //  the stack is evaluated after one step)
                pc = nc;
                const double c2 = 2 * pc;
                double sPrev = 0, sCur = ps;
                aA[0] += daA[0];
                double acc = aA[0] * sCur;
                for (int k = 1; k < addRun; ++k)
                {
                    const double sNext = c2 * sCur - sPrev;
                    sPrev = sCur; sCur = sNext;
                    aA[k] += daA[k];
                    acc += aA[k] * sNext;
                }
                s += acc;
            }
            const double wn = noiseOn || lipOn || clickOn ? v.nrng.bi() : 0.0;
            if (lipOn)
            {
                lipL += dlipL; lipG += dlipG;
                const double ph = v.lipPhase;
                const double saw = 2 * ph - 1 - polyBlep (ph, dt[0]);
                double p2 = ph + dt[0]; if (p2 >= 1) p2 -= 1;
                v.lipPhase = p2;
                v.lipF.process (lipPitch * saw + (1 - lipPitch) * wn, lipG, 0.7);
                s += v.lipF.bp * lipL * 3.0;
            }
            fg += dfg; fk += dfk; gain += dgain;
            if (flt)
            {
                const double lp = v.f1.process (s, fg, fk);
                if (ftype == fltBP)        s = v.f1.bp;
                else if (ftype == fltLP24) s = v.f2.process (lp, fg, fk);
                else                       s = lp;
            }
            if (shaper)
            {
                drv += ddrv; drvInv += ddrvInv;
                const double y = v.os.process (s * drv, osFactor, shapeType) * drvInv;
                const double dc = y - v.dcX + dcR * v.dcY;   // the asymmetric shapes add DC
                v.dcX = y; v.dcY = dc;
                v.toneZ += (dc - v.toneZ) * toneCoef;
                s = v.toneZ;
            }
            if (noiseOn)
            {
                nL += dnL; nG += dnG;
                v.noiseF.process (wn, nG, nK);
                s += v.noiseF.bp * nL;
            }
            if (clickOn)
            {
                cL += dcL;
                s += (wn - v.nPrev) * cL * 0.5;
            }
            v.nPrev = wn;
            pl += dpl; pr += dpr;
            const double y = s * gain;
            mixL[(size_t) i] += y * pl;
            mixR[(size_t) i] += y * pr;
        }
        v.panL = tPanL; v.panR = tPanR;

        for (int k = 0; k < kNumOsc; ++k)
        {
            auto& o = v.o[(size_t) k];
            o.dt = tdt[k]; o.lvl = tlvl[k]; o.shape = tshape[k]; o.pw = tpw[k]; o.g = tg[k]; o.k = tk[k];
        }
        for (int k = 0; k < addRun; ++k) v.add.amp[k] = tA[k];
        v.add.count = addCount;
        v.add.dt = tdtA;
        if (addRun > 0)
        {
            const double len = 1.0 / std::sqrt (pc * pc + ps * ps);   // keep the phasor on the unit circle
            v.add.c = pc * len; v.add.s = ps * len;
        }
        v.fg = tfg; v.fk = tfk; v.gain = tgain;
        v.drv = tDrv; v.drvInv = tDrvInv;
        v.lipLvl = tLip; v.lipG = tLipG; v.noiseLvl = tNoise; v.nG = tNG; v.clickLvl = tClick;

        // ---- advance to the next chunk ---------------------------------------------------
        for (int i = 0; i < kNumLfo; ++i)
            if (P.lfo[(size_t) i].perVoice)
                v.lfo[(size_t) i].advance (dst[dstLfo1Rate + i] != 0 ? lfoInc[(size_t) i] * std::pow (2.0, 2.0 * dst[dstLfo1Rate + i])
                                                                     : lfoInc[(size_t) i], rng);
        if (v.glidePos < 1)
        {
            v.glidePos = std::min (1.0, v.glidePos + n / (std::max (P.glideTime, 0.001) * sr));
            v.pitch = v.glideFrom + (v.glideTo - v.glideFrom) * curveShape (v.glidePos, P.glideCurve);
        }
        if (! alive || (v.killing && v.kill <= 0)) hardStop (v);
    }

    std::array<Voice, kPoolSize> voices;
    std::array<double, kChunk> mixL {}, mixR {};
    std::array<int, 128> keys {};
    int keyCount = 0;
    EngineParams params;
    Rng rng;
    double sr = 48000, masterGain = 0.7, masterSmooth = 0.001;
    double wheelSmooth = 0.5, wheel = 0, wheelTarget = 0, driftSmooth = 0.1, dynCoef = 1, busSmooth = 0.1, dcR = 0.999;
    double breath = 0, expression = 1, aftertouch = 0;
    bool seenBreath = false, seenExpr = false, seenAT = false;
    double bendGlobal = 0;
    std::array<double, kNumChannels> bendCh {};
    std::array<double, 128> ccValue {};
    // bus
    std::array<BiquadCoefs, kBodyBands> bodyC {}, muteC {};
    std::array<Biquad, kBodyBands> bodyL {}, bodyR {}, muteL {}, muteR {};
    SVF plL, plR;
    double busKey = -1, muteGain = 1;
    int muteKind = -1;
    double plungerCur = 0.5, plungerFrom = 0.5, bodyMixCur = 0, bodyMixFrom = 0, muteMixCur = 0, muteMixFrom = 0;
    double bpm = 120, ppqAtBlock = 0;
    bool playing = false;
    // modulation
    struct ActiveSlot { int src, via, dst; double amt; };
    std::array<Lfo, kNumLfo> lfoG;
    std::array<double, kNumLfo> lfoGlobal {}, lfoInc {};
    std::array<double, kNumModSlots> slotAmt {};
    std::array<ActiveSlot, kNumModSlots> activeSlots {};
    std::array<double, kNumModDests> gDst {};
    int numActive = 0, samplesIntoBlock = 0;
    double brsToneCoef = 1;
    double ctlSmooth = 0.5, lfoSmooth = 0.5;
    // FX
    Chorus chorus;
    Reverb reverb;
    Oversampler fxOsL, fxOsR;
    std::array<BiquadCoefs, 3> eqC {};
    std::array<Biquad, 3> eqL {}, eqR {};
    double ampCur = 1, drvG = 1, drvComp = 1, drvCompTarget = 1, drvMixCur = 1, drvLpL = 0, drvLpR = 0, drvPin = 0, drvPout = 0;
    double drvDcXL = 0, drvDcYL = 0, drvDcXR = 0, drvDcYR = 0;
    double eqKey = -1e300, chMixCur = 0, rvMixCur = 0, rvNorm = 1, rvDecaySet = -1, widthCur = 1;
    int rvTypeSet = -1;
    bool drvWasOn = false, eqWasOn = false, chWasOn = false, rvWasOn = false, fxInit = true;

    static constexpr int kSpecCache = 32;                // power of two
    std::array<AddSpectrum, kSpecCache> specCache {};    // per-note spectra (see AddSpectrum)
    uint64_t chunkStamp = 0;
    double artSmooth = 0.5, now = 0, lastAllUp = -1e9;   // engine time (s), time all keys went up
    int latchedArt = artNormal, pendingAtk = artNormal;
    int lastNote = -1, monoGroup = 0, groupCounter = 0;
    double lastPitch = 60, lastDyn = 0;
    uint64_t ageCounter = 0;
    bool sustainPedal = false, firstParams = true;
    int active = 0;
    float peak = 0;
};
} // namespace bk
