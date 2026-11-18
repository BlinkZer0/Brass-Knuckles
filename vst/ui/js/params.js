// ============================================================================
//  Parameter definitions. Must match vst/Source/Params.h exactly (IDs, ranges,
//  defaults, skew, choice lists, ORDER); tests/check-params.ps1 compares them.
//  New parameters are only appended. The web build is generated from this file.
// ============================================================================
'use strict';

const INSTRUMENTS = ['Trumpet', 'Flugelhorn', 'Trombone', 'French Horn', 'Tuba',
                     'Soprano Sax', 'Alto Sax', 'Tenor Sax', 'Baritone Sax', 'Synth'];
const ENGINE_MODES = ['Modeled', 'Analog'];
const VOICE_MODES = ['Poly', 'Mono', 'Legato'];
const GLIDE_MODES = ['Always', 'Legato'];
const OVERSAMPLE = ['1x', '2x', '4x'];
const FLT_TYPES = ['LP12', 'LP24', 'BP'];
const BRS_TYPES = ['Brass', 'Reed', 'Edge'];
const TONGUES = ['Soft', 'Normal', 'Hard'];
const MUTE_TYPES = ['Open', 'Straight', 'Cup', 'Harmon', 'Plunger'];
const MUTE_SRCS = ['Knob', 'Mod Wheel', 'Breath', 'Expression', 'Aftertouch', 'CC'];
const LFO_WAVES = ['Sine', 'Triangle', 'Square', 'Saw Up', 'Saw Down', 'S&H'];
const LFO_DIVS = ['1/1', '1/2D', '1/2', '1/2T', '1/4D', '1/4', '1/4T', '1/8D', '1/8', '1/8T',
                  '1/16D', '1/16', '1/16T', '1/32', '1/32T'];
const LFO_MODES = ['Global', 'Voice'];
const LFO_TRIGS = ['Free', 'Note'];
const MOD_SOURCES = ['None', 'LFO 1', 'LFO 2',
  'Osc 1 Amp Env', 'Osc 2 Amp Env', 'Osc 3 Amp Env',
  'Osc 1 Pitch Env', 'Osc 2 Pitch Env', 'Osc 3 Pitch Env',
  'Osc 1 Timbre Env', 'Osc 2 Timbre Env', 'Osc 3 Timbre Env',
  'Harm Low Env', 'Harm Mid Env', 'Harm High Env', 'Filter Env',
  'Velocity', 'Dynamics', 'Breath', 'Expression', 'Aftertouch',
  'MPE Pressure', 'MPE Slide', 'MPE Bend', 'Mod Wheel', 'Pitch Bend', 'Key', 'Random', 'Vibrato'];
const MOD_DESTS = ['None', 'Pitch (All)', 'Osc 1 Pitch', 'Osc 2 Pitch', 'Osc 3 Pitch',
  'Osc 1 Level', 'Osc 2 Level', 'Osc 3 Level',
  'Osc 1 Shape', 'Osc 2 Shape', 'Osc 3 Shape',
  'Osc 1 PW', 'Osc 2 PW', 'Osc 3 PW',
  'Osc 1 Bright', 'Osc 2 Bright', 'Osc 3 Bright',
  'Harm Level', 'Harm Tilt', 'Filter Cutoff', 'Filter Reso',
  'Dynamics', 'Brightness', 'Brassiness', 'Breath Noise', 'Lip Buzz',
  'Vibrato Depth', 'Vibrato Rate', 'Growl', 'Flutter', 'Amplitude', 'Pan',
  'LFO 1 Rate', 'LFO 2 Rate', 'Plunger', 'Body Shift', 'FX Drive', 'Chorus Mix', 'Reverb Mix'];
const FX_DRV_TYPES = ['Tube', 'Tape'];
const RV_TYPES = ['Plate', 'Hall'];
const SHAPE_NAMES = ['Sine', 'Tri', 'Saw', 'Pulse'];
const NUM_OSC = 3;
const NUM_MOD_SLOTS = 12;
const MAX_POLY = 64;

