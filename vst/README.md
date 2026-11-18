# Brass Knuckles — VST3 plugin (JUCE 8)

This folder is the source of truth for Brass Knuckles:
- **C++ DSP**: the plugin's sound engine (`Source/SynthEngine.h`, `Source/dsp/`).
- **HTML/SVG UI**: shown inside the plugin through WebView2.
- **Web host**: runs the same UI in a browser with a JS twin of the engine (`webhost/`, built into `../web/index.html`).

## Build

```
build.bat            VST3 + Standalone + BrassKnucklesTests (Release)
build.bat Debug
install.bat          copies "Brass Knuckles.vst3" to C:\Program Files\Common Files\VST3 (asks for admin)
```

Build files go to `%LOCALAPPDATA%\BrassKnucklesVST` so OneDrive doesn't sync them. If E-Piano already downloaded
JUCE 8.0.17 (`%LOCALAPPDATA%\EPianoVST\deps\juce-src`), that copy is used instead of downloading it again.

- `...\build\BrassKnucklesVST_artefacts\Release\VST3\Brass Knuckles.vst3`
- `...\build\BrassKnucklesVST_artefacts\Release\Standalone\Brass Knuckles.exe`: *Options → Audio/MIDI Settings* for
  the audio device and MIDI input
- `...\build\BrassKnucklesTests_artefacts\Release\BrassKnucklesTests.exe`

## Signal flow

```
dynamics (per voice, every 32 samples) = velocity, or breath/expression/aftertouch/pressure x velocity colour
   -> loudness (dB range) · brightness (octaves, own curve) · attack speed (at note-on)

per voice:  OSC 1-3  morph sine/tri/saw/pulse
              each: Amp env (DAHDSR + curves) · Pitch env (scoop + blip) · Timbre env (brightness, shape)
              -> brightness SVF (cutoff in harmonics x 2^(timbre env + dynamics brightness))
          + harmonic layer: 32 harmonics (Chebyshev recurrence from one phase), 9 envelope nodes
              (low/mid/high bands + spectral delay), instrument spectrum, slope follows dynamics
          + lip buzz / reed onset (pitched saw + noise -> band-pass, decaying)
            -> voice filter (optional LP12 / LP24 / BP, filter env, key, dynamics)
            -> brassiness: drive 0..+24 dB (amount + dynamics), Brass/Reed/Edge rational shapers,
               1x / 2x / 4x oversampled (Kaiser half-band FIRs), DC blocker, tone low-pass
          + breath noise (instrument noise band, follows dynamics and the amp env; chiff) + key click
            -> x loudness x vibrato amp x growl x flutter x lip-slur dip -> pan
  (Engine = Physical: the oscillators / harmonic layer / lip buzz are replaced by a driven-valve waveguide:
   valve (lips or reed) moved at OSC 1's pitch, pulse shape from lip tension and dynamics -> Bernoulli flow solved
   against the returning bore wave (closed form, sqrt only) -> bore delay (float, one period) -> bell one-pole
   low-pass + reflection loss, mean flow not reflected -> radiated wave (DC blocked). Blowing pressure = OSC 1
   amp env x (0.2 + 0.8 dynamics) x Blow. The rest of the chain is unchanged.)
bus:        sum -> body/bell formants (4 biquads per instrument, mix/shift/reso)
            -> mute (straight / cup / harmon biquads, plunger = moving SVF low-pass)
            -> patch level -> FX: drive (tube/tape, 1x/2x/4x, auto level-matched) -> 3-band EQ -> chorus
               -> reverb (plate/hall FDN) -> width -> master (smoothed) -> safety clip (linear below -1.9 dBFS)
modulation: global LFOs + matrix routes every 32 samples; each voice evaluates its own sources (per-voice LFOs,
            envelopes as of the last chunk, velocity, dynamics, controllers, MPE, key, random, vibrato) and sums
            the routes into its destinations; plunger / body shift / FX destinations use global sources only.
```

