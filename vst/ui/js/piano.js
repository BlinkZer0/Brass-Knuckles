// ============================================================================
//  SVG keyboard (default 49 keys). Click/touch to play, drag for glissando.
//  Velocity = how far down the key you click (lower = harder).
//  Extras for Brass Knuckles:
//   - keys outside the instrument's realistic range are dimmed (still playable)
//   - the keyswitch octave is coloured; arrows show where the range and the
//     keyswitches are when they're scrolled out of view
// ============================================================================
'use strict';

const BLACK_PCS = new Set([1, 3, 6, 8, 10]);

class Piano {
  constructor(container, { numKeys = 49, baseNote = 36, onNoteOn, onNoteOff }) {
    this.numKeys = numKeys;
    this.base = baseNote; // must be a C
    this.onNoteOn = onNoteOn;
    this.onNoteOff = onNoteOff;
    this.active = new Set();     // sounding MIDI notes (from any source)
    this.pointers = new Map();   // pointerId -> note
    this.range = null;           // [lo, hi] realistic range, or null
    this.ks = null;              // [lo, hi] keyswitch notes, or null
    this.ksActive = null;        // the latched articulation's keyswitch
    this.build(container);
  }

  build(container) {
    const WW = 28, WH = 150, BW = 17, BH = 92, TOP = 10;
    const whites = [], blacks = [];
    let wi = 0;
    this.keyX = [];
    for (let i = 0; i < this.numKeys; i++) {
      if (BLACK_PCS.has(i % 12)) {
        blacks.push(`<rect class="bkey" data-idx="${i}" x="${wi * WW - BW / 2}" y="${TOP}" width="${BW}" height="${BH}" rx="2"/>`);
        this.keyX.push(wi * WW);
      } else {
        whites.push(`<rect class="wkey" data-idx="${i}" x="${wi * WW + 1}" y="${TOP}" width="${WW - 2}" height="${WH}" rx="3"/>`);
        if (i % 12 === 0) whites.push(`<text class="klabel" data-label="${i}" x="${wi * WW + WW / 2}" y="${TOP + WH - 10}"></text>`);
        this.keyX.push(wi * WW + WW / 2);
        wi++;
      }
    }
    const W = wi * WW;
    this.W = W;
    container.innerHTML = `
      <svg class="piano-svg" viewBox="0 0 ${W} ${TOP + WH + 2}" preserveAspectRatio="xMidYMid meet">
        <rect class="felt" x="0" y="0" width="${W}" height="${TOP - 2}"/>
        <rect class="range-mark" data-mark="range" x="0" y="${TOP - 6}" width="0" height="3" rx="1.5"/>
        <rect class="ks-mark" data-mark="ks" x="0" y="${TOP - 6}" width="0" height="3" rx="1.5"/>
        ${whites.join('')}
        ${blacks.join('')}
      </svg>`;
    this.svg = container.querySelector('svg');
    this.keys = Array.from({ length: this.numKeys }, (_, i) => this.svg.querySelector(`[data-idx="${i}"]`));
    this.labels = Array.from(this.svg.querySelectorAll('[data-label]'));
    this.rangeMark = this.svg.querySelector('[data-mark="range"]');
    this.ksMark = this.svg.querySelector('[data-mark="ks"]');
    this.bindPointer();
    this.render();
  }

  setBase(note) { this.base = note; this.render(); }
  setRange(range) { this.range = range; this.render(); }
  setKeyswitches(ks) { this.ks = ks; this.render(); }
  setActiveKeyswitch(note) { this.ksActive = note; this.render(); }

  setNote(note, on) {
    if (on) this.active.add(note); else this.active.delete(note);
    const i = note - this.base;
    if (i >= 0 && i < this.numKeys) this.keys[i].classList.toggle('down', on);
  }

  clear() { this.active.clear(); this.render(); }

  // felt strip above the keys: where the range / keyswitches are (clipped to the view)
  markSpan(el, span) {
    if (!span) { el.setAttribute('width', 0); return; }
    const lo = Math.max(span[0], this.base), hi = Math.min(span[1], this.base + this.numKeys - 1);
    if (hi < lo) { el.setAttribute('width', 0); return; }
    const x0 = this.keyX[lo - this.base] - 8, x1 = this.keyX[hi - this.base] + 8;
    el.setAttribute('x', Math.max(0, x0).toFixed(1));
    el.setAttribute('width', Math.max(0, Math.min(this.W, x1) - Math.max(0, x0)).toFixed(1));
  }

  render() {
    this.keys.forEach((k, i) => {
      const n = this.base + i;
      k.classList.toggle('down', this.active.has(n));
      k.classList.toggle('out', !!this.range && (n < this.range[0] || n > this.range[1]));
      k.classList.toggle('ks', !!this.ks && n >= this.ks[0] && n <= this.ks[1]);
      k.classList.toggle('ks-active', this.ksActive === n);
    });
    this.labels.forEach(t => { t.textContent = midiNoteName(this.base + Number(t.dataset.label)); });
    this.markSpan(this.rangeMark, this.range);
    this.markSpan(this.ksMark, this.ks);
  }

  hit(x, y) {
    const t = document.elementFromPoint(x, y);
    if (!t || !this.svg.contains(t) || !t.dataset.idx) return null;
    const r = t.getBoundingClientRect();
    const depth = clamp((y - r.top) / r.height, 0, 1);
    return { note: this.base + Number(t.dataset.idx), vel: Math.round(25 + depth * 102) };
  }

  bindPointer() {
    const svg = this.svg;
    const press = (id, x, y) => {
      const h = this.hit(x, y);
      if (!h) return;
      this.pointers.set(id, h.note);
      this.onNoteOn(h.note, h.vel);
    };
    const lift = (id) => {
      const n = this.pointers.get(id);
      if (n !== undefined && n !== null) this.onNoteOff(n);
      this.pointers.delete(id);
    };
    svg.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      svg.setPointerCapture(e.pointerId);
      press(e.pointerId, e.clientX, e.clientY);
    });
    svg.addEventListener('pointermove', (e) => {
      if (!this.pointers.has(e.pointerId)) return;
      const h = this.hit(e.clientX, e.clientY);
      const cur = this.pointers.get(e.pointerId);
      const next = h ? h.note : null;
      if (next === cur) return;
      // glissando: new key first, then release the old one, so legato presets slur
      if (h) this.onNoteOn(h.note, h.vel);
      if (cur !== null) this.onNoteOff(cur);
      this.pointers.set(e.pointerId, next);
    });
    svg.addEventListener('pointerup', (e) => lift(e.pointerId));
    svg.addEventListener('pointercancel', (e) => lift(e.pointerId));
  }
}
