# Brass Knuckles — build plan

Status (2026-11-18): plan approved for phases 1-7. **All seven phases done** (VST v1.0 release candidate, web build,
physical-model engine); awaiting hand-testing.

Phase 7 notes:
- Engine = Physical (third choice, appended) + 6 parameters appended after stWidth (pmType, pmBlow, pmLip, pmLoss,
  pmBell, pmNoise; 347 total). 10 presets in a new "Physical" category, appended after the 120 (program numbers of
  the existing presets unchanged).
- Prototyped in Node first (pitch by 1-cent DFT search, odd-harmonic share to catch octave jumps, onset, level,
  harmonic centroid vs pressure):
  - STK-style lip filter in a delay loop: silent below ~G4 without excitation; with noise, pitch off by hundreds
    of cents. Explicit mass-spring lips + Bernoulli flow + cylindrical bore: oscillates everywhere but plays
    300-700 cents sharp, lip-dominated, pressure-dependent.
  - Saxofony-style reed on a two-delay bore: in tune within ~15 cents but overblows to the octave at many
    note / pressure / blow-position combinations, and is slow to speak at moderate pressure.
  - Chosen: **driven valve** (lips / reed moved at the played pitch, Bernoulli flow solved against the bore wave in
    closed form, bore = one-period delay with a bell one-pole low-pass, loss and a DC blocker in the reflection).
    In tune by construction; level even across the range (±1 dB C2..C6); louder and brighter with pressure.
    Brightness for the lips comes from the closure shape (looser at high dynamics); a sharper pulse measured
    darker, because the bore then limits the flow during most of the open phase.
- Bore buffers: one float buffer per voice (power of two >= sr / 20 Hz), one pool allocated in prepare().
  Float storage, a rotating phasor for the valve and only + - * / sqrt in the loop keep C++ and JS identical
  (174 parity scenarios, worst 3.5e-6).
- Brass checks treat pmNoise as breath noise (switched off for the measurement).
- CPU: the physical voice is cheaper than the modeled one (about 0.5% of a core per phrase in C++; JS 7% for a
  12-note chord, 16% for the 4-player section preset).

Phase 6 notes:
- `dsp-worklet.js` is a line-for-line port of `SynthEngine.h` + `dsp/*.h`; parameter values are rounded to float32
  like the plugin's. Parity: 158 scenarios (38 feature scenarios + every factory preset) agree within 3.5e-6
  (tolerance 1e-4). The C++ side renders each scenario with a fresh engine: `prepare()` does not reset
  `lastNote` / `lastPitch`, so a reused engine made mono presets glide in from the previous scenario's note.
- MIDI routing moved to `Source/MidiRouting.h` so the parity tool uses the plugin's own dispatch.
- JS speed: ~2.6x slower than C++ in steady state (Node / V8). Running the engine inside a Node `vm` sandbox made it
  ~10x slower (every global lookup such as `Math.sin` goes through an interceptor), so the Node tools use the main
  context. Browser: solo presets 2-14%, 4-note section chords 12-22%, 8-player sections with a 12-note chord ~63% of
  a core; the synthetic worst case (32 x 8 x 4x) ~215%, i.e. not playable in a browser. Further speed-ups would have
  to change the order of floating-point operations (and break bit-level parity), so they were not made.
- UI tests run the built page twice in headless Edge: from disk with virtual time (results read from the DOM dump)
  and served over http in real time (virtual time pauses the audio thread; the page POSTs its results), where the
  AudioWorklet must actually play a note.
- Browsers block AudioWorklet modules on `file://`; the page then renders on the main thread (ScriptProcessor).
  `start.bat` serves it for the worklet path.

Phase 5 notes:
- 120 presets from per-instrument templates; level-matched to -18.05 .. -17.95 dB.
- CPU: extra section players skip lip / noise / clicks (the lead player carries them, scaled by sqrt(players)).
  Worst case 91% -> 88%; real multi-player presets with 12-note chords 3-25%. The 75% target for the synthetic worst
  case was NOT reached without audible compromises (no oversampling or fewer harmonics for extra players); left as is.
- Bug found: `render (v, phrase (..., len), len)` read `len` before phrase() set it (unspecified argument order), so
  `--render-all` WAVs from phases 1-4 were empty. Fixed.
- Click detector: high band (>16 kHz) in windows of 2 periods of the lowest note, judged against both sides, only
  around note changes. Plain 4th-difference or short windows mistake every sharp waveform edge for a click.

Phase 4 notes:
- Patch level now sits before the FX (master after), so drive and reverb see every preset at the same level.
- Drive level-matching is adaptive (input vs output power, ~300 ms) rather than a fixed curve: a fixed
  compensation was +6 dB off for quiet signals.
- Global destinations (plunger, body shift, FX) take global sources only; per-voice routes to them are ignored.
- The harmonic-tilt route uses n^a = product over prime factors (11 exp instead of 32 per voice per chunk).
- CPU worst case with everything: 91% of one core (32 notes x 8 players x 4x, every FX, 12 routes). Phase 5 target:
  bring it under ~75% (candidates: skip the spectrum for silent delayed players, cheaper oscillators for players 2+).

Phase 3 notes:
- Analog unison is built from the section-player mechanism (players = section players × unison, max 8), so it
  gets stereo spread for free and needs no stereo path inside a voice.
- CPU for the brief's worst case (full polyphony × 8 players × 4x): 110% -> 85% of one core after: harmonic
  stack driven by a rotating phasor (no sin/cos per sample), per-note spectrum shared by its players, log tables,
  extra players at 12 harmonics and half the oversampling. 16 notes x 8 players: 42%.
- MPE slide is wired to brightness (+-1.5 oct) until the mod matrix (phase 4) offers it as a source.

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
web/index.html                      (generated by vst/webhost/build-web.ps1)
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
  webhost/ dsp-worklet.js  factory.js  web-host.js  build-web.ps1  dev.html (unbundled dev page)
  presets/factory_presets.json
  tests/ PresetTests.cpp  EngineTools.cpp  HostCheck.cpp  Parity.cpp  parity_scenarios.json  parity-runner.js
         parity-compare.js  run-parity.ps1  ui-tests.js  run-ui-tests.ps1  js-bench.js
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
7. **Physical model.** Lip-reed / single-reed voice as a third engine (driven-valve waveguide; see the Phase 7 notes).