Reverb and EQ / body / mute biquad feedback paths flush values below 1e-30 to exactly 0 (a compare, so fast-math
can't remove it), so long tails never turn into denormals; the tests check a reverb tail ends at exactly 0.

Body formants and mutes are linear and pitch-independent, so they run once on the stereo bus, not per voice.

**Note groups and players.** One played note = one group of 1-8 voices ("players"): section players, or unison
voices in the Analog engine (players = section players × unison, at most 8). Each player gets a detune, onset /
release delay, vibrato rate and delay, brightness and level offset (section), and a pan position; levels are
scaled by 1/√players. Polyphony counts notes. To keep 8-player sections affordable, a note's harmonic spectrum is
computed once per chunk and shared by its players, extra players render 12 harmonics, and they use half the
lead player's oversampling.

**Articulations.** Keyswitches (Keyswitch Base + 0..5: normal, fall, doit, rip, shake, scoop) latch an articulation
and never sound. Rip / scoop shape the pitch of new attacks, shake the sustain; fall / doit defer the release: the
voice keeps sounding while it bends and fades over the fall time, then its envelopes release. Section players also
release up to 60% of the section timing apart, through the same deferred release.
Instrument data (spectra, slopes, formants, noise bands) is in `Source/dsp/Instruments.h`.

Envelopes, dynamics, vibrato and pitch are evaluated every 32 samples; oscillator increments, levels, filter
coefficients and gain are ramped per sample in between. Voices are allocated in **note groups** (one played note
= one group) so section mode can add players per note in Phase 3.

## In the host

- Instrument (Instrument|Synth), MIDI in, stereo out. **347 automatable parameters** with stable IDs; knob and
  envelope-point drags record as single automation gestures.
- **Right-click any control** for the host's parameter menu (automation, link to controller). The Standalone app
  gets a small fallback menu with *Reset to default*. Shift+right-click opens DevTools.
- MIDI: notes, velocity, pitch bend, mod wheel (CC1), breath (CC2 or the **Breath CC** parameter), expression
  (CC11), sustain (CC64), channel and polyphonic aftertouch, all notes / sound off, reset all controllers. Events
  are applied at the exact sample they arrive.
- **Programs**: the factory presets appear in the host's program list as `Category - Name`.
- **State**: the project saves every parameter plus the selected preset.

## Presets

- **Factory**: [presets/factory_presets.json](presets/factory_presets.json) uses **templates** (`acoustic` →
  `brass` / `reed`) plus per-preset overrides: values = defaults ← template chain ← preset. `ampLevel` comes from
  `tests\level-match.ps1`.
- **User folder**: `Documents\Brass Knuckles\Presets`, subfolders = categories, rescanned every ~2 s. SAVE asks
  before replacing a file; the × on a user preset moves it to the Recycle Bin.
- **File format** (`.bkpreset`, plain JSON). Missing parameters load at their defaults, values are clamped, master
  is never stored. `PresetManager::migrate()` brings older versions up to date (the web build runs the same steps: `bkMigratePreset` in `webhost/factory.js`):
  ```json
  { "format": "Brass Knuckles Preset", "version": 1, "name": "My Horn", "category": "User",
    "params": { "instrument": 0, "osc1Shape": 2.4, "dynBright": 4.5, "vibDepth": 14 } }
  ```

## Tests

| Command | Checks |
|---|---|
| `BrassKnucklesTests.exe` | parameter table, factory presets (ranges, 13 categories, templates resolved), preset file round trip, partial / old / garbage files, engine basics (velocity, breath silence and swell, reset controllers, polyphony limit, legato glide pitch), modeled engine (half-band filters unity, harmonic layer brightens with dynamics, spectral delay holds the top back early, brassiness adds harmonics, straight mute cuts lows, plunger closed vs open, breath noise, key click/chiff, tongue, growl), every preset renders (not silent, no clipping, tail ends), brass checks, aliasing |
| `BrassKnucklesTests.exe --brass` | per preset: harmonic centroid (brightness of the tone, ignoring breath noise) rises with velocity (≥8% per step), RMS rises with velocity, a breath swell makes a held note louder **and** brighter; plus the aliasing check: C6 at velocity 127, the brassiness stage fed by the harmonic layer, inharmonic energy below 18 kHz < −60 dB at the preset's drive and < −50 dB at full drive with 4× |
| `BrassKnucklesTests.exe --host-check "<path>\Brass Knuckles.vst3"` | loads the built VST3 like a DAW: scans as an instrument, all parameters named, MIDI + breath CC make sound while parameters are automated, state restores exactly, programs switch |
| `BrassKnucklesTests.exe` (Phase 3 part) | keyswitches are silent and latch; fall bends down after release and ends; doit bends up; scoop / rip start below; shake trills; normal resets; keyswitches off = the key plays; auto scoop / no scoop / auto fall; 4 section players: one note, decorrelated stereo, level within 3 dB of one player; 32 notes × 8 players fit the pool; MPE member-channel vs master bend; analog unison width and blat |
| `BrassKnucklesTests.exe` (Phase 4 part) | LFO → pitch wobbles; via mod wheel gates it; random → pan places notes apart; synced 1/8 LFO gives 8 gates in 2 s at 120 BPM; mod wheel → plunger (global destination); reverb tail exists, is level-matched and ends at exactly 0; drive is level-matched and adds upper harmonics; EQ low shelf; chorus decorrelates and keeps the level; width 0 = mono, 2 = wider |
| `BrassKnucklesTests.exe` (Phase 7 part) | physical engine, lips (trumpet) and reed (alto): C2..C6 in tune within 3 cents (DFT, 1-cent steps; currently 0), level spread < 6 dB (currently < 1), velocity 30 / 80 / 127 each ≥ 3 dB louder and ≥ 8% brighter (harmonic centroid); legato glide lands within 3 cents and the release ends at exactly 0; 4 players within 3 dB of one; full blow / no loss / open bell / full turbulence / lip ±1 / both valves at notes 21, 60, 108 with breath swells stay finite and below full scale |
| `BrassKnucklesTests.exe` (Phase 5 part) | exactly 130 presets (12 × 10 + 10 Physical), 10 per category, unique names; every preset within ±1 dB of −18 dB; legato click check (below); the brass checks measure each preset's tone (1 player, no vibrato, routes, breath noise or growl) so detune / pitch modulation / noise don't hide the dynamic response |
| `BrassKnucklesTests.exe --clicks` | every legato preset (76) plays the legato phrase with its deliberate transients and vibrato off, at 4×; the energy above 16 kHz (4th-order high-pass) in windows of two periods of the lowest note, around each note change, must not stick out above both sides by more than 4×. Detector check: a −46 dBFS step at a note change of a clean sine line scores ~10 |
| `BrassKnucklesTests.exe --hf "<Category/Name>" t0 t1` | high-band level in 2 ms windows (diagnostics for the click check) |
| `BrassKnucklesTests.exe --bench` | 32 notes with every per-voice feature (3 oscillators, 32 harmonics, LP24, brassiness, transients, noise, growl, flutter) + body + plunger + every FX + both LFOs + 12 routes: 15% of one core at 1×, 19% at 4×. **Worst case, 32 notes × 8 players × 4×: 88%**; 16 notes × 8 players: 44%. Then every multi-player factory preset with a 12-note chord: 3-25% |
| `BrassKnucklesTests.exe --bench-parts` | the 32 × 8 section case at 2× with one feature switched off at a time (profiling aid) |
| `BrassKnucklesTests.exe --loudness` | loudness of every factory preset |
| `BrassKnucklesTests.exe --render "<Category/Name>" out.wav [demo\|velocity\|breath\|legato\|chord\|articulations]` | offline render of a preset |
| `BrassKnucklesTests.exe --render-all <folder>` | every preset × demo / velocity / breath phrases |
| `powershell -File tests\check-params.ps1` | `params.js` matches `Params.h` exactly (order, IDs, ranges, defaults, units, choices) |
| `powershell -File tests\level-match.ps1` | re-renders every preset and rewrites its `ampLevel` to the loudness target |

| `BrassKnucklesTests.exe --parity tests\parity_scenarios.json` | renders the parity scenarios with the C++ engine, prints RMS per window + peak as JSON |
| `powershell -File tests\run-parity.ps1 [-Tolerance 1e-4] [-Full]` | **C++/JS parity**: 174 scenarios (44 feature scenarios, including the physical engine with both valves, sections, MPE, articulations and extreme settings, covering every engine path: dynamics, breath, MPE, articulations, sections, analog, mutes, plunger, LFOs, matrix, FX, raw MIDI routing, and one phrase per factory preset) rendered by the C++ engine and by `dsp-worklet.js` in headless Edge; RMS of each channel in 16 windows and the peak must agree within the tolerance. Currently worst 3.5·10⁻⁶. Also prints each engine's render speed |
| `powershell -File tests\run-ui-tests.ps1` | builds `web/index.html` and drives it in headless Edge, twice: from disk (ScriptProcessor) and served over http (AudioWorklet, real time). 36 checks from disk, 38 served: start-up without errors, bridge config, 120 presets, every parameter has a control, all 8 pages, knob wheel / double-click and a select reach the host and the DSP, preset step / browse / save / replace / reload / delete, `.bkpreset` file load (old version, unknown and out-of-range values) and refusal of a foreign file, on-screen and computer keys send MIDI, articulation buttons, BREATH wheel, PANIC, the page's DSP renders the loaded sound and a Physical preset; served: the AudioWorklet plays a note |
| `node tests\js-bench.js` | JS engine speed (Node runs the same V8 engine as Chrome / Edge): solo and section presets, chords, the 32 × 8 × 4× worst case |
| `node tests\parity-runner.js` | the JS half of the parity test without Edge (prints JSON; `parity-compare.js cpp.json js.json` compares) |

## Web build

`webhost/build-web.ps1` writes `../web/index.html`: `ui/index.html` with the CSS and every script inlined, plus,
right before the bridge (`engine-juce.js`), the factory presets (`FACTORY_JSON`), `factory.js`, `dsp-worklet.js` and
`web-host.js`. The UI is unchanged: `web-host.js` provides `window.__JUCE__.backend` with the plugin editor's events.

- `dsp-worklet.js`: the engine. `SynthEngine.h` and `dsp/*.h` ported line for line (same control rate, RNG, maths
  and order of operations; values rounded to float32 like the plugin's parameters), wrapped in one function that
  becomes the AudioWorklet module (or runs on the main thread through a ScriptProcessor on `file://`). It allocates
  nothing while rendering. **Any change to the C++ engine must be made here too**; `run-parity.ps1` catches drift.
- `factory.js`: resolves the factory template chain exactly like `PresetManager::loadFactory`, and `bkMigratePreset`
  (the twin of `PresetManager::migrate`).
- `web-host.js`: Web Audio, Web MIDI (raw channel messages go to the engine and take the same routing as
  `MidiRouting.h`), tempo, presets in localStorage or a linked folder (File System Access API, rescanned every 2 s),
  `.bkpreset` load / save / delete. An AudioWorklet processor error is reported in the UI.
- `dev.html`: the web build unbundled. `serve.ps1 -Dev` (repo root) serves `vst/` and opens it, so edits to `ui/`
  or `webhost/` need only a reload.

## Layout

```
CMakeLists.txt  build.bat  install.bat
Source/
  Params.h                   parameter table (347 params; mirrors ui/js/params.js)
  SynthEngine.h              engine: voices, note groups, dynamics, vibrato, glide, voice modes
  dsp/Common.h               RNG, curves, PolyBLEP, TPT SVF
  dsp/Envelope.h             DAHDSR with per-segment curves (control rate)
  dsp/Oscillator.h           morphing oscillator
  dsp/Additive.h             harmonic layer: node/harmonic tables, per-voice state
  dsp/Oversampler.h          Kaiser half-band FIRs (2x / 4x), brassiness shapers
  dsp/Filters.h              RBJ biquads (body, mutes)
  dsp/Instruments.h          instrument models (spectrum, slopes, body, noise) and mute models
  dsp/PhysModel.h            physical model (Engine = Physical): driven-valve waveguide state and helpers
  dsp/Lfo.h                  LFO shapes, sync divisions
  dsp/Fx.h                   chorus, FDN reverb, anti-denormal flush
  EngineParamsFromValues.h   parameter values -> engine snapshot (plugin + tests)
  PresetManager.*            factory templates, user folder, .bkpreset read/write/migrate
  MidiRouting.h              MIDI message -> engine (plugin + parity tool; mirrored in dsp-worklet.js)
  PluginProcessor.*          APVTS, MIDI (CC, aftertouch, poly pressure), programs, state
  PluginEditor.*             WebView2 host + JS <-> C++ bridge
ui/                          HTML/CSS/SVG/JS UI, embedded in the binary
  js/params.js               parameter definitions (UI side)
  js/app.js                  UI wiring (shared with the web build)
  js/knob.js, controls.js    SVG knob, toggle, segmented, dropdown, wheels
  js/envelope.js             envelope graphs with draggable points
  js/instruments.js          instrument / mute data for the graphs (display only)
  js/piano.js                keyboard with range shading
  js/engine-juce.js          the bridge
webhost/                     dsp-worklet.js (JS engine), factory.js, web-host.js, build-web.ps1, dev.html
presets/factory_presets.json
tests/                       PresetTests.cpp, EngineTools.cpp, HostCheck.cpp, Parity.cpp, TestRender.h,
                             check-params.ps1, level-match.ps1,
                             parity_scenarios.json, parity-runner.js, parity-compare.js, run-parity.ps1,
                             ui-tests.js, run-ui-tests.ps1, js-bench.js
```

### Bridge events

| UI → host | host → UI (30 Hz, only changes) |
|---|---|
| `uiReady`, `setParam {id,value}`, `gesture {id,begin}` | `params {id:value,…}` |
| `noteOn {note,vel}`, `noteOff {note}`, `panic`, `modWheel {value}`, `breath {value}`, `articulation {index}` | `notes {on:[],off:[]}` |
| `loadPreset {id}`, `savePreset {name,overwrite}`, `deletePreset {id}`, `loadPresetFile`, `openPresetFolder`, `paramMenu {id,x,y}` | `presetList {presets,folder}`, `preset {id,name,category}`, `saveResult {ok,exists,name,error}`, `message {text}` |
| *web only:* `setTempo {bpm}`, `selectMidi {id}` (and `midiInputs {inputs,selected}` back) | `stats {voices,peak,sustain,modWheel,bpm,breath,expression,aftertouch,dynamics,bend,plunger,articulation}`, `config {…}` |

`breath`, `expression` and `aftertouch` in `stats` are −1 until that controller has been seen.

## Troubleshooting

- **Open a page directly:** set `BK_UI_PAGE` to `play`, `osc`, `harm`, `tone`, `attack`, `perform`, `mod` or `fx`
  (used for the README screenshots).
- **Live UI editing:** set `BK_UI_DIR` to this repo's `vst\ui` folder before starting the host or Standalone. The
  UI then loads from disk: edit the HTML/CSS/JS and reopen the plugin window. `serve.ps1 -Ui` in the repo root serves the
  same UI in a browser without an engine; `serve.ps1 -Dev` serves it with the JS engine (see *Web build*). Add `#osc` to the URL to open the Oscillators page.
- **DevTools:** Shift + right-click in the plugin window, then *Inspect*.
- **Blank window:** the WebView2 runtime is missing or blocked. Its data folder is `%TEMP%\BrassKnucklesVST-WebView2`;
  deleting it is safe.
- **Breath does nothing:** check the BREATH meter in the header. If it stays dark, your controller isn't sending the
  Breath CC; set **Breath CC** on the PLAY page to the CC it does send.
- **Notes are silent after using a breath controller:** breath at zero is silence, as on a real horn. Double-click the
  on-screen BREATH wheel, send *Reset All Controllers* (CC121), or reload the plugin to go back to velocity-only playing.
