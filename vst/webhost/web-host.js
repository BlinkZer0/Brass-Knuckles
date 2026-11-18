// ============================================================================
//  Web host: stands in for the plugin's C++ side when the UI runs in a
//  browser. It provides window.__JUCE__.backend with exactly the events the
//  JUCE editor uses (see PluginEditor.cpp), so vst/ui runs unchanged:
//    audio   Web Audio: AudioWorklet (or ScriptProcessor fallback on file://)
//            running dsp-worklet.js, the JS twin of SynthEngine.h
//    MIDI    Web MIDI (all inputs or one); every channel message goes to the
//            engine with the plugin's routing (notes, CCs incl. breath, bend,
//            aftertouch, poly pressure, MPE)
//    tempo   user-set BPM (the plugin uses the host's)
//    presets factory (FACTORY_JSON via factory.js), this browser (localStorage),
//            or a linked folder on disk (File System Access API), same
//            .bkpreset format as the plugin, rescanned every 2 s
//  Needs params.js, factory.js, dsp-worklet.js and FACTORY_JSON loaded first.
// ============================================================================
(function () {
  'use strict';

  // ---------------------------------------------------------- the bridge ---
  const listeners = {};
  const handlers = {};
  const toUi = (ev, data) => queueMicrotask(() => (listeners[ev] || []).forEach(f => f(data)));
  window.__JUCE__ = {
    backend: {
      addEventListener(ev, fn) { (listeners[ev] = listeners[ev] || []).push(fn); },
      // handled synchronously so file pickers / permission prompts keep the click's user activation
      emitEvent(ev, data) { const h = handlers[ev]; if (h) h(data || {}); },
    },
  };
  const message = (text) => toUi('message', { text });
  const store = {
    get(k) { try { return localStorage.getItem(k); } catch (e) { return null; } },
    set(k, v) { try { localStorage.setItem(k, v); return true; } catch (e) { return false; } },
  };

  // ------------------------------------------------------ factory presets ---
  const FACTORY = bkFactoryPresets(FACTORY_JSON).map((p, i) =>
    ({ id: `factory:${i}`, name: p.name, category: p.category, factory: true, values: p.values }));

  // ---------------------------------------------------------------- state ---
  const values = defaultParamValues();
  let current = { id: '', name: '', category: '' };
  let tempo = clamp(Number(store.get('bk.tempo')) || 120, 20, 300);

  // Every preset parameter: the preset's value, else the default (like PluginProcessor::applyPreset).
  // Master is never part of a preset. Stepped parameters snap like the plugin's int / choice params.
  function presetValues(pv) {
    const out = {};
    for (const d of PARAM_DEFS) {
      if (d.noPreset) continue;
      let v = Number(pv[d.id]);
      if (pv[d.id] === undefined || pv[d.id] === null || isNaN(v)) v = d.def;
      v = clamp(v, d.min, d.max);
      if (d.step) v = Math.round(v / d.step) * d.step;
      out[d.id] = v;
    }
    return out;
  }
  function applyPreset(pv, info) {
    const all = presetValues(pv);
    Object.assign(values, all);
    audio.post({ type: 'params', values: all });
    current = info;
    toUi('params', Object.assign({}, values));
    toUi('preset', current);
  }

  // a fresh instance starts on the first factory preset, like the plugin
  Object.assign(values, presetValues(FACTORY[0].values));
  current = { id: FACTORY[0].id, name: FACTORY[0].name, category: FACTORY[0].category };

  // ---------------------------------------------------------------- audio ---
  const audio = {
    ctx: null, node: null, port: null, mode: '', error: '',
    post(m) { if (this.port) this.port.postMessage(m); },
    get running() { return !!this.ctx && this.ctx.state === 'running'; },

    async start() {
      const ctx = new (window.AudioContext || window.webkitAudioContext)({ latencyHint: 'interactive' });
      this.ctx = ctx;
      ctx.onstatechange = sendConfig;
      const src = `(${synthWorkletModule.toString()})();`;
      let built = null;
      if (ctx.audioWorklet && location.protocol !== 'file:') {
        try {
          const url = URL.createObjectURL(new Blob([src], { type: 'application/javascript' }));
          try { await ctx.audioWorklet.addModule(url); } finally { URL.revokeObjectURL(url); }
          const node = new AudioWorkletNode(ctx, 'brass-knuckles', {
            numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
            processorOptions: { params: values },
          });
          node.port.onmessage = (e) => onDspMessage(e.data);
          // an exception inside the audio thread silences the node: say so instead of failing quietly
          node.onprocessorerror = (e) => {
            audio.error = 'the audio engine stopped (processor error); reload the page';
            console.error('AudioWorklet processor error', e);
            message(`Audio engine error: ${(e && e.message) || 'processor error'}. Reload the page.`);
            sendConfig();
          };
          built = { node, port: node.port, mode: 'AudioWorklet' };
        } catch (e) { console.warn('AudioWorklet unavailable, using ScriptProcessor', e); }
      }
      if (!built) built = this.scriptProcessor(ctx, src);
      this.node = built.node; this.port = built.port; this.mode = built.mode;
      this.node.connect(ctx.destination);
      this.post({ type: 'tempo', bpm: tempo });
      this.post({ type: 'params', values: Object.assign({}, values) }); // anything changed while starting
      sendConfig();
    },

    // Chrome/Edge block worklet modules on file://: run the same DSP on the main thread
    scriptProcessor(ctx, src) {
      class AudioWorkletProcessorShim { constructor() { this.port = { postMessage: onDspMessage, onmessage: null }; } }
      let Processor = null;
      new Function('AudioWorkletProcessor', 'registerProcessor', 'sampleRate', src)(
        AudioWorkletProcessorShim, (name, cls) => { Processor = cls; }, ctx.sampleRate);
      const proc = new Processor({ processorOptions: { params: values } });
      const L128 = new Float32Array(128), R128 = new Float32Array(128);
      const node = ctx.createScriptProcessor(1024, 0, 2);
      node.onaudioprocess = (e) => {
        const L = e.outputBuffer.getChannelData(0), R = e.outputBuffer.getChannelData(1);
        for (let off = 0; off < L.length; off += 128) {
          proc.process([], [[L128, R128]]);
          L.set(L128, off); R.set(R128, off);
        }
      };
      return { node, port: { postMessage: (m) => proc.port.onmessage({ data: m }) }, mode: 'ScriptProcessor' };
    },
  };

  function onDspMessage(m) { if (m.type === 'stats') toUi('stats', m); }

  function sendConfig() {
    const c = audio.ctx;
    let text;
    if (audio.error) text = `Audio error: ${audio.error}`;
    else if (!audio.running) text = 'Click anywhere to enable audio';
    else text = `Web · ${audio.mode} · ${(c.sampleRate / 1000).toFixed(1)} kHz · ~${(((c.baseLatency || 0) + (c.outputLatency || 0)) * 1000).toFixed(1)} ms`;
    toUi('config', { wrapper: 'Web', sampleRate: c ? c.sampleRate : 0, blockSize: 128, voices: MAX_POLY, audio: text, running: audio.running });
  }

  const started = audio.start().catch((e) => { audio.error = e.message || String(e); console.error(e); sendConfig(); });
  function unlockAudio() {
    if (audio.running || audio.error) return;
    started.then(() => audio.ctx && audio.ctx.resume()).then(sendConfig).catch(() => {});
  }
  document.addEventListener('pointerdown', unlockAudio, true);
  document.addEventListener('keydown', unlockAudio, true);

  // ---------------------------------------------------------------- notes ---
  // Sounding notes, for the on-screen keyboard (the plugin tracks them per channel the same way).
  const sounding = new Set();
  function noteShown(note, on) {
    if (on) { sounding.add(note); toUi('notes', { on: [note], off: [] }); }
    else if (sounding.delete(note)) toUi('notes', { on: [], off: [note] });
  }
  function allShownOff() {
    const off = [...sounding];
    sounding.clear();
    if (off.length) toUi('notes', { on: [], off });
  }
  function sendMidi(data) {
    if (!audio.running && (data[0] & 0xf0) === 0x90 && data[2] > 0) message('Audio is off: click anywhere on the page once to enable it');
    audio.post({ type: 'midi', data });
  }

  // ----------------------------------------------------------------- MIDI ---
  const midi = { access: null, selected: store.get('bk.midiInput') || 'all' };
  function midiInputs() {
    return midi.access ? Array.from(midi.access.inputs.values()).map(i => ({ id: i.id, name: i.name, state: i.state })) : [];
  }
  function bindMidi() {
    if (!midi.access) return;
    for (const input of midi.access.inputs.values()) {
      const on = midi.selected === 'all' || midi.selected === input.id;
      input.onmidimessage = on ? (e) => onMidi(e.data) : null;
    }
  }
  function onMidi(d) {
    const st = d[0];
    if (st >= 0xf0 || st < 0x80) return;   // no SysEx / clock / running status
    const cmd = st & 0xf0;
    sendMidi(Array.from(d.slice(0, cmd === 0xc0 || cmd === 0xd0 ? 2 : 3)));
    if (cmd === 0x90 && d[2] > 0) noteShown(d[1], true);
    else if (cmd === 0x80 || cmd === 0x90) noteShown(d[1], false);
    else if (cmd === 0xb0 && (d[1] === 120 || d[1] === 123)) allShownOff();
  }
  if (typeof navigator.requestMIDIAccess === 'function') {
    navigator.requestMIDIAccess({ sysex: false }).then((access) => {
      midi.access = access;
      access.onstatechange = () => { bindMidi(); toUi('midiInputs', { inputs: midiInputs(), selected: midi.selected }); };
      bindMidi();
      toUi('midiInputs', { inputs: midiInputs(), selected: midi.selected });
      const n = midiInputs().length;
      message(n ? `MIDI ready: ${n} input(s): ${midiInputs().map(i => i.name).join(', ')}` : 'MIDI ready: no devices found (plug one in, it appears automatically)');
    }).catch((e) => {
      toUi('midiInputs', { inputs: [], selected: 'all', error: true });
      message(`MIDI unavailable: ${e.message || e}`);
    });
  } else {
    queueMicrotask(() => toUi('midiInputs', { inputs: [], selected: 'all', error: true }));
  }

  // -------------------------------------------------------------- presets ---
  const LS_KEY = 'bk.userPresets.v1';
  const local = {
    read() { try { return JSON.parse(store.get(LS_KEY)) || {}; } catch (e) { return {}; } },
    write(obj) { return store.set(LS_KEY, JSON.stringify(obj)); },
  };
  const localPresets = () => Object.keys(local.read()).sort((a, b) => a.localeCompare(b, undefined, { numeric: true }))
    .map(name => ({ id: `local:${name}`, name, category: 'User', factory: false }));

  const fsSupported = typeof window.showDirectoryPicker === 'function';
  const folder = { handle: null, granted: false, files: [], sig: '' };
  const idb = {
    open() {
      return new Promise((res, rej) => {
        const r = indexedDB.open('brass-knuckles', 1);
        r.onupgradeneeded = () => r.result.createObjectStore('kv');
        r.onsuccess = () => res(r.result);
        r.onerror = () => rej(r.error);
      });
    },
    async get(key) {
      const db = await this.open();
      return new Promise((res) => { const r = db.transaction('kv').objectStore('kv').get(key); r.onsuccess = () => res(r.result); r.onerror = () => res(undefined); });
    },
    async set(key, val) {
      const db = await this.open();
      return new Promise((res) => { const tx = db.transaction('kv', 'readwrite'); tx.objectStore('kv').put(val, key); tx.oncomplete = () => res(true); tx.onerror = () => res(false); });
    },
  };

  async function scanFolder() {
    if (!folder.handle || !folder.granted) return false;
    const files = [];
    async function walk(dir, path) {
      for await (const [name, h] of dir.entries()) {
        const rel = path ? `${path}/${name}` : name;
        if (h.kind === 'directory') await walk(h, rel);
        else if (name.toLowerCase().endsWith(BK_PRESET_EXT)) {
          const f = await h.getFile();
          files.push({ id: `user:${rel}`, name: name.slice(0, -BK_PRESET_EXT.length), category: path || 'User',
                       factory: false, handle: h, parent: dir, fileName: name, stamp: `${f.size}:${f.lastModified}` });
        }
      }
    }
    try { await walk(folder.handle, ''); } catch (e) { folder.granted = false; return true; }
    files.sort((a, b) => a.category.localeCompare(b.category, undefined, { numeric: true }) || a.name.localeCompare(b.name, undefined, { numeric: true }));
    const sig = files.map(f => `${f.id}|${f.stamp}`).join(';');
    if (sig === folder.sig) return false;
    folder.sig = sig;
    folder.files = files;
    return true;
  }

  function allPresets() { return [...FACTORY, ...folder.files, ...localPresets()]; }
  function sendPresetList() {
    toUi('presetList', {
      presets: allPresets().map(({ id, name, category, factory }) => ({ id, name, category, factory })),
      folder: folder.granted ? `${folder.handle.name} (linked folder)` : 'this browser: click FOLDER to link a folder on disk',
    });
  }

  async function linkFolder(handle, ask) {
    folder.handle = handle;
    folder.sig = '';
    let state = await handle.queryPermission({ mode: 'readwrite' });
    if (state !== 'granted' && ask) state = await handle.requestPermission({ mode: 'readwrite' });
    folder.granted = state === 'granted';
    await idb.set('presetFolder', handle);
    if (folder.granted) await scanFolder();
    sendPresetList();
    return folder.granted;
  }
  if (fsSupported && window.indexedDB) {
    idb.get('presetFolder').then(async (h) => {
      if (h && !(await linkFolder(h, false))) message(`Click FOLDER to reconnect the preset folder "${h.name}"`);
    }).catch(() => {});
  }
  setInterval(async () => { if (await scanFolder()) sendPresetList(); }, 2000); // autoload, like the plugin

  // Same rules as PresetManager::readJson: migrate, then at least one known parameter.
  function parsePresetText(text, fallbackName) {
    let json;
    try { json = JSON.parse(text); } catch (e) { throw new Error(`Not a valid preset file: ${fallbackName}`); }
    if (!json || typeof json !== 'object') throw new Error(`Not a Brass Knuckles preset: ${fallbackName}`);
    bkMigratePreset(json);
    const params = {};
    if (bkApplyParamObject(json.params, params) === 0) throw new Error(`No Brass Knuckles settings found in ${fallbackName}`);
    return { name: String(json.name || '').trim() || fallbackName, category: String(json.category || ''), params };
  }
  function presetText(name, category) {
    const params = {};
    for (const d of PARAM_DEFS) if (!d.noPreset) params[d.id] = +Number(values[d.id]).toPrecision(6);
    return JSON.stringify({ format: BK_PRESET_FORMAT, version: BK_PRESET_VERSION, name, category, params }, null, 2);
  }
  const legalName = (s) => s.trim().replace(/[\\/:*?"<>|]/g, '_').slice(0, 60);

  async function loadPresetById(id) {
    const p = allPresets().find(x => x.id === id);
    if (!p) { message('Preset not found (was it moved or deleted?)'); toUi('preset', current); return; }
    try {
      const info = { id, name: p.name, category: p.category };
      if (p.factory) applyPreset(p.values, info);
      else {
        const text = id.startsWith('local:') ? local.read()[p.name] : await (await p.handle.getFile()).text();
        applyPreset(parsePresetText(text, p.name).params, info);
      }
    } catch (e) { message(e.message || String(e)); toUi('preset', current); }
  }

  async function savePreset(rawName, overwrite) {
    const name = legalName(rawName);
    const result = (r) => toUi('saveResult', Object.assign({ name, ok: false, exists: false, error: '' }, r));
    if (!name) return result({ error: 'Enter a name for the preset' });
    try {
      if (folder.granted) {
        const fileName = name + BK_PRESET_EXT;
        if (!overwrite) {
          let exists = true;
          try { await folder.handle.getFileHandle(fileName); } catch (e) { exists = false; }
          if (exists) return result({ exists: true });
        }
        const w = await (await folder.handle.getFileHandle(fileName, { create: true })).createWritable();
        await w.write(presetText(name, 'User'));
        await w.close();
        await scanFolder();
        current = { id: `user:${fileName}`, name, category: 'User' };
      } else {
        const all = local.read();
        if (!overwrite && all[name]) return result({ exists: true });
        all[name] = presetText(name, 'User');
        if (!local.write(all)) throw new Error('browser storage is unavailable (private window?)');
        current = { id: `local:${name}`, name, category: 'User' };
      }
      sendPresetList();
      toUi('preset', current);
      result({ ok: true });
    } catch (e) { result({ error: e.message || String(e) }); }
  }

  async function deletePreset(id) {
    const p = allPresets().find(x => x.id === id);
    try {
      if (!p || p.factory) throw new Error('Only user presets can be deleted');
      if (id.startsWith('local:')) { const all = local.read(); delete all[p.name]; local.write(all); }
      else { await p.parent.removeEntry(p.fileName); await scanFolder(); }
      if (current.id === id) current = { id: '', name: current.name, category: 'User' };
      message(`Deleted preset "${p.name}"`);
    } catch (e) { message(e.message || String(e)); }
    sendPresetList();
    toUi('preset', current);
  }

  const fileInput = document.createElement('input');
  fileInput.type = 'file';
  fileInput.accept = `${BK_PRESET_EXT},application/json`;
  fileInput.style.display = 'none';
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', () => document.body.appendChild(fileInput));
  else document.body.appendChild(fileInput);   // dev.html loads this after the document is ready
  fileInput.addEventListener('change', async () => {
    const f = fileInput.files && fileInput.files[0];
    fileInput.value = '';
    if (!f) return;
    try {
      const p = parsePresetText(await f.text(), f.name.replace(/\.[^.]+$/, ''));
      applyPreset(p.params, { id: `file:${f.name}`, name: p.name, category: 'File' });
      message(`Loaded ${f.name}`);
    } catch (e) { message(e.message || String(e)); }
  });

  async function folderButton() {
    if (!fsSupported) { message('Linking a preset folder needs Chrome or Edge: presets are saved in this browser instead.'); return; }
    try {
      if (folder.handle && !folder.granted) { if (await linkFolder(folder.handle, true)) message(`Reconnected "${folder.handle.name}"`); return; }
      const handle = await window.showDirectoryPicker({ id: 'bk-presets', mode: 'readwrite', startIn: 'documents' });
      await linkFolder(handle, true);
      message(`Preset folder linked: ${handle.name}, ${folder.files.length} preset(s). Link Documents\\Brass Knuckles\\Presets to share presets with the VST.`);
    } catch (e) { if (e && e.name !== 'AbortError') message(`Folder: ${e.message || e}`); }
  }

  // ------------------------------------------------- UI -> host events ---
  Object.assign(handlers, {
    uiReady() {
      sendConfig();
      sendPresetList();
      toUi('params', Object.assign({}, values));
      toUi('preset', current);
      toUi('stats', { voices: 0, peak: 0, sustain: false, modWheel: 0, bpm: tempo, breath: -1, expression: -1, aftertouch: -1,
                      dynamics: 0, bend: 0, plunger: values.mutePlunger, articulation: 0 });
      if (midi.access) toUi('midiInputs', { inputs: midiInputs(), selected: midi.selected });
    },
    paramMenu() {}, // no host automation menu in a browser
    setParam({ id, value }) {
      const d = PARAM_BY_ID[id];
      if (!d) return;
      let v = clamp(Number(value), d.min, d.max);
      if (d.step) v = Math.round(v / d.step) * d.step;
      values[id] = v;
      audio.post({ type: 'param', id, value: v });
    },
    gesture() {},
    // the on-screen keyboard plays MIDI channel 1, like the plugin's keyboard state
    noteOn({ note, vel }) { sendMidi([0x90, clamp(note | 0, 0, 127), clamp(vel | 0, 1, 127)]); noteShown(note | 0, true); },
    noteOff({ note }) { sendMidi([0x80, clamp(note | 0, 0, 127), 0]); noteShown(note | 0, false); },
    panic() { audio.post({ type: 'panic' }); allShownOff(); },
    modWheel({ value }) { audio.post({ type: 'modWheel', value: clamp(Number(value) || 0, 0, 1) }); },
    breath({ value }) { audio.post({ type: 'breath', value: Number(value) }); },
    articulation({ index }) { audio.post({ type: 'articulation', index: clamp(index | 0, 0, 5) }); },
    setTempo({ bpm }) { tempo = clamp(Number(bpm) || 120, 20, 300); store.set('bk.tempo', String(tempo)); audio.post({ type: 'tempo', bpm: tempo }); },
    selectMidi({ id }) { midi.selected = id; store.set('bk.midiInput', id); bindMidi(); },
    loadPreset({ id }) { loadPresetById(id); },
    savePreset({ name, overwrite }) { savePreset(String(name || ''), overwrite); },
    deletePreset({ id }) { deletePreset(id); },
    loadPresetFile() { fileInput.click(); },
    openPresetFolder() { folderButton(); },
  });

  // Test hook (tests/run-ui-tests.ps1): read-only view of the host state and a tap on what goes to the DSP.
  window.__bkWebHost = {
    values, factory: FACTORY, get current() { return current; }, get audioMode() { return audio.mode; },
    sent: [], tap: false,
  };
  const post = audio.post.bind(audio);
  audio.post = (m) => { if (window.__bkWebHost.tap) window.__bkWebHost.sent.push(m); post(m); };
})();