const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
function midiNoteName(n) { return NOTE_NAMES[((n % 12) + 12) % 12] + (Math.floor(n / 12) - 1); }

// Realistic written-pitch ranges (MIDI notes, concert pitch) per instrument; used to
// shade the keyboard. Same order as INSTRUMENTS. Synth = no limits.
const INSTRUMENT_RANGES = [
  [52, 84],  // Trumpet       E3 - C6
  [52, 79],  // Flugelhorn    E3 - G5
  [40, 72],  // Trombone      E2 - C5
  [34, 77],  // French Horn   B1 - F5
  [26, 58],  // Tuba          D1 - A#3
  [56, 87],  // Soprano Sax   G#3 - D#6
  [49, 80],  // Alto Sax      C#3 - G#5
  [44, 75],  // Tenor Sax     G#2 - D#5
  [36, 68],  // Baritone Sax  C2 - G#4
  [0, 127],  // Synth
];

// ---- builders (same shapes as Params.h) ----------------------------------------
const P = (id, name, group, min, max, def, unit, extra) =>
  Object.assign({ id, name, group, type: 'float', min, max, def, unit: unit || '' }, extra || {});
const L = (id, name, group, min, max, def, unit) => P(id, name, group, min, max, def, unit, { skew: 'log' });
const I = (id, name, group, min, max, def, unit) => P(id, name, group, min, max, def, unit || 'count', { type: 'int', step: 1 });
const B = (id, name, group, def) => ({ id, name, group, type: 'bool', min: 0, max: 1, step: 1, def: def ? 1 : 0, unit: '' });
const C = (id, name, group, choices, def) =>
  ({ id, name, group, type: 'choice', min: 0, max: choices.length - 1, step: 1, def, choices, unit: '' });
const BIP = { bipolar: true };

function oscParams(n) {
  const o = `osc${n}`, g = o;
  return [
    B(`${o}On`, 'On', g, n === 1),
    P(`${o}Shape`, 'Shape', g, 0, 3, 2, 'shape'),
    P(`${o}PW`, 'PW', g, 0.05, 0.95, 0.5, '%'),
    I(`${o}Pitch`, 'Pitch', g, -36, 36, 0, 'st'),
    I(`${o}Fine`, 'Fine', g, -100, 100, 0, 'ct'),
    P(`${o}Level`, 'Level', g, 0, 1, 0.7, '%'),
    L(`${o}Cut`, 'Bright', g, 1, 64, 12, 'harm'),
    P(`${o}Res`, 'Reso', g, 0, 1, 0.1, '%'),

    L(`${o}AmpDly`, 'Delay', g, 0.001, 4, 0.001, 's'),
    L(`${o}AmpAtk`, 'Attack', g, 0.001, 10, 0.03, 's'),
    L(`${o}AmpHld`, 'Hold', g, 0.001, 4, 0.001, 's'),
    L(`${o}AmpDec`, 'Decay', g, 0.005, 10, 0.5, 's'),
    P(`${o}AmpSus`, 'Sustain', g, 0, 1, 0.8, '%'),
    L(`${o}AmpRel`, 'Release', g, 0.005, 10, 0.15, 's'),
    P(`${o}AmpAtkCrv`, 'A Curve', g, -1, 1, 0.3, 'bip', BIP),
    P(`${o}AmpDecCrv`, 'D Curve', g, -1, 1, 0.5, 'bip', BIP),
    P(`${o}AmpRelCrv`, 'R Curve', g, -1, 1, 0.5, 'bip', BIP),
    P(`${o}AmpVel`, 'Vel Amt', g, 0, 1, 0, '%'),
    P(`${o}AmpVelTime`, 'Vel Time', g, -1, 1, 0, 'bip', BIP),
    P(`${o}AmpKeyTime`, 'Key Time', g, -1, 1, 0, 'bip', BIP),
    P(`${o}AmpKeyAmt`, 'Key Amt', g, -1, 1, 0, 'bip', BIP),

    P(`${o}PEnvAmt`, 'Scoop', g, -1200, 1200, 0, 'ct', BIP),
    L(`${o}PEnvTime`, 'Time', g, 0.001, 2, 0.06, 's'),
    P(`${o}PEnvCrv`, 'Curve', g, -1, 1, 0.5, 'bip', BIP),
    P(`${o}BlipAmt`, 'Blip', g, -200, 200, 0, 'ct', BIP),
    L(`${o}BlipTime`, 'Blip Time', g, 0.001, 0.5, 0.03, 's'),
    P(`${o}PEnvVel`, 'Vel', g, -1, 1, 0, 'bip', BIP),
    P(`${o}PEnvKeyTime`, 'Key Time', g, -1, 1, 0, 'bip', BIP),

    L(`${o}TEnvDly`, 'Delay', g, 0.001, 4, 0.001, 's'),
    L(`${o}TEnvAtk`, 'Attack', g, 0.001, 10, 0.05, 's'),
    L(`${o}TEnvDec`, 'Decay', g, 0.005, 10, 0.4, 's'),
    P(`${o}TEnvSus`, 'Sustain', g, 0, 1, 0.6, '%'),
    L(`${o}TEnvRel`, 'Release', g, 0.005, 10, 0.2, 's'),
    P(`${o}TEnvCrv`, 'Curve', g, -1, 1, 0.3, 'bip', BIP),
    P(`${o}TEnvCut`, '> Bright', g, -6, 6, 0, 'oct', BIP),
    P(`${o}TEnvShape`, '> Shape', g, -3, 3, 0, '', BIP),
    P(`${o}TEnvVel`, 'Vel', g, -1, 1, 0, 'bip', BIP),
    P(`${o}TEnvKeyTime`, 'Key Time', g, -1, 1, 0, 'bip', BIP),
  ];
}

