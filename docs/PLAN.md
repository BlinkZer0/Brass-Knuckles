# Brass Knuckles — build plan

Status (2026-11-17): plan approved for phases 1-7. **Phases 1 and 2 done**, awaiting hand-testing in Minihost
Modular before Phase 3.

Phase 2 notes:
- Output clipper changed from tanh to a soft knee that is exactly linear below -1.9 dBFS (the tanh coloured and
  aliased every note slightly).
- Brassiness drive capped at +24 dB. The aliasing check measures the nonlinear stage with the alias-free harmonic
  layer as the source; PolyBLEP oscillators alone sit around -40 to -54 dB at C6, so modeled presets use the
  harmonic layer for brightness and keep the oscillators darker.
- CPU, 32 notes with every per-voice feature: 17-22%. Section mode (x8 players) needs cheaper players in phase 3
  (e.g. fewer harmonics and spectrum updates every other chunk for players 2+).
- Brightness checks use the harmonic centroid (tone only), because breath noise inflates a plain centroid.

Phase 1 notes:
- The full parameter table (341 params, every phase except the physical model) is defined and frozen now, so
  projects saved during testing keep loading. Params for later phases exist but do nothing yet.
- 12 starter presets (one per final category); the 120 come in phase 5.
- Extra check beyond the plan: `--host-check` loads the built VST3 through JUCE's VST3 hosting (scan, MIDI + CC,
  automation during playback, state round trip, programs).

## Architecture

```
MIDI / MPE / CC2 / CC11 / AT ──► Dynamics blender (per voice, continuous, smoothed)
                                      │ loudness · brightness · brassiness · attack speed
per voice (× section players 1–8):    ▼
  ┌ Modeled ─────────────────────────────────────────────────────────────┐
  │ OSC 1-3 (morph sine→tri→saw→pulse, PolyBLEP)                         │
  │   each: Amp env DAHDSR+curves · Pitch env (scoop/blip) · Timbre env  │
  │ Additive layer: 32 partials, low/mid/high band envs, spectral delay  │
  │ Lip/reed transient · breath noise (formant-shaped) · key click/chiff │
  │ Vibrato (pitch/amp, drift) · growl · flutter · articulations · glide │
  │ ─► brightness filter (SVF) ─► brassiness shaper (ADAA, 2×/4× OS)     │
  ├ Analog brass ────────────────────────────────────────────────────────┤
  │ saw stacks + unison ─► SVF/ladder with "blat" filter env ─► drive    │
  ├ Physical model (later) ─ interface stub only ────────────────────────┤
  └──────────────────────────────────────────────────────── ─► pan ─────┘
global (stereo bus): body/bell formant bank ─► mutes (straight/cup/harmon/plunger)
  ─► FX: drive(tube/tape) ─► 3-band EQ ─► chorus ─► plate/hall reverb ─► width
  ─► level × master ─► soft clip
```

Key decisions:
- **Header-only `SynthEngine.h`, double precision, 32-sample control rate with per-sample ramps**, same
  as E-Piano, so the JS port can be a line-for-line twin. Split into a few headers (`dsp/Envelope.h`,
  `dsp/Additive.h`, `dsp/Oversampler.h`, `dsp/Reverb.h` …) because it will be ~5× larger.
- **Body formants and mutes are linear and pitch-independent, so they run once on the stereo bus**, not
  per voice. That keeps CPU down at 32 voices × 8 players. The plunger wah is a CC-driven filter on the same bus.
