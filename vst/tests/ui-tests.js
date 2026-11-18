// ============================================================================
//  Headless UI tests for the web build (run by tests/run-ui-tests.ps1, which
//  appends this script to a copy of web/index.html and loads it in headless
//  Edge). They drive the real UI (clicks, wheel, keys, dialogs) and check what
//  reaches the web host and the DSP (window.__bkWebHost taps the messages).
//  Results: JSON in a pre element (id bk-test-out); the title becomes "done".
//  Served over http (real-time run) they are also POSTed to the test server.
// ============================================================================
(function () {
  'use strict';
  const results = [];
  const check = (name, ok, detail) => results.push({ name, ok: !!ok, detail: detail === undefined ? '' : String(detail) });
  const sleep = (ms) => new Promise(r => setTimeout(r, ms));
  const $ = (s) => document.querySelector(s);
  const $$ = (s) => Array.from(document.querySelectorAll(s));
  const host = window.__bkWebHost;
  const sent = (type) => host.sent.filter(m => m.type === type);
  const lastSent = (type) => sent(type).slice(-1)[0];
  const clearSent = () => { host.sent.length = 0; };
  const ptr = (el, type, x, y, extra) => el.dispatchEvent(new PointerEvent(type, Object.assign({ bubbles: true, cancelable: true, pointerId: 1, isPrimary: true, clientX: x, clientY: y, button: 0, buttons: type === 'pointerup' ? 0 : 1 }, extra || {})));
  // File reads take real time, while headless Edge's virtual clock makes sleep() return at once:
  // read the same file ourselves (real I/O) a few times so the page's own read has finished.
  async function settleFileRead(file, done) {
    for (let i = 0; i < 20 && !done(); i++) { await file.text(); await sleep(20); }
  }
  const center = (el) => { const r = el.getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; };

  const messages = [];
  window.__JUCE__.backend.addEventListener('message', (m) => messages.push(m.text));

  async function run() {
    await sleep(300);
    for (let i = 0; i < 100 && !host.audioMode; i++) await sleep(50);   // the AudioWorklet module loads asynchronously
    host.tap = true;

    // ---- start-up ------------------------------------------------------------
    check('no script errors during start-up', window.__bkErrors.length === 0, window.__bkErrors.join(' | '));
    check('bridge connected (config received, web mode)', document.body.classList.contains('web'), document.body.className);
    check('audio started (ScriptProcessor on file://)', host.audioMode === 'ScriptProcessor' || host.audioMode === 'AudioWorklet', host.audioMode || 'none');
    check('120 factory presets in 12 categories', host.factory.length === 120 && new Set(host.factory.map(p => p.category)).size === 12,
          `${host.factory.length} presets`);
    check('starts on the first factory preset', $('#preset-name').textContent.startsWith(host.factory[0].name) && host.current.id === 'factory:0',
          $('#preset-name').textContent);

    // ---- every parameter has a control -----------------------------------------
    // (each oscillator's AMP / PITCH / TIMBRE envelope tabs swap their knobs in and out)
    const withControl = new Set($$('[data-param]').map(e => e.dataset.param));
    const envTabs = $$('.env-tab');
    for (const t of envTabs) { t.click(); $$('[data-param]').forEach(e => withControl.add(e.dataset.param)); }
    for (let i = 0; i < envTabs.length; i += 3) envTabs[i].click();   // back to AMP
    const missing = PARAM_DEFS.filter(d => !withControl.has(d.id)).map(d => d.id);
    check('every parameter has a control on some page', missing.length === 0, missing.join(', '));

    // ---- pages -----------------------------------------------------------------
    for (const tab of $$('.page-tab')) {
      tab.click();
      await sleep(30);
      const name = tab.dataset.page;
      const active = $$('.page.active').map(p => p.dataset.page);
      const page = $(`.page[data-page="${name}"]`);
      const visible = page && $$(`.page[data-page="${name}"] [data-param]`).filter(e => e.getClientRects().length > 0).length;
      check(`page ${name}: opens alone and shows its controls`, active.length === 1 && active[0] === name && visible > 0, `active=${active} visible controls=${visible}`);
    }

    // ---- a knob: wheel + double-click reach the host and the DSP -----------------------
    $('.page-tab[data-page="perform"]').click();
    await sleep(30);
    const knobEl = $('[data-param="vibDepth"]');
    const knobSvg = knobEl && knobEl.querySelector('svg');
    if (!knobSvg) check('vibDepth knob found', false);
    else {
      clearSent();
      const before = host.values.vibDepth;
      knobSvg.dispatchEvent(new WheelEvent('wheel', { deltaY: -100, bubbles: true, cancelable: true }));
      await sleep(30);
      const m = lastSent('param');
      check('knob wheel changes the value in the host and sends it to the DSP',
            host.values.vibDepth !== before && m && m.id === 'vibDepth' && m.value === host.values.vibDepth, `${before} -> ${host.values.vibDepth}`);
      knobSvg.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
      await sleep(30);
      check('knob double-click resets to the default', host.values.vibDepth === PARAM_BY_ID.vibDepth.def, host.values.vibDepth);
    }
    // a choice control: the instrument select
    const instSel = $('select[data-param="instrument"], [data-param="instrument"] select');
    if (instSel) {
      clearSent();
      instSel.value = '6';
      instSel.dispatchEvent(new Event('change', { bubbles: true }));
      await sleep(30);
      check('instrument select reaches the host', host.values.instrument === 6 && lastSent('param') && lastSent('param').id === 'instrument', host.values.instrument);
    } else check('instrument select found', false, 'no select for instrument');

    // ---- presets: step, browse, save, reload, delete, load file -----------------------
    clearSent();
    $('#preset-next').click();
    await sleep(60);
    const p1 = host.factory[1];
    const expectV = (id) => { const d = PARAM_BY_ID[id]; return p1.values[id] !== undefined ? p1.values[id] : d.def; };
    const ids = ['instrument', 'ampLevel', 'vibDepth', 'tongue', 'addOn'];
    check('next preset loads factory preset 2 into host, DSP and UI',
          host.current.id === 'factory:1' && ids.every(id => Math.abs(host.values[id] - expectV(id)) < 1e-6) && !!lastSent('params')
          && $('#preset-name').textContent.startsWith(p1.name), `${host.current.id} ${$('#preset-name').textContent}`);
    check('preset load resets parameters the preset does not set (vibDepth back from the edit)',
          Math.abs(host.values.vibDepth - expectV('vibDepth')) < 1e-6, host.values.vibDepth);

    // browse: open a category tab and click a preset
    const lastCat = host.factory[host.factory.length - 1].category;
    const catTab = $$('.cat-tab').find(t => t.textContent === lastCat);
    if (catTab) {
      catTab.click();
      await sleep(30);
      const btn = $$('.preset-btn').find(b => b.title === host.factory[host.factory.length - 1].name);
      if (btn) btn.click();
      await sleep(60);
      check('preset browser: category tab + preset button load it', host.current.id === `factory:${host.factory.length - 1}`, host.current.id);
    } else check('preset browser category tab found', false, lastCat);

    // save to this browser (no folder linked)
    try { localStorage.removeItem('bk.userPresets.v1'); } catch (e) { /* ignore */ }
    const savedLevel = host.values.ampLevel;
    $('#preset-save').click();
    await sleep(30);
    check('save opens the name dialog', !$('#modal').classList.contains('hidden'));
    $('#modal-input').value = 'UI Test Preset';
    $('#modal-ok').click();
    await sleep(100);
    let stored = null;
    try { stored = JSON.parse(JSON.parse(localStorage.getItem('bk.userPresets.v1'))['UI Test Preset']); } catch (e) { /* checked below */ }
    check('saved preset is a versioned .bkpreset document with every preset parameter',
          stored && stored.format === BK_PRESET_FORMAT && stored.version === BK_PRESET_VERSION
          && Object.keys(stored.params).length === PARAM_DEFS.filter(d => !d.noPreset).length && !('master' in stored.params),
          stored ? `${stored.format} v${stored.version}, ${Object.keys(stored.params).length} params` : 'not stored');
    check('saved preset becomes the current preset', host.current.id === 'local:UI Test Preset', host.current.id);

    // load something else, then the saved one back from the User tab
    $('#preset-prev').click();
    await sleep(60);
    const userTab = $$('.cat-tab').find(t => t.textContent === 'User');
    if (userTab) {
      userTab.click();
      await sleep(30);
      const btn = $$('.preset-btn').find(b => b.title === 'UI Test Preset');
      if (btn) btn.click();
      await sleep(100);
      check('user preset loads back with its values', host.current.id === 'local:UI Test Preset' && host.values.ampLevel === savedLevel,
            `${host.current.id} ampLevel ${host.values.ampLevel}`);
      // save again under the same name: asks to replace
      $('#preset-save').click(); await sleep(30);
      $('#modal-input').value = 'UI Test Preset'; $('#modal-ok').click(); await sleep(100);
      check('saving over an existing name asks to replace', !$('#modal').classList.contains('hidden') && /Replace/i.test($('#modal-title').textContent),
            $('#modal-title').textContent);
      $('#modal-ok').click(); await sleep(100);
      // delete
      const del = $$('.preset-btn').find(b => b.title === 'UI Test Preset');
      const x = del && del.querySelector('.pb-del');
      if (x) { x.click(); await sleep(30); $('#modal-ok').click(); await sleep(100); }
      check('user preset can be deleted', !$$('.preset-btn').some(b => b.title === 'UI Test Preset')
            && !Object.keys(JSON.parse(localStorage.getItem('bk.userPresets.v1') || '{}')).length, 'still listed');
    } else check('User category appears after saving', false);

    // LOAD: a .bkpreset file (older version, unknown + out-of-range params) through the file input
    const fileInput = $$('input[type="file"]').find(i => i.accept.includes(BK_PRESET_EXT));
    if (fileInput) {
      const doc = { format: BK_PRESET_FORMAT, version: 0, name: 'From Disk', params: { instrument: 7, vibDepth: 33, dynRange: 999, notAParam: 5 } };
      const dt = new DataTransfer();
      const f1 = new File([JSON.stringify(doc)], 'From Disk.bkpreset', { type: 'application/json' });
      dt.items.add(f1);
      fileInput.files = dt.files;
      fileInput.dispatchEvent(new Event('change'));
      await settleFileRead(f1, () => host.current.name === 'From Disk');
      check('preset file loads (migrated, unknown params ignored, values clamped, the rest default)',
            host.current.name === 'From Disk' && host.values.instrument === 7 && host.values.vibDepth === 33
            && host.values.dynRange === PARAM_BY_ID.dynRange.max && host.values.tongue === PARAM_BY_ID.tongue.def,
            `${host.current.name} inst=${host.values.instrument} vib=${host.values.vibDepth} range=${host.values.dynRange}`);
      const dt2 = new DataTransfer();
      const f2 = new File(['{"hello": 1}'], 'junk.bkpreset');
      dt2.items.add(f2);
      fileInput.files = dt2.files;
      const nameBefore = host.current.name;
      fileInput.dispatchEvent(new Event('change'));
      await settleFileRead(f2, () => false);
      check('a file without Brass Knuckles settings is refused', host.current.name === nameBefore);
    } else check('preset file input found', false);

    // ---- playing: on-screen keys, computer keys, articulations, wheels -----------------
    $('.page-tab[data-page="play"]').click();
    await sleep(30);
    clearSent();
    const key = $$('.piano [data-idx], svg [data-idx]')[12];
    if (key) {
      const [x, y] = center(key);
      ptr(key, 'pointerdown', x, y);
      await sleep(30);
      ptr(key, 'pointerup', x, y);
      await sleep(30);
      const midi = sent('midi').map(m => m.data);
      check('on-screen key sends note on + note off (MIDI channel 1)',
            midi.length >= 2 && midi[0][0] === 0x90 && midi[0][2] >= 1 && midi[1][0] === 0x80 && midi[1][1] === midi[0][1], JSON.stringify(midi));
    } else check('piano key found', false);
    clearSent();
    window.dispatchEvent(new KeyboardEvent('keydown', { key: 'a', code: 'KeyA', bubbles: true }));
    await sleep(20);
    window.dispatchEvent(new KeyboardEvent('keyup', { key: 'a', code: 'KeyA', bubbles: true }));
    await sleep(20);
    const km = sent('midi').map(m => m.data);
    check('computer key A plays a note', km.length >= 2 && km[0][0] === 0x90 && km[1][0] === 0x80, JSON.stringify(km));

    const artBtns = $$('.art-btn');
    if (artBtns.length === 6) {
      clearSent();
      artBtns[3].click();
      await sleep(20);
      const a = lastSent('articulation');
      check('articulation button latches it in the DSP', a && a.index === 3, JSON.stringify(a));
      artBtns[0].click();
    } else check('six articulation buttons', false, artBtns.length);

    const breathSvg = $$('[title^="On-screen breath controller"] svg, [title^="On-screen breath controller"]').find(e => e.tagName.toLowerCase() === 'svg');
    if (breathSvg) {
      clearSent();
      breathSvg.dispatchEvent(new MouseEvent('dblclick', { bubbles: true }));
      await sleep(20);
      const b = lastSent('breath');
      check('BREATH wheel double-click forgets the breath controller', b && b.value < 0, JSON.stringify(b));
    } else check('BREATH wheel found', false);

    clearSent();
    $('#panic') && $('#panic').click();
    await sleep(20);
    check('PANIC reaches the DSP', !$('#panic') || !!lastSent('panic'));

    // ---- the DSP itself renders this page's current sound ------------------------------
    {
      let Proc = null;
      class AudioWorkletProcessor { constructor() { this.port = { postMessage() {}, onmessage: null }; } }
      new Function('AudioWorkletProcessor', 'registerProcessor', 'sampleRate', `(${synthWorkletModule.toString()})();`)(
        AudioWorkletProcessor, (n, c) => { Proc = c; }, 48000);
      const p = new Proc({ processorOptions: { params: Object.assign({}, host.values, PARAM_DEFS.reduce((o, d) => (d.noPreset ? Object.assign(o, { [d.id]: d.def }) : o), {})) } });
      const L = new Float32Array(128), R = new Float32Array(128);
      let e = 0, bad = 0;
      p.port.onmessage({ data: { type: 'midi', data: [0x90, 64, 100] } });
      for (let b = 0; b < 200; b++) {
        p.process([], [[L, R]]);
        for (let i = 0; i < 128; i++) { e += L[i] * L[i] + R[i] * R[i]; if (!isFinite(L[i]) || !isFinite(R[i])) bad++; }
      }
      check('the page\'s DSP renders the loaded sound (audible, finite)', e > 1e-3 && bad === 0, `energy ${e.toExponential(2)}, non-finite ${bad}`);
    }

    // ---- served over http: the AudioWorklet runs and renders in real time ---------------
    if (location.protocol.startsWith('http')) {
      check('served over http the engine runs in an AudioWorklet', host.audioMode === 'AudioWorklet', host.audioMode);
      let heard = null, nStats = 0, maxPeak = 0;
      window.__JUCE__.backend.addEventListener('stats', (s) => { nStats++; maxPeak = Math.max(maxPeak, s.peak || 0); if (s.voices > 0 && s.peak > 0.001) heard = s; });
      document.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));   // "click anywhere" to unlock audio
      const sp = window.__JUCE__.backend;
      sp.emitEvent('noteOn', { note: 64, vel: 100 });
      // real time must pass for the audio thread: network round trips are real, timers are virtual here
      for (let i = 0; i < 60 && !heard; i++) await fetch(`ping?${i}`).catch(() => {});
      sp.emitEvent('noteOff', { note: 64 });
      check('the AudioWorklet plays a note (stats report a voice and level)', !!heard, heard ? `voices ${heard.voices}, peak ${heard.peak.toFixed(3)}` : `no stats with sound (${nStats} stats, max peak ${maxPeak}, audio: ${document.getElementById('audio-info').textContent}; messages: ${messages.join(' / ')})`);
    }

    check('no script errors during the tests', window.__bkErrors.length === 0, window.__bkErrors.join(' | '));
    host.tap = false;
  }

  window.addEventListener('load', () => {
    run().catch((e) => check('test script crashed', false, e.stack || e)).finally(() => {
      const out = document.createElement('pre');
      out.id = 'bk-test-out';
      out.textContent = JSON.stringify(results);
      document.body.appendChild(out);
      document.title = 'done';
      // served (real-time run): hand the results to the test server
      if (location.protocol.startsWith('http')) fetch('result', { method: 'POST', body: out.textContent }).catch(() => {});
    });
  });
})();
