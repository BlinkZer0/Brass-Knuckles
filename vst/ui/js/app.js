// ============================================================================
//  UI wiring, shared by the plugin and the web build.
//   - talks only to JuceEngine (engine-juce.js). In the plugin that is JUCE's
//     native bridge to C++; in the web build vst/webhost/web-host.js provides
//     the same bridge on top of Web Audio + Web MIDI.
//   - the host owns all parameter state; automation, preset loads and MIDI
//     notes flow back in as events
//   - the whole panel scales to fit the window
// ============================================================================
(function () {
  'use strict';

  const $ = (sel) => document.querySelector(sel);
  function el(tag, cls, html) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (html !== undefined) e.innerHTML = html;
    return e;
  }
  const store = {
    get(k) { try { return localStorage.getItem(k); } catch (e) { return null; } },
    set(k, v) { try { localStorage.setItem(k, v); } catch (e) { /* storage unavailable */ } },
  };

  // ---------------------------------------------------------------- state ---
  const values = defaultParamValues();      // replaced by the plugin's state on connect
  let presetList = [];                      // [{id, name, category, factory}] factory first, then user
  let presetFolder = '';
  let current = { id: '', name: '—', category: '' };
  let presetDirty = false;
  let activeCat = null;

  const engine = new JuceEngine();
  const knobs = {};
  const bound = {};             // id -> [controls with setValue()]
  const dragging = new Set();   // params being dragged in the UI
  const lastLocal = {};         // id -> time of last UI-side change
  const refreshers = [];        // functions run after values change: (changedIds:Set|null) => void

  // ---------------------------------------------------- parameter plumbing ---
  function applyValue(id, v) {
    values[id] = v;
    if (knobs[id]) knobs[id].setValue(v);
    (bound[id] || []).forEach(c => c.setValue(v));
  }

  function refresh(ids) { refreshers.forEach(fn => fn(ids)); }

  // A change made in this UI
  function setParam(id, v) {
    lastLocal[id] = performance.now();
    applyValue(id, v);
    engine.setParam(id, v);
    if (!PARAM_BY_ID[id].noPreset && !presetDirty) { presetDirty = true; renderPresetName(); }
    refresh(new Set([id]));
  }

  const gesture = {
    begin(id) { dragging.add(id); engine.beginGesture(id); },
    end(id) { dragging.delete(id); engine.endGesture(id); },
  };

  function addKnob(parent, id, size) {
    const k = new Knob(PARAM_BY_ID[id], values[id], (v) => setParam(id, v),
      (begin) => (begin ? gesture.begin(id) : gesture.end(id)));
    if (size) k.el.classList.add(size);
    knobs[id] = k;
    k.el.dataset.param = id;
    parent.appendChild(k.el);
    return k;
  }

  // Bind a non-knob control (ToggleButton / Segmented / SelectBox) to a param
  function control(Cls, id, ...extra) {
    const c = new Cls(PARAM_BY_ID[id], values[id], (v) => setParam(id, v), ...extra);
    (bound[id] = bound[id] || []).push(c);
    c.el.dataset.param = id;
    return c;
  }

  // Changes coming from the plugin (automation, presets, state restore)
  engine.on('params', (obj) => {
    const now = performance.now();
    const changed = new Set();
    for (const id in obj) {
      if (!PARAM_BY_ID[id]) continue;
      // don't fight the user's hand: skip params being dragged or changed here in the last 300 ms
      if (dragging.has(id) || (lastLocal[id] !== undefined && now - lastLocal[id] < 300)) continue;
      applyValue(id, obj[id]);
      changed.add(id);
    }
    if (changed.size) refresh(changed);
  });

  // ------------------------------------------------------------ presets ----
  const esc = (s) => String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

  engine.on('presetList', ({ presets, folder }) => {
    presetList = presets;
    presetFolder = folder;
    if (!activeCat || !categories().includes(activeCat)) activeCat = current.category || (presets[0] && presets[0].category);
    renderBrowser();
  });

  engine.on('preset', (p) => {
    const changed = p.id !== current.id || p.name !== current.name;
    current = p;
    if (changed) presetDirty = false;
    if (p.category && categories().includes(p.category)) activeCat = p.category;
    renderPresetName();
    renderBrowser();
  });

  engine.on('message', ({ text }) => logMidi(text));

  function categories() {
    const cats = [];
    for (const p of presetList) if (!cats.includes(p.category)) cats.push(p.category);
    if (!presetList.some(p => !p.factory) && !cats.includes('User')) cats.push('User');
    return cats;
  }

  function loadPreset(p) {
    presetDirty = false;
    current = { id: p.id, name: p.name, category: p.category };
    for (const id in lastLocal) delete lastLocal[id]; // let all incoming values through
    renderPresetName();
    renderBrowser();
    engine.loadPreset(p.id); // the plugin applies the values and sends them back
  }

  function stepPreset(dir) {
    if (!presetList.length) return;
    const i = presetList.findIndex(p => p.id === current.id);
    const next = i < 0 ? 0 : (i + dir + presetList.length) % presetList.length;
    loadPreset(presetList[next]);
  }

  function renderPresetName() {
    $('#preset-name').textContent = current.name + (presetDirty ? ' *' : '');
    $('#preset-cat').textContent = (current.category || 'Preset').toUpperCase();
  }

  const tabsEl = $('#cat-tabs');
  const gridEl = $('#preset-grid');
  function renderBrowser() {
    const cats = categories();
    tabsEl.innerHTML = '';
    for (const c of cats) {
      const t = el('button', 'cat-tab', esc(c));
      t.type = 'button';
      t.setAttribute('role', 'tab');
      t.setAttribute('aria-selected', c === activeCat);
      t.classList.toggle('active', c === activeCat);
      t.classList.toggle('has-current', c === current.category);
      t.addEventListener('click', () => { activeCat = c; renderBrowser(); });
      tabsEl.appendChild(t);
    }
    gridEl.innerHTML = '';
    const items = presetList.filter(p => p.category === activeCat);
    if (!items.length) {
      gridEl.appendChild(el('div', 'grid-empty',
        `No user presets yet. Click <b>SAVE</b> to store the current sound.<br>
         Preset files in <span class="path">${esc(presetFolder)}</span> show up here automatically.`));
      return;
    }
    for (const p of items) {
      const b = el('button', 'preset-btn', `<span class="pb-name">${esc(p.name)}</span>`);
      b.type = 'button';
      b.title = p.name;
      b.classList.toggle('selected', p.id === current.id);
      b.addEventListener('click', () => loadPreset(p));
      if (!p.factory) {
        const del = el('span', 'pb-del', '×');
        del.title = 'Delete preset';
        del.addEventListener('click', (e) => {
          e.stopPropagation();
          openModal({
            title: 'Delete preset', body: `Move <b>${esc(p.name)}</b> to the Recycle Bin?`,
            ok: 'DELETE', danger: true, onOk: () => engine.deletePreset(p.id),
          });
        });
        b.appendChild(del);
      }
      gridEl.appendChild(b);
    }
  }

  function savePrompt() {
    openModal({
      title: 'Save preset',
      body: `Saved to <span class="path">${esc(presetFolder)}</span>`,
      input: current.category === 'User' || !current.id.startsWith('factory:') ? current.name : `${current.name} (copy)`,
      ok: 'SAVE',
      onOk: (name) => { if (name.trim()) engine.savePreset(name.trim(), false); },
    });
  }
  engine.on('saveResult', ({ ok, exists, name, error }) => {
    if (ok) { presetDirty = false; renderPresetName(); logMidi(`Saved preset "${name}"`); }
    else if (exists) {
      openModal({
        title: 'Replace preset?', body: `A preset named <b>${esc(name)}</b> already exists. Replace it?`,
        ok: 'REPLACE', danger: true, onOk: () => engine.savePreset(name, true),
      });
    } else logMidi(`Save failed: ${error}`);
  });

  $('#preset-prev').addEventListener('click', () => stepPreset(-1));
  $('#preset-next').addEventListener('click', () => stepPreset(1));
  $('#preset-save').addEventListener('click', savePrompt);
  $('#preset-load').addEventListener('click', () => engine.loadPresetFile());
  $('#preset-folder').addEventListener('click', () => engine.openPresetFolder());

  // ------------------------------------------------------------- modal -----
  const modal = $('#modal'), modalInput = $('#modal-input'), modalOk = $('#modal-ok');
  let modalOnOk = null;
  function openModal({ title, body, input, ok, danger, onOk }) {
    $('#modal-title').textContent = title;
    $('#modal-body').innerHTML = body || '';
    modalOk.textContent = ok || 'OK';
    modalOk.classList.toggle('danger', !!danger);
    modalInput.style.display = input === undefined ? 'none' : '';
    modalInput.value = input || '';
    modalOnOk = onOk;
    modal.classList.remove('hidden');
    if (input !== undefined) { modalInput.focus(); modalInput.select(); } else modalOk.focus();
  }
  function closeModal(accept) {
    if (modal.classList.contains('hidden')) return;
    modal.classList.add('hidden');
    const fn = modalOnOk;
    modalOnOk = null;
    if (accept && fn) fn(modalInput.value);
  }
  modalOk.addEventListener('click', () => closeModal(true));
  $('#modal-cancel').addEventListener('click', () => closeModal(false));
  modal.addEventListener('pointerdown', (e) => { if (e.target === modal) closeModal(false); });
  modal.addEventListener('keydown', (e) => {
    e.stopPropagation(); // typing a name must not play notes
    if (e.key === 'Enter') { e.preventDefault(); closeModal(true); }
    else if (e.key === 'Escape') { e.preventDefault(); closeModal(false); }
  });
  modal.addEventListener('keyup', (e) => e.stopPropagation());

  // ------------------------------------------------------------- pages -----
  const PAGE_HINTS = {
    play: 'Dynamics: velocity, breath (CC2), expression, aftertouch → loudness, brightness and attack',
    osc: 'Each oscillator has its own Amp, Pitch and Timbre envelopes · drag the points, Shift = fine',
    harm: '32 harmonics, each with its own envelope: upper harmonics arrive later and fade first',
    tone: 'Brassiness grows with dynamics · body and mute filters follow the instrument',
    attack: 'How each note starts and breathes',
  };
  let activePage = location.hash.slice(1) || store.get('bk.page') || 'play'; // #osc opens a page directly (tests)
  function showPage(name) {
    if (!PAGE_HINTS[name]) name = 'play';
    activePage = name;
    store.set('bk.page', name);
    document.querySelectorAll('.page-tab').forEach(t => t.classList.toggle('active', t.dataset.page === name));
    document.querySelectorAll('.page').forEach(p => p.classList.toggle('active', p.dataset.page === name));
    $('#page-hint').textContent = PAGE_HINTS[name];
    refresh(null); // graphs measure their size when visible
  }
  document.querySelectorAll('.page-tab').forEach(t => t.addEventListener('click', () => showPage(t.dataset.page)));

  function makePanel(title, extraHeader) {
    const p = el('div', 'panel');
    const h = el('div', 'panel-head', `<span class="panel-title">${title}</span>`);
    if (extraHeader) (Array.isArray(extraHeader) ? extraHeader : [extraHeader]).forEach(x => h.appendChild(x));
    p.appendChild(h);
    return p;
  }
  function knobRow(parent, ids, size, cls) {
    const row = el('div', 'knob-row' + (cls ? ' ' + cls : ''));
    ids.forEach(id => addKnob(row, id, size));
    parent.appendChild(row);
    return row;
  }
  function field(label, ctl) {
    const f = el('div', 'field', `<span class="ctl-label">${label}</span>`);
    f.appendChild(ctl.el);
    return f;
  }

  // ========================================================== PLAY page ====
  const row1 = $('#play-row-1'), row2 = $('#play-row-2');

  // ---- instrument
  {
    const p = makePanel('INSTRUMENT');
    const grid = el('div', 'field-grid');
    grid.appendChild(field('Instrument', control(SelectBox, 'instrument')));
    grid.appendChild(field('Engine', control(Segmented, 'engineMode')));
    p.appendChild(grid);
    knobRow(p, ['ampLevel', 'tune', 'transpose']);
    row1.appendChild(p);
  }

  // ---- dynamics
  const dynUi = {};
  {
    const p = makePanel('DYNAMICS');
    const scr = el('div', 'screen dyn-screen');
    const bar = (key, label, title) => {
      const s = el('div', 'dyn-src', `<div class="dyn-bar"><div></div></div><span>${label}</span>`);
      s.title = title;
      scr.appendChild(s);
      dynUi[key] = { root: s, fill: s.querySelector('.dyn-bar > div') };
    };
    bar('breath', 'BREATH', 'Breath controller (the Breath CC, default CC2)');
    bar('expression', 'EXPR', 'Expression (CC11)');
    bar('aftertouch', 'AFTT', 'Channel aftertouch');
    bar('bend', 'BEND', 'Pitch bend');
    bar('dynamics', 'DYN', 'Resulting dynamics of the latest note');
    dynUi.dynamics.root.classList.add('out');
    const curves = el('div', 'dyn-curves', `
      <svg viewBox="0 0 100 60" preserveAspectRatio="none">
        <path class="scr-grid" d="M0 30 H100"/>
        <path class="dc-loud" d=""/><path class="dc-bright" d=""/><path class="dc-now" d=""/>
      </svg>
      <div class="dc-legend"></div>`);
    scr.appendChild(curves);
    dynUi.loud = curves.querySelector('.dc-loud');
    dynUi.bright = curves.querySelector('.dc-bright');
    dynUi.now = curves.querySelector('.dc-now');
    dynUi.legend = curves.querySelector('.dc-legend');
    p.appendChild(scr);
    knobRow(p, ['dynRange', 'dynBright', 'dynBrCurve', 'dynCurve', 'dynAttack', 'dynSmooth'], 'knob-md');
    knobRow(p, ['dynBreath', 'dynExpr', 'dynAT', 'dynMPE', 'dynVel', 'dynBreathCC'], 'knob-md');
    row1.appendChild(p);
  }
  function responseCurve(x, c) { return Math.abs(c) < 1e-3 ? x : Math.pow(x, Math.pow(2, 2 * c)); }
  function refreshDynCurves() {
    const N = 40, loud = [], bright = [];
    const range = values.dynRange, oct = values.dynBright;
    for (let i = 0; i <= N; i++) {
      const x = i / N, d = responseCurve(x, values.dynCurve);
      const db = -range * (1 - d) + (d < 0.05 ? 20 * Math.log10(Math.max(d / 0.05, 1e-3)) : 0);
      loud.push(`${(x * 100).toFixed(1)},${(58 - clamp((db + 48) / 48, 0, 1) * 56).toFixed(1)}`);
      bright.push(`${(x * 100).toFixed(1)},${(58 - responseCurve(d, values.dynBrCurve) * (oct / 8) * 56).toFixed(1)}`);
    }
    dynUi.loud.setAttribute('d', 'M' + loud.join(' L'));
    dynUi.bright.setAttribute('d', 'M' + bright.join(' L'));
    dynUi.legend.innerHTML = `LOUD ${Math.round(range)} dB · <b>BRIGHT +${oct.toFixed(1)} oct</b>`;
  }
  refreshers.push(refreshDynCurves);

  // ---- vibrato
  {
    const p = makePanel('VIBRATO');
    knobRow(p, ['vibDepth', 'vibAmp', 'vibRate', 'vibDelay'], 'knob-md');
    knobRow(p, ['vibFade', 'vibDrift', 'vibWheel'], 'knob-md');
    row2.appendChild(p);
  }

  // ---- voice / glide
  {
    const p = makePanel('VOICE · GLIDE', control(Segmented, 'voiceMode').el);
    const sw = el('div', 'switch-row left');
    sw.appendChild(control(ToggleButton, 'glideOn', 'GLIDE').el);
    sw.appendChild(control(Segmented, 'glideMode', ['ALWAYS', 'LEGATO']).el);
    p.appendChild(sw);
    knobRow(p, ['glideTime', 'glideCurve', 'slurDip', 'polyphony', 'bendRange'], 'knob-md');
    row2.appendChild(p);
  }

  // ---- output
  {
    const p = makePanel('OUTPUT');
    p.appendChild(el('div', 'screen voice-screen', `
      <div class="vs-row"><span class="vs-label">NOTES</span><span class="vs-value"><b id="voice-count">0</b>/<span id="voice-max">32</span></span></div>
      <div class="meter"><div id="meter-fill"></div><div class="meter-clip"></div></div>`));
    knobRow(p, ['master']);
    row2.appendChild(p);
  }
  refreshers.push(() => { $('#voice-max').textContent = values.voiceMode >= 0.5 ? 1 : Math.round(values.polyphony); });

  // ===================================================== OSCILLATORS page ===
  function shapePath(shape, pw, w, h) {
    const N = 200, pad = 5, pts = [];
    const i0 = Math.min(2, Math.floor(shape)), f = shape - i0;
    const wave = (i, t) => {
      if (i === 1) return 1 - 4 * Math.abs(((t + 0.25) % 1) - 0.5);
      if (i === 2) return 2 * t - 1;
      if (i === 3) return (t < pw ? 1 : -1) - (2 * pw - 1);
      return Math.sin(2 * Math.PI * t);
    };
    for (let i = 0; i <= N; i++) {
      const x = i / N, t = (x * 2) % 1;
      let y = wave(i0, t);
      if (f > 1e-6) y += (wave(i0 + 1, t) - y) * f;
      y = clamp(y, -1.6, 1.6) / 1.6;
      pts.push(`${(x * w).toFixed(1)},${(h / 2 - y * (h / 2 - pad)).toFixed(1)}`);
    }
    return 'M' + pts.join(' L');
  }

  const ENV_KNOBS = {
    amp: ['AmpDly', 'AmpAtk', 'AmpHld', 'AmpDec', 'AmpSus', 'AmpRel', 'AmpAtkCrv',
          'AmpDecCrv', 'AmpRelCrv', 'AmpVel', 'AmpVelTime', 'AmpKeyTime', 'AmpKeyAmt'],
    pitch: ['PEnvAmt', 'PEnvTime', 'PEnvCrv', 'BlipAmt', 'BlipTime', 'PEnvVel', 'PEnvKeyTime'],
    timbre: ['TEnvDly', 'TEnvAtk', 'TEnvDec', 'TEnvSus', 'TEnvRel', 'TEnvCrv', 'TEnvCut',
             'TEnvShape', 'TEnvVel', 'TEnvKeyTime'],
  };
  const graphCtx = {
    get: (id) => values[id],
    set: (id, v) => setParam(id, v),
    begin: gesture.begin,
    end: gesture.end,
  };
  const oscRow = $('#osc-row');
  const oscUi = [];
  for (let n = 1; n <= NUM_OSC; n++) {
    const o = `osc${n}`;
    const on = control(ToggleButton, `${o}On`);
    on.el.setAttribute('aria-label', `Oscillator ${n} on/off`);
    const panel = makePanel(`OSC ${n}`, on.el);
    const scr = el('div', 'screen', `<svg viewBox="0 0 200 48" preserveAspectRatio="none">
      <path class="scr-grid" d="M0 24 H200"/><path class="scr-line" d=""/></svg><div class="scr-text"></div>`);
    panel.appendChild(scr);
    const main = el('div', 'env-knobs');
    main.style.gridTemplateColumns = 'repeat(7, minmax(0, 1fr))';
    ['Shape', 'PW', 'Pitch', 'Fine', 'Level', 'Cut', 'Res'].forEach(f => addKnob(main, o + f, 'knob-md'));
    panel.appendChild(main);

    // envelope block: tabs + graph + that envelope's knobs
    const block = el('div', 'env-block');
    const tabs = el('div', 'env-tabs');
    const views = {};
    let activeEnv = store.get(`bk.env.${n}`) || 'amp';
    for (const kind of ['amp', 'pitch', 'timbre']) {
      const t = el('button', 'env-tab', `<span class="led"></span>${kind.toUpperCase()}`);
      t.type = 'button';
      t.addEventListener('click', () => { activeEnv = kind; store.set(`bk.env.${n}`, kind); showEnv(); });
      tabs.appendChild(t);
      const graph = new EnvGraph(o, kind, graphCtx);
      const kn = el('div', 'env-knobs');
      ENV_KNOBS[kind].forEach(f => addKnob(kn, o + f, 'knob-md'));
      kn.style.gridTemplateColumns = `repeat(${kind === 'amp' ? 7 : kind === 'pitch' ? 7 : 5}, minmax(0, 1fr))`;
      views[kind] = { tab: t, graph, knobs: kn, led: t.querySelector('.led') };
    }
    block.appendChild(tabs);
    const graphSlot = el('div');
    const knobSlot = el('div');
    block.append(graphSlot, knobSlot);
    panel.appendChild(block);
    oscRow.appendChild(panel);

    function showEnv() {
      for (const kind in views) views[kind].tab.classList.toggle('active', kind === activeEnv);
      graphSlot.replaceChildren(views[activeEnv].graph.el);
      knobSlot.replaceChildren(views[activeEnv].knobs);
      views[activeEnv].graph.render();
    }
    showEnv();
    oscUi.push({ o, panel, scr, views, get activeEnv() { return activeEnv; } });
  }
  refreshers.push((ids) => {
    for (const u of oscUi) {
      const o = u.o;
      if (ids && ![...ids].some(id => id.startsWith(o))) continue;
      const isOn = values[`${o}On`] >= 0.5;
      u.panel.classList.toggle('off', !isOn);
      u.scr.querySelector('.scr-line').setAttribute('d', shapePath(values[`${o}Shape`], values[`${o}PW`], 200, 48));
      u.scr.querySelector('.scr-text').textContent = isOn
        ? `${formatParam(PARAM_BY_ID[`${o}Shape`], values[`${o}Shape`]).toUpperCase()}  ${formatParam(PARAM_BY_ID[`${o}Pitch`], values[`${o}Pitch`])}`
        : 'OFF';
      u.views.amp.led.classList.toggle('on', isOn);
      u.views.pitch.led.classList.toggle('on', Math.abs(values[`${o}PEnvAmt`]) > 0.5 || Math.abs(values[`${o}BlipAmt`]) > 0.5);
      u.views.timbre.led.classList.toggle('on', Math.abs(values[`${o}TEnvCut`]) > 0.01 || Math.abs(values[`${o}TEnvShape`]) > 0.01);
      if (activePage === 'osc') u.views[u.activeEnv].graph.render();
    }
  });

  // ---- voice filter
  {
    const p = makePanel('VOICE FILTER', [control(Segmented, 'fltType').el, control(ToggleButton, 'fltOn').el]);
    p.querySelector('.panel-head').style.justifyContent = 'flex-start';
    p.querySelector('.panel-title').style.marginRight = 'auto';
    knobRow(p, ['fltCutoff', 'fltReso', 'fltKey', 'fltDyn', 'fltEnvAmt', 'fltAtk', 'fltDec', 'fltSus', 'fltRel', 'fltVel'], 'knob-md');
    $('#filter-row').appendChild(p);
    refreshers.push(() => p.classList.toggle('off', values.fltOn < 0.5));
  }

  // ======================================================= graph helpers ====
  const SR_UI = 48000;
  function makeGraph(parent, tall) {
    const g = el('div', 'graph' + (tall ? ' tall' : ''), '<svg preserveAspectRatio="none"></svg><div class="gr-legend"></div>');
    parent.appendChild(g);
    return { root: g, svg: g.querySelector('svg'), legend: g.querySelector('.gr-legend') };
  }
  function graphSize(g) { return [g.root.clientWidth || 300, g.root.clientHeight || 110]; }
  const touches = (ids, ...prefixes) => !ids || [...ids].some(id => prefixes.some(p => id.startsWith(p)));
  const logX = (f, W) => Math.log(f / 40) / Math.log(16000 / 40) * W;
  function responsePath(fn, W, H, lo, hi) {   // fn(f) -> dB; y range lo..hi dB
    const pts = [];
    for (let i = 0; i <= 120; i++) {
      const f = 40 * Math.pow(16000 / 40, i / 120);
      const y = H - 4 - (clamp(fn(f), lo, hi) - lo) / (hi - lo) * (H - 8);
      pts.push(`${(i / 120 * W).toFixed(1)},${y.toFixed(1)}`);
    }
    return 'M' + pts.join(' L');
  }
  function freqGrid(W, H) {
    return [100, 1000, 10000].map(f => `<path class="gr-grid" d="M${logX(f, W).toFixed(1)} 0 V${H}"/>
      <text class="gr-label" x="${(logX(f, W) + 3).toFixed(1)}" y="${H - 4}">${f >= 1000 ? f / 1000 + 'k' : f}</text>`).join('');
  }
  const centreNote = () => { const r = INSTRUMENT_RANGES[clamp(Math.round(values.instrument), 0, 9)]; return r[1] > 120 ? 60 : Math.round((r[0] + r[1]) / 2); };

  // ========================================================= HARMONICS page ===
  {
    const r1 = $('#harm-row-1'), r2 = $('#harm-row-2');
    const layer = makePanel('HARMONIC LAYER', control(ToggleButton, 'addOn').el);
    const spec = makeGraph(layer, true);
    const dynRow = el('div', 'dyn-slider', '<span>PREVIEW DYNAMICS</span><input type="range" min="0" max="1" step="0.01" value="0.9"><span class="dv">90%</span>');
    layer.appendChild(dynRow);
    const dynIn = dynRow.querySelector('input'), dynTxt = dynRow.querySelector('.dv');
    dynIn.addEventListener('input', () => { dynTxt.textContent = `${Math.round(dynIn.value * 100)}%`; drawSpectrum(); });
    knobRow(layer, ['addLevel', 'addPartials', 'addTilt', 'addOddEven', 'addDynSpread'], 'knob-md');
    r1.appendChild(layer);

    const sd = makePanel('SPECTRAL DELAY');
    const timing = makeGraph(sd, true);
    knobRow(sd, ['addSpecDly', 'addCurve'], 'knob-md');
    r1.appendChild(sd);

    const bandNames = [['Lo', 'LOW BAND · harmonics 1-4'], ['Mid', 'MID BAND · 4-14'], ['Hi', 'HIGH BAND · 14-32']];
    for (const [id, title] of bandNames) {
      const p = makePanel(title);
      knobRow(p, ['Atk', 'Dec', 'Sus', 'Rel', 'Lvl'].map(f => `add${id}${f}`), 'knob-md');
      r2.appendChild(p);
    }

    function drawSpectrum() {
      const [W, H] = graphSize(spec);
      const note = centreNote(), f0 = 440 * Math.pow(2, (note - 69) / 12);
      const soft = harmonicAmplitudes(values, f0, 0.15, SR_UI), now = harmonicAmplitudes(values, f0, Number(dynIn.value), SR_UI);
      const N = Math.max(soft.length, 1), bw = W / Math.max(N, 32);
      const y = (a) => H - 14 - clamp((20 * Math.log10(Math.max(a, 1e-6)) + 60) / 60, 0, 1) * (H - 22);
      let bars = '';
      for (let k = 0; k < now.length; k++) {
        const x = k * bw + bw * 0.15;
        bars += `<rect class="gr-bar-soft" x="${x.toFixed(1)}" y="${y(soft[k] || 1e-6).toFixed(1)}" width="${(bw * 0.32).toFixed(1)}" height="${(H - 14 - y(soft[k] || 1e-6)).toFixed(1)}"/>`;
        bars += `<rect class="gr-bar-loud" x="${(x + bw * 0.36).toFixed(1)}" y="${y(now[k]).toFixed(1)}" width="${(bw * 0.32).toFixed(1)}" height="${(H - 14 - y(now[k])).toFixed(1)}"/>`;
      }
      const labels = [1, 4, 8, 16, 32].filter(k => k <= N).map(k => `<text class="gr-label" x="${((k - 1) * bw + 1).toFixed(1)}" y="${H - 3}">${k}</text>`).join('');
      spec.svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
      spec.svg.innerHTML = [-20, -40].map(db => `<path class="gr-grid" d="M0 ${y(Math.pow(10, db / 20)).toFixed(1)} H${W}"/>`).join('') + bars + labels;
      spec.legend.innerHTML = `${midiNoteName(note)} · ${INSTRUMENTS[Math.round(values.instrument)]} · grey: soft · <b>brass: preview</b>`;
      layer.classList.toggle('off', values.addOn < 0.5);
    }

    // node envelope attacks over the first 400 ms (same timing maths as the engine)
    function drawTiming() {
      const [W, H] = graphSize(timing);
      const T = 0.4, nodes = [0, 3, 5, 7];   // harmonics 1, 4, 9, 21
      const cls = ['gr-line', 'gr-line mid', 'gr-line dim', 'gr-line dim'];
      let paths = '';
      nodes.forEach((j, idx) => {
        const x = Math.log2(ADD_NODE_HARMONIC[j]) / 5;
        const w0 = clamp(1 - x / 0.45, 0, 1), w2 = clamp((x - 0.55) / 0.45, 0, 1), w1 = 1 - w0 - w2;
        const b = (f) => w0 * values[`addLo${f}`] + w1 * values[`addMid${f}`] + w2 * values[`addHi${f}`];
        const dly = values.addSpecDly * 0.08 * Math.pow(x, 0.8);
        const atk = b('Atk') * (1 + 1.5 * values.addSpecDly * x), dec = b('Dec'), sus = b('Sus'), lvl = b('Lvl');
        const pts = [];
        for (let i = 0; i <= 80; i++) {
          const t = T * i / 80;
          let e = 0;
          if (t > dly) {
            const ta = t - dly;
            e = ta < atk ? curveShape(ta / atk, values.addCurve) : 1 + (sus - 1) * curveShape(Math.min((ta - atk) / dec, 1), values.addCurve);
          }
          pts.push(`${(t / T * W).toFixed(1)},${(H - 14 - e * lvl * (H - 24)).toFixed(1)}`);
        }
        paths += `<path class="${cls[idx]}" d="M${pts.join(' L')}"/>`;
      });
      const ticks = [0.1, 0.2, 0.3].map(t => `<path class="gr-grid" d="M${(t / T * W).toFixed(1)} 0 V${H}"/><text class="gr-label" x="${(t / T * W + 3).toFixed(1)}" y="${H - 3}">${t * 1000} ms</text>`).join('');
      timing.svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
      timing.svg.innerHTML = ticks + paths;
      timing.legend.innerHTML = '<b>harmonic 1</b> · 4 · 9 · 21';
    }
    refreshers.push((ids) => {
      if (activePage !== 'harm' && ids) return;
      if (touches(ids, 'add', 'instrument', 'dynBrCurve')) { drawSpectrum(); drawTiming(); }
    });
  }

  // ============================================================ TONE page ====
  let plungerNow = 0.5;
  const toneUi = {};
  {
    const row = $('#tone-row');
    const brs = makePanel('BRASSINESS', control(Segmented, 'brsType').el);
    toneUi.curve = makeGraph(brs);
    knobRow(brs, ['brsAmt', 'dynBrass', 'brsTone'], 'knob-md');
    const os = el('div', 'switch-row', '<span class="ctl-label">OVERSAMPLING</span>');
    os.appendChild(control(Segmented, 'oversample').el);
    brs.appendChild(os);
    row.appendChild(brs);

    const body = makePanel('BODY · BELL');
    toneUi.bodyTitle = body.querySelector('.panel-title');
    toneUi.body = makeGraph(body);
    knobRow(body, ['bodyMix', 'bodyShift', 'bodyReso'], 'knob-md');
    row.appendChild(body);

    const mute = makePanel('MUTE');
    mute.appendChild(control(Segmented, 'muteType').el);
    toneUi.mute = makeGraph(mute);
    const mk = el('div', 'knob-row');
    addKnob(mk, 'muteMix', 'knob-md');
    addKnob(mk, 'mutePlunger', 'knob-md');
    const srcF = field('Plunger on', control(SelectBox, 'muteSrc'));
    srcF.style.width = '110px';
    mk.appendChild(srcF);
    addKnob(mk, 'muteCC', 'knob-md');
    mute.appendChild(mk);
    toneUi.mutePanel = mute;
    row.appendChild(mute);
  }
  function drawTone(ids) {
    if (activePage !== 'tone' && ids) return;
    if (touches(ids, 'brs', 'dynBrass', 'dynBrCurve')) {
      const g = toneUi.curve, [W, H] = graphSize(g), type = Math.round(values.brsType);
      const pathFor = (d) => {
        const bc = Math.abs(values.dynBrCurve) < 1e-3 ? d : Math.pow(d, Math.pow(2, 2 * values.dynBrCurve));
        const drv = Math.pow(10, (values.brsAmt * 12 + values.dynBrass * bc * 12) / 20);
        const inv = 2 / (shapeFn(type, drv) - shapeFn(type, -drv));
        const pts = [];
        for (let i = 0; i <= 80; i++) {
          const x = -1 + 2 * i / 80, y = values.brsAmt > 1e-3 || values.dynBrass > 1e-3 ? shapeFn(type, x * drv) * inv : x;
          pts.push(`${(i / 80 * W).toFixed(1)},${(H / 2 - clamp(y, -1.3, 1.3) / 1.3 * (H / 2 - 4)).toFixed(1)}`);
        }
        return 'M' + pts.join(' L');
      };
      g.svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
      g.svg.innerHTML = `<path class="gr-zero" d="M0 ${H / 2} H${W} M${W / 2} 0 V${H}"/>
        <path class="gr-line dim" d="${pathFor(0.2)}"/><path class="gr-line" d="${pathFor(1)}"/>`;
      const drvLoud = values.brsAmt * 12 + values.dynBrass * 12;
      g.legend.innerHTML = `soft · <b>loud +${drvLoud.toFixed(1)} dB drive</b>`;
    }
    if (touches(ids, 'body', 'instrument')) {
      const g = toneUi.body, [W, H] = graphSize(g);
      g.svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
      g.svg.innerHTML = freqGrid(W, H) + `<path class="gr-zero" d="M0 ${H / 2} H${W}"/>
        <path class="gr-line" d="${responsePath(f => bodyResponseDb(values, f, SR_UI), W, H, -18, 18)}"/>`;
      g.legend.innerHTML = `<b>${INSTRUMENTS[Math.round(values.instrument)]}</b> body · ±18 dB`;
      toneUi.bodyTitle.textContent = `BODY · ${Math.round(values.instrument) >= 5 && Math.round(values.instrument) <= 8 ? 'SAX BODY' : 'BELL'}`;
    }
    if (touches(ids, 'mute', 'plunger')) {
      const g = toneUi.mute, [W, H] = graphSize(g), type = Math.round(values.muteType);
      const p = values.muteSrc < 0.5 ? values.mutePlunger : plungerNow;
      g.svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
      g.svg.innerHTML = freqGrid(W, H) + `<path class="gr-zero" d="M0 ${H / 2} H${W}"/>
        <path class="gr-line" d="${responsePath(f => muteResponseDb(values, f, SR_UI, p), W, H, -30, 18)}"/>`;
      g.legend.innerHTML = type === 4 ? `plunger <b>${Math.round(p * 100)}% open</b>` : `<b>${MUTE_TYPES[type]}</b>`;
      toneUi.mutePanel.classList.toggle('off', type === 0);
    }
  }
  refreshers.push(drawTone);

  // ======================================================== ATTACK page =====
  {
    const r1 = $('#attack-row-1'), r2 = $('#attack-row-2');
    const tg = makePanel('TONGUE');
    tg.appendChild(control(Segmented, 'tongue').el);
    tg.appendChild(el('div', 'kbd-hint', 'Soft: gentle onset, more scoop.<br>Hard: sharp, bright onset, a bigger blip, quicker attack.'));
    r1.appendChild(tg);
    const lip = makePanel('LIP BUZZ · REED ONSET');
    knobRow(lip, ['lipAmt', 'lipTone', 'lipDecay', 'lipPitch'], 'knob-md');
    r1.appendChild(lip);
    const br = makePanel('BREATH NOISE');
    knobRow(br, ['brNoise', 'brTone', 'brDyn'], 'knob-md');
    r1.appendChild(br);
    const gr = makePanel('GROWL');
    knobRow(gr, ['growlAmt', 'growlRate', 'growlRough', 'growlDyn'], 'knob-md');
    r2.appendChild(gr);
    const fl = makePanel('FLUTTER TONGUE');
    knobRow(fl, ['flutAmt', 'flutRate'], 'knob-md');
    r2.appendChild(fl);
    const keys = makePanel('SAX KEYS');
    knobRow(keys, ['keyClick', 'chiff'], 'knob-md');
    r2.appendChild(keys);
    refreshers.push(() => {
      gr.classList.toggle('off', values.growlAmt < 0.005);
      fl.classList.toggle('off', values.flutAmt < 0.005);
    });
  }

  // page LEDs: which of the extra pages are doing something
  refreshers.push(() => {
    $('#harm-led').classList.toggle('on', values.addOn >= 0.5);
    $('#tone-led').classList.toggle('on', Math.round(values.muteType) > 0 || values.brsAmt > 0.005 || values.dynBrass > 0.005);
    $('#attack-led').classList.toggle('on', values.growlAmt > 0.005 || values.flutAmt > 0.005);
  });

  // ---------------------------------------------------------- notes / UI ---
  const monitor = $('#midi-monitor');
  const midiLed = $('#midi-led');
  let ledTimer = 0;
  function logMidi(text) { monitor.textContent = text; }
  function flashLed() {
    midiLed.classList.add('on');
    clearTimeout(ledTimer);
    ledTimer = setTimeout(() => midiLed.classList.remove('on'), 90);
  }

  const piano = new Piano($('#piano'), {
    numKeys: 49,
    baseNote: 36,
    onNoteOn: (n, v) => noteOn(n, v),
    onNoteOff: (n) => noteOff(n),
  });
  function noteOn(note, vel) { engine.noteOn(note, vel); piano.setNote(note, true); }
  function noteOff(note) { engine.noteOff(note); piano.setNote(note, false); }

  // realistic range shading (a UI preference, remembered per machine)
  let rangeShow = store.get('bk.range') !== '0';
  const rangeBtn = $('#range-toggle');
  function refreshRange() {
    rangeBtn.classList.toggle('on', rangeShow);
    piano.setRange(rangeShow ? INSTRUMENT_RANGES[Math.round(values.instrument)] : null);
  }
  rangeBtn.addEventListener('click', () => { rangeShow = !rangeShow; store.set('bk.range', rangeShow ? '1' : '0'); refreshRange(); });
  refreshers.push((ids) => { if (!ids || ids.has('instrument')) refreshRange(); });

  engine.on('notes', ({ on, off }) => {
    off.forEach(n => piano.setNote(n, false));
    on.forEach(n => piano.setNote(n, true));
    flashLed();
    if (on.length) logMidi(`NOTE ON   ${on.map(n => `${midiNoteName(n)} (${n})`).join('  ')}`);
    else if (off.length) logMidi(`NOTE OFF  ${off.map(n => `${midiNoteName(n)} (${n})`).join('  ')}`);
  });

  function setOctave(base) {
    base = clamp(base, 0, 72);
    for (const [k, n] of heldKeys) { noteOff(n); heldKeys.delete(k); }
    piano.setBase(base);
    $('#octave-readout').textContent = `${midiNoteName(base)}–${midiNoteName(base + 48)}`;
    store.set('bk.octave', String(base));
  }
  $('#oct-down').addEventListener('click', () => setOctave(piano.base - 12));
  $('#oct-up').addEventListener('click', () => setOctave(piano.base + 12));

  // Computer keyboard: A = C of the second octave shown (the middle of the keyboard)
  const KEYMAP = { a: 0, w: 1, s: 2, e: 3, d: 4, f: 5, t: 6, g: 7, y: 8, h: 9, u: 10, j: 11,
                   k: 12, o: 13, l: 14, p: 15, ';': 16, "'": 17 };
  const heldKeys = new Map();
  window.addEventListener('keydown', (e) => {
    if (e.repeat || e.ctrlKey || e.metaKey || e.altKey) return;
    if (/^(INPUT|SELECT|TEXTAREA)$/.test(e.target.tagName) || !modal.classList.contains('hidden')) return;
    const key = e.key.toLowerCase();
    if (key === 'z') { setOctave(piano.base - 12); return; }
    if (key === 'x') { setOctave(piano.base + 12); return; }
    if (key in KEYMAP && !heldKeys.has(key)) {
      const note = piano.base + 12 + KEYMAP[key];
      heldKeys.set(key, note);
      noteOn(note, 100);
    }
  });
  window.addEventListener('keyup', (e) => {
    const key = e.key.toLowerCase();
    if (heldKeys.has(key)) { noteOff(heldKeys.get(key)); heldKeys.delete(key); }
  });
  window.addEventListener('blur', () => {
    for (const [, n] of heldKeys) noteOff(n);
    heldKeys.clear();
  });

  $('#panic-btn').addEventListener('click', () => {
    engine.panic();
    piano.clear();
    heldKeys.clear();
    logMidi('PANIC: all voices stopped');
  });

  // --------------------------------------------------------- wheels -------
  const wheel = new ModWheel((v) => engine.modWheel(v));
  $('#wheel-slot').replaceWith(wheel.el);
  let breathDragging = false;
  const breathWheel = new ModWheel((v) => engine.breath(v), (b) => { breathDragging = b; }, 'BREATH', 'Breath controller');
  breathWheel.el.title = 'On-screen breath controller: sends the Breath CC. Once used (or once a real breath controller '
    + 'is detected), breath sets the dynamics and zero breath is silence. Double-click: back to velocity-only playing.';
  $('#breath-slot').replaceWith(breathWheel.el);
  breathWheel.svg.addEventListener('dblclick', () => {
    engine.breath(-1);
    logMidi('Breath controller released: velocity sets the dynamics again');
  });

  // ------------------------------------------------------------- stats -----
  const meterFill = $('#meter-fill');
  const meterClip = document.querySelector('.meter-clip');
  const sustainLed = $('#sustain-led');
  const breathInd = $('#breath-ind'), breathMini = $('#breath-mini');
  let clipTimer = 0;
  function setBar(key, v, bipolar) {
    const u = dynUi[key];
    const present = v !== undefined && v !== null && v >= (bipolar ? -1 : 0);
    u.root.classList.toggle('absent', !present);
    u.fill.style.height = `${present ? clamp(bipolar ? Math.abs(v) : v, 0, 1) * 100 : 0}%`;
  }
  engine.on('stats', (s) => {
    $('#voice-count').textContent = s.voices;
    sustainLed.classList.toggle('on', !!s.sustain);
    if (typeof s.modWheel === 'number') wheel.setValue(s.modWheel);
    const breath = typeof s.breath === 'number' ? s.breath : -1;
    breathInd.classList.toggle('absent', breath < 0);
    breathMini.style.width = `${Math.max(0, breath) * 100}%`;
    if (breath >= 0 && !breathDragging) breathWheel.setValue(breath);
    setBar('breath', breath);
    setBar('expression', s.expression);
    setBar('aftertouch', s.aftertouch);
    setBar('bend', s.bend, true);
    setBar('dynamics', s.voices > 0 ? s.dynamics : 0);
    const dx = (clamp(s.voices > 0 ? s.dynamics : 0, 0, 1) * 100).toFixed(1);
    dynUi.now.setAttribute('d', s.voices > 0 ? `M${dx} 0 V60` : '');
    const db = 20 * Math.log10(Math.max(s.peak, 1e-5));
    meterFill.style.width = `${clamp((db + 60) / 60, 0, 1) * 100}%`;
    if (s.peak > 0.95) {
      meterClip.classList.add('on');
      clearTimeout(clipTimer);
      clipTimer = setTimeout(() => meterClip.classList.remove('on'), 800);
    }
    if (typeof s.bpm === 'number' && s.bpm > 0 && document.activeElement !== bpmInput) bpmInput.value = String(Math.round(s.bpm * 10) / 10);
    if (typeof s.plunger === 'number' && Math.abs(s.plunger - plungerNow) > 0.01) {
      plungerNow = s.plunger;
      if (Math.round(values.muteType) === 4 && values.muteSrc >= 0.5) drawTone(new Set(['plunger']));
    }
  });

  let connectedOnce = false;
  engine.on('config', ({ sampleRate, blockSize, wrapper, audio, running }) => {
    const web = wrapper === 'Web';
    document.body.classList.toggle('web', web);
    const sr = sampleRate > 0 ? `${(sampleRate / 1000).toFixed(1)} kHz · ${blockSize} smp` : 'not playing yet';
    $('#audio-info').textContent = audio || `${wrapper} · ${sr}`;
    $('#audio-led').classList.toggle('on', running !== false);
    if (!connectedOnce) logMidi('Connected: play your MIDI controller, the keys, or A–\' on the computer keyboard');
    connectedOnce = true;
    fit();
  });

  // ---- tempo + MIDI input (web build) --------------------------------------
  const bpmInput = $('#bpm-input');
  bpmInput.addEventListener('change', () => {
    const v = clamp(Number(bpmInput.value) || 120, 20, 300);
    bpmInput.value = String(v);
    engine.setTempo(v);
  });
  const midiSelect = $('#midi-select');
  engine.on('midiInputs', ({ inputs, selected, error }) => {
    midiSelect.innerHTML = '';
    if (error) { midiSelect.appendChild(new Option('MIDI unavailable', 'all')); midiSelect.disabled = true; return; }
    midiSelect.disabled = false;
    midiSelect.appendChild(new Option(inputs.length ? `All inputs (${inputs.length})` : 'No MIDI devices', 'all'));
    inputs.forEach(d => midiSelect.appendChild(new Option(d.name + (d.state === 'connected' ? '' : ' (off)'), d.id)));
    midiSelect.value = Array.from(midiSelect.options).some(o => o.value === selected) ? selected : 'all';
  });
  midiSelect.addEventListener('change', () => engine.selectMidi(midiSelect.value));

  // --------------------------------------------------------- fit window ----
  const synth = $('.synth');
  function fit() {
    const w = synth.offsetWidth + 24, h = synth.offsetHeight + 24;
    const s = Math.min(window.innerWidth / w, window.innerHeight / h);
    synth.style.transform = `translateX(-50%) scale(${s})`;
  }
  window.addEventListener('resize', fit);

  // Right-click on any control = the host's parameter menu (automation, link to controller...).
  // Elsewhere no browser menu; Shift+right-click still opens it (Inspect).
  document.addEventListener('contextmenu', (e) => {
    if (!engine.connected || e.shiftKey) return;
    e.preventDefault();
    const ctl = e.target.closest('[data-param]');
    if (ctl) engine.paramMenu(ctl.dataset.param, e.clientX, e.clientY);
  });

  // -------------------------------------------------------------- init -----
  renderPresetName();
  renderBrowser();
  showPage(activePage);
  setOctave(Number(store.get('bk.octave')) || 36);
  refresh(null);
  fit();
  if (engine.connected) engine.uiReady();
  else logMidi('Not running inside the plugin: no audio engine connected (UI preview only)');
})();
