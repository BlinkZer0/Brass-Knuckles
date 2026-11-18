// ============================================================================
//  Envelope editor: an SVG graph of one oscillator envelope with draggable
//  handles. Each handle drives one or two parameters (x = a time, y = a level
//  or amount); drags are relative in the parameter's normalised range, so
//  log-scaled times feel even. Shift = fine. Double-click a handle = defaults.
//
//  kinds:  'amp'    Delay, Attack, Hold, Decay/Sustain, Release
//          'timbre' Delay, Attack, Decay/Sustain, Release
//          'pitch'  scoop start (cents), scoop time, blip (time + cents)
// ============================================================================
'use strict';

// Same as curveShape() in vst/Source/dsp/Common.h
function curveShape(p, c) {
  if (c > -1e-3 && c < 1e-3) return p;
  const k = 5 * c;
  return (1 - Math.exp(-k * p)) / (1 - Math.exp(-k));
}

class EnvGraph {
  // ctx: { get(id), set(id, v), begin(id), end(id) }
  constructor(prefix, kind, ctx) {
    this.prefix = prefix;
    this.kind = kind;
    this.ctx = ctx;
    this.drag = null;
    const el = document.createElement('div');
    el.className = 'env-graph';
    el.innerHTML = `<svg preserveAspectRatio="none"></svg><div class="eg-text"></div>`;
    this.el = el;
    this.svg = el.querySelector('svg');
    this.text = el.querySelector('.eg-text');
    this.bind();
  }

  id(f) { return this.prefix + f; }
  v(f) { return this.ctx.get(this.id(f)); }

  // display width of a segment of t seconds (log-ish, so 1 ms and 10 s both fit)
  static tw(t, seg) {
    if (t <= 0.0011) return 0;
    return seg * (0.08 + 0.92 * Math.log10(1 + t * 100) / Math.log10(1 + 1000));
  }

  layout(W, H) {
    const pad = 7, top = pad, bot = H - pad;
    const pts = [], handles = [];
    let text = '';
    const fmt = (f) => formatParam(PARAM_BY_ID[this.id(f)], this.v(f));
    const seg = (x0, x1, y0, y1, c) => {
      const n = Math.max(2, Math.round((x1 - x0) / 4));
      for (let i = 1; i <= n; i++) {
        const p = i / n;
        pts.push([x0 + (x1 - x0) * p, y0 + (y1 - y0) * curveShape(p, c)]);
      }
    };

    if (this.kind === 'amp' || this.kind === 'timbre') {
      const amp = this.kind === 'amp';
      const P = amp ? 'Amp' : 'TEnv';
      const dly = this.v(`${P}Dly`), atk = this.v(`${P}Atk`), hld = amp ? this.v('AmpHld') : 0;
      const dec = this.v(`${P}Dec`), sus = this.v(`${P}Sus`), rel = this.v(`${P}Rel`);
      const ac = amp ? this.v('AmpAtkCrv') : this.v('TEnvCrv');
      const dc = amp ? this.v('AmpDecCrv') : this.v('TEnvCrv');
      const rc = amp ? this.v('AmpRelCrv') : this.v('TEnvCrv');
      const S = (W - 2 * pad) * 0.2;
      const ys = bot - sus * (bot - top);
      let x = pad;
      pts.push([x, bot]);
      const xd = x + EnvGraph.tw(dly, S); pts.push([xd, bot]);
      const xa = xd + Math.max(2, EnvGraph.tw(atk, S)); seg(xd, xa, bot, top, ac);
      const xh = xa + EnvGraph.tw(hld, S); pts.push([xh, top]);
      const xs = xh + Math.max(2, EnvGraph.tw(dec, S)); seg(xh, xs, top, ys, dc);
      const xr0 = Math.max(xs + 10, W - pad - Math.max(2, EnvGraph.tw(rel, S)));
      pts.push([xr0, ys]);
      seg(xr0, W - pad, ys, bot, rc);
      handles.push({ x: xd, y: bot, xf: `${P}Dly` });
      handles.push({ x: xa, y: top, xf: `${P}Atk` });
      if (amp) handles.push({ x: xh, y: top, xf: 'AmpHld' });
      handles.push({ x: xs, y: ys, xf: `${P}Dec`, yf: `${P}Sus` });
      handles.push({ x: W - pad, y: bot, xf: `${P}Rel`, xRev: true });
      text = `A ${fmt(`${P}Atk`)}  D ${fmt(`${P}Dec`)}  S ${fmt(`${P}Sus`)}  R ${fmt(`${P}Rel`)}`;
      if (!amp) text = `${fmt('TEnvCut')} · ${text}`;
      return { pts, handles, text, zero: null };
    }

    // pitch: offset in cents over time; sign-preserving sqrt scale (±1200 ct)
    const mid = H / 2, half = H / 2 - pad;
    const yc = (c) => mid - Math.sign(c) * Math.sqrt(Math.min(1, Math.abs(c) / 1200)) * half;
    const amt = this.v('PEnvAmt'), T = this.v('PEnvTime'), crv = this.v('PEnvCrv');
    const blip = this.v('BlipAmt'), BT = this.v('BlipTime');
    const S = (W - 2 * pad) * 0.42;
    const xOf = (t) => pad + EnvGraph.tw(Math.max(t, 0.0012), S);
    const tEnd = Math.max(T, BT) * 1.6 + 0.02;
    const n = 90;
    for (let i = 0; i <= n; i++) {
      const t = tEnd * i / n;
      let c = amt * (1 - curveShape(Math.min(t / T, 1), crv));
      if (t < BT) c += blip * Math.sin(Math.PI * t / BT);
      pts.push([i === 0 ? pad : xOf(t), yc(c)]);
    }
    pts.push([W - pad, mid]);
    handles.push({ x: pad, y: yc(amt), yf: 'PEnvAmt' });
    handles.push({ x: xOf(T), y: mid, xf: 'PEnvTime' });
    handles.push({ x: xOf(BT / 2), y: yc(blip), xf: 'BlipTime', yf: 'BlipAmt' });
    text = `SCOOP ${fmt('PEnvAmt')} / ${fmt('PEnvTime')}  BLIP ${fmt('BlipAmt')}`;
    return { pts, handles, text, zero: mid };
  }