- **Brassiness shaper is per voice** (it depends on that voice's dynamics), oversampled with polyphase
  half-band IIR filters, plus ADAA. The C++ and JS versions use the same coefficients.
- **Instrument models** are data: a table per instrument (spectral envelope, formant set, range, default
  vibrato/transient character). The `instrument` param picks one; presets tweak from there.
- **Section mode**: each player is a lightweight sub-voice (own phase, detune, timing offset, vibrato
  phase, timbre offset, pan) sharing its note's envelopes and settings. Partial count drops for players 2+
  if the benchmark needs it.
- **MPE**: voices remember their MIDI channel; per-channel bend/pressure/slide feed the dynamics blender
  and the mod matrix. Standard (non-MPE) mode uses channel-wide values.
- **Parameter IDs**: append-only with neutral defaults, as in E-Piano. I treat the table as frozen
  from the first build you load in Minihost, so test projects keep working.
- Plugin identity: company "BrassKnuckles", manufacturer code `Bknk`, plugin code `Bk01`.
- JUCE: same pin (8.0.17-security). `build.bat` reuses an existing JUCE download if one exists, otherwise fetches it.

## File tree

```
README.md  .gitignore  start.bat  serve.ps1
docs/   CONVENTIONS.md  PLAN.md  screenshots
web/index.html                      (generated in phase 6)
vst/
  CMakeLists.txt  build.bat  install.bat  README.md
  Source/
    Params.h                        parameter table (mirrors ui/js/params.js)
    EngineParamsFromValues.h
    SynthEngine.h                   engine, voices, allocation, section, MPE
    dsp/ Envelope.h  Oscillator.h  Additive.h  Filters.h  Shaper.h  Oversampler.h
         Noise.h  Formants.h  Articulations.h  Fx.h  Reverb.h  Instruments.h  PhysModel.h (stub)
    PresetManager.*  PluginProcessor.*  PluginEditor.*
  ui/  index.html  css/style.css
       js/ params.js engine-juce.js knob.js controls.js envelope.js harmonics.js piano.js app.js
  webhost/ dsp-worklet.js  web-host.js  build-web.ps1  dev.html (unbundled dev page)
  presets/factory_presets.json
  tests/ PresetTests.cpp  EngineTools.cpp  BrassChecks.cpp  parity_scenarios.json
         run-parity.ps1  run-ui-tests.ps1
```

## Dev loop

- **Engine**: Standalone exe (no DAW), plus `BrassKnucklesTests --render <preset> <scenario> out.wav`
  for offline renders I can measure and you can audition.
- **UI**: `BK_UI_DIR=…\vst\ui` (live reload in plugin/Standalone); `start.bat` serves a UI preview in the
  browser. From phase 6, `webhost/dev.html` runs the real JS engine unbundled.

## Phases (each ends with a summary + "what to listen for" in Minihost Modular)

1. **Skeleton + core voice.** CMake, build/install, full param table, processor/editor/bridge, preset manager
   (`.bkpreset`, migration, user folder), brass/gunmetal UI shell with SYNTH page and keyboard. Engine: 3
   envelope oscillators (amp/pitch/timbre envs), dynamics blender (velocity/CC2/CC11/AT) driving loudness,
   brightness and attack, continuous breath swells, vibrato, poly/mono/legato + glide, pitch bend. Tests: preset
   basics, `--dump-params` diff, `--render`.
2. **Modeled realism.** Additive harmonic layer and spectral delay macro, brassiness shaper with 2×/4×
   oversampling, lip/reed transients, breath noise, key click/chiff, tongue types, instrument models
   (formants/body), mutes and plunger wah, growl, flutter tongue.
3. **Playing and sections.** Keyswitched and automatic falls, doits, rips, shakes and scoops (keyswitch octave
   shown on the keyboard), lip-slur legato, MPE, section mode 2–8 players, Analog Brass engine mode, realistic
   range shading, physical-model stub.
4. **Modulation, FX, global.** 2 LFOs and mod matrix (all envelopes, LFOs, dynamics sources, MPE, wheel, key,
   random → any continuous engine param), FX chain (level-matched), polyphony setting, voice stealing,
   remaining UI pages (OSC/ENV, HARMONICS, BODY, MOD, FX, PLAY).
5. **Presets + verification → VST v1.0 candidate.** 120 level-matched presets (12 × 10), preset tests,
   brass checks (centroid rises with dynamics, legato click detector, aliasing threshold), `--bench` (32 voices
   × 8 players × 4× OS), README with screenshots and breath controller setup.
6. **Web build.** JS twin `dsp-worklet.js`, `web-host.js`, `build-web.ps1`, `start.bat`/`serve.ps1`, parity
   scenarios and `run-parity.ps1`, headless UI tests.
7. **Later.** Lip-reed / single-reed waveguide voice behind the stubbed interface.