function bandParams() {
  const ids = ['Lo', 'Mid', 'Hi'], names = ['Low', 'Mid', 'High'];
  const atk = [0.02, 0.04, 0.07], dec = [0.3, 0.3, 0.25], sus = [0.9, 0.7, 0.5], rel = [0.2, 0.15, 0.1], lvl = [1, 0.8, 0.6];
  const out = [];
  for (let b = 0; b < 3; b++) {
    const p = `add${ids[b]}`, g = `band${b + 1}`;
    out.push(
      L(`${p}Atk`, 'Attack', g, 0.001, 10, atk[b], 's'),
      L(`${p}Dec`, 'Decay', g, 0.005, 10, dec[b], 's'),
      P(`${p}Sus`, 'Sustain', g, 0, 1, sus[b], '%'),
      L(`${p}Rel`, 'Release', g, 0.005, 10, rel[b], 's'),
      P(`${p}Lvl`, names[b], g, 0, 1, lvl[b], '%'));
  }
  return out;
}

function lfoParams(n) {
  const g = `lfo${n}`;
  return [
    B(`${g}On`, 'On', g, 0),
    C(`${g}Wave`, 'Wave', g, LFO_WAVES, 0),
    L(`${g}Rate`, 'Rate', g, 0.02, 40, 2, 'lfohz'),
    B(`${g}Sync`, 'Sync', g, 0),
    C(`${g}Div`, 'Rate', g, LFO_DIVS, 8),
    C(`${g}Mode`, 'Mode', g, LFO_MODES, 0),
    C(`${g}Trig`, 'Trigger', g, LFO_TRIGS, 0),
    P(`${g}Phase`, 'Phase', g, 0, 360, 0, 'deg'),
    P(`${g}Amount`, 'Amount', g, 0, 1, 1, '%'),
  ];
}

function modSlotParams(n) {
  const g = 'matrix';
  return [
    B(`mod${n}On`, 'On', g, 1),
    C(`mod${n}Src`, 'Source', g, MOD_SOURCES, 0),
    C(`mod${n}Via`, 'Via', g, MOD_SOURCES, 0),
    C(`mod${n}Dst`, 'Destination', g, MOD_DESTS, 0),
    P(`mod${n}Amt`, 'Amount', g, -1, 1, 0, 'bip', BIP),
  ];
}

