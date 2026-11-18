// ============================================================================
//  Brass Knuckles DSP engine: JavaScript twin of vst/Source/SynthEngine.h and
//  vst/Source/dsp/*.h. Kept line-for-line equivalent (same control rate, same
//  RNG, same maths, same order of operations) and checked against the C++
//  build by vst/tests/run-parity.ps1.
//
//  This whole function is stringified and loaded as an AudioWorklet module
//  (or run on the main thread as a ScriptProcessor fallback), so it must not
//  reference anything outside itself.
//
//  Performance notes: nothing allocates on the audio path. Per-chunk scratch
//  arrays live on the engine; envelope times are written into reused objects.
// ============================================================================
function synthWorkletModule() {
  'use strict';

  // ======================================================== dsp/Common.h ===
  const kPi = 3.141592653589793, kTwoPi = 6.283185307179586, kSqrt2 = 1.4142135623730951, kLn2 = 0.6931471805599453;
  const kSilence = 1e-4;

  const clampd = (v, lo, hi) => (v < lo ? lo : (v > hi ? hi : v));
  const clampi = (v, lo, hi) => (v < lo ? lo : (v > hi ? hi : v));
  const dbToGain = (db) => Math.pow(10, db / 20);
  const lround = (x) => (x < 0 ? -Math.round(-x) : Math.round(x)); // std::lround: halves away from zero

  class Rng {
    constructor() { this.s = 0x2545F491; }
    seed(v) { this.s = v >>> 0; }
    next() { this.s = (Math.imul(this.s, 1664525) + 1013904223) >>> 0; return this.s; }
    uni() { return (this.next() >>> 8) * (1 / 16777216); }
    bi() { return this.uni() * 2 - 1; }
  }

  function curveShape(p, c) {
    if (c > -1e-3 && c < 1e-3) return p;
    const k = 5 * c;
    return (1 - Math.exp(-k * p)) / (1 - Math.exp(-k));
  }

  function responseCurve(x, c) {
    x = clampd(x, 0, 1);
    if (c > -1e-3 && c < 1e-3) return x;
    return Math.pow(x, Math.pow(2, 2 * c));
  }

  function polyBlep(t, dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1; }
    if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
    return 0;
  }

  class SVF {
    constructor() { this.ic1 = 0; this.ic2 = 0; this.bp = 0; }
    reset() { this.ic1 = this.ic2 = this.bp = 0; }
    static gFor(fc, sr) {
      fc = Math.min(Math.max(fc, 20), sr * 0.45);
      return Math.tan(kPi * fc / sr);
    }
    static kFor(reso) { return 2 - 1.9 * reso; }
    process(x, g, k) {
      const a1 = 1 / (1 + g * (g + k));
      const a2 = g * a1;
      const a3 = g * a2;
      const v3 = x - this.ic2;
      const v1 = a1 * this.ic1 + a2 * v3;
      const v2 = this.ic2 + a2 * this.ic1 + a3 * v3;
      this.ic1 = 2 * v1 - this.ic1;
      this.ic2 = 2 * v2 - this.ic2;
      this.bp = v1;
      return v2;
    }
  }

  // ====================================================== dsp/Envelope.h ===
  class EnvTimes {
    constructor() { this.dly = 0; this.atk = 0.01; this.hld = 0; this.dec = 0.3; this.sus = 1; this.rel = 0.2; this.atkC = 0; this.decC = 0; this.relC = 0; }
  }
  const kEnvMinStage = 0.0011;
  const ST_IDLE = 0, ST_DELAY = 1, ST_ATTACK = 2, ST_HOLD = 3, ST_DECAY = 4, ST_SUSTAIN = 5, ST_RELEASE = 6;

  class Envelope {
    constructor() { this.stage = ST_IDLE; this.pos = 0; this.level = 0; this.from = 0; this.dt = 0; }
    active() { return this.stage !== ST_IDLE; }
    gateOn() { this.stage = ST_DELAY; this.pos = 0; this.from = this.level; }
    gateOff() {
      if (this.stage === ST_IDLE || this.stage === ST_RELEASE) return;
      this.stage = ST_RELEASE; this.pos = 0; this.from = this.level;
    }
    kill() { this.stage = ST_IDLE; this.level = 0; this.pos = 0; this.from = 0; }

    advance(e, dt) {
      this.dt = dt;
      for (;;) {
        switch (this.stage) {
          case ST_DELAY:
            if (!this.step(e.dly > kEnvMinStage ? e.dly : 0)) { this.level = this.from; return this.level; }
            this.stage = ST_ATTACK; this.pos = 0;
            break;
          case ST_ATTACK:
            if (!this.step(e.atk)) { this.level = this.from + (1 - this.from) * curveShape(this.pos, e.atkC); return this.level; }
            this.level = 1; this.stage = ST_HOLD; this.pos = 0;
            break;
          case ST_HOLD:
            if (!this.step(e.hld > kEnvMinStage ? e.hld : 0)) { this.level = 1; return this.level; }
            this.stage = ST_DECAY; this.pos = 0;
            break;
          case ST_DECAY:
            if (!this.step(e.dec)) { this.level = 1 + (e.sus - 1) * curveShape(this.pos, e.decC); return this.level; }
            this.level = e.sus;
            if (e.sus < kSilence) { this.kill(); return 0; }
            this.stage = ST_SUSTAIN; this.pos = 0;
            break;
          case ST_SUSTAIN:
            this.level = e.sus;
            if (e.sus < kSilence) this.kill();
            return this.level;
          case ST_RELEASE:
            if (!this.step(e.rel)) { this.level = this.from * (1 - curveShape(this.pos, e.relC)); return this.level; }
            this.kill();
            return 0;
          default:
            return this.level;
        }
      }
    }

    // consumes time from this.dt for a segment of length T; true when the segment finished
    step(T) {
      if (T <= 0) { this.pos = 1; return true; }
      const rem = (1 - this.pos) * T;
      if (this.dt < rem) { this.pos += this.dt / T; this.dt = 0; return false; }
      this.dt -= rem;
      this.pos = 1;
      return true;
    }
  }

  // ==================================================== dsp/Oscillator.h ===
  function oscWave(w, t, dt, pw) {
    switch (w) {
      case 1: { let t3 = t + 0.25; if (t3 >= 1) t3 -= 1; return 1 - 4 * Math.abs(t3 - 0.5); }
      case 2: return 2 * t - 1 - polyBlep(t, dt);
      case 3: {
        let t2 = t + (1 - pw); if (t2 >= 1) t2 -= 1;
        return (t < pw ? 1 : -1) + polyBlep(t, dt) - polyBlep(t2, dt) - (2 * pw - 1);
      }
      default: return Math.sin(kTwoPi * t);
    }
  }

  class MorphOsc {
    constructor() { this.phase = 0; }
    reset(p) { this.phase = p; }
    next(shape, pw, dt) {
      let i = Math.trunc(shape);
      if (i > 2) i = 2;
      if (i < 0) i = 0;
      const f = shape - i;
      let y = oscWave(i, this.phase, dt, pw);
      if (f > 1e-6) y += (oscWave(i + 1, this.phase, dt, pw) - y) * f;
      let p = this.phase + dt; if (p >= 1) p -= 1;
      this.phase = p;
      return y;
    }
  }

  // ======================================================= dsp/Filters.h ===
  const bqPeak = 0, bqLowShelf = 1, bqHighShelf = 2, bqLowPass = 3, bqHighPass = 4, bqBandPass = 5;

  // writes the coefficients into c (no allocation on the audio thread)
  function designBiquad(c, type, fc, q, db, sr) {
    fc = clampd(fc, 10, sr * 0.45);
    q = Math.max(q, 0.05);
    const A = Math.pow(10, db / 40);
    const w0 = kTwoPi * fc / sr;
    const cw = Math.cos(w0), sw = Math.sin(w0);
    const alpha = sw / (2 * q);
    const sa = 2 * Math.sqrt(A) * alpha;
    let b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (type) {
      case bqLowShelf:
        b0 = A * ((A + 1) - (A - 1) * cw + sa);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - sa);
        a0 = (A + 1) + (A - 1) * cw + sa;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - sa;
        break;
      case bqHighShelf:
        b0 = A * ((A + 1) + (A - 1) * cw + sa);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - sa);
        a0 = (A + 1) - (A - 1) * cw + sa;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - sa;
        break;
      case bqLowPass:
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
      case bqHighPass:
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
      case bqBandPass:
        b0 = alpha; b1 = 0; b2 = -alpha;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
      default:
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
        break;
    }
    c.b0 = b0 / a0; c.b1 = b1 / a0; c.b2 = b2 / a0; c.a1 = a1 / a0; c.a2 = a2 / a0;
  }
  const unityCoefs = () => ({ b0: 1, b1: 0, b2: 0, a1: 0, a2: 0 });

  class Biquad {
    constructor() { this.z1 = 0; this.z2 = 0; }
    reset() { this.z1 = this.z2 = 0; }
    process(x, c) {
      const y = c.b0 * x + this.z1;
      let z1 = c.b1 * x - c.a1 * y + this.z2;
      let z2 = c.b2 * x - c.a2 * y;
      if (z1 < 1e-30 && z1 > -1e-30) z1 = 0;
      if (z2 < 1e-30 && z2 > -1e-30) z2 = 0;
      this.z1 = z1; this.z2 = z2;
      return y;
    }
  }

  // =================================================== dsp/Oversampler.h ===
  function besselI0(x) {
    let sum = 1, term = 1;
    for (let k = 1; k < 30; ++k) {
      term *= (x / (2 * k)) * (x / (2 * k));
      sum += term;
    }
    return sum;
  }

  class HalfbandDesign {
    constructor(m, beta) {
      this.M = m;
      this.g = new Float64Array(16);
      const half = 2 * m;
      let sum = 0;
      for (let i = 0; i < m; ++i) {
        const d = 2 * i + 1;
        const ideal = Math.sin(kPi * d / 2) / (kPi * d);
        const r = d / half;
        const w = besselI0(beta * Math.sqrt(Math.max(0, 1 - r * r))) / besselI0(beta);
        this.g[i] = ideal * w;
        sum += this.g[i];
      }
      for (let i = 0; i < m; ++i) this.g[i] *= 0.25 / sum;
    }
  }
  const HB_A = new HalfbandDesign(14, 7.0);
  const HB_B = new HalfbandDesign(4, 6.0);
  const kHbMaxHist = 64;

  class HalfbandUp {
    constructor() { this.h = new Float64Array(2 * kHbMaxHist); this.p = 0; this.out1 = 0; }
    reset() { this.h.fill(0); this.p = 0; }
    // returns out0, leaves out1 in this.out1
    process(x, d) {
      const M = d.M, N = 2 * M, h = this.h, g = d.g;
      const p = this.p + 1 >= N ? 0 : this.p + 1;
      this.p = p;
      h[p] = x; h[p + N] = x;
      const b = p + N;
      let acc = 0;
      for (let i = 0; i < M; ++i) acc += g[i] * (h[b - (M - 1 - i)] + h[b - (M + i)]);
      this.out1 = h[b - (M - 1)];
      return 2 * acc;
    }
  }

  class HalfbandDown {
    constructor() { this.h = new Float64Array(2 * kHbMaxHist); this.p = 0; }
    reset() { this.h.fill(0); this.p = 0; }
    process(v0, v1, d) {
      const M = d.M, N = 4 * M, h = this.h, g = d.g;
      let p = this.p + 1 >= N ? 0 : this.p + 1;
      h[p] = v0; h[p + N] = v0;
      p = p + 1 >= N ? 0 : p + 1;
      h[p] = v1; h[p + N] = v1;
      this.p = p;
      const b = p + N;
      let acc = 0.5 * h[b - (2 * M - 1)];
      for (let i = 0; i < M; ++i) acc += g[i] * (h[b - (2 * M - 2 - 2 * i)] + h[b - (2 * M + 2 * i)]);
      return acc;
    }
  }

  const shapeBrass = 0, shapeReed = 1, shapeEdge = 2, shapeTube = 3, shapeTape = 4;

  function tanhRational(x) {
    x = clampd(x, -3, 3);
    const x2 = x * x;
    return x * (27 + x2) / (27 + 9 * x2);
  }

  function shapeFn(type, x) {
    switch (type) {
      case shapeReed: { const t = x / (1 + Math.abs(x)); return t + 0.12 * t * t; }
      case shapeEdge: { const c = clampd(x, -1, 1); return 1.5 * c - 0.5 * c * c * c; }
      case shapeTube: return tanhRational(x + 0.25) - tanhRational(0.25);
      case shapeTape: { const t = tanhRational(0.8 * x); return t / 0.8 - 0.15 * t * t * t; }
      default: { const t = tanhRational(x); return t + 0.18 * t * t; }
    }
  }

  class Oversampler {
    constructor() { this.upA = new HalfbandUp(); this.upB = new HalfbandUp(); this.downA = new HalfbandDown(); this.downB = new HalfbandDown(); }
    reset() { this.upA.reset(); this.upB.reset(); this.downA.reset(); this.downB.reset(); }
    process(x, factor, type) {
      if (factor <= 1) return shapeFn(type, x);
      const a0 = this.upA.process(x, HB_A), a1 = this.upA.out1;
      if (factor === 2) return this.downA.process(shapeFn(type, a0), shapeFn(type, a1), HB_A);
      const b0 = this.upB.process(a0, HB_B), b1 = this.upB.out1;
      const b2 = this.upB.process(a1, HB_B), b3 = this.upB.out1;
      const c0 = this.downB.process(shapeFn(type, b0), shapeFn(type, b1), HB_B);
      const c1 = this.downB.process(shapeFn(type, b2), shapeFn(type, b3), HB_B);
      return this.downA.process(c0, c1, HB_A);
    }
  }

  // =================================================== dsp/Instruments.h ===
  const kNumInstruments = 10, kSpecPoints = 4, kBodyBands = 4;
  const band = (type, hz, db, q) => ({ type, hz, db, q });
  const model = (softSlope, loudSlope, specHz, specDb, body, noiseHz, noiseQ) => ({ softSlope, loudSlope, specHz, specDb, body, noiseHz, noiseQ });
  const MODELS = [
    model(-7.0, -1.2, [300, 1200, 3000, 8000], [-6, 0, 0, -12],
      [band(bqLowShelf, 250, -4, 0.7), band(bqPeak, 1300, 5, 1.2), band(bqPeak, 2600, 3, 1.6), band(bqHighShelf, 7000, -4, 0.7)], 2500, 1.0),
    model(-9.0, -3.0, [250, 800, 2000, 6000], [-3, 0, -6, -20],
      [band(bqLowShelf, 150, 2, 0.7), band(bqPeak, 700, 4, 1.0), band(bqPeak, 1500, 2, 1.2), band(bqHighShelf, 3500, -7, 0.7)], 1800, 1.0),
    model(-8.0, -1.5, [150, 500, 1500, 5000], [-6, 0, 0, -12],
      [band(bqLowShelf, 120, -3, 0.7), band(bqPeak, 520, 5, 1.0), band(bqPeak, 1200, 3, 1.4), band(bqHighShelf, 5000, -4, 0.7)], 1500, 1.0),
    model(-10.0, -3.5, [150, 400, 1000, 3000], [-3, 0, -4, -20],
      [band(bqLowShelf, 100, 1, 0.7), band(bqPeak, 380, 5, 0.9), band(bqPeak, 900, 2, 1.2), band(bqHighShelf, 1800, -9, 0.7)], 1200, 1.0),
    model(-10.0, -3.5, [60, 250, 700, 2500], [-3, 0, -4, -20],
      [band(bqLowShelf, 50, -2, 0.7), band(bqPeak, 230, 5, 0.9), band(bqPeak, 600, 3, 1.2), band(bqHighShelf, 1400, -8, 0.7)], 800, 1.0),
    model(-5.0, -1.0, [500, 1500, 3500, 9000], [-6, 0, 0, -10],
      [band(bqPeak, 900, 2, 1.5), band(bqPeak, 1600, 5, 2.0), band(bqPeak, 3200, 4, 2.5), band(bqHighShelf, 8000, -3, 0.7)], 3500, 1.2),
    model(-5.5, -1.2, [400, 1200, 3000, 8000], [-6, 0, -1, -12],
      [band(bqPeak, 700, 2, 1.5), band(bqPeak, 1300, 5, 2.0), band(bqPeak, 2700, 4, 2.5), band(bqHighShelf, 7000, -3, 0.7)], 3000, 1.2),
    model(-6.0, -1.3, [250, 900, 2500, 7000], [-6, 0, -2, -14],
      [band(bqPeak, 500, 2, 1.5), band(bqPeak, 950, 5, 2.0), band(bqPeak, 2100, 4, 2.5), band(bqHighShelf, 6000, -3, 0.7)], 2500, 1.2),
    model(-6.5, -1.5, [150, 600, 1800, 5000], [-6, 0, -3, -15],
      [band(bqPeak, 350, 3, 1.5), band(bqPeak, 650, 5, 2.0), band(bqPeak, 1500, 4, 2.5), band(bqHighShelf, 4500, -4, 0.7)], 2000, 1.2),
    model(-6.0, -1.0, [100, 1000, 5000, 10000], [0, 0, 0, 0],
      [band(bqPeak, 500, 0, 1.0), band(bqPeak, 1000, 0, 1.0), band(bqPeak, 2000, 0, 1.0), band(bqPeak, 4000, 0, 1.0)], 3000, 1.0),
  ];
  const instrumentModel = (i) => MODELS[i < 0 ? 0 : (i >= kNumInstruments ? kNumInstruments - 1 : i)];
  const SPEC_LOG = MODELS.map(m => m.specHz.map(h => Math.log(h)));

  function spectralEnvelopeDbLog(instrument, logHz) {
    const i = instrument < 0 ? 0 : (instrument >= kNumInstruments ? kNumInstruments - 1 : instrument);
    const m = MODELS[i], L = SPEC_LOG[i];
    if (logHz <= L[0]) return m.specDb[0];
    for (let k = 1; k < kSpecPoints; ++k)
      if (logHz <= L[k] || k === kSpecPoints - 1)
        return m.specDb[k - 1] + (m.specDb[k] - m.specDb[k - 1]) * (logHz - L[k - 1]) / (L[k] - L[k - 1]);
    return m.specDb[kSpecPoints - 1];
  }

  const muteOpen = 0, mutePlungerType = 4;
  const flat = band(bqPeak, 1000, 0, 1);
  const MUTES = [
    { bands: [flat, flat, flat, flat], gainDb: 0 },
    { bands: [band(bqHighPass, 380, 0, 0.7), band(bqPeak, 1800, 9, 2.2), band(bqPeak, 5000, 3, 1.5), band(bqHighShelf, 9000, -6, 0.7)], gainDb: -3 },
    { bands: [band(bqHighPass, 200, 0, 0.7), band(bqPeak, 800, 4, 1.5), band(bqLowPass, 2200, 0, 0.8), flat], gainDb: -2 },
    { bands: [band(bqHighPass, 700, 0, 0.8), band(bqPeak, 1900, 12, 3.5), band(bqPeak, 4200, 7, 3.0), band(bqLowPass, 9000, 0, 0.7)], gainDb: -9 },
    { bands: [band(bqPeak, 1100, 2, 1.0), flat, flat, flat], gainDb: 0 },
  ];
  const muteModel = (t) => MUTES[t < 0 ? 0 : (t > 4 ? 4 : t)];

  // ====================================================== dsp/Additive.h ===
  const kMaxPartials = 32, kAddNodes = 9;
  const kAddNodeHarmonic = [1, 2, 3, 4, 6, 9, 14, 21, 32];
  const ADD = (() => {
    const T = {
      lo: new Int32Array(kMaxPartials), w: new Float64Array(kMaxPartials), log2n: new Float64Array(kMaxPartials),
      logn: new Float64Array(kMaxPartials), nodePos: new Float64Array(kAddNodes), nodeDly: new Float64Array(kAddNodes),
      nodeW: [], spf: new Int32Array(kMaxPartials + 1),
    };
    T.spf[0] = T.spf[1] = 1;
    for (let n = 2; n <= kMaxPartials; ++n) { let p = 2; while (n % p !== 0) ++p; T.spf[n] = p; }
    for (let k = 0; k < kMaxPartials; ++k) {
      const n = k + 1;
      T.log2n[k] = Math.log2(n);
      T.logn[k] = Math.log(n);
      let j = 0;
      while (j < kAddNodes - 2 && kAddNodeHarmonic[j + 1] < n) ++j;
      T.lo[k] = j;
      T.w[k] = (Math.log(n) - Math.log(kAddNodeHarmonic[j])) / (Math.log(kAddNodeHarmonic[j + 1]) - Math.log(kAddNodeHarmonic[j]));
    }
    for (let j = 0; j < kAddNodes; ++j) {
      T.nodePos[j] = Math.log2(kAddNodeHarmonic[j]) / 5;
      T.nodeDly[j] = Math.pow(T.nodePos[j], 0.8);
      const x = T.nodePos[j];
      const w0 = clampd(1 - x / 0.45, 0, 1), w2 = clampd((x - 0.55) / 0.45, 0, 1);
      T.nodeW.push([w0, 1 - w0 - w2, w2]);
    }
    return T;
  })();

  class AddSpectrum {
    constructor() { this.group = -1; this.stamp = 0; this.count = 0; this.lin = new Float64Array(kMaxPartials); }
  }

  class AddVoice {
    constructor() {
      this.c = 1; this.s = 0; this.dt = 0;
      this.node = []; for (let j = 0; j < kAddNodes; ++j) this.node.push(new Envelope());
      this.amp = new Float64Array(kMaxPartials);
      this.count = 0;
    }
    kill() { for (const e of this.node) e.kill(); this.amp.fill(0); this.count = 0; }
  }

  // =========================================================== dsp/Lfo.h ===
  const lfoSine = 0, lfoTriangle = 1, lfoSquare = 2, lfoSawUp = 3, lfoSawDown = 4, lfoSampleHold = 5;
  const kLfoDivBeats = [4, 3, 2, 4 / 3, 1.5, 1, 2 / 3, 0.75, 0.5, 1 / 3, 0.375, 0.25, 1 / 6, 0.125, 1 / 12];
  const kNumLfoDivs = 15;

  class Lfo {
    constructor() { this.phase = 0; this.held = 0; this.out = 0; this.primed = false; }
    start(p, rng) { this.phase = p; this.held = rng.bi(); this.primed = false; }
    retrigger(p, rng) { this.phase = p; this.held = rng.bi(); }
    shape(wave, offset) {
      let t = this.phase + offset;
      t -= Math.floor(t);
      switch (wave) {
        case lfoTriangle: { let t3 = t + 0.25; if (t3 >= 1) t3 -= 1; return 1 - 4 * Math.abs(t3 - 0.5); }
        case lfoSquare: return t < 0.5 ? 1 : -1;
        case lfoSawUp: return 2 * t - 1;
        case lfoSawDown: return 1 - 2 * t;
        case lfoSampleHold: return this.held;
        default: return Math.sin(kTwoPi * t);
      }
    }
    advance(inc, rng) {
      this.phase += inc;
      if (this.phase >= 1) { this.phase -= Math.floor(this.phase); this.held = rng.bi(); }
    }
    smooth(target, coef) {
      if (!this.primed) { this.out = target; this.primed = true; }
      else this.out += (target - this.out) * coef;
      return this.out;
    }
  }

  // ============================================================ dsp/Fx.h ===
  const flushTiny = (x) => ((x < 1e-30 && x > -1e-30) ? 0 : x);

  class Chorus {
    constructor() { this.bufL = new Float64Array(1); this.bufR = new Float64Array(1); this.mask = 0; this.w = 0; this.phase = 0; this.dL = 0; this.dR = 0; }
    prepare(sr) {
      let size = 1;
      while (size < Math.trunc(0.05 * sr) + 4) size <<= 1;
      this.bufL = new Float64Array(size);
      this.bufR = new Float64Array(size);
      this.mask = size - 1;
      this.w = 0; this.phase = 0;
      this.dL = this.dR = 0.012 * sr;
    }
    reset() { this.bufL.fill(0); this.bufR.fill(0); }
    read(b, d) {
      const pos = this.w - d;
      const i = Math.floor(pos);
      const f = pos - i;
      const a = b[i & this.mask], c = b[(i + 1) & this.mask];
      return a + (c - a) * f;
    }
  }

  const kRvLines = 8;
  const kPlateMs = [11.3, 13.7, 17.2, 19.9, 23.1, 27.4, 30.3, 35.7];
  const kHallMs = [29.7, 37.1, 41.1, 43.7, 53.0, 59.9, 67.1, 73.3];
  const kDiffMs = [2.96, 2.23, 7.9, 5.77];

  class Reverb {
    constructor() {
      this.line = []; this.diff = []; this.pre = new Float64Array(1);
      this.len = new Int32Array(kRvLines); this.pos = new Int32Array(kRvLines);
      this.dlen = new Int32Array(4); this.dpos = new Int32Array(4);
      this.lp = new Float64Array(kRvLines); this.g = new Float64Array(kRvLines);
      this.o = new Float64Array(kRvLines);
      this.preMask = 0; this.preW = 0; this.type = -1; this.sr = 48000;
      this.outL = 0; this.outR = 0;
    }
    prepare(sampleRate) {
      const sr = this.sr = sampleRate;
      this.line = [];
      for (let i = 0; i < kRvLines; ++i) this.line.push(new Float64Array(Math.trunc(Math.max(kPlateMs[i], kHallMs[i]) * 0.001 * sr) + 2));
      this.diff = [];
      for (let i = 0; i < 4; ++i) {
        this.dlen[i] = Math.max(1, lround(kDiffMs[i] * 0.001 * sr));
        this.diff.push(new Float64Array(this.dlen[i]));
        this.dpos[i] = 0;
      }
      let size = 1;
      while (size < Math.trunc(0.25 * sr) + 4) size <<= 1;
      this.pre = new Float64Array(size);
      this.preMask = size - 1;
      this.preW = 0;
      this.type = -1;
    }
    reset() {
      for (const l of this.line) l.fill(0);
      for (const d of this.diff) d.fill(0);
      this.pre.fill(0);
      for (let i = 0; i < kRvLines; ++i) { this.lp[i] = 0; this.pos[i] = 0; }
    }
    setType(t) {
      if (t === this.type) return;
      this.type = t;
      const ms = t === 0 ? kPlateMs : kHallMs;
      for (let i = 0; i < kRvLines; ++i) { this.len[i] = lround(ms[i] * 0.001 * this.sr); this.pos[i] = 0; this.lp[i] = 0; }
      for (const l of this.line) l.fill(0);
    }
    setDecay(t60) {
      for (let i = 0; i < kRvLines; ++i) this.g[i] = Math.pow(10, -3 * this.len[i] / (Math.max(t60, 0.05) * this.sr));
    }
    // results in this.outL / this.outR
    process(input, preDelay, damp) {
      const pre = this.pre;
      pre[this.preW] = input;
      let x = pre[(this.preW - preDelay) & this.preMask];
      this.preW = (this.preW + 1) & this.preMask;
      for (let d = 0; d < 4; ++d) {
        const b = this.diff[d], dp = this.dpos[d];
        const z = b[dp];
        const v = x + 0.6 * z;
        b[dp] = flushTiny(v);
        x = z - 0.6 * v;
        this.dpos[d] = dp + 1 >= this.dlen[d] ? 0 : dp + 1;
      }
      const o = this.o, lp = this.lp, line = this.line, pos = this.pos, g = this.g;
      for (let i = 0; i < kRvLines; ++i) {
        const y = line[i][pos[i]];
        lp[i] = flushTiny(lp[i] + (y - lp[i]) * damp);
        o[i] = lp[i] * g[i];
      }
      for (let h = 1; h < kRvLines; h <<= 1)
        for (let i = 0; i < kRvLines; i += h << 1)
          for (let j = i; j < i + h; ++j) { const a = o[j], b = o[j + h]; o[j] = a + b; o[j + h] = a - b; }
      const norm = 0.35355339059327373;
      let l = 0, r = 0;
      for (let i = 0; i < kRvLines; ++i) {
        const y = line[i][pos[i]];
        if (i & 1) r += y; else l += y;
        line[i][pos[i]] = flushTiny(o[i] * norm + ((i & 2) ? -x : x));
        pos[i] = pos[i] + 1 >= this.len[i] ? 0 : pos[i] + 1;
      }
      this.outL = l * 0.5; this.outR = r * 0.5;
    }
  }

  // ======================================================= SynthEngine.h ===
  const kNumOsc = 3, kMaxPoly = 64, kMaxPlayers = 8, kPoolSize = 288, kChunk = 32;
  const kVoiceGain = 0.45, kKillTime = 0.006, kNumChannels = 16, kLn10Over20 = 0.11512925464970229;

  const modePoly = 0, modeMono = 1, modeLegato = 2;
  const fltLP12 = 0, fltLP24 = 1, fltBP = 2;
  const plKnob = 0, plWheel = 1, plBreath = 2, plExpression = 3, plAftertouch = 4, plCC = 5;
  const engModeled = 0, engAnalog = 1;
  const artNormal = 0, artFall = 1, artDoit = 2, artRip = 3, artShake = 4, artScoop = 5, kNumArticulations = 6;

  const kNumLfo = 2, kNumModSlots = 12;
  const srcNone = 0, srcLfo1 = 1, srcLfo2 = 2, srcAmpEnv1 = 3, srcPitchEnv1 = 6, srcTmbEnv1 = 9,
        srcHarmLo = 12, srcHarmMid = 13, srcHarmHi = 14, srcFltEnv = 15, srcVelocity = 16, srcDynamics = 17,
        srcBreath = 18, srcExpression = 19, srcAftertouch = 20, srcMpePressure = 21, srcMpeSlide = 22, srcMpeBend = 23,
        srcModWheel = 24, srcPitchBend = 25, srcKey = 26, srcRandom = 27, srcVibrato = 28, kNumModSources = 29;
  const dstNone = 0, dstPitchAll = 1, dstOsc1Pitch = 2, dstOsc1Level = 5, dstOsc1Shape = 8, dstOsc1PW = 11, dstOsc1Bright = 14,
        dstHarmLevel = 17, dstHarmTilt = 18, dstFltCutoff = 19, dstFltReso = 20, dstDynamics = 21, dstBrightness = 22,
        dstBrassiness = 23, dstBreathNoise = 24, dstLipBuzz = 25, dstVibDepth = 26, dstVibRate = 27, dstGrowl = 28,
        dstFlutter = 29, dstAmp = 30, dstPan = 31, dstLfo1Rate = 32, dstPlunger = 34, dstBodyShift = 35,
        dstFxDrive = 36, dstChorusMix = 37, dstReverbMix = 38, kNumModDests = 39;
  const kFirstGlobalDest = dstPlunger;

  const isBipolarSource = (s) => s === srcLfo1 || s === srcLfo2 || s === srcMpeBend || s === srcPitchBend || s === srcKey || s === srcRandom || s === srcVibrato;

  // ---- parameter snapshot (EngineParams) ----
  function newEngineParams() {
    const osc = () => ({
      on: false, shape: 2, pw: 0.5, semis: 0, level: 0.7, cutHarm: 12, res: 0.1, amp: new EnvTimes(),
      ampVel: 0, ampVelTime: 0, ampKeyTime: 0, ampKeyAmt: 0,
      pAmt: 0, pTime: 0.06, pCrv: 0.5, blipAmt: 0, blipTime: 0.03, pVel: 0, pKeyTime: 0,
      tmb: new EnvTimes(), tCut: 0, tShape: 0, tVel: 0, tKeyTime: 0,
    });
    const lfo = () => ({ on: false, wave: lfoSine, rate: 2, sync: false, div: 8, perVoice: false, retrig: false, phase: 0, amount: 1 });
    const slot = () => ({ on: true, src: srcNone, via: srcNone, dst: dstNone, amount: 0 });
    return {
      osc: [osc(), osc(), osc()],
      instrument: 0, engineMode: 0, ampGain: 1, master: 0.7, tuneSemis: 0, transpose: 0, polyphony: 32,
      voiceMode: modePoly, glideOn: false, glideLegatoOnly: true, glideTime: 0.08, glideCurve: 0, slurDip: 0,
      bendRange: 2, mpeOn: false, mpeBend: 48, oversample: 1,
      addOn: false, addLevel: 0.6, addPartials: 24, addTilt: 0, addOddEven: 0, addSpecDly: 0.3, addDynSpread: 0.5, addCurve: 0.3,
      band: [0, 1, 2].map(() => ({ atk: 0.03, dec: 0.3, sus: 0.8, rel: 0.15, lvl: 1 })),
      wBreath: 1, wExpr: 0, wAT: 0, wMPE: 1, dynVel: 0.3, breathCC: 2,
      dynCurve: 0, dynRange: 24, dynBright: 3, dynBrCurve: 0.4, dynBrass: 0.5, dynAttack: 0.5, dynSmooth: 0.02,
      fltOn: false, fltType: fltLP12, fltCutoff: 8000, fltReso: 0.1, fltKey: 0.5, fltDyn: 0, fltEnvAmt: 0, fltVel: 0.5, fenv: new EnvTimes(),
      brsAmt: 0.2, brsTone: 0.8, brsType: shapeBrass,
      lipAmt: 0.3, lipTone: 0.5, lipDecay: 0.03, lipPitch: 0.6,
      brNoise: 0.1, brTone: 0.5, brDyn: 0.7, keyClick: 0, chiff: 0, tongue: 1,
      bodyMix: 0.6, bodyShift: 0, bodyReso: 0.5, muteType: muteOpen, muteMix: 1, mutePlunger: 0.5, muteSrc: plWheel, muteCC: 4,
      vibDepth: 15, vibAmp: 0.1, vibRate: 5.5, vibDelay: 0.3, vibFade: 0.5, vibDrift: 0.3, vibWheel: 0,
      growlAmt: 0, growlRate: 28, growlRough: 0.5, growlDyn: 0.3, flutAmt: 0, flutRate: 22,
      artKs: true, artAuto: false, artKsBase: 12, autoLo: 45, autoHi: 115,
      fallRng: 7, fallTime: 0.35, doitRng: 5, doitTime: 0.3, ripRng: 7, ripTime: 0.12,
      shakeRate: 6.5, shakeRng: 3, scoopRng: 3, scoopTime: 0.12, autoGap: 0.4,
      secPlayers: 1, anaUnison: 1, secDetune: 8, secTiming: 20, secVibDesync: 0.7, secTimbre: 0.3, secWidth: 0.7,
      anaDetune: 12, anaSpread: 0.5, anaBlat: 0.3,
      lfo: [lfo(), lfo()],
      mod: Array.from({ length: kNumModSlots }, slot),
      fxDrvOn: false, eqOn: false, chOn: false, rvOn: false, fxDrvType: 0, rvType: 1,
      fxDrvAmt: 0.3, fxDrvTone: 0.7, fxDrvMix: 1,
      eqLoGain: 0, eqLoFreq: 200, eqMidGain: 0, eqMidFreq: 1200, eqMidQ: 1, eqHiGain: 0, eqHiFreq: 6000,
      chRate: 0.6, chDepth: 0.4, chMix: 0.35, rvDecay: 1.8, rvPre: 20, rvDamp: 0.5, rvMix: 0.2, stWidth: 1,
    };
  }

  // Twin of EngineParamsFromValues.h. values: { paramId: number }. Values are rounded to
  // float32 first, because the plugin stores them as floats.
  function makeEngineParams(values, p) {
    const num = (id) => Math.fround(+values[id]);
    const choice = (id, max) => clampi(lround(num(id)), 0, max);
    const on = (id) => num(id) >= 0.5;
    const inum = (id) => lround(num(id));

    p.instrument = choice('instrument', 9);
    p.engineMode = choice('engineMode', 1);
    p.ampGain = Math.pow(10, num('ampLevel') / 20);
    p.master = num('master');
    p.tuneSemis = num('tune') / 100;
    p.transpose = inum('transpose');
    p.polyphony = inum('polyphony');
    p.voiceMode = choice('voiceMode', 2);
    p.glideOn = on('glideOn');
    p.glideLegatoOnly = choice('glideMode', 1) === 1;
    p.glideTime = num('glideTime');
    p.glideCurve = num('glideCurve');
    p.slurDip = num('slurDip');
    p.bendRange = inum('bendRange');
    p.mpeOn = on('mpeOn');
    p.mpeBend = inum('mpeBend');
    p.oversample = choice('oversample', 2);

    for (let k = 0; k < kNumOsc; ++k) {
      const pre = `osc${k + 1}`;
      const f = (field) => num(pre + field);
      const o = p.osc[k];
      o.on = on(pre + 'On');
      o.shape = f('Shape');
      o.pw = f('PW');
      o.semis = f('Pitch') + f('Fine') / 100;
      o.level = f('Level');
      o.cutHarm = f('Cut');
      o.res = f('Res');
      o.amp.dly = f('AmpDly'); o.amp.atk = f('AmpAtk'); o.amp.hld = f('AmpHld'); o.amp.dec = f('AmpDec');
      o.amp.sus = f('AmpSus'); o.amp.rel = f('AmpRel');
      o.amp.atkC = f('AmpAtkCrv'); o.amp.decC = f('AmpDecCrv'); o.amp.relC = f('AmpRelCrv');
      o.ampVel = f('AmpVel'); o.ampVelTime = f('AmpVelTime'); o.ampKeyTime = f('AmpKeyTime'); o.ampKeyAmt = f('AmpKeyAmt');
      o.pAmt = f('PEnvAmt'); o.pTime = f('PEnvTime'); o.pCrv = f('PEnvCrv');
      o.blipAmt = f('BlipAmt'); o.blipTime = f('BlipTime'); o.pVel = f('PEnvVel'); o.pKeyTime = f('PEnvKeyTime');
      o.tmb.dly = f('TEnvDly'); o.tmb.atk = f('TEnvAtk'); o.tmb.hld = 0; o.tmb.dec = f('TEnvDec');
      o.tmb.sus = f('TEnvSus'); o.tmb.rel = f('TEnvRel');
      o.tmb.atkC = o.tmb.decC = o.tmb.relC = f('TEnvCrv');
      o.tCut = f('TEnvCut'); o.tShape = f('TEnvShape'); o.tVel = f('TEnvVel'); o.tKeyTime = f('TEnvKeyTime');
    }

    p.wBreath = num('dynBreath');
    p.wExpr = num('dynExpr');
    p.wAT = num('dynAT');
    p.wMPE = num('dynMPE');
    p.dynVel = num('dynVel');
    p.breathCC = inum('dynBreathCC');
    p.dynCurve = num('dynCurve');
    p.dynRange = num('dynRange');
    p.dynBright = num('dynBright');
    p.dynBrCurve = num('dynBrCurve');
    p.dynBrass = num('dynBrass');
    p.dynAttack = num('dynAttack');
    p.dynSmooth = num('dynSmooth');

    p.fltOn = on('fltOn');
    p.fltType = choice('fltType', 2);
    p.fltCutoff = num('fltCutoff');
    p.fltReso = num('fltReso');
    p.fltKey = num('fltKey');
    p.fltDyn = num('fltDyn');
    p.fltEnvAmt = num('fltEnvAmt');
    p.fenv.dly = 0; p.fenv.atk = num('fltAtk'); p.fenv.hld = 0; p.fenv.dec = num('fltDec');
    p.fenv.sus = num('fltSus'); p.fenv.rel = num('fltRel');
    p.fenv.atkC = 0.3; p.fenv.decC = 0.6; p.fenv.relC = 0.6;
    p.fltVel = num('fltVel');

    p.addOn = on('addOn');
    p.addLevel = num('addLevel');
    p.addPartials = inum('addPartials');
    p.addTilt = num('addTilt');
    p.addOddEven = num('addOddEven');
    p.addSpecDly = num('addSpecDly');
    p.addDynSpread = num('addDynSpread');
    p.addCurve = num('addCurve');
    const bands = ['Lo', 'Mid', 'Hi'];
    for (let b = 0; b < 3; ++b) {
      const B = p.band[b], pre = 'add' + bands[b];
      B.atk = num(pre + 'Atk'); B.dec = num(pre + 'Dec'); B.sus = num(pre + 'Sus');
      B.rel = num(pre + 'Rel'); B.lvl = num(pre + 'Lvl');
    }

    p.brsAmt = num('brsAmt');
    p.brsType = choice('brsType', 2);
    p.brsTone = num('brsTone');

    p.lipAmt = num('lipAmt');
    p.lipTone = num('lipTone');
    p.lipDecay = num('lipDecay');
    p.lipPitch = num('lipPitch');
    p.brNoise = num('brNoise');
    p.brTone = num('brTone');
    p.brDyn = num('brDyn');
    p.keyClick = num('keyClick');
    p.chiff = num('chiff');
    p.tongue = choice('tongue', 2);

    p.bodyMix = num('bodyMix');
    p.bodyShift = num('bodyShift');
    p.bodyReso = num('bodyReso');
    p.muteType = choice('muteType', 4);
    p.muteMix = num('muteMix');
    p.mutePlunger = num('mutePlunger');
    p.muteSrc = choice('muteSrc', 5);
    p.muteCC = inum('muteCC');

    p.growlAmt = num('growlAmt');
    p.growlRate = num('growlRate');
    p.growlRough = num('growlRough');
    p.growlDyn = num('growlDyn');
    p.flutAmt = num('flutAmt');
    p.flutRate = num('flutRate');

    p.artKs = on('artKs');
    p.artKsBase = inum('artKsBase');
    p.fallRng = num('artFallRng');
    p.fallTime = num('artFallTime');
    p.doitRng = num('artDoitRng');
    p.doitTime = num('artDoitTime');
    p.ripRng = num('artRipRng');
    p.ripTime = num('artRipTime');
    p.shakeRate = num('artShakeRate');
    p.shakeRng = num('artShakeRng');
    p.scoopRng = num('artScoopRng');
    p.scoopTime = num('artScoopTime');
    p.artAuto = on('artAuto');
    p.autoGap = num('artAutoGap');
    p.autoLo = inum('artAutoLo');
    p.autoHi = inum('artAutoHi');

    p.secPlayers = inum('secPlayers');
    p.secDetune = num('secDetune');
    p.secTiming = num('secTiming');
    p.secVibDesync = num('secVibDesync');
    p.secTimbre = num('secTimbre');
    p.secWidth = num('secWidth');
    p.anaUnison = inum('anaUnison');
    p.anaDetune = num('anaDetune');
    p.anaSpread = num('anaSpread');
    p.anaBlat = num('anaBlat');

    for (let i = 0; i < kNumLfo; ++i) {
      const l = p.lfo[i], pre = `lfo${i + 1}`;
      l.on = on(pre + 'On');
      l.wave = choice(pre + 'Wave', 5);
      l.rate = num(pre + 'Rate');
      l.sync = on(pre + 'Sync');
      l.div = choice(pre + 'Div', kNumLfoDivs - 1);
      l.perVoice = choice(pre + 'Mode', 1) === 1;
      l.retrig = choice(pre + 'Trig', 1) === 1;
      l.phase = num(pre + 'Phase') / 360;
      l.amount = num(pre + 'Amount');
    }
    for (let s = 0; s < kNumModSlots; ++s) {
      const m = p.mod[s], pre = `mod${s + 1}`;
      m.on = on(pre + 'On');
      m.src = choice(pre + 'Src', kNumModSources - 1);
      m.via = choice(pre + 'Via', kNumModSources - 1);
      m.dst = choice(pre + 'Dst', kNumModDests - 1);
      m.amount = num(pre + 'Amt');
    }

    p.fxDrvOn = on('fxDrvOn');
    p.fxDrvType = choice('fxDrvType', 1);
    p.fxDrvAmt = num('fxDrvAmt');
    p.fxDrvTone = num('fxDrvTone');
    p.fxDrvMix = num('fxDrvMix');
    p.eqOn = on('eqOn');
    p.eqLoGain = num('eqLoGain');
    p.eqLoFreq = num('eqLoFreq');
    p.eqMidGain = num('eqMidGain');
    p.eqMidFreq = num('eqMidFreq');
    p.eqMidQ = num('eqMidQ');
    p.eqHiGain = num('eqHiGain');
    p.eqHiFreq = num('eqHiFreq');
    p.chOn = on('chOn');
    p.chRate = num('chRate');
    p.chDepth = num('chDepth');
    p.chMix = num('chMix');
    p.rvOn = on('rvOn');
    p.rvType = choice('rvType', 1);
    p.rvDecay = num('rvDecay');
    p.rvPre = num('rvPre');
    p.rvDamp = num('rvDamp');
    p.rvMix = num('rvMix');
    p.stWidth = num('stWidth');

    p.vibDepth = num('vibDepth');
    p.vibAmp = num('vibAmp');
    p.vibRate = num('vibRate');
    p.vibDelay = num('vibDelay');
    p.vibFade = num('vibFade');
    p.vibDrift = num('vibDrift');
    p.vibWheel = num('vibWheel');
    return p;
  }

  // ---- voices ----
  class OscVoice {
    constructor() {
      this.osc = new MorphOsc(); this.filt = new SVF(); this.amp = new Envelope(); this.tmb = new Envelope();
      this.ampScale = 1; this.ampTime = 1; this.ampAtkTime = 1; this.pScale = 1; this.pTime = 1; this.tScale = 1; this.tTime = 1; this.tAtkTime = 1;
      this.dt = 0; this.lvl = 0; this.shape = 2; this.pw = 0.5; this.g = 0; this.k = 2;
    }
  }

  class Voice {
    constructor() {
      this.o = [new OscVoice(), new OscVoice(), new OscVoice()];
      this.add = new AddVoice();
      this.fenv = new Envelope();
      this.f1 = new SVF(); this.f2 = new SVF();
      this.os = new Oversampler();
      this.nrng = new Rng();
      this.lipF = new SVF(); this.noiseF = new SVF();
      this.on = false;
      this.note = -1; this.channel = 0; this.group = 0; this.player = 0;
      this.vel = 0; this.dyn = 0; this.dynOnset = 0; this.fltVelScale = 1; this.addAtkScale = 1;
      this.age = 0;
      this.held = false; this.sustained = false; this.killing = false;
      this.kill = 1;
      this.tNote = 0; this.tVib = 0; this.tTrans = 0; this.tClick = 0;
      this.pitch = 60; this.glideFrom = 60; this.glideTo = 60; this.glidePos = 1;
      this.slurT = 1e9; this.slurDur = 0.06;
      this.vibPhase = 0; this.driftR = 0; this.driftRT = 0; this.driftD = 0; this.driftDT = 0;
      this.driftLeft = 0;
      this.growlPh = 0; this.growlN = 0; this.growlNT = 0; this.flutPh = 0;
      this.growlLeft = 0;
      this.pressure = 0; this.pressureSeen = false;
      this.tgLip = 1; this.tgDecay = 1; this.tgScoop = 1; this.tgBlip = 1;
      this.lipPeak = 0; this.chiffPeak = 0; this.clickPeak = 0;
      this.gain = 0; this.fg = 0; this.fk = 2; this.panL = 1; this.panR = 1;
      this.drv = 1; this.drvInv = 1; this.lipLvl = 0; this.lipG = 0.1; this.noiseLvl = 0; this.nG = 0.1; this.clickLvl = 0;
      this.lipPhase = 0; this.dcX = 0; this.dcY = 0; this.toneZ = 0; this.nPrev = 0;
      this.snap = true;
      this.artAtk = artNormal; this.artShake = false;
      this.relPending = false; this.relKind = artNormal;
      this.relT = 0; this.relDur = 0; this.artSemis = 0;
      this.noteStart = 0;
      this.plDetune = 0; this.plDelay = 0; this.plRelDelay = 0; this.plBright = 0; this.plGain = 1; this.plVibRate = 1; this.plVibDelay = 1; this.plPan = 0;
      this.plCount = 1;
      this.panMod = false;
      this.delayLeft = 0;
      this.slide = -1;
      this.lfo = [new Lfo(), new Lfo()];
      this.rnd = 0;
    }
  }

  const TG_LIP = [0.45, 1.0, 1.7], TG_DECAY = [1.6, 1.0, 0.6], TG_SCOOP = [1.3, 1.0, 0.75], TG_BLIP = [0.5, 1.0, 1.6], TG_ATK = [1.5, 1.0, 0.6];
  const kSpecCache = 32;

  class SynthEngine {
    constructor() {
      this.voices = [];
      for (let i = 0; i < kPoolSize; ++i) this.voices.push(new Voice());
      this.mixL = new Float64Array(kChunk); this.mixR = new Float64Array(kChunk);
      this.keys = new Int32Array(128); this.keyCount = 0;
      this.params = newEngineParams();
      this.rng = new Rng();
      this.sr = 48000; this.masterGain = 0.7; this.masterSmooth = 0.001;
      this.wheelSmooth = 0.5; this.wheel = 0; this.wheelTarget = 0; this.driftSmooth = 0.1; this.dynCoef = 1; this.busSmooth = 0.1; this.dcR = 0.999;
      this.breath = 0; this.expression = 1; this.aftertouch = 0;
      this.seenBreath = false; this.seenExpr = false; this.seenAT = false;
      this.bendGlobal = 0;
      this.bendCh = new Float64Array(kNumChannels);
      this.ccValue = new Float64Array(128);
      // bus
      this.bodyC = [0, 1, 2, 3].map(unityCoefs); this.muteC = [0, 1, 2, 3].map(unityCoefs);
      this.bodyL = [0, 1, 2, 3].map(() => new Biquad()); this.bodyR = [0, 1, 2, 3].map(() => new Biquad());
      this.muteL = [0, 1, 2, 3].map(() => new Biquad()); this.muteR = [0, 1, 2, 3].map(() => new Biquad());
      this.plL = new SVF(); this.plR = new SVF();
      this.busKey = -1; this.muteGain = 1; this.muteKind = -1;
      this.plungerCur = 0.5; this.plungerFrom = 0.5; this.bodyMixCur = 0; this.bodyMixFrom = 0; this.muteMixCur = 0; this.muteMixFrom = 0;
      this.bpm = 120; this.ppqAtBlock = 0; this.playing = false;
      // modulation
      this.lfoG = [new Lfo(), new Lfo()];
      this.lfoGlobal = new Float64Array(kNumLfo); this.lfoInc = new Float64Array(kNumLfo);
      this.slotAmt = new Float64Array(kNumModSlots);
      this.activeSlots = Array.from({ length: kNumModSlots }, () => ({ src: 0, via: 0, dst: 0, amt: 0 }));
      this.gDst = new Float64Array(kNumModDests);
      this.numActive = 0; this.samplesIntoBlock = 0;
      this.brsToneCoef = 1;
      this.ctlSmooth = 0.5; this.lfoSmooth = 0.5;
      // FX
      this.chorus = new Chorus(); this.reverb = new Reverb();
      this.fxOsL = new Oversampler(); this.fxOsR = new Oversampler();
      this.eqC = [0, 1, 2].map(unityCoefs);
      this.eqL = [0, 1, 2].map(() => new Biquad()); this.eqR = [0, 1, 2].map(() => new Biquad());
      this.ampCur = 1; this.drvG = 1; this.drvComp = 1; this.drvCompTarget = 1; this.drvMixCur = 1; this.drvLpL = 0; this.drvLpR = 0; this.drvPin = 0; this.drvPout = 0;
      this.drvDcXL = 0; this.drvDcYL = 0; this.drvDcXR = 0; this.drvDcYR = 0;
      this.eqKey = -1e300; this.chMixCur = 0; this.rvMixCur = 0; this.rvNorm = 1; this.rvDecaySet = -1; this.widthCur = 1;
      this.rvTypeSet = -1;
      this.drvWasOn = false; this.eqWasOn = false; this.chWasOn = false; this.rvWasOn = false; this.fxInit = true;

      this.specCache = Array.from({ length: kSpecCache }, () => new AddSpectrum());
      this.chunkStamp = 0;
      this.artSmooth = 0.5; this.now = 0; this.lastAllUp = -1e9;
      this.latchedArt = artNormal; this.pendingAtk = artNormal;
      this.lastNote = -1; this.monoGroup = 0; this.groupCounter = 0;
      this.lastPitch = 60; this.lastDyn = 0;
      this.ageCounter = 0;
      this.sustainPedal = false; this.firstParams = true;
      this.active = 0;
      this.peak = 0;

      // per-chunk scratch (renderVoice is not re-entrant)
      this.sDst = new Float64Array(kNumModDests);
      this.sSrc = new Float64Array(kNumModSources);
      this.sEnv = new EnvTimes();
      this.sTdt = new Float64Array(kNumOsc); this.sTlvl = new Float64Array(kNumOsc); this.sTshape = new Float64Array(kNumOsc);
      this.sTpw = new Float64Array(kNumOsc); this.sTg = new Float64Array(kNumOsc); this.sTk = new Float64Array(kNumOsc);
      this.sDt = new Float64Array(kNumOsc); this.sDdt = new Float64Array(kNumOsc); this.sLv = new Float64Array(kNumOsc); this.sDlv = new Float64Array(kNumOsc);
      this.sSh = new Float64Array(kNumOsc); this.sDsh = new Float64Array(kNumOsc); this.sPw = new Float64Array(kNumOsc); this.sDpw = new Float64Array(kNumOsc);
      this.sG = new Float64Array(kNumOsc); this.sDg = new Float64Array(kNumOsc); this.sKk = new Float64Array(kNumOsc); this.sDkk = new Float64Array(kNumOsc);
      this.sRun = new Uint8Array(kNumOsc);
      this.sTA = new Float64Array(kMaxPartials); this.sAA = new Float64Array(kMaxPartials); this.sDaA = new Float64Array(kMaxPartials);
      this.sNodeLvl = new Float64Array(kAddNodes);
      this.sTilt = new Float64Array(kMaxPartials + 1);
    }

    prepare(sampleRate) {
      const sr = this.sr = sampleRate;
      this.masterSmooth = 1 - Math.exp(-1 / (0.02 * sr));
      this.wheelSmooth = 1 - Math.exp(-kChunk / (0.010 * sr));
      this.driftSmooth = 1 - Math.exp(-kChunk / (0.30 * sr));
      this.busSmooth = 1 - Math.exp(-kChunk / (0.015 * sr));
      this.artSmooth = 1 - Math.exp(-kChunk / (0.004 * sr));
      this.ctlSmooth = 1 - Math.exp(-kChunk / (0.005 * sr));
      this.lfoSmooth = 1 - Math.exp(-kChunk / (0.002 * sr));
      this.chorus.prepare(sr);
      this.reverb.prepare(sr);
      this.lfoG = [new Lfo(), new Lfo()];
      this.slotAmt.fill(0);
      this.fxInit = true;
      this.rvDecaySet = -1; this.rvTypeSet = -1; this.eqKey = -1e300; this.widthCur = 1;
      this.drvWasOn = this.eqWasOn = this.chWasOn = this.rvWasOn = false;
      this.samplesIntoBlock = 0;
      this.now = 0;
      this.lastAllUp = -1e9;
      this.latchedArt = artNormal;
      this.dcR = 1 - kTwoPi * 8.0 / sr;
      this.rng.seed(0x2545F491);
      this.firstParams = true;
      this.busKey = -1;
      this.resetControllers();
      this.panic();
    }

    setParams(p) {
      this.params = p;
      if (this.firstParams) {
        this.masterGain = p.master;
        this.ampCur = p.ampGain;
        this.bodyMixCur = p.bodyMix;
        this.muteMixCur = p.muteType === muteOpen ? 0 : p.muteMix;
        this.plungerCur = this.plungerTarget();
        this.firstParams = false;
      }
    }

    setTransport(bpmIn, ppqIn, playingIn) {
      this.bpm = bpmIn > 0 ? bpmIn : 120;
      this.ppqAtBlock = ppqIn;
      this.playing = playingIn;
      this.samplesIntoBlock = 0;
    }

    // ---------------------------------------------------------------- notes ---
    noteOn(note, vel, ch = 0) {
      if (vel <= 0) { this.noteOff(note, ch); return; }
      ch &= 15;
      const P = this.params, voices = this.voices;
      if (this.isKeyswitch(note)) {
        const k = note - P.artKsBase;
        if (k < kNumArticulations) this.latchedArt = k;
        return;
      }
      const heldBefore = this.keyCount;
      this.pushKey(note);
      const velN = vel / 127;
      if (heldBefore === 0)
        for (let i = 0; i < kNumLfo; ++i)
          if (!P.lfo[i].perVoice && P.lfo[i].retrig) this.lfoG[i].retrigger(0, this.rng);

      this.pendingAtk = artNormal;
      if (this.latchedArt === artRip || this.latchedArt === artScoop) this.pendingAtk = this.latchedArt;
      else if (P.artAuto && heldBefore === 0 && this.now - this.lastAllUp >= P.autoGap) {
        if (vel <= P.autoLo) this.pendingAtk = artScoop;
        else if (vel >= P.autoHi) this.pendingAtk = artRip;
      }

      if (P.voiceMode === modePoly) {
        let g = -1;
        for (const v of voices)
          if (v.on && !v.killing && v.note === note && (!P.mpeOn || v.channel === ch)) { g = v.group; break; }
        if (g >= 0) {
          for (const v of voices)
            if (v.on && v.group === g) this.startVoice(v, note, velN, ch, g, v.player, note);
        } else {
          const limit = Math.min(Math.min(Math.max(P.polyphony, 1), kMaxPoly), Math.trunc(kPoolSize / this.playersPerNote()));
          for (let groups = this.countGroups(); groups >= limit; --groups)
            if (!this.stealOldestGroup()) break;
          const glide = P.glideOn && this.lastNote >= 0 && (!P.glideLegatoOnly || heldBefore > 0);
          this.newGroup(note, velN, ch, glide ? this.lastPitch : note);
        }
      } else {
        const lead = this.monoLead();
        const releasing = lead !== null && !lead.held && !lead.sustained;
        const overlapping = heldBefore > 0 && lead !== null && !releasing;
        const glide = P.glideOn && this.lastNote >= 0 && (overlapping || !P.glideLegatoOnly);
        if (P.voiceMode === modeLegato && overlapping) {
          for (const v of voices) {
            if (!(v.on && v.group === this.monoGroup)) continue;
            this.setVoicePitch(v, note, glide);
            v.note = note;
            v.held = true;
            v.sustained = false;
            this.legatoChange(v, glide);
          }
        } else if (lead !== null) {
          const from = glide ? lead.pitch : note;
          for (const v of voices)
            if (v.on && v.group === this.monoGroup) this.startVoice(v, note, velN, ch, this.monoGroup, v.player, from);
        } else {
          this.monoGroup = this.newGroup(note, velN, ch, glide ? this.lastPitch : note);
        }
      }
      this.lastNote = note;
      this.lastPitch = note;
    }

    noteOff(note, ch = 0) {
      ch &= 15;
      const P = this.params, voices = this.voices;
      if (this.isKeyswitch(note)) return;
      this.removeKey(note);
      if (this.keyCount === 0) this.lastAllUp = this.now;
      const mono = P.voiceMode !== modePoly;
      for (const v of voices) {
        if (!v.on || v.killing || !v.held || v.note !== note) continue;
        if (mono && v.group === this.monoGroup) continue;
        if (P.mpeOn && v.channel !== ch) continue;
        if (this.sustainPedal) { v.held = false; v.sustained = true; }
        else this.releaseVoice(v);
      }
      if (!mono) return;

      const lead = this.monoLead();
      if (lead === null || !lead.held || lead.note !== note) return;
      if (this.keyCount > 0) {
        const back = this.keys[this.keyCount - 1];
        for (const v of voices) {
          if (!(v.on && v.group === this.monoGroup)) continue;
          this.setVoicePitch(v, back, P.glideOn);
          v.note = back;
          this.legatoChange(v, P.glideOn);
        }
        this.lastNote = back;
        this.lastPitch = back;
      } else {
        for (const v of voices)
          if (v.on && v.group === this.monoGroup) {
            if (this.sustainPedal) { v.held = false; v.sustained = true; }
            else this.releaseVoice(v);
          }
      }
    }

    setSustain(down) {
      this.sustainPedal = down;
      if (!down) for (const v of this.voices) if (v.on && v.sustained) this.releaseVoice(v);
    }

    setPitchBend(v, ch = 0) {
      ch &= 15;
      this.bendCh[ch] = v;
      if (!this.params.mpeOn || ch === 0) this.bendGlobal = v;
    }

    setArticulation(a) { this.latchedArt = Math.min(Math.max(a, 0), kNumArticulations - 1); }
    articulation() { return this.latchedArt; }
    setModWheel(v) { this.wheelTarget = clampd(v, 0, 1); }

    setController(cc, value, ch = 0) {
      ch &= 15;
      if (cc < 0 || cc > 127) return;
      value = clampd(value, 0, 1);
      if (this.params.mpeOn && cc === 74 && ch !== 0) {
        for (const v of this.voices) if (v.on && v.channel === ch) v.slide = value;
        return;
      }
      this.ccValue[cc] = value;
      if (cc === 1) this.setModWheel(value);
      if (cc === this.params.breathCC) { this.breath = value; this.seenBreath = true; }
      if (cc === 11) { this.expression = value; this.seenExpr = true; }
      if (cc === 64) this.setSustain(value >= 0.5);
      if (cc === 121) this.resetControllers();
    }

    forgetBreath() { this.breath = 0; this.seenBreath = false; }

    setChannelPressure(value, ch = 0) {
      value = clampd(value, 0, 1);
      if (this.params.mpeOn) {
        for (const v of this.voices)
          if (v.on && v.channel === (ch & 15)) { v.pressure = value; v.pressureSeen = true; }
        return;
      }
      this.aftertouch = value;
      this.seenAT = true;
    }

    setPolyPressure(note, value) {
      value = clampd(value, 0, 1);
      for (const v of this.voices)
        if (v.on && v.note === note) { v.pressure = value; v.pressureSeen = true; }
    }

    allNotesOff() {
      this.sustainPedal = false;
      this.keyCount = 0;
      for (const v of this.voices) if (v.on) this.releaseVoice(v);
    }

    panic() {
      this.sustainPedal = false;
      this.keyCount = 0;
      for (const v of this.voices) this.hardStop(v);
      for (let b = 0; b < kBodyBands; ++b) { this.bodyL[b].reset(); this.bodyR[b].reset(); this.muteL[b].reset(); this.muteR[b].reset(); }
      this.plL.reset(); this.plR.reset();
      this.chorus.reset(); this.reverb.reset();
      for (let b = 0; b < 3; ++b) { this.eqL[b].reset(); this.eqR[b].reset(); }
      this.fxOsL.reset(); this.fxOsR.reset();
    }

    // ---------------------------------------------------------------- render ---
    // Writes numSamples into L (and R if non-null; mono sums), starting at offset.
    render(L, R, offset, numSamples) {
      const mixL = this.mixL, mixR = this.mixR, voices = this.voices;
      let pos = 0;
      while (pos < numSamples) {
        const n = Math.min(kChunk, numSamples - pos);
        this.now += n / this.sr;
        ++this.chunkStamp;
        mixL.fill(0, 0, n);
        mixR.fill(0, 0, n);
        this.updateControl(n);

        let count = 0;
        for (let i = 0; i < kPoolSize; ++i) {
          const v = voices[i];
          if (v.on) {
            this.renderVoice(v, n);
            if (v.player === 0 && !v.killing) ++count;
          }
        }
        this.active = count;

        this.processBus(n);
        this.processFx(n);

        const target = this.params.master;
        let g = this.masterGain, peak = this.peak;
        const o = offset + pos, ms = this.masterSmooth;
        for (let i = 0; i < n; ++i) {
          g += (target - g) * ms;
          if (R) {
            const yl = Math.fround(safetyClip(mixL[i] * g)), yr = Math.fround(safetyClip(mixR[i] * g));
            L[o + i] = yl; R[o + i] = yr;
            peak = Math.max(peak, Math.max(Math.abs(yl), Math.abs(yr)));
          } else {
            const y = Math.fround(safetyClip(0.5 * (mixL[i] + mixR[i]) * g));
            L[o + i] = y;
            peak = Math.max(peak, Math.abs(y));
          }
        }
        this.masterGain = g; this.peak = peak;
        pos += n;
        this.samplesIntoBlock += n;
      }
    }

    activeVoices() { return this.active; }
    breathValue() { return this.seenBreath ? this.breath : -1; }
    expressionValue() { return this.seenExpr ? this.expression : -1; }
    aftertouchValue() { return this.seenAT ? this.aftertouch : -1; }
    takePeak() { const p = this.peak; this.peak = 0; return p; }

    // ------------------------------------------------------------ internals ---
    resetControllers() {
      this.breath = 0; this.expression = 1; this.aftertouch = 0;
      this.seenBreath = this.seenExpr = this.seenAT = false;
      this.bendGlobal = 0;
      this.bendCh.fill(0);
      this.ccValue.fill(0);
      this.wheelTarget = 0;
    }

    pushKey(note) {
      this.removeKey(note);
      if (this.keyCount < 128) this.keys[this.keyCount++] = note;
    }

    removeKey(note) {
      let w = 0;
      for (let r = 0; r < this.keyCount; ++r)
        if (this.keys[r] !== note) this.keys[w++] = this.keys[r];
      this.keyCount = w;
    }

    countGroups() {
      let c = 0;
      for (const v of this.voices) if (v.on && !v.killing && v.player === 0) ++c;
      return c;
    }

    stealOldestGroup() {
      let best = null;
      for (const v of this.voices) {
        if (!v.on || v.killing || v.player !== 0) continue;
        if (best === null) { best = v; continue; }
        const vr = !v.held && !v.sustained, br = !best.held && !best.sustained;
        if ((vr && !br) || (vr === br && v.age < best.age)) best = v;
      }
      if (best === null) return false;
      const g = best.group;
      for (const v of this.voices) if (v.on && v.group === g) v.killing = true;
      return true;
    }

    monoLead() {
      if (this.monoGroup <= 0) return null;
      for (const v of this.voices) if (v.on && !v.killing && v.group === this.monoGroup && v.player === 0) return v;
      return null;
    }

    allocVoice() {
      for (const v of this.voices) if (!v.on) return v;
      let best = null;
      for (const v of this.voices) {
        if (best === null) { best = v; continue; }
        if ((v.killing && !best.killing) || (v.killing === best.killing && v.age < best.age)) best = v;
      }
      return best;
    }

    isKeyswitch(note) {
      const P = this.params;
      return P.artKs && note >= P.artKsBase && note < P.artKsBase + 12;
    }

    playersPerNote() {
      const P = this.params;
      const sec = Math.min(Math.max(P.secPlayers, 1), kMaxPlayers);
      if (P.engineMode === engAnalog) return Math.min(Math.max(sec * Math.max(P.anaUnison, 1), 1), kMaxPlayers);
      return sec;
    }

    newGroup(note, vel, ch, fromPitch) {
      const P = this.params, rng = this.rng;
      const g = ++this.groupCounter;
      const N = this.playersPerNote();
      const analog = P.engineMode === engAnalog;
      const section = P.secPlayers > 1;
      for (let p = 0; p < N; ++p) {
        const v = this.allocVoice();
        const pos = N === 1 ? 0 : -1 + 2 * p / (N - 1);
        const r1 = rng.bi(), r2 = rng.uni(), r3 = rng.bi(), r4 = rng.bi(), r5 = rng.bi(), r6 = rng.uni();
        let cents = 0;
        if (analog) cents += P.anaDetune * pos;
        if (section) cents += P.secDetune * (0.6 * pos + 0.4 * r1);
        v.plDetune = cents / 100;
        v.plDelay = section && p > 0 ? P.secTiming / 1000 * r2 : 0;
        v.plRelDelay = section && p > 0 ? P.secTiming / 1000 * 0.6 * r6 : 0;
        v.plBright = section ? P.secTimbre * 0.6 * r3 : 0;
        v.plGain = (section ? dbToGain(P.secTimbre * 1.5 * r4) : 1) / Math.sqrt(N);
        v.plVibRate = 1 + (section ? P.secVibDesync * 0.1 * r5 : 0);
        v.plVibDelay = 1 + (section ? P.secVibDesync * 0.5 * r2 : 0);
        const width = section ? Math.max(P.secWidth, analog ? P.anaSpread : 0) : (analog ? P.anaSpread : 0);
        v.plPan = clampd(pos * width, -1, 1);
        v.plCount = N;
        v.panMod = false;
        const ang = (v.plPan + 1) * kPi / 4;
        v.panL = Math.min(1, kSqrt2 * Math.cos(ang));
        v.panR = Math.min(1, kSqrt2 * Math.sin(ang));
        this.startVoice(v, note, vel, ch, g, p, fromPitch);
      }
      return g;
    }

    setVoicePitch(v, note, glide) {
      v.glideFrom = v.pitch;
      v.glideTo = note;
      v.glidePos = glide ? 0 : 1;
      if (!glide) v.pitch = note;
    }

    legatoChange(v, glide) {
      const P = this.params;
      v.slurT = 0;
      v.slurDur = Math.max(glide ? P.glideTime : 0, 0.05);
      v.tVib = 0;
      v.tTrans = 0;
      v.tClick = 0;
      v.lipPeak = P.lipAmt * v.tgLip * 0.3 * (0.35 + 0.65 * v.dyn);
      v.chiffPeak = P.chiff * v.tgLip * 0.3 * (0.3 + 0.7 * v.dyn);
      v.clickPeak = P.keyClick * 0.7;
    }

    releaseVoice(v) {
      if (v.relPending) return;
      const P = this.params;
      v.held = v.sustained = false;
      let kind = artNormal;
      if (this.latchedArt === artFall || this.latchedArt === artDoit) kind = this.latchedArt;
      else if (P.artAuto && this.keyCount === 0 && v.vel * 127 >= P.autoHi && this.now - v.noteStart >= 0.25) kind = artFall;
      const dur = (kind === artFall ? P.fallTime : (kind === artDoit ? P.doitTime : 0)) + v.plRelDelay;
      if (dur > 0) {
        v.relPending = true;
        v.relKind = kind;
        v.relT = 0;
        v.relDur = dur;
        return;
      }
      gateOffAll(v);
    }

    hardStop(v) {
      v.on = false;
      v.held = v.sustained = v.killing = false;
      for (const o of v.o) { o.amp.kill(); o.tmb.kill(); o.filt.reset(); o.lvl = 0; }
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

    dynamicsTarget(v) {
      const P = this.params;
      let num = 0, den = 0;
      if (this.seenBreath && P.wBreath > 0) { num += P.wBreath * this.breath; den += P.wBreath; }
      if (this.seenExpr && P.wExpr > 0) { num += P.wExpr * this.expression; den += P.wExpr; }
      if (this.seenAT && P.wAT > 0) { num += P.wAT * this.aftertouch; den += P.wAT; }
      if (v.pressureSeen && P.wMPE > 0) { num += P.wMPE * v.pressure; den += P.wMPE; }
      const x = den > 0 ? (num / den) * (1 - P.dynVel + P.dynVel * v.vel) : v.vel;
      return responseCurve(x, P.dynCurve);
    }

    startVoice(v, note, vel, ch, group, player, fromPitch) {
      const P = this.params, rng = this.rng;
      const fresh = !v.on;
      if (fresh) {
        for (let k = 0; k < kNumOsc; ++k) {
          const o = v.o[k];
          o.osc.reset(P.osc[k].shape >= 1.5 ? rng.uni() : 0);
          o.filt.reset();
          o.amp.kill();
          o.tmb.kill();
          o.lvl = 0;
        }
        v.add.kill();
        v.add.c = 1; v.add.s = 0;
        v.fenv.kill();
        v.f1.reset(); v.f2.reset();
        v.os.reset();
        v.lipF.reset(); v.noiseF.reset();
        v.nrng.seed(rng.next());
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
        for (let i = 0; i < kNumLfo; ++i)
          v.lfo[i].start(P.lfo[i].retrig ? 0 : rng.uni(), rng);
        v.snap = true;
      } else {
        for (let i = 0; i < kNumLfo; ++i)
          if (P.lfo[i].retrig) v.lfo[i].retrigger(0, rng);
      }
      v.rnd = rng.bi();
      v.artAtk = this.pendingAtk;
      v.artShake = this.latchedArt === artShake;
      v.relPending = false;
      v.relKind = artNormal;
      v.relT = 0;
      v.noteStart = this.now;
      v.delayLeft = v.plDelay;
      v.on = true;
      v.killing = false;
      v.kill = 1;
      v.note = note;
      v.channel = ch;
      v.group = group;
      v.player = player;
      v.vel = vel;
      v.age = ++this.ageCounter;
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
      v.glidePos = fromPitch === note ? 1 : 0;
      v.pitch = fromPitch;

      v.dyn = this.dynamicsTarget(v);
      v.dynOnset = v.dyn;

      const tg = Math.min(Math.max(P.tongue, 0), 2);
      v.tgLip = TG_LIP[tg]; v.tgDecay = TG_DECAY[tg]; v.tgScoop = TG_SCOOP[tg]; v.tgBlip = TG_BLIP[tg];

      const atkSpeed = Math.pow(2, P.dynAttack * (2 - 4 * v.dynOnset)) * TG_ATK[tg];
      const key = (note - 60) / 24;
      const velB = 2 * vel - 1;

      for (let k = 0; k < kNumOsc; ++k) {
        const O = P.osc[k], o = v.o[k];
        o.ampScale = (1 - O.ampVel * (1 - vel)) * Math.pow(2, O.ampKeyAmt * key);
        o.ampTime = Math.pow(2, -O.ampKeyTime * key);
        o.ampAtkTime = o.ampTime * Math.pow(2, -O.ampVelTime * velB) * atkSpeed;
        o.pScale = Math.max(0, 1 + O.pVel * velB);
        o.pTime = Math.pow(2, -O.pKeyTime * key);
        o.tScale = Math.max(0, 1 + O.tVel * velB);
        o.tTime = Math.pow(2, -O.tKeyTime * key);
        o.tAtkTime = o.tTime * atkSpeed;
        o.amp.gateOn();
        o.tmb.gateOn();
      }
      v.addAtkScale = atkSpeed;
      for (const e of v.add.node) e.gateOn();
      v.fltVelScale = 1 - P.fltVel + P.fltVel * vel;
      v.fenv.gateOn();

      v.lipPeak = P.lipAmt * v.tgLip * (0.35 + 0.65 * v.dynOnset);
      v.chiffPeak = P.chiff * v.tgLip * (0.3 + 0.7 * v.dynOnset);
      v.clickPeak = P.keyClick * (0.5 + 0.5 * vel);
    }

    // ----------------------------------------------------------- control ---
    plungerTarget() {
      const P = this.params;
      switch (P.muteSrc) {
        case plWheel: return this.wheel;
        case plBreath: return this.seenBreath ? this.breath : P.mutePlunger;
        case plExpression: return this.seenExpr ? this.expression : P.mutePlunger;
        case plAftertouch: return this.seenAT ? this.aftertouch : P.mutePlunger;
        case plCC: return this.ccValue[Math.min(Math.max(P.muteCC, 0), 127)];
        default: return P.mutePlunger;
      }
    }

    updateControl(n) {
      const P = this.params, sr = this.sr;
      this.wheel += (this.wheelTarget - this.wheel) * this.wheelSmooth;
      this.dynCoef = 1 - Math.exp(-n / (Math.max(P.dynSmooth, 0.001) * sr));

      for (let i = 0; i < kNumLfo; ++i) {
        const lp = P.lfo[i];
        this.lfoInc[i] = this.lfoRateHz(lp) * n / sr;
        if (lp.perVoice) { this.lfoGlobal[i] = 0; continue; }
        const g = this.lfoG[i];
        const locked = lp.sync && this.playing && !lp.retrig;
        if (locked) {
          const beats = kLfoDivBeats[Math.min(Math.max(lp.div, 0), kNumLfoDivs - 1)];
          let ph = (this.ppqAtBlock + this.samplesIntoBlock / sr * this.bpm / 60) / beats;
          ph -= Math.floor(ph);
          if (ph < g.phase - 0.5) g.held = this.rng.bi();
          g.phase = ph;
        }
        const raw = lp.on ? g.shape(lp.wave, lp.phase) * lp.amount : 0;
        this.lfoGlobal[i] = g.smooth(raw, this.lfoSmooth);
        if (!locked) g.advance(this.lfoInc[i], this.rng);
      }

      let na = 0;
      for (let s = 0; s < kNumModSlots; ++s) {
        const m = P.mod[s];
        const routed = m.src !== srcNone && m.dst !== dstNone;
        const target = (m.on && routed) ? m.amount : 0;
        this.slotAmt[s] += (target - this.slotAmt[s]) * this.ctlSmooth;
        if (routed && Math.abs(this.slotAmt[s]) > 1e-6) {
          const a = this.activeSlots[na++];
          a.src = m.src; a.via = m.via; a.dst = m.dst; a.amt = this.slotAmt[s];
        }
      }
      this.numActive = na;
      const gDst = this.gDst;
      gDst.fill(0);
      for (let a = 0; a < na; ++a) {
        const s = this.activeSlots[a];
        if (s.dst < kFirstGlobalDest) continue;
        let m = this.globalSource(s.src) * s.amt;
        if (s.via !== srcNone) {
          let w = this.globalSource(s.via);
          if (isBipolarSource(s.via)) w = 0.5 * (w + 1);
          m *= w;
        }
        gDst[s.dst] += m;
      }

      this.brsToneCoef = P.brsTone >= 0.999 ? 1 : 1 - Math.exp(-kTwoPi * 1500 * Math.pow(2, P.brsTone * 3.75) / sr);

      const bodyShift = clampd(P.bodyShift + 12 * gDst[dstBodyShift], -24, 24);
      const key = P.instrument * 1e6 + bodyShift * 1e3 + P.bodyReso * 10 + P.muteType + 0.5;
      if (key !== this.busKey) {
        const M = instrumentModel(P.instrument);
        const shift = Math.pow(2, bodyShift / 12);
        for (let b = 0; b < kBodyBands; ++b) {
          const B = M.body[b];
          const db = B.db * (0.5 + P.bodyReso);
          designBiquad(this.bodyC[b], B.type, B.hz * shift, B.q, db, sr);
        }
        const MM = muteModel(P.muteType);
        if (P.muteType !== this.muteKind) {
          for (let b = 0; b < kBodyBands; ++b) { this.muteL[b].reset(); this.muteR[b].reset(); }
          this.plL.reset(); this.plR.reset();
          this.muteKind = P.muteType;
        }
        for (let b = 0; b < 4; ++b)
          designBiquad(this.muteC[b], MM.bands[b].type, MM.bands[b].hz, MM.bands[b].q, MM.bands[b].db, sr);
        this.muteGain = dbToGain(MM.gainDb);
        this.busKey = key;
      }
      this.plungerFrom = this.plungerCur;
      this.plungerCur += (clampd(this.plungerTarget() + gDst[dstPlunger], 0, 1) - this.plungerCur) * this.busSmooth;
      this.bodyMixFrom = this.bodyMixCur;
      this.bodyMixCur += (P.bodyMix - this.bodyMixCur) * this.busSmooth;
      this.muteMixFrom = this.muteMixCur;
      this.muteMixCur += ((P.muteType === muteOpen ? 0 : P.muteMix) - this.muteMixCur) * this.busSmooth;
    }

    processBus(n) {
      const P = this.params, sr = this.sr, mixL = this.mixL, mixR = this.mixR;
      const inv = 1 / n;
      if (this.bodyMixCur > 1e-4 || this.bodyMixFrom > 1e-4) {
        let m = this.bodyMixFrom;
        const dm = (this.bodyMixCur - this.bodyMixFrom) * inv;
        const bL = this.bodyL, bR = this.bodyR, C = this.bodyC;
        for (let i = 0; i < n; ++i) {
          m += dm;
          const l = mixL[i], r = mixR[i];
          let bl = l, br = r;
          for (let b = 0; b < kBodyBands; ++b) {
            bl = bL[b].process(bl, C[b]);
            br = bR[b].process(br, C[b]);
          }
          mixL[i] = l + m * (bl - l);
          mixR[i] = r + m * (br - r);
        }
      }
      if (this.muteMixCur > 1e-4 || this.muteMixFrom > 1e-4) {
        const plunger = P.muteType === mutePlungerType;
        let pg = SVF.gFor(plungerCut(this.plungerFrom), sr);
        const dpg = (SVF.gFor(plungerCut(this.plungerCur), sr) - pg) * inv;
        const pk = SVF.kFor(0.35);
        let pgain = dbToGain(-6 * (1 - clampd(this.plungerFrom, 0, 1)));
        const dpgain = (dbToGain(-6 * (1 - clampd(this.plungerCur, 0, 1))) - pgain) * inv;
        let m = this.muteMixFrom;
        const dm = (this.muteMixCur - this.muteMixFrom) * inv;
        const mL = this.muteL, mR = this.muteR, C = this.muteC, mg = this.muteGain;
        for (let i = 0; i < n; ++i) {
          m += dm; pg += dpg; pgain += dpgain;
          const l = mixL[i], r = mixR[i];
          let ml = l, mr = r;
          for (let b = 0; b < 4; ++b) {
            ml = mL[b].process(ml, C[b]);
            mr = mR[b].process(mr, C[b]);
          }
          if (plunger) {
            ml = this.plL.process(ml, pg, pk) * pgain;
            mr = this.plR.process(mr, pg, pk) * pgain;
          }
          ml *= mg; mr *= mg;
          mixL[i] = l + m * (ml - l);
          mixR[i] = r + m * (mr - r);
        }
      }
    }

    lfoRateHz(l) {
      if (!l.sync) return l.rate;
      return (this.bpm / 60) / kLfoDivBeats[Math.min(Math.max(l.div, 0), kNumLfoDivs - 1)];
    }

    globalSource(s) {
      switch (s) {
        case srcLfo1: case srcLfo2: {
          const i = s - srcLfo1;
          return this.params.lfo[i].perVoice ? 0 : this.lfoGlobal[i];
        }
        case srcBreath: return this.seenBreath ? this.breath : 0;
        case srcExpression: return this.seenExpr ? this.expression : 0;
        case srcAftertouch: return this.seenAT ? this.aftertouch : 0;
        case srcModWheel: return this.wheel;
        case srcPitchBend: return this.bendGlobal;
        default: return 0;
      }
    }

    processFx(n) {
      const P = this.params, sr = this.sr, mixL = this.mixL, mixR = this.mixR, gDst = this.gDst;
      const inv = 1 / n;
      const drvAmt = clampd(P.fxDrvAmt + gDst[dstFxDrive], 0, 1);
      const chMix = clampd(P.chMix + gDst[dstChorusMix], 0, 1);
      const rvMix = clampd(P.rvMix + gDst[dstReverbMix], 0, 1);

      {
        let a = this.ampCur;
        const t = P.ampGain, ms = this.masterSmooth;
        for (let i = 0; i < n; ++i) {
          a += (t - a) * ms;
          mixL[i] *= a;
          mixR[i] *= a;
        }
        this.ampCur = a;
      }

      if (P.fxDrvOn) {
        const type = P.fxDrvType === 1 ? shapeTape : shapeTube;
        const tg = dbToGain(drvAmt * 36);
        const tmix = P.fxDrvMix;
        if (!this.drvWasOn || this.fxInit) {
          this.fxOsL.reset(); this.fxOsR.reset();
          this.drvLpL = this.drvLpR = this.drvDcXL = this.drvDcYL = this.drvDcXR = this.drvDcYR = 0;
          this.drvG = tg; this.drvMixCur = tmix;
          this.drvComp = 1 / tg;
          this.drvPin = this.drvPout = 0;
        }
        const toneC = P.fxDrvTone >= 0.999 ? 1 : 1 - Math.exp(-kTwoPi * 2000 * Math.pow(2, P.fxDrvTone * 3.3) / sr);
        const factor = P.oversample >= 2 ? 4 : (P.oversample === 1 ? 2 : 1);
        let g = this.drvG, m = this.drvMixCur, c = this.drvComp;
        const dg = (tg - g) * inv, dm = (tmix - m) * inv, dc = (this.drvCompTarget - c) * inv;
        let pin = 0, pout = 0;
        let lpL = this.drvLpL, lpR = this.drvLpR, dxL = this.drvDcXL, dyL = this.drvDcYL, dxR = this.drvDcXR, dyR = this.drvDcYR;
        const dcR = this.dcR;
        for (let i = 0; i < n; ++i) {
          g += dg; m += dm; c += dc;
          const xl = mixL[i], xr = mixR[i];
          const yl = this.fxOsL.process(xl * g, factor, type);
          const yr = this.fxOsR.process(xr * g, factor, type);
          const hl = yl - dxL + dcR * dyL; dxL = yl; dyL = hl;
          const hr = yr - dxR + dcR * dyR; dxR = yr; dyR = hr;
          lpL += (hl - lpL) * toneC;
          lpR += (hr - lpR) * toneC;
          pin += xl * xl + xr * xr;
          pout += lpL * lpL + lpR * lpR;
          mixL[i] = xl + m * (lpL * c - xl);
          mixR[i] = xr + m * (lpR * c - xr);
        }
        this.drvLpL = lpL; this.drvLpR = lpR; this.drvDcXL = dxL; this.drvDcYL = dyL; this.drvDcXR = dxR; this.drvDcYR = dyR;
        const a = 1 - Math.exp(-n / (0.3 * sr));
        this.drvPin += (pin * inv - this.drvPin) * a;
        this.drvPout += (pout * inv - this.drvPout) * a;
        if (this.drvPout > 1e-12 && this.drvPin > 1e-12)
          this.drvCompTarget = clampd(Math.sqrt(this.drvPin / this.drvPout), 0.01, 4);
        else
          this.drvCompTarget = this.drvComp;
        this.drvG = tg; this.drvMixCur = tmix; this.drvComp = c;
      }
      this.drvWasOn = P.fxDrvOn;

      if (P.eqOn) {
        const key = P.eqLoGain * 1e9 + P.eqLoFreq * 1e5 + P.eqMidGain * 1e3 + P.eqMidFreq + P.eqMidQ * 0.1
                    + P.eqHiGain * 1e-3 + P.eqHiFreq * 1e-7;
        if (key !== this.eqKey) {
          designBiquad(this.eqC[0], bqLowShelf, P.eqLoFreq, 0.7071, P.eqLoGain, sr);
          designBiquad(this.eqC[1], bqPeak, P.eqMidFreq, P.eqMidQ, P.eqMidGain, sr);
          designBiquad(this.eqC[2], bqHighShelf, P.eqHiFreq, 0.7071, P.eqHiGain, sr);
          this.eqKey = key;
        }
        if (!this.eqWasOn) for (let b = 0; b < 3; ++b) { this.eqL[b].reset(); this.eqR[b].reset(); }
        const eL = this.eqL, eR = this.eqR, C = this.eqC;
        for (let i = 0; i < n; ++i) {
          let l = mixL[i], r = mixR[i];
          for (let b = 0; b < 3; ++b) { l = eL[b].process(l, C[b]); r = eR[b].process(r, C[b]); }
          mixL[i] = l; mixR[i] = r;
        }
      }
      this.eqWasOn = P.eqOn;

      if (P.chOn) {
        const ch = this.chorus;
        if (!this.chWasOn || this.fxInit) { ch.reset(); this.chMixCur = chMix; }
        ch.phase += P.chRate * n / sr;
        ch.phase -= Math.floor(ch.phase);
        const tl = (0.012 + 0.006 * P.chDepth * Math.sin(kTwoPi * ch.phase)) * sr;
        const tr = (0.012 + 0.006 * P.chDepth * Math.sin(kTwoPi * (ch.phase + 0.25))) * sr;
        let dl = ch.dL, dr = ch.dR, m = this.chMixCur;
        const ddl = (tl - dl) * inv, ddr = (tr - dr) * inv, dm = (chMix - m) * inv;
        for (let i = 0; i < n; ++i) {
          dl += ddl; dr += ddr; m += dm;
          const l = mixL[i], r = mixR[i];
          ch.bufL[ch.w] = l;
          ch.bufR[ch.w] = r;
          const wl = ch.read(ch.bufL, dl), wr = ch.read(ch.bufR, dr);
          ch.w = (ch.w + 1) & ch.mask;
          const norm = 1 / (1 + 0.5 * m);
          mixL[i] = (l + m * wl) * norm;
          mixR[i] = (r + m * wr) * norm;
        }
        ch.dL = tl; ch.dR = tr; this.chMixCur = chMix;
      }
      this.chWasOn = P.chOn;

      if (P.rvOn) {
        const rv = this.reverb;
        if (!this.rvWasOn || this.fxInit) { rv.reset(); this.rvMixCur = rvMix; }
        rv.setType(P.rvType === 0 ? 0 : 1);
        if (P.rvDecay !== this.rvDecaySet || rv.type !== this.rvTypeSet) {
          rv.setDecay(P.rvDecay);
          let gm = 0;
          for (let i = 0; i < kRvLines; ++i) gm += rv.g[i];
          gm /= kRvLines;
          this.rvNorm = Math.sqrt(Math.max(1 - gm * gm, 1e-4)) * 2.2;
          this.rvDecaySet = P.rvDecay;
          this.rvTypeSet = rv.type;
        }
        const pre = lround(P.rvPre * 0.001 * sr);
        const damp = 1 - Math.exp(-kTwoPi * 18000 * Math.pow(2, -P.rvDamp * 5) / sr);
        let dry0 = Math.cos(this.rvMixCur * kPi / 2), wet0 = Math.sin(this.rvMixCur * kPi / 2) * this.rvNorm;
        const dry1 = Math.cos(rvMix * kPi / 2), wet1 = Math.sin(rvMix * kPi / 2) * this.rvNorm;
        const ddry = (dry1 - dry0) * inv, dwet = (wet1 - wet0) * inv;
        for (let i = 0; i < n; ++i) {
          dry0 += ddry; wet0 += dwet;
          const l = mixL[i], r = mixR[i];
          rv.process(0.5 * (l + r), pre, damp);
          mixL[i] = l * dry0 + rv.outL * wet0;
          mixR[i] = r * dry0 + rv.outR * wet0;
        }
        this.rvMixCur = rvMix;
      }
      this.rvWasOn = P.rvOn;

      if (Math.abs(P.stWidth - 1) > 1e-4 || Math.abs(this.widthCur - 1) > 1e-4) {
        let w = this.widthCur;
        const dw = (P.stWidth - w) * inv;
        for (let i = 0; i < n; ++i) {
          w += dw;
          const mid = 0.5 * (mixL[i] + mixR[i]), side = 0.5 * (mixL[i] - mixR[i]) * w;
          mixL[i] = mid + side;
          mixR[i] = mid - side;
        }
        this.widthCur = P.stWidth;
      }
      this.fxInit = false;
    }

    // ------------------------------------------------------------- voice ---
    renderVoice(v, n) {
      const P = this.params, sr = this.sr, rng = this.rng;
      const M = instrumentModel(P.instrument);
      const dtSec = n / sr;
      if (v.delayLeft > 0) { v.delayLeft -= dtSec; return; }
      const analog = P.engineMode === engAnalog;
      v.tNote += dtSec;
      v.tVib += dtSec;
      v.slurT += dtSec;
      v.tTrans += dtSec;
      v.tClick += dtSec;

      v.dyn += (this.dynamicsTarget(v) - v.dyn) * this.dynCoef;

      // LFOs
      let lfo0, lfo1;
      {
        const lp0 = P.lfo[0], lp1 = P.lfo[1];
        lfo0 = lp0.perVoice ? v.lfo[0].smooth(lp0.on ? v.lfo[0].shape(lp0.wave, lp0.phase) * lp0.amount : 0, this.lfoSmooth) : this.lfoGlobal[0];
        lfo1 = lp1.perVoice ? v.lfo[1].smooth(lp1.on ? v.lfo[1].shape(lp1.wave, lp1.phase) * lp1.amount : 0, this.lfoSmooth) : this.lfoGlobal[1];
      }

      // vibrato signal
      if (--v.driftLeft <= 0) {
        v.driftRT = rng.bi();
        v.driftDT = rng.bi();
        v.driftLeft = Math.trunc(0.25 * sr / kChunk);
      }
      v.driftR += (v.driftRT - v.driftR) * this.driftSmooth;
      v.driftD += (v.driftDT - v.driftD) * this.driftSmooth;
      const vibDelay = (P.vibDelay > kEnvMinStage ? P.vibDelay : 0) * v.plVibDelay;
      const vibEnv = v.tVib < vibDelay ? 0 : Math.min(1, (v.tVib - vibDelay) / Math.max(P.vibFade, 0.001));
      const wheelScale = 1 - P.vibWheel + P.vibWheel * this.wheel;
      const vs = Math.sin(kTwoPi * v.vibPhase) * vibEnv * wheelScale;

      // mod matrix
      const dst = this.sDst;
      dst.fill(0);
      const numActive = this.numActive;
      if (numActive > 0) {
        const src = this.sSrc;
        src[srcNone] = 0;
        src[srcLfo1] = lfo0;
        src[srcLfo2] = lfo1;
        for (let k = 0; k < kNumOsc; ++k) {
          const O = P.osc[k], o = v.o[k];
          src[srcAmpEnv1 + k] = o.amp.level;
          src[srcPitchEnv1 + k] = 1 - curveShape(Math.min(v.tNote / (O.pTime * o.pTime), 1), O.pCrv);
          src[srcTmbEnv1 + k] = o.tmb.level;
        }
        src[srcHarmLo] = v.add.node[0].level;
        src[srcHarmMid] = v.add.node[4].level;
        src[srcHarmHi] = v.add.node[7].level;
        src[srcFltEnv] = v.fenv.level;
        src[srcVelocity] = v.vel;
        src[srcDynamics] = v.dyn;
        src[srcBreath] = this.seenBreath ? this.breath : 0;
        src[srcExpression] = this.seenExpr ? this.expression : 0;
        src[srcAftertouch] = this.seenAT ? this.aftertouch : 0;
        src[srcMpePressure] = v.pressure;
        src[srcMpeSlide] = v.slide >= 0 ? v.slide : 0;
        src[srcMpeBend] = P.mpeOn && v.channel !== 0 ? this.bendCh[v.channel] : 0;
        src[srcModWheel] = this.wheel;
        src[srcPitchBend] = this.bendGlobal;
        src[srcKey] = (v.note - 60) / 60;
        src[srcRandom] = v.rnd;
        src[srcVibrato] = vs;
        for (let a = 0; a < numActive; ++a) {
          const s = this.activeSlots[a];
          if (s.dst >= kFirstGlobalDest) continue;
          let m = src[s.src] * s.amt;
          if (s.via !== srcNone) {
            let w = src[s.via];
            if (isBipolarSource(s.via)) w = 0.5 * (w + 1);
            m *= w;
          }
          dst[s.dst] += m;
        }
      }

      const d = clampd(v.dyn + dst[dstDynamics], 0, 1);
      this.lastDyn = d;
      const loud = (d < 0.05 ? d / 0.05 : 1) * dbToGain(-P.dynRange * (1 - d));
      const brightCurve = responseCurve(d, P.dynBrCurve);
      const blat = analog ? P.anaBlat * Math.exp(-v.tNote / 0.06) * (0.4 + 0.6 * v.vel) : 0;
      let bright = P.dynBright * brightCurve + v.plBright + 3 * blat + 4 * dst[dstBrightness];
      if (P.mpeOn && v.slide >= 0) bright += (v.slide - 0.5) * 3;

      // articulations
      let artTarget = 0, artGain = 1;
      if (v.artAtk === artScoop && v.tNote < P.scoopTime) {
        const x = v.tNote / P.scoopTime;
        artTarget -= P.scoopRng * (1 - x) * (1 - x);
      }
      if (v.artAtk === artRip && v.tNote < P.ripTime) {
        const x = v.tNote / P.ripTime;
        artTarget -= Math.floor(P.ripRng * (1 - x) + 0.5);
        artGain *= 0.55 + 0.45 * x;
      }
      if (v.artShake && v.tNote > 0.12) {
        const x = v.tNote - 0.12;
        artTarget += P.shakeRng * Math.min(1, x / 0.25) * (0.5 + 0.5 * Math.tanh(3 * Math.sin(kTwoPi * P.shakeRate * x)));
      }
      if (v.relPending) {
        v.relT += dtSec;
        const x = Math.min(1, v.relT / Math.max(v.relDur, 1e-3));
        if (v.relKind === artFall) { artTarget -= P.fallRng * x * x; artGain *= 1 - Math.pow(x, 1.5); }
        else if (v.relKind === artDoit) { artTarget += P.doitRng * x * x; artGain *= 1 - Math.pow(x, 1.5); }
        if (x >= 1) { v.relPending = false; gateOffAll(v); }
      } else if (v.relKind !== artNormal)
        artGain = 0;
      v.artSemis += (artTarget - v.artSemis) * this.artSmooth;

      // vibrato
      const vibDepthNow = Math.max(0, P.vibDepth + 100 * dst[dstVibDepth]);
      const vibCents = vibDepthNow * vs * (1 + 0.3 * P.vibDrift * v.driftD);
      const vibAmpMul = 1 + 0.5 * P.vibAmp * vs;
      const vibRateMod = dst[dstVibRate] !== 0 ? Math.pow(2, 2 * dst[dstVibRate]) : 1;
      v.vibPhase += P.vibRate * vibRateMod * v.plVibRate * (1 + 0.12 * P.vibDrift * v.driftR) * dtSec;
      v.vibPhase -= Math.floor(v.vibPhase);

      // growl + flutter
      let growlAM = 1, growlCents = 0;
      const growl = clampd(P.growlAmt + dst[dstGrowl], 0, 1) * (1 - P.growlDyn + P.growlDyn * d);
      if (growl > 1e-4) {
        if (--v.growlLeft <= 0) {
          v.growlNT = v.nrng.bi();
          v.growlLeft = Math.max(1, Math.trunc(sr / (P.growlRate * 2) / kChunk));
        }
        v.growlN += (v.growlNT - v.growlN) * 0.3;
        const gs = Math.sin(kTwoPi * v.growlPh);
        const gm = (1 - P.growlRough) * gs + P.growlRough * v.growlN;
        growlAM = 1 - 0.45 * growl * (1 + gm);
        growlCents = 18 * growl * gm;
        v.growlPh += P.growlRate * (1 + 0.15 * P.growlRough * v.growlN) * dtSec;
        v.growlPh -= Math.floor(v.growlPh);
      }
      let flutAM = 1, flutNoise = 0;
      const flutAmt = clampd(P.flutAmt + dst[dstFlutter], 0, 1);
      if (flutAmt > 1e-4) {
        const fs = 0.5 + 0.5 * Math.sin(kTwoPi * v.flutPh);
        const f3 = fs * fs * fs;
        flutAM = 1 - 0.85 * flutAmt * f3;
        flutNoise = flutAmt * f3;
        v.flutPh += P.flutRate * dtSec;
        v.flutPh -= Math.floor(v.flutPh);
      }

      const dip = v.slurT < v.slurDur ? P.slurDip * Math.sin(kPi * v.slurT / v.slurDur) : 0;

      // per-oscillator targets
      const bendSemis = P.mpeOn ? this.bendGlobal * P.bendRange + (v.channel !== 0 ? this.bendCh[v.channel] * P.mpeBend : 0)
                                : this.bendGlobal * P.bendRange;
      const base = v.pitch + P.transpose - 69 + P.tuneSemis + bendSemis + v.artSemis + v.plDetune
                   + (vibCents + growlCents) / 100 + 0.25 * blat + 12 * dst[dstPitchAll];
      const tdt = this.sTdt, tlvl = this.sTlvl, tshape = this.sTshape, tpw = this.sTpw, tg = this.sTg, tk = this.sTk;
      let alive = false;
      let envMain = 0, pe0 = 0, hz0 = 440;
      const et = this.sEnv;
      for (let k = 0; k < kNumOsc; ++k) {
        const O = P.osc[k], o = v.o[k];
        scaledInto(et, O.amp, o.ampTime, o.ampAtkTime);
        const aLvl = o.amp.advance(et, dtSec);
        scaledInto(et, O.tmb, o.tTime, o.tAtkTime);
        const tLvl = o.tmb.advance(et, dtSec);
        if (O.on && o.amp.active()) alive = true;
        if (O.on) envMain = Math.max(envMain, aLvl);

        let pe = O.pAmt * o.pScale * v.tgScoop * (1 - curveShape(Math.min(v.tNote / (O.pTime * o.pTime), 1), O.pCrv));
        const bx = v.tNote / (O.blipTime * o.pTime);
        if (bx < 1) pe += O.blipAmt * o.pScale * v.tgBlip * Math.sin(kPi * bx);

        const semis = base + O.semis + pe / 100 + 12 * dst[dstOsc1Pitch + k];
        const hz = 440 * Math.pow(2, semis / 12);
        if (k === 0) { pe0 = pe; hz0 = hz; }
        tdt[k] = Math.min(hz / sr, 0.45);
        const cut = hz * O.cutHarm * Math.pow(2, tLvl * O.tCut * o.tScale + bright - 2 * dip + 5 * dst[dstOsc1Bright + k]);
        tg[k] = SVF.gFor(cut, sr);
        tk[k] = SVF.kFor(O.res * 0.9);
        tshape[k] = clampd(O.shape + tLvl * O.tShape * o.tScale + 3 * dst[dstOsc1Shape + k], 0, 3);
        tpw[k] = clampd(O.pw + 0.45 * dst[dstOsc1PW + k], 0.05, 0.95);
        tlvl[k] = O.on ? O.level * aLvl * o.ampScale * clampd(1 + dst[dstOsc1Level + k], 0, 2) : 0;
      }

      // additive harmonic layer
      const tA = this.sTA;
      let addCount = 0;
      let tdtA = v.add.dt;
      const addOn = P.addOn && !analog;
      if (addOn) {
        const T = ADD;
        const nodeLvl = this.sNodeLvl;
        const B0 = P.band[0], B1 = P.band[1], B2 = P.band[2];
        for (let j = 0; j < kAddNodes; ++j) {
          const x = T.nodePos[j];
          const W = T.nodeW[j], w0 = W[0], w1 = W[1], w2 = W[2];
          et.dly = P.addSpecDly * 0.08 * T.nodeDly[j];
          et.atk = (w0 * B0.atk + w1 * B1.atk + w2 * B2.atk) * (1 + 1.5 * P.addSpecDly * x) * v.addAtkScale;
          et.hld = 0;
          et.dec = w0 * B0.dec + w1 * B1.dec + w2 * B2.dec;
          et.sus = w0 * B0.sus + w1 * B1.sus + w2 * B2.sus;
          et.rel = (w0 * B0.rel + w1 * B1.rel + w2 * B2.rel) * (1 - 0.6 * P.addSpecDly * x);
          et.atkC = et.decC = et.relC = P.addCurve;
          const lvl = v.add.node[j].advance(et, dtSec);
          nodeLvl[j] = lvl * (w0 * B0.lvl + w1 * B1.lvl + w2 * B2.lvl);
          if (v.add.node[j].active()) alive = true;
          if (j === 0) envMain = Math.max(envMain, lvl);
        }
        const hzA = 440 * Math.pow(2, (base + pe0 / 100) / 12);
        tdtA = Math.min(hzA / sr, 0.45);

        const S = this.specCache[v.group & (kSpecCache - 1)];
        if (S.group !== v.group || S.stamp !== this.chunkStamp) {
          const slope = 0.5 * (M.softSlope + M.loudSlope)
                        + (1 - 2 * brightCurve) * P.addDynSpread * 0.5 * (M.softSlope - M.loudSlope);
          const maxN = Math.min(Math.max(P.addPartials, 1), kMaxPartials);
          const logHz = Math.log(hzA);
          let power = 0;
          S.count = 0;
          const lin = S.lin;
          for (let k = 0; k < maxN; ++k) {
            const f = hzA * (k + 1);
            if (f >= 0.45 * sr) break;
            const db = spectralEnvelopeDbLog(P.instrument, logHz + T.logn[k]) + slope * k + P.addTilt * T.log2n[k];
            let a = Math.exp(db * kLn10Over20);
            const even = ((k + 1) & 1) === 0;
            if (P.addOddEven > 0 && even) a *= 1 - P.addOddEven;
            else if (P.addOddEven < 0 && !even && k > 0) a *= 1 + P.addOddEven;
            if (f > 0.4 * sr) a *= (0.45 * sr - f) / (0.05 * sr);
            lin[k] = a;
            power += a * a;
            S.count = k + 1;
          }
          const norm = power > 0 ? P.addLevel / Math.sqrt(power) : 0;
          for (let k = 0; k < S.count; ++k) lin[k] *= norm;
          S.group = v.group;
          S.stamp = this.chunkStamp;
        }
        addCount = Math.min(S.count, v.player > 0 ? 12 : kMaxPartials);
        const lvlMod = clampd(1 + dst[dstHarmLevel], 0, 2);
        const tiltMod = 12 * dst[dstHarmTilt];
        const lin = S.lin, lo = T.lo, tw = T.w;
        for (let k = 0; k < addCount; ++k) {
          const j = lo[k];
          tA[k] = lin[k] * lvlMod * (nodeLvl[j] + (nodeLvl[j + 1 < kAddNodes ? j + 1 : j] - nodeLvl[j]) * tw[k]);
        }
        if (tiltMod > 1e-4 || tiltMod < -1e-4) {
          const alpha = tiltMod * kLn10Over20 / kLn2;
          const f = this.sTilt;
          f[1] = 1;
          for (let nn = 2; nn <= addCount; ++nn) {
            const p = T.spf[nn];
            f[nn] = p === nn ? Math.exp(alpha * T.logn[nn - 1]) : f[p] * f[nn / p];
          }
          for (let k = 0; k < addCount; ++k) tA[k] *= f[k + 1];
        }
      }
      for (let k = addCount; k < kMaxPartials; ++k) tA[k] = 0;
      const addRun = Math.max(addCount, v.add.count);

      // transients + noise
      const extras = !analog && v.player === 0;
      const plScale = Math.sqrt(v.plCount);
      let tLip = 0, tClick = 0, tNoise = 0, tLipG = v.lipG, tNG = v.nG;
      if (extras) {
        const tChiff = v.chiffPeak * Math.exp(-v.tTrans / 0.035);
        tLip = v.lipPeak * Math.exp(-v.tTrans / Math.max(P.lipDecay * v.tgDecay, 0.001))
               * clampd(1 + dst[dstLipBuzz], 0, 3) * plScale;
        tClick = v.clickPeak * Math.exp(-v.tClick / 0.005) * plScale;
        tNoise = (P.brNoise * (1 - P.brDyn + P.brDyn * d) * envMain * (1 + 2 * flutNoise) * clampd(1 + dst[dstBreathNoise], 0, 3)
                  + tChiff) * 2 * plScale;
        if (tLip > 1e-5 || v.lipLvl > 1e-5)
          tLipG = SVF.gFor(hz0 * Math.pow(2, P.lipTone * 4), sr);
        if (tNoise > 1e-6 || v.noiseLvl > 1e-6)
          tNG = SVF.gFor(M.noiseHz * Math.pow(2, (P.brTone - 0.5) * 3 + P.bodyShift / 12)
                         * (1 + 1.5 * tChiff / (tChiff + 0.05)), sr);
      }
      const nK = 1.2 / M.noiseQ;
      const lipOn = extras && (tLip > 1e-5 || v.lipLvl > 1e-5);
      const noiseOn = extras && (tNoise > 1e-6 || v.noiseLvl > 1e-6);
      const clickOn = extras && (tClick > 1e-6 || v.clickLvl > 1e-6);

      // voice filter
      const fLvl = v.fenv.advance(P.fenv, dtSec);
      let tfg = v.fg, tfk = v.fk;
      if (P.fltOn) {
        const oct = P.fltKey * (v.pitch + P.transpose - 60) / 12 + P.fltEnvAmt * fLvl * v.fltVelScale
                    + P.fltDyn * d - 2 * dip + 3 * blat + 5 * dst[dstFltCutoff];
        tfg = SVF.gFor(P.fltCutoff * Math.pow(2, oct), sr);
        tfk = SVF.kFor(clampd(P.fltReso + dst[dstFltReso], 0, 1));
      }

      // brassiness
      const shaper = P.brsAmt > 1e-3 || P.dynBrass > 1e-3 || dst[dstBrassiness] > 1e-3;
      const shapeType = Math.min(Math.max(P.brsType, 0), 2);
      const tDrv = dbToGain(clampd(P.brsAmt * 12 + P.dynBrass * brightCurve * 12 + 12 * dst[dstBrassiness], 0, 30));
      const tDrvInv = 2 / (shapeFn(shapeType, tDrv) - shapeFn(shapeType, -tDrv));
      let osFactor = P.oversample >= 2 ? 4 : (P.oversample === 1 ? 2 : 1);
      if (v.player > 0) osFactor = Math.max(1, osFactor >> 1);
      const toneCoef = this.brsToneCoef;

      // voice gain + pan
      if (v.killing) v.kill = Math.max(0, v.kill - dtSec / kKillTime);
      const tgain = kVoiceGain * loud * vibAmpMul * (1 - 0.5 * dip) * v.kill * growlAM * flutAM * artGain * v.plGain
                    * clampd(1 + dst[dstAmp], 0, 2);
      let tPanL = v.panL, tPanR = v.panR;
      if (dst[dstPan] !== 0 || v.panMod) {
        const panNow = clampd(v.plPan + dst[dstPan], -1, 1);
        const panAng = (panNow + 1) * kPi / 4;
        tPanL = Math.min(1, kSqrt2 * Math.cos(panAng));
        tPanR = Math.min(1, kSqrt2 * Math.sin(panAng));
        v.panMod = dst[dstPan] !== 0;
      }

      if (v.snap) {
        for (let k = 0; k < kNumOsc; ++k) {
          const o = v.o[k];
          o.dt = tdt[k]; o.shape = tshape[k]; o.pw = tpw[k]; o.g = tg[k]; o.k = tk[k];
        }
        v.add.dt = tdtA;
        v.fg = tfg; v.fk = tfk;
        v.drv = tDrv; v.drvInv = tDrvInv;
        v.lipG = tLipG; v.nG = tNG;
        v.snap = false;
      }

      // ---- per-sample rendering with linear ramps
      const inv = 1 / n;
      const dt = this.sDt, ddt = this.sDdt, lv = this.sLv, dlv = this.sDlv, sh = this.sSh, dsh = this.sDsh,
            pw = this.sPw, dpw = this.sDpw, g = this.sG, dg = this.sDg, kk = this.sKk, dkk = this.sDkk, run = this.sRun;
      for (let k = 0; k < kNumOsc; ++k) {
        const o = v.o[k];
        dt[k] = o.dt; ddt[k] = (tdt[k] - o.dt) * inv;
        lv[k] = o.lvl; dlv[k] = (tlvl[k] - o.lvl) * inv;
        sh[k] = o.shape; dsh[k] = (tshape[k] - o.shape) * inv;
        pw[k] = o.pw; dpw[k] = (tpw[k] - o.pw) * inv;
        g[k] = o.g; dg[k] = (tg[k] - o.g) * inv;
        kk[k] = o.k; dkk[k] = (tk[k] - o.k) * inv;
        run[k] = (P.osc[k].on || o.lvl > 0) ? 1 : 0;
      }
      const aA = this.sAA, daA = this.sDaA, vamp = v.add.amp;
      for (let k = 0; k < addRun; ++k) { aA[k] = vamp[k]; daA[k] = (tA[k] - aA[k]) * inv; }
      const rotC = Math.cos(kPi * (v.add.dt + tdtA)), rotS = Math.sin(kPi * (v.add.dt + tdtA));
      let pc = v.add.c, ps = v.add.s;
      let fg = v.fg, fk = v.fk, gain = v.gain, pl = v.panL, pr = v.panR;
      const dfg = (tfg - fg) * inv, dfk = (tfk - fk) * inv, dgain = (tgain - gain) * inv;
      const dpl = (tPanL - pl) * inv, dpr = (tPanR - pr) * inv;
      let drv = v.drv, drvInv = v.drvInv;
      const ddrv = (tDrv - drv) * inv, ddrvInv = (tDrvInv - drvInv) * inv;
      let lipL = v.lipLvl, lipG = v.lipG, nL = v.noiseLvl, nG = v.nG, cL = v.clickLvl;
      const dlipL = (tLip - lipL) * inv, dlipG = (tLipG - lipG) * inv, dnL = (tNoise - nL) * inv,
            dnG = (tNG - nG) * inv, dcL = (tClick - cL) * inv;
      const flt = P.fltOn;
      const ftype = P.fltType;
      const lipPitch = P.lipPitch;
      const mixL = this.mixL, mixR = this.mixR, dcR = this.dcR;
      const o0 = v.o[0], o1 = v.o[1], o2 = v.o[2];
      const run0 = run[0] === 1, run1 = run[1] === 1, run2 = run[2] === 1;
      const anyNoise = noiseOn || lipOn || clickOn;
      let dcX = v.dcX, dcY = v.dcY, toneZ = v.toneZ, nPrev = v.nPrev;

      for (let i = 0; i < n; ++i) {
        let s = 0;
        if (run0) {
          dt[0] += ddt[0]; lv[0] += dlv[0]; sh[0] += dsh[0]; pw[0] += dpw[0]; g[0] += dg[0]; kk[0] += dkk[0];
          s += o0.filt.process(o0.osc.next(sh[0], pw[0], dt[0]), g[0], kk[0]) * lv[0];
        }
        if (run1) {
          dt[1] += ddt[1]; lv[1] += dlv[1]; sh[1] += dsh[1]; pw[1] += dpw[1]; g[1] += dg[1]; kk[1] += dkk[1];
          s += o1.filt.process(o1.osc.next(sh[1], pw[1], dt[1]), g[1], kk[1]) * lv[1];
        }
        if (run2) {
          dt[2] += ddt[2]; lv[2] += dlv[2]; sh[2] += dsh[2]; pw[2] += dpw[2]; g[2] += dg[2]; kk[2] += dkk[2];
          s += o2.filt.process(o2.osc.next(sh[2], pw[2], dt[2]), g[2], kk[2]) * lv[2];
        }
        if (addRun > 0) {
          const nc = pc * rotC - ps * rotS;
          ps = ps * rotC + pc * rotS;
          pc = nc;
          const c2 = 2 * pc;
          let sPrev = 0, sCur = ps;
          aA[0] += daA[0];
          let acc = aA[0] * sCur;
          for (let k = 1; k < addRun; ++k) {
            const sNext = c2 * sCur - sPrev;
            sPrev = sCur; sCur = sNext;
            aA[k] += daA[k];
            acc += aA[k] * sNext;
          }
          s += acc;
        }
        const wn = anyNoise ? v.nrng.bi() : 0;
        if (lipOn) {
          lipL += dlipL; lipG += dlipG;
          const ph = v.lipPhase;
          const saw = 2 * ph - 1 - polyBlep(ph, dt[0]);
          let p2 = ph + dt[0]; if (p2 >= 1) p2 -= 1;
          v.lipPhase = p2;
          v.lipF.process(lipPitch * saw + (1 - lipPitch) * wn, lipG, 0.7);
          s += v.lipF.bp * lipL * 3;
        }
        fg += dfg; fk += dfk; gain += dgain;
        if (flt) {
          const lp = v.f1.process(s, fg, fk);
          if (ftype === fltBP) s = v.f1.bp;
          else if (ftype === fltLP24) s = v.f2.process(lp, fg, fk);
          else s = lp;
        }
        if (shaper) {
          drv += ddrv; drvInv += ddrvInv;
          const y = v.os.process(s * drv, osFactor, shapeType) * drvInv;
          const dc = y - dcX + dcR * dcY;
          dcX = y; dcY = dc;
          toneZ += (dc - toneZ) * toneCoef;
          s = toneZ;
        }
        if (noiseOn) {
          nL += dnL; nG += dnG;
          v.noiseF.process(wn, nG, nK);
          s += v.noiseF.bp * nL;
        }
        if (clickOn) {
          cL += dcL;
          s += (wn - nPrev) * cL * 0.5;
        }
        nPrev = wn;
        pl += dpl; pr += dpr;
        const y = s * gain;
        mixL[i] += y * pl;
        mixR[i] += y * pr;
      }
      v.dcX = dcX; v.dcY = dcY; v.toneZ = toneZ; v.nPrev = nPrev;
      v.panL = tPanL; v.panR = tPanR;

      for (let k = 0; k < kNumOsc; ++k) {
        const o = v.o[k];
        o.dt = tdt[k]; o.lvl = tlvl[k]; o.shape = tshape[k]; o.pw = tpw[k]; o.g = tg[k]; o.k = tk[k];
      }
      for (let k = 0; k < addRun; ++k) vamp[k] = tA[k];
      v.add.count = addCount;
      v.add.dt = tdtA;
      if (addRun > 0) {
        const len = 1 / Math.sqrt(pc * pc + ps * ps);
        v.add.c = pc * len; v.add.s = ps * len;
      }
      v.fg = tfg; v.fk = tfk; v.gain = tgain;
      v.drv = tDrv; v.drvInv = tDrvInv;
      v.lipLvl = tLip; v.lipG = tLipG; v.noiseLvl = tNoise; v.nG = tNG; v.clickLvl = tClick;

      // advance to the next chunk
      for (let i = 0; i < kNumLfo; ++i)
        if (P.lfo[i].perVoice)
          v.lfo[i].advance(dst[dstLfo1Rate + i] !== 0 ? this.lfoInc[i] * Math.pow(2, 2 * dst[dstLfo1Rate + i]) : this.lfoInc[i], rng);
      if (v.glidePos < 1) {
        v.glidePos = Math.min(1, v.glidePos + n / (Math.max(P.glideTime, 0.001) * sr));
        v.pitch = v.glideFrom + (v.glideTo - v.glideFrom) * curveShape(v.glidePos, P.glideCurve);
      }
      if (!alive || (v.killing && v.kill <= 0)) this.hardStop(v);
    }
  }

  const plungerCut = (pos) => 350 * Math.pow(2, clampd(pos, 0, 1) * 4.3);

  function safetyClip(x) {
    const a = Math.abs(x);
    if (a <= 0.8) return x;
    const y = 0.8 + 0.2 * Math.tanh((a - 0.8) / 0.2);
    return x < 0 ? -y : y;
  }

  function gateOffAll(v) {
    for (const o of v.o) { o.amp.gateOff(); o.tmb.gateOff(); }
    for (const e of v.add.node) e.gateOff();
    v.fenv.gateOff();
  }

  function scaledInto(r, e, t, atk) {
    r.dly = e.dly * t; r.atk = e.atk * atk; r.hld = e.hld * t; r.dec = e.dec * t; r.sus = e.sus; r.rel = e.rel * t;
    r.atkC = e.atkC; r.decC = e.decC; r.relC = e.relC;
  }

  // ========================================================== processor ===
  // Messages in (from web-host.js, or the parity runner):
  //   param {id,value} · params {values} · midi {data:[status,d1,d2]} (same routing as
  //   PluginProcessor::handleMidi) · noteOn {note,vel,ch} · noteOff {note,ch} · cc {cc,value,ch} ·
  //   bend {value,ch} · pressure {value,ch} · polyPressure {note,value} · sustain {on} · modWheel {value} ·
  //   breath {value} (negative = forget the breath controller) · articulation {index} · tempo {bpm} ·
  //   allNotesOff · panic
  // Messages out: stats (the same fields the plugin editor sends)
  class SynthProcessor extends AudioWorkletProcessor {
    constructor(options) {
      super();
      const init = (options && options.processorOptions && options.processorOptions.params) || {};
      this.values = Object.assign({}, init);
      this.engine = new SynthEngine();
      this.engine.prepare(sampleRate);
      this.P = newEngineParams();
      this.dirty = true;
      this.bpm = 120;
      this.bend = 0;
      this.blockCount = 0;
      this.applyParams();
      this.port.onmessage = (e) => this.onMessage(e.data);
    }

    onMessage(m) {
      const e = this.engine;
      switch (m.type) {
        case 'param': this.values[m.id] = m.value; this.dirty = true; break;
        case 'params': Object.assign(this.values, m.values); this.dirty = true; break;
        case 'midi': this.applyParams(); this.midi(m.data); break;
        case 'noteOn': this.applyParams(); e.noteOn(m.note, m.vel, m.ch | 0); break;
        case 'noteOff': this.applyParams(); e.noteOff(m.note, m.ch | 0); break;
        case 'cc': this.applyParams(); e.setController(m.cc, m.value, m.ch | 0); break;
        case 'bend': e.setPitchBend(m.value, m.ch | 0); this.bend = m.value; break;
        case 'pressure': this.applyParams(); e.setChannelPressure(m.value, m.ch | 0); break;
        case 'polyPressure': e.setPolyPressure(m.note, m.value); break;
        case 'sustain': e.setSustain(!!m.on); break;
        case 'modWheel': e.setModWheel(m.value); break;
        case 'breath':
          this.applyParams();
          if (m.value >= 0) e.setController(lround(Math.fround(+this.values.dynBreathCC)), clampd(m.value, 0, 1), 0);
          else e.forgetBreath();
          break;
        case 'articulation': e.setArticulation(m.index | 0); break;
        case 'tempo': this.bpm = m.bpm; break;
        case 'allNotesOff': e.allNotesOff(); break;
        case 'panic': e.panic(); break;
      }
    }

    // same dispatch as PluginProcessor::handleMidi
    midi(d) {
      const e = this.engine, st = d[0];
      if (st >= 0xf0 || st < 0x80) return;
      const ch = st & 15, cmd = st & 0xf0;
      if (cmd === 0x90 && d[2] > 0) e.noteOn(d[1], d[2], ch);
      else if (cmd === 0x80 || cmd === 0x90) e.noteOff(d[1], ch);
      else if (cmd === 0xe0) { const v = (((d[2] << 7) | d[1]) - 8192) / 8192; e.setPitchBend(v, ch); this.bend = v; }
      else if (cmd === 0xd0) e.setChannelPressure(d[1] / 127, ch);
      else if (cmd === 0xa0) e.setPolyPressure(d[1], d[2] / 127);
      else if (cmd === 0xb0) {
        if (d[1] === 120) e.panic();
        else if (d[1] === 123) e.allNotesOff();
        else e.setController(d[1], d[2] / 127, ch);
      }
    }

    applyParams() {
      if (!this.dirty) return;
      this.engine.setParams(makeEngineParams(this.values, this.P));
      this.dirty = false;
    }

    process(inputs, outputs) {
      const out = outputs[0];
      const L = out && out[0];
      if (!L) return true;
      const R = out.length > 1 ? out[1] : null;
      this.applyParams();
      this.engine.setTransport(this.bpm, 0, false);
      this.engine.render(L, R, 0, L.length);

      if (++this.blockCount >= 16) {
        const e = this.engine;
        this.port.postMessage({
          type: 'stats', voices: e.activeVoices(), peak: e.takePeak(), sustain: e.sustainPedal, modWheel: e.wheelTarget,
          bpm: this.bpm, breath: e.breathValue(), expression: e.expressionValue(), aftertouch: e.aftertouchValue(),
          dynamics: e.lastDyn, bend: this.bend, plunger: e.plungerCur, articulation: e.articulation(),
        });
        this.blockCount = 0;
      }
      return true;
    }
  }

  registerProcessor('brass-knuckles', SynthProcessor);
}
