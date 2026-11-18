// JS engine speed check (Node): renders a few heavy cases with the web build's engine and
// prints CPU use as % of real time (one core). Same cases as the C++ --bench where they overlap.
//   node tests/js-bench.js [--prof]
'use strict';
const fs = require('fs'), path = require('path'), vm = require('vm');
const vst = path.join(__dirname, '..');
// Runs in Node's main context (a vm sandbox context makes every global lookup such as Math.sin
// go through an interceptor: about 10x slower than a browser's AudioWorklet).
globalThis.sampleRate = 48000;
const ctx = globalThis;
const run = (file, tail) => vm.runInThisContext(fs.readFileSync(path.join(vst, file), 'utf8') + (tail || ''), { filename: path.join(vst, file) });
run('ui/js/params.js', ';globalThis.defaultParamValues = defaultParamValues;');
run('webhost/factory.js');
run('webhost/dsp-worklet.js', `
  ;globalThis.AudioWorkletProcessor = class { constructor() { this.port = { postMessage() {}, onmessage: null }; } };
  globalThis.registerProcessor = (n, c) => { globalThis.Proc = c; };
  synthWorkletModule();`);
const factory = vm.runInThisContext('bkFactoryPresets')(JSON.parse(fs.readFileSync(path.join(vst, 'presets/factory_presets.json'), 'utf8')));
const preset = (name) => factory.find(p => p.name === name).values;

function bench(label, values, notes, seconds) {
  const v = Object.assign(vm.runInThisContext('defaultParamValues()'), values);
  const p = new ctx.Proc({ processorOptions: { params: v } });
  const L = new Float32Array(128), R = new Float32Array(128);
  for (const n of notes) p.port.onmessage({ data: { type: 'noteOn', note: n, vel: 100 } });
  const blocks = Math.round(seconds * 48000 / 128);
  for (let b = 0; b < 40; b++) p.process([], [[L, R]]);          // warm-up (JIT)
  const t0 = performance.now();
  for (let b = 0; b < blocks; b++) p.process([], [[L, R]]);
  const ms = performance.now() - t0;
  console.log(`${label.padEnd(52)} ${(100 * ms / (blocks * 128 / 48)).toFixed(1).padStart(6)}%`);
}

const chord12 = [48, 52, 55, 59, 60, 62, 64, 67, 69, 71, 72, 76];
const poly32 = Array.from({ length: 32 }, (_, i) => 36 + i);
bench('Lead Trumpet, 1 note', preset('Lead Trumpet'), [67], 3);
bench('Lead Trumpet params, poly, 12-note chord', Object.assign({}, preset('Lead Trumpet'), { voiceMode: 0 }), chord12, 3);
bench('Trombone Section, 4-note chord', preset('Trombone Section'), [48, 52, 55, 60], 3);
bench('Shout Section, 4-note chord', preset('Shout Section'), [60, 64, 67, 70], 3);
bench('Tenor Sax, 1 note', preset('Tenor Sax'), [55], 3);
bench('Wagner Horns, 12-note chord', preset('Wagner Horns'), chord12, 3);
bench('Shout Section, 12-note chord', preset('Shout Section'), chord12, 3);
bench('Blat Brass, 12-note chord', preset('Blat Brass'), chord12, 3);
if (!process.argv.includes('--quick'))
  bench('worst case: 32 notes x 8 players x 4x, all on', {
    voiceMode: 0, polyphony: 32, secPlayers: 8, oversample: 2, addOn: 1, addPartials: 32, osc1On: 1, osc2On: 1, osc3On: 1,
    fltOn: 1, brsAmt: 0.5, growlAmt: 0.3, flutAmt: 0.2, keyClick: 0.3, chiff: 0.3, brNoise: 0.2, lfo1On: 1, lfo1Mode: 1,
    mod1Src: 1, mod1Dst: 22, mod1Amt: 0.2, mod2Src: 17, mod2Dst: 18, mod2Amt: 0.3,
    fxDrvOn: 1, eqOn: 1, chOn: 1, rvOn: 1, muteType: 4, muteSrc: 0 }, poly32, 2);