  render() {
    const W = this.el.clientWidth || 340, H = this.el.clientHeight || 92;
    this.W = W; this.H = H;
    const { pts, handles, text, zero } = this.layout(W, H);
    this.handles = handles;
    const line = 'M' + pts.map(p => `${p[0].toFixed(1)},${p[1].toFixed(1)}`).join(' L');
    const base = zero === null ? H - 7 : zero;
    const fill = `${line} L${pts[pts.length - 1][0].toFixed(1)},${base} L${pts[0][0].toFixed(1)},${base} Z`;
    const grid = [0.25, 0.5, 0.75].map(f => `<path class="eg-grid" d="M0 ${(H * f).toFixed(1)} H${W}"/>`).join('');
    this.svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
    this.svg.innerHTML = `${grid}
      ${zero === null ? '' : `<path class="eg-zero" d="M0 ${zero} H${W}"/>`}
      <path class="eg-fill" d="${fill}"/><path class="eg-line" d="${line}"/>
      ${handles.map((h, i) => `<circle class="eg-handle${this.drag && this.drag.i === i ? ' drag' : ''}" data-h="${i}"
          cx="${h.x.toFixed(1)}" cy="${h.y.toFixed(1)}" r="4.5"/>`).join('')}`;
    this.text.textContent = text;
  }

  ids(h) { return [h.xf, h.yf].filter(Boolean).map(f => this.id(f)); }

  bind() {
    const svg = this.svg;
    svg.addEventListener('pointerdown', (e) => {
      const t = e.target.closest('[data-h]');
      if (!t) return;
      e.preventDefault();
      svg.setPointerCapture(e.pointerId);
      const i = Number(t.dataset.h), h = this.handles[i];
      const norm = (f) => f ? paramToNorm(PARAM_BY_ID[this.id(f)], this.v(f)) : 0;
      this.drag = { i, h, x: e.clientX, y: e.clientY, nx: norm(h.xf), ny: norm(h.yf), dx: 0, dy: 0 };
      this.ids(h).forEach(id => this.ctx.begin(id));
      this.render();
    });
    svg.addEventListener('pointermove', (e) => {
      const d = this.drag;
      if (!d) return;
      const k = e.shiftKey ? 0.2 : 1;
      // the panel may be scaled: convert screen pixels to graph pixels
      const r = this.el.getBoundingClientRect();
      const sx = this.W / Math.max(1, r.width), sy = this.H / Math.max(1, r.height);
      d.dx += (e.clientX - d.x) * sx * k; d.dy += (e.clientY - d.y) * sy * k;
      d.x = e.clientX; d.y = e.clientY;
      if (d.h.xf) {
        const def = PARAM_BY_ID[this.id(d.h.xf)];
        this.ctx.set(def.id, paramFromNorm(def, d.nx + (d.h.xRev ? -d.dx : d.dx) / (this.W * 0.45)));
      }
      if (d.h.yf) {
        const def = PARAM_BY_ID[this.id(d.h.yf)];
        this.ctx.set(def.id, paramFromNorm(def, d.ny - d.dy / (this.H - 14)));
      }
      this.render();
    });
    const end = () => {
      if (!this.drag) return;
      this.ids(this.drag.h).forEach(id => this.ctx.end(id));
      this.drag = null;
      this.render();
    };
    svg.addEventListener('pointerup', end);
    svg.addEventListener('pointercancel', end);
    svg.addEventListener('dblclick', (e) => {
      const t = e.target.closest('[data-h]');
      if (!t) return;
      const h = this.handles[Number(t.dataset.h)];
      this.ids(h).forEach(id => this.ctx.set(id, PARAM_BY_ID[id].def));
      this.render();
    });
  }
}
