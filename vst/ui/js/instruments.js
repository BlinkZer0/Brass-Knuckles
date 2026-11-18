// ============================================================================
//  Instrument models and mute filters for the UI's graphs (harmonic spectrum,
//  body and mute responses). Copied from vst/Source/dsp/Instruments.h; the
//  audio engine has its own copy (C++, and the JS twin in the web build), so
//  this file only affects what the graphs show.
// ============================================================================
'use strict';

const BQ = { peak: 0, lowShelf: 1, highShelf: 2, lowPass: 3, highPass: 4, bandPass: 5 };

// [softSlope, loudSlope, specHz[4], specDb[4], body [[type, hz, db, q] x4], noiseHz]
const INSTRUMENT_MODELS = [
  [-7.0, -1.2, [300, 1200, 3000, 8000], [-6, 0, 0, -12], [[1, 250, -4, 0.7], [0, 1300, 5, 1.2], [0, 2600, 3, 1.6], [2, 7000, -4, 0.7]], 2500],
  [-9.0, -3.0, [250, 800, 2000, 6000], [-3, 0, -6, -20], [[1, 150, 2, 0.7], [0, 700, 4, 1.0], [0, 1500, 2, 1.2], [2, 3500, -7, 0.7]], 1800],
  [-8.0, -1.5, [150, 500, 1500, 5000], [-6, 0, 0, -12], [[1, 120, -3, 0.7], [0, 520, 5, 1.0], [0, 1200, 3, 1.4], [2, 5000, -4, 0.7]], 1500],
  [-10.0, -3.5, [150, 400, 1000, 3000], [-3, 0, -4, -20], [[1, 100, 1, 0.7], [0, 380, 5, 0.9], [0, 900, 2, 1.2], [2, 1800, -9, 0.7]], 1200],
  [-10.0, -3.5, [60, 250, 700, 2500], [-3, 0, -4, -20], [[1, 50, -2, 0.7], [0, 230, 5, 0.9], [0, 600, 3, 1.2], [2, 1400, -8, 0.7]], 800],
  [-5.0, -1.0, [500, 1500, 3500, 9000], [-6, 0, 0, -10], [[0, 900, 2, 1.5], [0, 1600, 5, 2.0], [0, 3200, 4, 2.5], [2, 8000, -3, 0.7]], 3500],
  [-5.5, -1.2, [400, 1200, 3000, 8000], [-6, 0, -1, -12], [[0, 700, 2, 1.5], [0, 1300, 5, 2.0], [0, 2700, 4, 2.5], [2, 7000, -3, 0.7]], 3000],
  [-6.0, -1.3, [250, 900, 2500, 7000], [-6, 0, -2, -14], [[0, 500, 2, 1.5], [0, 950, 5, 2.0], [0, 2100, 4, 2.5], [2, 6000, -3, 0.7]], 2500],
  [-6.5, -1.5, [150, 600, 1800, 5000], [-6, 0, -3, -15], [[0, 350, 3, 1.5], [0, 650, 5, 2.0], [0, 1500, 4, 2.5], [2, 4500, -4, 0.7]], 2000],
  [-6.0, -1.0, [100, 1000, 5000, 10000], [0, 0, 0, 0], [[0, 500, 0, 1], [0, 1000, 0, 1], [0, 2000, 0, 1], [0, 4000, 0, 1]], 3000],
];

// [bands [[type, hz, db, q] x4], gainDb]
const MUTE_MODELS = [
  [[[0, 1000, 0, 1], [0, 1000, 0, 1], [0, 1000, 0, 1], [0, 1000, 0, 1]], 0],
  [[[4, 380, 0, 0.7], [0, 1800, 9, 2.2], [0, 5000, 3, 1.5], [2, 9000, -6, 0.7]], -3],
  [[[4, 200, 0, 0.7], [0, 800, 4, 1.5], [3, 2200, 0, 0.8], [0, 1000, 0, 1]], -2],
  [[[4, 700, 0, 0.8], [0, 1900, 12, 3.5], [0, 4200, 7, 3.0], [3, 9000, 0, 0.7]], -9],
  [[[0, 1100, 2, 1.0], [0, 1000, 0, 1], [0, 1000, 0, 1], [0, 1000, 0, 1]], 0],
];

const ADD_NODE_HARMONIC = [1, 2, 3, 4, 6, 9, 14, 21, 32];

function spectralEnvelopeDb(m, hz) {
  const H = m[2], D = m[3];
  hz = Math.max(hz, 1);
  if (hz <= H[0]) return D[0];
  for (let i = 1; i < 4; i++) {
    if (hz <= H[i] || i === 3) {
      const t = Math.log(hz / H[i - 1]) / Math.log(H[i] / H[i - 1]);
      return D[i - 1] + (D[i] - D[i - 1]) * t;
    }
  }
  return D[3];
}

