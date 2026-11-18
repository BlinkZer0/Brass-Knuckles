# Brass Knuckles — VST3 plugin (JUCE 8)

This folder is the source of truth for Brass Knuckles:
- **C++ DSP**: the plugin's sound engine (`Source/SynthEngine.h`, `Source/dsp/`).
- **HTML/SVG UI**: shown inside the plugin through WebView2.
- **Web host** (Phase 6): runs the same UI in a browser with a JS twin of the engine.

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
bus:        sum -> body/bell formants (4 biquads per instrument, mix/shift/reso)
            -> mute (straight / cup / harmon biquads, plunger = moving SVF low-pass)
            -> patch level x master (smoothed) -> safety clip (linear below -1.9 dBFS) -> out (stereo)
```

Body formants and mutes are linear and pitch-independent, so they run once on the stereo bus, not per voice.
Instrument data (spectra, slopes, formants, noise bands) is in `Source/dsp/Instruments.h`.

Envelopes, dynamics, vibrato and pitch are evaluated every 32 samples; oscillator increments, levels, filter
coefficients and gain are ramped per sample in between. Voices are allocated in **note groups** (one played note
= one group) so section mode can add players per note in Phase 3.

## In the host

- Instrument (Instrument|Synth), MIDI in, stereo out. **341 automatable parameters** with stable IDs; knob and
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
  is never stored. `PresetManager::migrate()` brings older versions up to date (and the web build will do the same):
  ```json
  { "format": "Brass Knuckles Preset", "version": 1, "name": "My Horn", "category": "User",
    "params": { "instrument": 0, "osc1Shape": 2.4, "dynBright": 4.5, "vibDepth": 14 } }
  ```

## Tests

| Command | Checks |
|---|---|
| `BrassKnucklesTests.exe` | parameter table, factory presets (ranges, 12 categories, templates resolved), preset file round trip, partial / old / garbage files, engine basics (velocity, breath silence and swell, reset controllers, polyphony limit, legato glide pitch), modeled engine (half-band filters unity, harmonic layer brightens with dynamics, spectral delay holds the top back early, brassiness adds harmonics, straight mute cuts lows, plunger closed vs open, breath noise, key click/chiff, tongue, growl), every preset renders (not silent, no clipping, tail ends), brass checks, aliasing |
| `BrassKnucklesTests.exe --brass` | per preset: harmonic centroid (brightness of the tone, ignoring breath noise) rises with velocity (≥8% per step), RMS rises with velocity, a breath swell makes a held note louder **and** brighter; plus the aliasing check: C6 at velocity 127, the brassiness stage fed by the harmonic layer, inharmonic energy below 18 kHz < −60 dB at the preset's drive and < −50 dB at full drive with 4× |
| `BrassKnucklesTests.exe --host-check "<path>\Brass Knuckles.vst3"` | loads the built VST3 like a DAW: scans as an instrument, all parameters named, MIDI + breath CC make sound while parameters are automated, state restores exactly, programs switch |
| `BrassKnucklesTests.exe --bench` | worst case so far: 32 notes, every per-voice feature on (3 oscillators, 32 harmonics, LP24, brassiness, transients, noise, growl, flutter) + body + plunger: 17% of one core at 1×, 20% at 2×, 22% at 4× |
| `BrassKnucklesTests.exe --loudness` | loudness of every factory preset |
| `BrassKnucklesTests.exe --render "<Category/Name>" out.wav [demo\|velocity\|breath\|legato\|chord]` | offline render of a preset |
| `BrassKnucklesTests.exe --render-all <folder>` | every preset × demo / velocity / breath phrases |
| `powershell -File tests\check-params.ps1` | `params.js` matches `Params.h` exactly (order, IDs, ranges, defaults, units, choices) |
| `powershell -File tests\level-match.ps1` | re-renders every preset and rewrites its `ampLevel` to the loudness target |

Coming later: C++/JS parity (`run-parity.ps1`), headless UI tests of the web build, legato click and aliasing checks.

## Layout

```
CMakeLists.txt  build.bat  install.bat
Source/
  Params.h                   parameter table (341 params; mirrors ui/js/params.js)
  SynthEngine.h              engine: voices, note groups, dynamics, vibrato, glide, voice modes
  dsp/Common.h               RNG, curves, PolyBLEP, TPT SVF
  dsp/Envelope.h             DAHDSR with per-segment curves (control rate)
  dsp/Oscillator.h           morphing oscillator
  dsp/Additive.h             harmonic layer: node/harmonic tables, per-voice state
  dsp/Oversampler.h          Kaiser half-band FIRs (2x / 4x), brassiness shapers
  dsp/Filters.h              RBJ biquads (body, mutes)
  dsp/Instruments.h          instrument models (spectrum, slopes, body, noise) and mute models
  EngineParamsFromValues.h   parameter values -> engine snapshot (plugin + tests)
  PresetManager.*            factory templates, user folder, .bkpreset read/write/migrate
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
presets/factory_presets.json
tests/                       PresetTests.cpp, EngineTools.cpp, HostCheck.cpp, TestRender.h,
                             check-params.ps1, level-match.ps1
```

### Bridge events

| UI → host | host → UI (30 Hz, only changes) |
|---|---|
| `uiReady`, `setParam {id,value}`, `gesture {id,begin}` | `params {id:value,…}` |
| `noteOn {note,vel}`, `noteOff {note}`, `panic`, `modWheel {value}`, `breath {value}` | `notes {on:[],off:[]}` |
| `loadPreset {id}`, `savePreset {name,overwrite}`, `deletePreset {id}`, `loadPresetFile`, `openPresetFolder`, `paramMenu {id,x,y}` | `presetList {presets,folder}`, `preset {id,name,category}`, `saveResult {ok,exists,name,error}`, `message {text}` |
| *web only (Phase 6):* `setTempo {bpm}`, `selectMidi {id}` | `stats {voices,peak,sustain,modWheel,bpm,breath,expression,aftertouch,dynamics,bend}`, `config {…}` |

`breath`, `expression` and `aftertouch` in `stats` are −1 until that controller has been seen.

## Troubleshooting

- **Live UI editing:** set `BK_UI_DIR` to this repo's `vst\ui` folder before starting the host or Standalone. The
  UI then loads from disk: edit the HTML/CSS/JS and reopen the plugin window. `start.bat` in the repo root serves the
  same UI in a browser (preview only until the web build exists). Add `#osc` to the URL to open the Oscillators page.
- **DevTools:** Shift + right-click in the plugin window, then *Inspect*.
- **Blank window:** the WebView2 runtime is missing or blocked. Its data folder is `%TEMP%\BrassKnucklesVST-WebView2`;
  deleting it is safe.
- **Breath does nothing:** check the BREATH meter in the header. If it stays dark, your controller isn't sending the
  Breath CC; set **Breath CC** on the PLAY page to the CC it does send.
- **Notes are silent after using a breath controller:** breath at zero is silence, as on a real horn. Double-click the
  on-screen BREATH wheel, send *Reset All Controllers* (CC121), or reload the plugin to go back to velocity-only playing.