const PARAM_DEFS = [
  // ---- global
  C('instrument', 'Instrument', 'global', INSTRUMENTS, 0),
  C('engineMode', 'Engine', 'global', ENGINE_MODES, 0),
  P('ampLevel', 'Level', 'global', -30, 6, 0, 'dB'),
  P('master', 'Master', 'output', 0, 1, 0.7, '%', { noPreset: true }),
  I('tune', 'Tune', 'global', -100, 100, 0, 'ct'),
  I('transpose', 'Transpose', 'global', -24, 24, 0, 'st'),
  I('polyphony', 'Voices', 'voice', 1, 64, 32),
  C('voiceMode', 'Mode', 'voice', VOICE_MODES, 0),
  B('glideOn', 'Glide', 'voice', 0),
  C('glideMode', 'Glide Mode', 'voice', GLIDE_MODES, 1),
  L('glideTime', 'Glide', 'voice', 0.001, 2, 0.08, 's'),
  P('glideCurve', 'Curve', 'voice', -1, 1, 0, 'bip', BIP),
  P('slurDip', 'Slur Dip', 'voice', 0, 1, 0, '%'),
  I('bendRange', 'Bend', 'voice', 0, 24, 2, 'st'),
  B('mpeOn', 'MPE', 'voice', 0),
  I('mpeBend', 'MPE Bend', 'voice', 1, 96, 48, 'st'),
  C('oversample', 'Oversampling', 'global', OVERSAMPLE, 1),
  // ---- oscillators
  ...oscParams(1), ...oscParams(2), ...oscParams(3),
  // ---- additive harmonic layer
  B('addOn', 'On', 'harm', 0),
  P('addLevel', 'Level', 'harm', 0, 1, 0.6, '%'),
  I('addPartials', 'Partials', 'harm', 4, 32, 24),
  P('addTilt', 'Tilt', 'harm', -12, 12, 0, 'dboct', BIP),
  P('addOddEven', 'Odd/Even', 'harm', -1, 1, 0, 'bip', BIP),
  P('addSpecDly', 'Spec Delay', 'harm', 0, 1, 0.3, '%'),
  P('addDynSpread', 'Dyn Spread', 'harm', 0, 1, 0.5, '%'),
  P('addCurve', 'Curve', 'harm', -1, 1, 0.3, 'bip', BIP),
  ...bandParams(),
  // ---- dynamics
  P('dynBreath', 'Breath', 'dyn', 0, 1, 1, '%'),
  P('dynExpr', 'Expr', 'dyn', 0, 1, 0, '%'),
  P('dynAT', 'Aftert.', 'dyn', 0, 1, 0, '%'),
  P('dynMPE', 'MPE Prs', 'dyn', 0, 1, 1, '%'),
  P('dynVel', 'Vel + Ctl', 'dyn', 0, 1, 0.3, '%'),
  I('dynBreathCC', 'Breath CC', 'dyn', 1, 119, 2),
  P('dynCurve', 'Curve', 'dyn', -1, 1, 0, 'bip', BIP),
  P('dynRange', 'Range', 'dyn', 0, 48, 24, 'dB'),
  P('dynBright', 'Bright', 'dyn', 0, 8, 3, 'oct'),
  P('dynBrCurve', 'Br Curve', 'dyn', -1, 1, 0.4, 'bip', BIP),
  P('dynBrass', 'Dyn Drive', 'dyn', 0, 1, 0.5, '%'),
  P('dynAttack', 'Attack', 'dyn', 0, 1, 0.5, '%'),
  L('dynSmooth', 'Smooth', 'dyn', 0.002, 0.3, 0.02, 's'),
  // ---- voice filter
  B('fltOn', 'On', 'filter', 0),
  C('fltType', 'Type', 'filter', FLT_TYPES, 0),
  L('fltCutoff', 'Cutoff', 'filter', 20, 20000, 8000, 'Hz'),
  P('fltReso', 'Reso', 'filter', 0, 1, 0.1, '%'),
  P('fltKey', 'Key', 'filter', 0, 1, 0.5, '%'),
  P('fltDyn', 'Dyn', 'filter', 0, 8, 0, 'oct'),
  P('fltEnvAmt', 'Env Amt', 'filter', -8, 8, 0, 'oct', BIP),
  L('fltAtk', 'Attack', 'filter', 0.001, 10, 0.01, 's'),
  L('fltDec', 'Decay', 'filter', 0.005, 10, 0.3, 's'),
  P('fltSus', 'Sustain', 'filter', 0, 1, 0, '%'),
  L('fltRel', 'Release', 'filter', 0.005, 10, 0.3, 's'),
  P('fltVel', 'Vel', 'filter', 0, 1, 0.5, '%'),
  // ---- brassiness
  P('brsAmt', 'Brassy', 'brass', 0, 1, 0.2, '%'),
  C('brsType', 'Type', 'brass', BRS_TYPES, 0),
  P('brsTone', 'Tone', 'brass', 0, 1, 0.8, '%'),
  // ---- transients + noise
  P('lipAmt', 'Lip Buzz', 'trans', 0, 1, 0.3, '%'),
  P('lipTone', 'Lip Tone', 'trans', 0, 1, 0.5, '%'),
  L('lipDecay', 'Lip Decay', 'trans', 0.002, 0.3, 0.03, 's'),
  P('lipPitch', 'Pitched', 'trans', 0, 1, 0.6, '%'),
  P('brNoise', 'Breath', 'trans', 0, 1, 0.1, '%'),
  P('brTone', 'Br Tone', 'trans', 0, 1, 0.5, '%'),
  P('brDyn', 'Br Dyn', 'trans', 0, 1, 0.7, '%'),
  P('keyClick', 'Key Click', 'trans', 0, 1, 0, '%'),
  P('chiff', 'Chiff', 'trans', 0, 1, 0, '%'),
  C('tongue', 'Tongue', 'trans', TONGUES, 1),
  // ---- body
  P('bodyMix', 'Body', 'body', 0, 1, 0.6, '%'),
  P('bodyShift', 'Shift', 'body', -12, 12, 0, 'st', BIP),
  P('bodyReso', 'Reso', 'body', 0, 1, 0.5, '%'),
  // ---- mutes
  C('muteType', 'Mute', 'mute', MUTE_TYPES, 0),
  P('muteMix', 'Amount', 'mute', 0, 1, 1, '%'),
  P('mutePlunger', 'Plunger', 'mute', 0, 1, 0.5, '%'),
  C('muteSrc', 'Source', 'mute', MUTE_SRCS, 1),
  I('muteCC', 'CC', 'mute', 0, 127, 4),
  // ---- vibrato / growl / flutter
  P('vibDepth', 'Depth', 'vib', 0, 100, 15, 'ct'),
  P('vibAmp', 'Amp', 'vib', 0, 1, 0.1, '%'),
  P('vibRate', 'Rate', 'vib', 0.5, 12, 5.5, 'lfohz'),
  L('vibDelay', 'Delay', 'vib', 0.001, 3, 0.3, 's'),
  L('vibFade', 'Fade-in', 'vib', 0.001, 4, 0.5, 's'),
  P('vibDrift', 'Drift', 'vib', 0, 1, 0.3, '%'),
  P('vibWheel', 'Wheel', 'vib', 0, 1, 0, '%'),
  P('growlAmt', 'Growl', 'growl', 0, 1, 0, '%'),
  P('growlRate', 'Rate', 'growl', 10, 80, 28, 'lfohz'),
  P('growlRough', 'Rough', 'growl', 0, 1, 0.5, '%'),
  P('growlDyn', 'Dyn', 'growl', 0, 1, 0.3, '%'),
  P('flutAmt', 'Flutter', 'growl', 0, 1, 0, '%'),
  P('flutRate', 'Fl Rate', 'growl', 10, 35, 22, 'lfohz'),
  // ---- articulations
  B('artKs', 'Keyswitch', 'art', 1),
  I('artKsBase', 'KS Base', 'art', 0, 108, 12, 'note'),
  P('artFallRng', 'Fall', 'art', 1, 24, 7, 'st'),
  L('artFallTime', 'Fall Time', 'art', 0.05, 2, 0.35, 's'),
  P('artDoitRng', 'Doit', 'art', 1, 24, 5, 'st'),
  L('artDoitTime', 'Doit Time', 'art', 0.05, 2, 0.3, 's'),
  P('artRipRng', 'Rip', 'art', 1, 24, 7, 'st'),
  L('artRipTime', 'Rip Time', 'art', 0.02, 1, 0.12, 's'),
  P('artShakeRate', 'Shake Rate', 'art', 3, 12, 6.5, 'lfohz'),
  P('artShakeRng', 'Shake', 'art', 1, 5, 3, 'st'),
  P('artScoopRng', 'Scoop', 'art', 0, 12, 3, 'st'),
  L('artScoopTime', 'Scoop Time', 'art', 0.02, 1, 0.12, 's'),
  B('artAuto', 'Auto', 'art', 0),
  L('artAutoGap', 'Gap', 'art', 0.05, 2, 0.4, 's'),
  I('artAutoLo', 'Soft Vel', 'art', 1, 127, 45),
  I('artAutoHi', 'Hard Vel', 'art', 1, 127, 115),
  // ---- section
  I('secPlayers', 'Players', 'section', 1, 8, 1),
  P('secDetune', 'Detune', 'section', 0, 40, 8, 'ct'),
  P('secTiming', 'Timing', 'section', 0, 80, 20, 'ms'),
  P('secVibDesync', 'Vib Desync', 'section', 0, 1, 0.7, '%'),
  P('secTimbre', 'Timbre', 'section', 0, 1, 0.3, '%'),
  P('secWidth', 'Width', 'section', 0, 1, 0.7, '%'),
  // ---- analog brass
  I('anaUnison', 'Unison', 'analog', 1, 7, 1),
  P('anaDetune', 'Detune', 'analog', 0, 50, 12, 'ct'),
  P('anaSpread', 'Spread', 'analog', 0, 1, 0.5, '%'),
  P('anaBlat', 'Blat', 'analog', 0, 1, 0.3, '%'),
  // ---- LFOs + matrix
  ...lfoParams(1), ...lfoParams(2),
  ...Array.from({ length: NUM_MOD_SLOTS }, (_, i) => modSlotParams(i + 1)).flat(),
  // ---- FX
  B('fxDrvOn', 'On', 'fxdrive', 0),
  C('fxDrvType', 'Type', 'fxdrive', FX_DRV_TYPES, 0),
  P('fxDrvAmt', 'Drive', 'fxdrive', 0, 1, 0.3, '%'),
  P('fxDrvTone', 'Tone', 'fxdrive', 0, 1, 0.7, '%'),
  P('fxDrvMix', 'Mix', 'fxdrive', 0, 1, 1, '%'),
  B('eqOn', 'On', 'eq', 0),
  P('eqLoGain', 'Low', 'eq', -15, 15, 0, 'dB', BIP),
  L('eqLoFreq', 'Lo Freq', 'eq', 40, 800, 200, 'Hz'),
  P('eqMidGain', 'Mid', 'eq', -15, 15, 0, 'dB', BIP),
  L('eqMidFreq', 'Mid Freq', 'eq', 200, 8000, 1200, 'Hz'),
  L('eqMidQ', 'Mid Q', 'eq', 0.3, 6, 1, ''),
  P('eqHiGain', 'High', 'eq', -15, 15, 0, 'dB', BIP),
  L('eqHiFreq', 'Hi Freq', 'eq', 1500, 16000, 6000, 'Hz'),
  B('chOn', 'On', 'chorus', 0),
  L('chRate', 'Rate', 'chorus', 0.05, 5, 0.6, 'lfohz'),
  P('chDepth', 'Depth', 'chorus', 0, 1, 0.4, '%'),
  P('chMix', 'Mix', 'chorus', 0, 1, 0.35, '%'),
  B('rvOn', 'On', 'reverb', 0),
  C('rvType', 'Type', 'reverb', RV_TYPES, 1),
  L('rvDecay', 'Decay', 'reverb', 0.3, 12, 1.8, 's'),
  P('rvPre', 'Pre-delay', 'reverb', 0, 200, 20, 'ms'),
  P('rvDamp', 'Damping', 'reverb', 0, 1, 0.5, '%'),
  P('rvMix', 'Mix', 'reverb', 0, 1, 0.2, '%'),
  P('stWidth', 'Width', 'output', 0, 2, 1, '%'),
];