// Harmonic amplitudes (linear, normalised to unit power) for a note at dynamics d (0..1)
function harmonicAmplitudes(values, f0, d, sr) {
  const m = INSTRUMENT_MODELS[clamp(Math.round(values.instrument), 0, 9)];
  const bc = Math.abs(values.dynBrCurve) < 1e-3 ? d : Math.pow(d, Math.pow(2, 2 * values.dynBrCurve));
  const slope = 0.5 * (m[0] + m[1]) + (1 - 2 * bc) * values.addDynSpread * 0.5 * (m[0] - m[1]);
  const out = [];
  let power = 0;
  for (let k = 0; k < Math.round(values.addPartials); k++) {
    const f = f0 * (k + 1);
    if (f >= 0.45 * sr) break;
    let a = Math.exp((spectralEnvelopeDb(m, f) + slope * k + values.addTilt * Math.log2(k + 1)) * 0.11512925464970229);
    const even = (k + 1) % 2 === 0;
    if (values.addOddEven > 0 && even) a *= 1 - values.addOddEven;
    else if (values.addOddEven < 0 && !even && k > 0) a *= 1 + values.addOddEven;
    out.push(a);
    power += a * a;
  }
  const norm = power > 0 ? 1 / Math.sqrt(power) : 0;
  return out.map(a => a * norm);
}

// RBJ biquad (same as designBiquad in Filters.h) and its magnitude at f
function biquadCoefs(type, fc, q, db, sr) {
  fc = clamp(fc, 10, sr * 0.45); q = Math.max(q, 0.05);
  const A = Math.pow(10, db / 40), w0 = 2 * Math.PI * fc / sr, cw = Math.cos(w0), sw = Math.sin(w0);
  const alpha = sw / (2 * q), sa = 2 * Math.sqrt(A) * alpha;
  let b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
  switch (type) {
    case BQ.lowShelf:
      b0 = A * ((A + 1) - (A - 1) * cw + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - sa);
      a0 = (A + 1) + (A - 1) * cw + sa; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - sa; break;
    case BQ.highShelf:
      b0 = A * ((A + 1) + (A - 1) * cw + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - sa);
      a0 = (A + 1) - (A - 1) * cw + sa; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - sa; break;
    case BQ.lowPass:  b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case BQ.highPass: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    case BQ.bandPass: b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
    default: b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
  }
  return [b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0];
}

function biquadMagDb(c, f, sr) {
  const w = 2 * Math.PI * f / sr;
  const cr = Math.cos(w), ci = -Math.sin(w), c2r = Math.cos(2 * w), c2i = -Math.sin(2 * w);
  const nr = c[0] + c[1] * cr + c[2] * c2r, ni = c[1] * ci + c[2] * c2i;
  const dr = 1 + c[3] * cr + c[4] * c2r, di = c[3] * ci + c[4] * c2i;
  return 10 * Math.log10((nr * nr + ni * ni) / Math.max(dr * dr + di * di, 1e-30));
}

// Body response (dB) at f with the current body settings, blended by bodyMix
function bodyResponseDb(values, f, sr) {
  const m = INSTRUMENT_MODELS[clamp(Math.round(values.instrument), 0, 9)];
  const shift = Math.pow(2, values.bodyShift / 12);
  let db = 0;
  for (const b of m[4]) db += biquadMagDb(biquadCoefs(b[0], b[1] * shift, b[3], b[2] * (0.5 + values.bodyReso), sr), f, sr);
  const lin = Math.pow(10, db / 20), mix = values.bodyMix;
  return 20 * Math.log10(Math.max(1e-4, Math.abs(1 + mix * (lin - 1))));   // magnitude-only approximation of the blend
}

// Mute response (dB) at f, plunger position p (0 closed .. 1 open)
function muteResponseDb(values, f, sr, p) {
  const type = Math.round(values.muteType);
  if (type === 0) return 0;
  const mm = MUTE_MODELS[type];
  let db = mm[1];
  for (const b of mm[0]) db += biquadMagDb(biquadCoefs(b[0], b[1], b[3], b[2], sr), f, sr);
  if (type === 4) {
    const fc = 350 * Math.pow(2, clamp(p, 0, 1) * 4.3), x = f / fc, k = 2 - 1.9 * 0.35;
    db += -10 * Math.log10((1 - x * x) ** 2 + (k * x) ** 2) - 6 * (1 - clamp(p, 0, 1));
  }
  const lin = Math.pow(10, db / 20), mix = values.muteMix;
  return 20 * Math.log10(Math.max(1e-4, Math.abs(1 + mix * (lin - 1))));
}

// Brassiness transfer curve (same shapes as shapeFn in Oversampler.h)
function shapeFn(type, x) {
  if (type === 1) { const t = x / (1 + Math.abs(x)); return t + 0.12 * t * t; }
  if (type === 2) { const c = clamp(x, -1, 1); return 1.5 * c - 0.5 * c * c * c; }
  const c = clamp(x, -3, 3), x2 = c * c, t = c * (27 + x2) / (27 + 9 * x2);
  return t + 0.18 * t * t;
}
