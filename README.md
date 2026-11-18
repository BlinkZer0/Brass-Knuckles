# Brass Knuckles — v0.1 (in development)

Brass Knuckles is a polyphonic brass and reed synthesizer: trumpet, flugelhorn, trombone, French horn, tuba,
soprano, alto, tenor and baritone sax, sections and synth brass. It aims to play the way a horn player does:
louder notes get brighter, notes scoop and blip on the attack, and a breath controller swells and brightens a
held note.

| Build | Where | Status |
|---|---|---|
| **VST3 plugin** + Standalone | [`vst/`](vst/README.md) | Phases 1-2 of 5 done: core voice, dynamics, envelopes, harmonic layer, brassiness, transients, body, mutes |
| **Web build** | `web/index.html` | Phase 6: generated from the plugin once the VST is finished |

The build plan is in [docs/PLAN.md](docs/PLAN.md). The conventions it inherits from
[E-PianoVST](https://github.com/BlinkZer0/E-PianoVST) are in [docs/CONVENTIONS.md](docs/CONVENTIONS.md).

## What works now (Phases 1-2)

**Modeled brass (Phase 2)**
- **Harmonic layer**: up to 32 exact harmonics, each with its own envelope. Low, mid and high band envelopes
  are blended across the harmonics, and **Spectral Delay** staggers them: upper harmonics arrive later, swell in
  more slowly and fade first. The spectrum comes from an **instrument model** (trumpet, flugelhorn, trombone,
  French horn, tuba, four saxes, neutral synth) and its slope follows the dynamics: steep and dark when soft,
  nearly flat and blazing when loud. Tilt and odd/even controls on top.
- **Brassiness**: a waveshaper whose drive grows with dynamics (Brass, Reed and Edge curves, tone control),
  oversampled **1×, 2× or 4×**. Measured at C6, velocity 127: aliasing below hearing range falls from −32 dB (1×)
  to −52 dB (2×) and −80 dB (4×) at full drive.
- **Attack and breath**: lip buzz / reed onset burst (tone, decay, pitched vs. noisy), breath noise shaped by
  the instrument's noise band and following the dynamics, sax key click and reed chiff, and **tongue** types
  (soft, normal, hard) that change the transient, scoop, blip and attack together.
- **Body / bell**: each instrument's resonances (bell for brass, body formants for saxes) with amount, shift
  and resonance.
- **Mutes**: straight, cup, harmon and **plunger**. The plunger is a wah you can play from the mod wheel,
  breath, expression, aftertouch, any CC, or the knob.
- **Growl** (rough AM/FM that follows dynamics) and **flutter tongue**.
- New pages: HARMONICS (live spectrum soft vs. loud, spectral-delay timing), TONE · MUTES (transfer curve,
  body and mute responses, live plunger), ATTACK · BREATH.

**Envelope oscillators**
- 3 oscillators per voice. Each morphs continuously Sine → Triangle → Saw → Pulse, with pulse width, pitch,
  fine and level, and has its own keytracked brightness filter (in harmonics, so ×4 means "4 harmonics" on
  every note).
- Every oscillator has **its own three envelopes**:
  - **Amp**: Delay, Attack, Hold, Decay, Sustain, Release, with a curve per segment, plus velocity amount,
    velocity → attack time, key → times, and key → level.
  - **Pitch**: the brass **scoop** into the note (start offset in cents, time, curve) plus a short
    **blip**/overshoot on the attack, scaled by velocity and key.
  - **Timbre**: a DADSR that moves that oscillator's brightness (octaves) and shape, so the tone changes over
    the note.
- Draggable envelope graphs on the OSCILLATORS page (Shift = fine, double-click a point = default).

**Dynamics: louder is brighter**
- One *dynamics* value per note, from velocity and/or continuous controllers: **breath** (CC2, or any CC you
  pick), **expression** (CC11), **channel aftertouch**, and per-note pressure (poly aftertouch). Each has a
  weight, so they can be blended.
- Dynamics drives **loudness** (range in dB), **brightness** (octaves, on its own curve, so loud notes brighten
  faster than they get louder), and **attack speed** (soft notes swell in, hard notes bite).
- **Breath works over time**: hold a note and push the breath controller, and it swells and brightens. Until
  a breath controller sends anything, velocity alone sets the dynamics, so every preset plays properly from a
  plain keyboard. *Reset All Controllers* (CC121) forgets the controller again.
- An on-screen **BREATH** wheel next to the mod wheel lets you try this without a breath controller.
  Double-click it to hand control back to velocity.

**Playing**
- Poly, Mono and **Legato** with glide (time, curve, always or legato only) and a **lip-slur dip**: a short
  drop in level and brightness while the lips change partial.
- **Vibrato** with delay, fade-in, rate, pitch depth and amplitude depth (lip vs. jaw), humanised drift, and an
  amount you can put on the mod wheel.
- Pitch bend (range up to ±24), polyphony up to 64 (default 32), voice stealing with a short fade, sustain pedal.
- 49-key keyboard. Keys outside the selected instrument's real range are dimmed but still play (RANGE button).

**Presets**
- 15 starter presets across the 12 final categories, level-matched, including Harmon Trumpet, Plunger Bone
  (plunger on the mod wheel) and Growl Tenor. The full 120 come in Phase 5.
- `.bkpreset` files (plain JSON, versioned) in `Documents\Brass Knuckles\Presets`, one subfolder per category,
  rescanned every 2 seconds.

**Not built yet** (the parameters already exist, so projects saved now keep working): articulations and
keyswitches, section mode, analog engine, MPE (Phase 3); LFOs, mod matrix, FX (Phase 4).

## Build and install

Requirements: Windows, Visual Studio 2022 or 2026 with the C++ workload, Git, and the WebView2 runtime
(built into Windows 11). JUCE 8.0.17 downloads on the first build, or is reused from E-Piano's build folder.

```
vst\build.bat      build VST3 + Standalone + test tool (Release)
vst\install.bat    copy "Brass Knuckles.vst3" to C:\Program Files\Common Files\VST3 (asks for admin)
```

The Standalone app (`%LOCALAPPDATA%\BrassKnucklesVST\build\BrassKnucklesVST_artefacts\Release\Standalone\Brass Knuckles.exe`)
is the quickest way to play without a host.

## MIDI and breath controller setup

- **Breath controllers** (TEControl, Yamaha BC, Akai EWI in breath mode…) usually send **CC2**. If yours
  sends something else, set **Breath CC** on the PLAY page to that CC number. Expression pedals are CC11; turn up
  **Expr** under Dynamics to blend one in.
- The **BREATH** meter in the header lights up once a breath controller is detected.
- **Vel + Ctl** sets how much velocity still matters once a controller is in charge (0% = breath only).
- In a VST3 host, CCs arrive through the host's MIDI routing as usual; no MIDI-learn is needed.

## Repository layout

```
vst/                      VST3 plugin (JUCE 8): Source/, ui/, webhost/, presets/, tests/
docs/                     CONVENTIONS.md, PLAN.md
start.bat, serve.ps1      local server: the web build once it exists, the UI preview until then
web/index.html            web build (generated in Phase 6)
```