const PARAM_BY_ID = Object.fromEntries(PARAM_DEFS.map(d => [d.id, d]));

function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

function defaultParamValues() {
  return Object.fromEntries(PARAM_DEFS.map(d => [d.id, d.def]));
}

// value <-> 0..1 normalized (same mapping the VST host sees)
function paramToNorm(d, v) {
  if (d.skew === 'log') return clamp(Math.log(v / d.min) / Math.log(d.max / d.min), 0, 1);
  return clamp((v - d.min) / (d.max - d.min), 0, 1);
}

function paramFromNorm(d, n) {
  n = clamp(n, 0, 1);
  let v = d.skew === 'log' ? d.min * Math.pow(d.max / d.min, n) : d.min + n * (d.max - d.min);
  if (d.step) v = Math.round(v / d.step) * d.step;
  return clamp(v, d.min, d.max);
}

function formatParam(d, v) {
  if (d.type === 'choice') return d.choices[clamp(Math.round(v), 0, d.choices.length - 1)];
  if (d.type === 'bool') return v >= 0.5 ? 'On' : 'Off';
  const sgn = (x) => (x > 0 ? '+' : '');
  switch (d.unit) {
    case 's':     return v < 1 ? `${Math.round(v * 1000)} ms` : `${v.toFixed(2)} s`;
    case 'Hz':    return v >= 1000 ? `${(v / 1000).toFixed(v >= 10000 ? 1 : 2)} kHz` : `${Math.round(v)} Hz`;
    case 'lfohz': return `${v.toFixed(v < 1 ? 3 : v < 10 ? 2 : 1)} Hz`;
    case 'st':    return Math.abs(v - Math.round(v)) > 0.05 ? `${sgn(v)}${v.toFixed(1)} st` : `${sgn(v)}${Math.round(v)} st`;
    case 'ct':    return `${sgn(v)}${Math.round(v)} ct`;
    case '%':     return `${Math.round(v * 100)}%`;
    case 'bip':   return `${v > 0.004 ? '+' : ''}${Math.round(v * 100)}%`;
    case 'dB':    return `${v > 0.05 ? '+' : ''}${v.toFixed(1)} dB`;
    case 'deg':   return `${Math.round(v)}°`;
    case 'oct':   return `${v > 0.005 ? '+' : ''}${v.toFixed(1)} oct`;
    case 'harm':  return `×${v.toFixed(v < 10 ? 1 : 0)}`;
    case 'ms':    return `${Math.round(v)} ms`;
    case 'dboct': return `${v > 0.05 ? '+' : ''}${v.toFixed(1)} dB/oct`;
    case 'count': return String(Math.round(v));
    case 'note':  return midiNoteName(Math.round(v));
    case 'shape': {
      const i = clamp(Math.floor(v), 0, 3), f = v - i;
      if (f < 0.01 || i === 3) return SHAPE_NAMES[i];
      return `${SHAPE_NAMES[i]}›${SHAPE_NAMES[i + 1]} ${Math.round(f * 100)}%`;
    }
    default:      return v.toFixed(2);
  }
}
