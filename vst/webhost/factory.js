// ============================================================================
//  Factory presets for the web build: resolves vst/presets/factory_presets.json
//  exactly like PresetManager::loadFactory (C++):
//    preset values = parameter defaults <- template chain (base first) <- own params
//  Values are clamped to the parameter range (not rounded), like applyParamObject.
//  Also the preset-file migration steps (twin of PresetManager::migrate).
//  Needs params.js (PARAM_BY_ID, clamp).
// ============================================================================
'use strict';

const BK_PRESET_VERSION = 1;
const BK_PRESET_FORMAT = 'Brass Knuckles Preset';
const BK_PRESET_EXT = '.bkpreset';

// Copies every known { paramId: number } into out (master excluded). Returns how many were found.
function bkApplyParamObject(obj, out) {
  let found = 0;
  if (!obj || typeof obj !== 'object') return 0;
  for (const id of Object.keys(obj)) {
    const d = PARAM_BY_ID[id];
    const raw = obj[id];
    if (!d || id === 'master' || !(typeof raw === 'number' || typeof raw === 'boolean')) continue;
    out[id] = clamp(Number(raw), d.min, d.max);
    ++found;
  }
  return found;
}

// [{ name, category, values }] where values holds only what the template chain + preset set.
function bkFactoryPresets(json) {
  const templates = (json && json.templates) || {};
  const applyTemplate = (name, out, depth) => {
    if (!name || depth > 8) return;
    const t = templates[name];
    if (!t || typeof t !== 'object') { console.warn(`unknown preset template "${name}"`); return; }
    applyTemplate(t.base, out, depth + 1);
    bkApplyParamObject(t.params, out);
  };
  const list = [];
  for (const cat of (json && json.categories) || []) {
    for (const p of cat.presets || []) {
      const values = {};
      applyTemplate(p.base, values, 0);
      bkApplyParamObject(p.params, values);
      list.push({ name: String(p.name), category: String(cat.name), values });
    }
  }
  if (!list.length) list.push({ name: 'Init', category: 'Factory', values: {} });
  return list;
}

// Brings a preset object written by an older version up to BK_PRESET_VERSION (in place).
// Same steps as PresetManager::migrate. Plain parameter additions need no step.
function bkMigratePreset(json) {
  if (!json || typeof json !== 'object') return json;
  let version = Number.isFinite(Number(json.version)) ? Math.trunc(Number(json.version)) : 1;
  version = Math.max(version, 1);
  // while (version < BK_PRESET_VERSION) { switch (version) { case 1: /* 1 -> 2 */ break; } ++version; }
  if (version < BK_PRESET_VERSION) version = BK_PRESET_VERSION;
  json.version = version;
  return json;
}
