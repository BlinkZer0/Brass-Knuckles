#pragma once
// Maps raw parameter values (indexed by ParamIndex) to the engine's snapshot.
// Shared by the plugin (atomic APVTS values) and the console tests.
#include "Params.h"
#include "SynthEngine.h"

namespace bk
{
template <typename GetValue> // float (int paramIndex)
EngineParams makeEngineParams (GetValue&& get)
{
    auto num    = [&get] (int index) { return (double) get (index); };
    auto choice = [&get] (int index, int maxIndex) { return juce::jlimit (0, maxIndex, (int) std::lround (get (index))); };
    auto on     = [&get] (int index) { return get (index) >= 0.5f; };
    auto inum   = [&get] (int index) { return (int) std::lround (get (index)); };

    EngineParams p;
    p.instrument = choice (instrument, instrumentChoices().size() - 1);
    p.engineMode = choice (engineMode, engineChoices().size() - 1);
    p.ampGain    = std::pow (10.0, num (ampLevel) / 20.0);
    p.master     = num (master);
    p.tuneSemis  = num (tune) / 100.0;
    p.transpose  = inum (transpose);
    p.polyphony  = inum (polyphony);
    p.voiceMode  = choice (voiceMode, 2);
    p.glideOn    = on (glideOn);
    p.glideLegatoOnly = choice (glideMode, 1) == 1;
    p.glideTime  = num (glideTime);
    p.glideCurve = num (glideCurve);
    p.slurDip    = num (slurDip);
    p.bendRange  = inum (bendRange);
    p.mpeOn      = on (mpeOn);
    p.mpeBend    = inum (mpeBend);
    p.oversample = choice (oversample, 2);

    for (int k = 0; k < kNumOsc; ++k)
    {
        auto f = [k, &num] (int field) { return num (oscParam (k, field)); };
        auto& o = p.osc[(size_t) k];
        o.on      = on (oscParam (k, oOn));
        o.shape   = f (oShape);
        o.pw      = f (oPW);
        o.semis   = f (oPitch) + f (oFine) / 100.0;
        o.level   = f (oLevel);
        o.cutHarm = f (oCut);
        o.res     = f (oRes);
        o.amp.dly = f (oAmpDly); o.amp.atk = f (oAmpAtk); o.amp.hld = f (oAmpHld); o.amp.dec = f (oAmpDec);
        o.amp.sus = f (oAmpSus); o.amp.rel = f (oAmpRel);
        o.amp.atkC = f (oAmpAtkCrv); o.amp.decC = f (oAmpDecCrv); o.amp.relC = f (oAmpRelCrv);
        o.ampVel = f (oAmpVel); o.ampVelTime = f (oAmpVelTime); o.ampKeyTime = f (oAmpKeyTime); o.ampKeyAmt = f (oAmpKeyAmt);
        o.pAmt = f (oPEnvAmt); o.pTime = f (oPEnvTime); o.pCrv = f (oPEnvCrv);
        o.blipAmt = f (oBlipAmt); o.blipTime = f (oBlipTime); o.pVel = f (oPEnvVel); o.pKeyTime = f (oPEnvKeyTime);
        o.tmb.dly = f (oTEnvDly); o.tmb.atk = f (oTEnvAtk); o.tmb.hld = 0; o.tmb.dec = f (oTEnvDec);
        o.tmb.sus = f (oTEnvSus); o.tmb.rel = f (oTEnvRel);
        o.tmb.atkC = o.tmb.decC = o.tmb.relC = f (oTEnvCrv);
        o.tCut = f (oTEnvCut); o.tShape = f (oTEnvShape); o.tVel = f (oTEnvVel); o.tKeyTime = f (oTEnvKeyTime);
    }

    p.wBreath    = num (dynBreath);
    p.wExpr      = num (dynExpr);
    p.wAT        = num (dynAT);
    p.wMPE       = num (dynMPE);
    p.dynVel     = num (dynVel);
    p.breathCC   = inum (dynBreathCC);
    p.dynCurve   = num (dynCurve);
    p.dynRange   = num (dynRange);
    p.dynBright  = num (dynBright);
    p.dynBrCurve = num (dynBrCurve);
    p.dynBrass   = num (dynBrass);
    p.dynAttack  = num (dynAttack);
    p.dynSmooth  = num (dynSmooth);

    p.fltOn      = on (fltOn);
    p.fltType    = choice (fltType, 2);
    p.fltCutoff  = num (fltCutoff);
    p.fltReso    = num (fltReso);
    p.fltKey     = num (fltKey);
    p.fltDyn     = num (fltDyn);
    p.fltEnvAmt  = num (fltEnvAmt);
    p.fenv.dly = 0; p.fenv.atk = num (fltAtk); p.fenv.hld = 0; p.fenv.dec = num (fltDec);
    p.fenv.sus = num (fltSus); p.fenv.rel = num (fltRel);
    p.fenv.atkC = 0.3; p.fenv.decC = 0.6; p.fenv.relC = 0.6;
    p.fltVel     = num (fltVel);

    p.addOn        = on (addOn);
    p.addLevel     = num (addLevel);
    p.addPartials  = inum (addPartials);
    p.addTilt      = num (addTilt);
    p.addOddEven   = num (addOddEven);
    p.addSpecDly   = num (addSpecDly);
    p.addDynSpread = num (addDynSpread);
    p.addCurve     = num (addCurve);
    for (int b = 0; b < kNumBands; ++b)
    {
        auto& B = p.band[(size_t) b];
        B.atk = num (bandParam (b, 0)); B.dec = num (bandParam (b, 1)); B.sus = num (bandParam (b, 2));
        B.rel = num (bandParam (b, 3)); B.lvl = num (bandParam (b, 4));
    }

    p.brsAmt  = num (brsAmt);
    p.brsType = choice (brsType, 2);
    p.brsTone = num (brsTone);

    p.lipAmt   = num (lipAmt);
    p.lipTone  = num (lipTone);
    p.lipDecay = num (lipDecay);
    p.lipPitch = num (lipPitch);
    p.brNoise  = num (brNoise);
    p.brTone   = num (brTone);
    p.brDyn    = num (brDyn);
    p.keyClick = num (keyClick);
    p.chiff    = num (chiff);
    p.tongue   = choice (tongue, 2);

    p.bodyMix   = num (bodyMix);
    p.bodyShift = num (bodyShift);
    p.bodyReso  = num (bodyReso);
    p.muteType    = choice (muteType, 4);
    p.muteMix     = num (muteMix);
    p.mutePlunger = num (mutePlunger);
    p.muteSrc     = choice (muteSrc, 5);
    p.muteCC      = inum (muteCC);

    p.growlAmt   = num (growlAmt);
    p.growlRate  = num (growlRate);
    p.growlRough = num (growlRough);
    p.growlDyn   = num (growlDyn);
    p.flutAmt    = num (flutAmt);
    p.flutRate   = num (flutRate);

    p.artKs     = on (artKs);
    p.artKsBase = inum (artKsBase);
    p.fallRng   = num (artFallRng);
    p.fallTime  = num (artFallTime);
    p.doitRng   = num (artDoitRng);
    p.doitTime  = num (artDoitTime);
    p.ripRng    = num (artRipRng);
    p.ripTime   = num (artRipTime);
    p.shakeRate = num (artShakeRate);
    p.shakeRng  = num (artShakeRng);
    p.scoopRng  = num (artScoopRng);
    p.scoopTime = num (artScoopTime);
    p.artAuto   = on (artAuto);
    p.autoGap   = num (artAutoGap);
    p.autoLo    = inum (artAutoLo);
    p.autoHi    = inum (artAutoHi);

    p.secPlayers   = inum (secPlayers);
    p.secDetune    = num (secDetune);
    p.secTiming    = num (secTiming);
    p.secVibDesync = num (secVibDesync);
    p.secTimbre    = num (secTimbre);
    p.secWidth     = num (secWidth);
    p.anaUnison    = inum (anaUnison);
    p.anaDetune    = num (anaDetune);
    p.anaSpread    = num (anaSpread);
    p.anaBlat      = num (anaBlat);

    for (int i = 0; i < kNumLfo; ++i)
    {
        auto& l = p.lfo[(size_t) i];
        l.on       = on (lfoParam (i, 0));
        l.wave     = choice (lfoParam (i, 1), 5);
        l.rate     = num (lfoParam (i, 2));
        l.sync     = on (lfoParam (i, 3));
        l.div      = choice (lfoParam (i, 4), kNumLfoDivs - 1);
        l.perVoice = choice (lfoParam (i, 5), 1) == 1;
        l.retrig   = choice (lfoParam (i, 6), 1) == 1;
        l.phase    = num (lfoParam (i, 7)) / 360.0;
        l.amount   = num (lfoParam (i, 8));
    }
    for (int s = 0; s < kNumModSlots; ++s)
    {
        auto& m = p.mod[(size_t) s];
        m.on     = on (modParam (s, 0));
        m.src    = choice (modParam (s, 1), kNumModSources - 1);
        m.via    = choice (modParam (s, 2), kNumModSources - 1);
        m.dst    = choice (modParam (s, 3), kNumModDests - 1);
        m.amount = num (modParam (s, 4));
    }

    p.fxDrvOn   = on (fxDrvOn);
    p.fxDrvType = choice (fxDrvType, 1);
    p.fxDrvAmt  = num (fxDrvAmt);
    p.fxDrvTone = num (fxDrvTone);
    p.fxDrvMix  = num (fxDrvMix);
    p.eqOn      = on (eqOn);
    p.eqLoGain  = num (eqLoGain);
    p.eqLoFreq  = num (eqLoFreq);
    p.eqMidGain = num (eqMidGain);
    p.eqMidFreq = num (eqMidFreq);
    p.eqMidQ    = num (eqMidQ);
    p.eqHiGain  = num (eqHiGain);
    p.eqHiFreq  = num (eqHiFreq);
    p.chOn      = on (chOn);
    p.chRate    = num (chRate);
    p.chDepth   = num (chDepth);
    p.chMix     = num (chMix);
    p.rvOn      = on (rvOn);
    p.rvType    = choice (rvType, 1);
    p.rvDecay   = num (rvDecay);
    p.rvPre     = num (rvPre);
    p.rvDamp    = num (rvDamp);
    p.rvMix     = num (rvMix);
    p.stWidth   = num (stWidth);

    p.pmType  = choice (pmType, 2);
    p.pmBlow  = num (pmBlow);
    p.pmLip   = num (pmLip);
    p.pmLoss  = num (pmLoss);
    p.pmBell  = num (pmBell);
    p.pmNoise = num (pmNoise);

    p.vibDepth = num (vibDepth);
    p.vibAmp   = num (vibAmp);
    p.vibRate  = num (vibRate);
    p.vibDelay = num (vibDelay);
    p.vibFade  = num (vibFade);
    p.vibDrift = num (vibDrift);
    p.vibWheel = num (vibWheel);
    return p;
}
} // namespace bk
