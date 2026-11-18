# Conventions (inherited from E-PianoVST)

Brass Knuckles follows the house style of [E-PianoVST](https://github.com/BlinkZer0/E-PianoVST) v1.0
(read from a local clone at `../E-PianoVST`, commit `ab6748d`). This file records what that project does,
and where Brass Knuckles extends a pattern, the extension is listed next to the original.

## 1. Repository layout

```
README.md                    user-facing: features, screenshot, build/install, release history, layout
.gitignore                   .vs/  build/  *.user
start.bat -> serve.ps1       tiny PowerShell HttpListener serving web/ on localhost:8080 (no installs)
docs/                        screenshots, (E-Piano also mirrors web/index.html here for GitHub Pages)
web/index.html               GENERATED single-file web build (never edited by hand)
vst/
  CMakeLists.txt             JUCE via FetchContent, plugin + console test app
  build.bat / install.bat    build Release (or Debug) / copy .vst3 to Common Files\VST3 (UAC prompt)
  README.md                  developer docs: signal flow, bridge events, tests, troubleshooting
  Source/                    C++ (see 3)
  ui/                        HTML/CSS/SVG/JS UI, compiled into the binary as BinaryData
  webhost/                   JS twin of the DSP + browser stand-in for the C++ bridge + build-web.ps1
  presets/factory_presets.json
  tests/                     PresetTests.cpp, EngineTools.cpp, parity_scenarios.json, run-parity.ps1
```

Brass Knuckles: identical layout. Extra files are added only inside these folders (e.g. `docs/PLAN.md`,
`vst/tests/run-ui-tests.ps1`, which E-Piano's README mentions but does not commit).

## 2. Build system

- **CMake 3.22+, C++17, JUCE 8.0.17** (`GIT_TAG 8.0.17-security`), fetched with `FetchContent`, shallow.
  An optional cache var points at a local JUCE checkout instead (`EPIANO_JUCE_DIR` -> `BK_JUCE_DIR`).
- `juce_add_plugin`: `FORMATS VST3 Standalone`, `IS_SYNTH TRUE`, `NEEDS_MIDI_INPUT TRUE`,
  `NEEDS_WEBVIEW2 TRUE`, `VST3_CATEGORIES Instrument Synth`, `EDITOR_WANTS_KEYBOARD_FOCUS TRUE`,
  `COPY_PLUGIN_AFTER_BUILD FALSE`.
- Defines: `JUCE_WEB_BROWSER=1`, `JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1`, `JUCE_USE_CURL=0`,
  `JUCE_VST3_CAN_REPLACE_VST2=0`, `JUCE_DISPLAY_SPLASH_SCREEN=0`.
- UI files (`ui/*.html|css|js`) and `presets/factory_presets.json` go in one `juce_add_binary_data` target.
  BinaryData is looked up **by file name**, so every UI file name must be unique.
- Links `juce_audio_utils`, `juce_gui_extra`, `juce_dsp` + recommended config/LTO/warning flags.
- Console test app (`juce_add_console_app`, `<Name>Tests`) compiles the preset manager, sample code and the
  test sources; it includes the header-only engine directly.
- **`build.bat`**: build dir and JUCE download live in `%LOCALAPPDATA%\<Project>` (keeps ~1 GB out of
  OneDrive). Finds CMake on PATH or inside Visual Studio via `vswhere`; adds Git to PATH if needed;
  configures with `-DFETCHCONTENT_BASE_DIR=%WORK%\deps`; builds `<T>_VST3` and `<T>_Standalone` targets
  `--parallel`; prints artefact paths. Messages prefixed `[E-Piano]`.
- **`install.bat`**: copies the bundle to `%CommonProgramFiles%\VST3` via an elevated PowerShell
  `Start-Process -Verb RunAs`, then checks `Contents\x86_64-win\<Name>.vst3` exists.

Brass Knuckles: target `BrassKnucklesVST`, product `Brass Knuckles`, work dir
`%LOCALAPPDATA%\BrassKnucklesVST`, tests target `BrassKnucklesTests`, messages prefixed `[Brass Knuckles]`.

## 3. C++ source structure

| File | Role |
|---|---|
| `Params.h` | The parameter table. `enum ParamIndex` (order = storage order) + `paramDefs()` vector of `{id, hostName, Kind, min, max, def, Unit, logSkew, choices}`. Choice lists as functions. `formatValue(Unit, v)` for host display strings. `logRange()` matches JS `paramToNorm`. `createParameterLayout()` builds APVTS params with `ParameterID{id, 1}`. |
| `SynthEngine.h` | **Header-only DSP, no JUCE, no allocation, no locks.** `EngineParams` snapshot struct, deterministic `Rng` (LCG, identical in JS), oscillators (PolyBLEP), envelopes, TPT SVF, LFOs, drive (ADAA), voices, `SynthEngine` with `prepare / setParams / noteOn / noteOff / setSustain / setPitchBend / setModWheel / setTransport / render(L, R, n) / panic`. |
| `EngineParamsFromValues.h` | `makeEngineParams(get)` template: raw param values -> `EngineParams`. Shared by the plugin (atomic APVTS values) and the tests. |
| `PresetManager.*` | Factory presets parsed from BinaryData JSON; user folder scan with a path/size/mtime signature (`rescanIfChanged`); read/write of the JSON preset files; ids `factory:<n>` / `user:<relative path>`. |
| `PluginProcessor.*` | APVTS, cached `RangedAudioParameter*` + raw atomics per index, MIDI handling **sample-accurate** (render up to each event, then apply), host tempo/ppq, factory presets exposed as **programs**, state = APVTS XML + preset id/name/category (+ embedded extras), lock-free buffer handoff to the audio thread (atomic `incoming` / `retired` pointers). |
| `PluginEditor.*` | `WebBrowserComponent` (WebView2) with a resource provider serving BinaryData, native integration, event listeners, 30 Hz timer pushing only changed values. |

Rules visible in the code:
- `juce::ScopedNoDenormals` in `processBlock`.
- Engine runs in **double** precision (so the JS twin, which is all doubles, can match it).
- **Control rate `kChunk = 32` samples**: modulation evaluated once per chunk, every per-sample target
  (pitch, levels, filter g/k, amp, pan) **ramped linearly** across the chunk. `v.snap` avoids ramping from
  stale values on a fresh voice.
- Master gain smoothed per sample, then `tanh` soft clip on the output.
- Voice allocation: retrigger same note -> free voice -> steal oldest *released*, then oldest.
- Mono/legato: last-note-priority key stack; legato doesn't retrigger envelopes; glide "always" or
  "legato only"; sustain pedal respected.
- Parameter changes from presets go through `beginChangeGesture / setValueNotifyingHost / endChangeGesture`.
  `master` is never stored in presets.

## 4. Parameters: stability and compatibility

- **Append-only**: new parameters are added at the end of `ParamIndex`; indices and string IDs of existing
  ones never change. `ParameterID` version is `1`.
- **New parameters default to neutral** (off / zero), so older presets and projects sound unchanged.
- Params come in **group helpers** (`oscParams(n)`, `lfoParams(n)`, `modSlotParams(n)`) in JS and loops in C++,
  with index helpers `lfoParam(lfo, field)`, `modParam(slot, field)`.
- Kinds: Float (optionally log skew), Bool, Choice, Int. Units drive formatting identically in C++ and JS.
- `Params.h` and `ui/js/params.js` are **hand-mirrored** (IDs, ranges, defaults, skew, choices, order).

Brass Knuckles extension: the param count will be several times E-Piano's 96, so a `--dump-params` test
command prints the C++ table as JSON and the test scripts diff it against `params.js`. Same two files,
same pattern, but a mismatch fails a test.

## 5. UI <-> C++ bridge (JUCE 8 native integration)

- JS side: `ui/js/engine-juce.js` wraps `window.__JUCE__.backend` (`emitEvent`, `addEventListener`) in a
  `JuceEngine` class. `app.js` talks only to that class.
- **UI -> host**: `uiReady`, `setParam {id,value}`, `gesture {id,begin}`, `noteOn {note,vel}`, `noteOff {note}`,
  `panic`, `modWheel {value}`, `loadPreset {id}`, `savePreset {name,overwrite}`, `deletePreset {id}`,
  `loadPresetFile`, `openPresetFolder`, `paramMenu {id,x,y}`; web only: `setTempo`, `selectMidi`.
- **Host -> UI** (30 Hz, only changes): `params {id:value…}`, `notes {on[],off[]}`, `stats {voices,peak,…}`,
  `config {sampleRate,blockSize,wrapper,voices}`, `presetList {presets,folder}`, `preset {id,name,category}`,
  `saveResult {ok,exists,name,error}`, `message {text}`; web only: `midiInputs`.
- On `uiReady` the editor clears its `lastSent` cache (NaN) so everything is resent.
- Knob drags send `gesture begin/end` so the DAW records one automation move; one-shot changes
  (wheel, keys, double-click) are wrapped in a gesture by the editor.
- **Right-click a control** -> `getHostContext()->getContextMenuForParameter()` (FL Studio's *Create
  automation clip*), with a fallback *Reset to default* menu. Shift+right-click = DevTools.
- UI notes go through `MidiKeyboardState` so the processor merges them sample-accurately and the editor can
  light up every sounding note.
- WebView2 user data folder in `%TEMP%\<Project>-WebView2`.
- **Dev mode**: env var `EPIANO_UI_DIR` -> UI loads from disk; edit and reopen the window, no rebuild.

Brass Knuckles: same events, plus `stats` grows `breath`, `expression`, `pressure`, `dynamics` and
new host->UI `meters` for envelope/dynamics displays. Dev env var `BK_UI_DIR`.

## 6. UI code

- Plain ES (no framework, no build step), `'use strict'`, scripts loaded in order:
  `params.js, engine-juce.js, knob.js, controls.js, piano.js, app.js`.
- All controls are **SVG**: `Knob` (270° sweep, drag/Shift fine/wheel/arrows/double-click reset, bipolar
  arcs from 12 o'clock, choice ticks), toggles, segmented, dropdown, mod wheel; on-screen keyboard with
  velocity from click height, glissando, computer keys `A W S E D … ;'`, `Z/X` octave.
- Panels built programmatically in `app.js` (`makePanel`, `addKnob`, `control(Class, id)`), every control
  tagged `data-param` (for the right-click host menu).
- Pages via tabs (SYNTH / MODULATION / DRIVE·VOICE) with LEDs showing whether a page's features are active.
- Whole panel scales to fit the window (`transform: scale`).
- CSS custom properties on `:root`: dark chassis (`--bg #0a0b0d`, `--panel`, `--panel-edge`), orange accent.

Brass Knuckles: same widget set and code style, new identity: **gunmetal chassis + polished-brass accent**.
New widgets follow the `Knob` pattern: an SVG envelope editor (draggable DAHDSR nodes + curve handles),
a harmonic bar display, and keyswitch/range shading on the keyboard.

## 7. Presets

- **120 factory presets, 12 categories × 10**, in `presets/factory_presets.json` (compact format: per-preset
  arrays for the original params plus an optional `{paramId: value}` object for the rest).
- **Level-matched**: `<Tests> --loudness` renders each preset (chord, or one note for mono/bass, vel 100,
  loudest 100 ms RMS) and `ampLevel` is set from that.
- **User presets**: `Documents\<Product>\Presets`, subfolders = categories, rescanned every ~2 s (60 ticks of
  the 30 Hz timer) by signature; delete = Recycle Bin.
- **File format**: plain JSON `{ "format": "<Product> Preset", "version": 1, "name", "category", "params": {id: value} }`.
  Missing params load at their default; values are clamped to range; master is never stored. Same format in
  plugin and web build.
- Factory presets are also host **programs** (`"<Category> - <Name>"`).

Brass Knuckles: extension `.bkpreset`, format string `"Brass Knuckles Preset"`. Adds a real **version
migration** step (`migratePreset(json)` in C++ and JS: renames/re-maps per version, then fill defaults), which
E-Piano only has implicitly through neutral defaults. Factory JSON uses **instrument templates + overrides**
instead of fixed-position arrays, because there are too many params for positional arrays.

## 8. Web build

- `webhost/build-web.ps1` reads `ui/index.html`, inlines `css/style.css`, inlines every `<script src="js/…">`,
  and **just before `engine-juce.js`** injects `FACTORY_JSON`, `dsp-worklet.js` and `web-host.js`. Refuses any
  code containing `</script`. Adds a GENERATED banner and version (from `CMakeLists.txt`) to the title.
- `web-host.js` provides `window.__JUCE__.backend` itself, so the UI runs unchanged: Web Audio
  (AudioWorklet when served, ScriptProcessor fallback on `file://`), Web MIDI, tempo, presets
  (factory, localStorage, or a linked folder through the File System Access API, same file format).
- `dsp-worklet.js`: the whole engine is one function `synthWorkletModule()` that is stringified and loaded as
  a worklet module, so it must not reference anything outside itself. **Line-for-line twin** of
  `SynthEngine.h`: same constants, control rate, RNG, maths.

## 9. Tests

| Command | What |
|---|---|
| `<Tests>.exe` | preset counts/categories/ranges, neutral defaults, save -> load round trip, old/partial/garbage files |
| `<Tests>.exe --parity <scenarios.json>` | renders scenarios at 48 kHz, 128-sample blocks, prints RMS per window (8 per channel) + peak as JSON |
| `tests\run-parity.ps1 [-Tolerance]` | builds a temp HTML page with `params.js` + presets + scenarios + `dsp-worklet.js`, runs it in **headless Edge** (`--dump-dom`), compares with C++ (relative error per window). E-Piano reaches 5×10⁻⁵ |
| `<Tests>.exe --bench` | worst-case CPU, 10 s of audio, % of one core |
| `<Tests>.exe --loudness` | per-preset loudness for level matching |

Scenarios are JSON: `{ name, preset?: "Category/Name", params?: {}, blocks, bpm?, events: [[block, "on"|"off"|"wheel"|"sustain"|"bend", …]] }`.

Brass Knuckles adds event types (`cc`, `pressure`, `mpe`), brass-specific checks (centroid vs dynamics,
legato click detector, aliasing), and commits the headless UI test script.

## 10. Docs and release style

- README: version in the title, one-table build overview, screenshot, "What it can do" bullet lists,
  per-build sections, release history with "still to confirm by hand", known limitations, ideas, layout.
- `vst/README.md`: signal flow diagram, DAW behaviour, feature tables, preset format, tests table, layout,
  bridge events table, troubleshooting (dev UI env var, DevTools, blank window).
- Plain, concrete language; numbers measured, not guessed (CPU %, parity error).
