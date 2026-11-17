// Runs production ArkTS service/launch code with an in-memory Harmony preferences adapter.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '..');
const ts = require(process.env.RUNTIME_TYPESCRIPT || '/apps/harmony/hvigor/hvigor/node_modules/typescript');
const disk = new Map();
const memory = new Map();
const noop = () => {};
const store = {getSync: (k,d) => disk.has(k) ? disk.get(k) : d,
  putSync: (k,v) => disk.set(k,v), flush: noop};
const app = {get: k => memory.get(k), set: (k,v) => memory.set(k,v),
  setOrCreate: (k,v) => memory.set(k,v)};
const noService = {getInstance: () => ({init: noop, preferredRoute: () => 'x11',
  applyRoute: noop, desktopOutputSize: () => [960,540], shouldUseMultiwindow: () => false,
  applyWant: noop, setOverrideMode: noop})};
let storageDenied = false;
let modules;
const api = {
  '@kit.PerformanceAnalysisKit': {hilog: {info:noop,warn:noop,error:noop}},
  '@kit.AbilityKit': {UIAbility: class {}, ConfigurationConstant:{}, AbilityConstant:{},wantConstant:{}},
  '@kit.BasicServicesKit': {deviceInfo:{productModel:'test', incrementalVersion:'test'}},
  '@kit.ArkData': {preferences:{getPreferencesSync: () => {
    if (storageDenied) throw new Error('service unavailable'); return store;
  }}},
  '@kit.CoreFileKit': {}, '@kit.ArkUI': {}, 'libentry.so': {},
};
function load(name) {
  const file = path.resolve(root, name);
  if (modules.has(file)) return modules.get(file);
  const output = ts.transpileModule(fs.readFileSync(file, 'utf8'), {
    fileName:file.replace(/\.ets$/,'.ts'), compilerOptions:{module:ts.ModuleKind.CommonJS,target:ts.ScriptTarget.ES2021}
  }).outputText;
  const module={exports:{}};
  modules.set(file,module.exports);
  const req = key => {
    if (key in api) return api[key];
    if (/GraphicsBackendPolicy|WineEnvService/.test(key)) return load(path.relative(root,path.resolve(path.dirname(file),key+'.ets')));
    return {[path.basename(key)]:noService};
  };
  vm.runInNewContext(output, {module,exports:module.exports,require:req,
    AppStorage:app,PersistentStorage:{persistProp:noop},console,setTimeout,clearTimeout,ArrayBuffer,Uint8Array});
  return module.exports;
}
function coldStart() {
  memory.clear(); modules=new Map();
  // Existing default/persistProp keys already exist before preferences load.
  memory.set('winehua.d3d.preference','dxvk_modern_2_6');
  memory.set('winehua.dxvk.preference','auto');
  const service=load('entry/src/main/ets/service/WineEnvService.ets').WineEnvService.getInstance();
  service.initPreferences({});
  const Entry=load('entry/src/main/ets/entryability/EntryAbility.ets').default;
  const entry=new Entry();
  entry.publishLaunchRequest({parameters:{}});
  service.updateLaunchRequest();
  return {service,entry};
}
const backends=['wined3d','dxvk_legacy_d3d9','wined3d_zink','dxvk_legacy','dxvk_modern_2_6','vkd3d_limited_500k'];
let {service}=coldStart();
assert.equal(service.getD3dBackend(),'dxvk_modern_2_6','unchanged first-launch default');
for (const backend of backends) {
  service.setD3dPreference(backend);
  assert.equal(service.getD3dBackend(),backend,'settings to launch state');
  assert.equal(disk.get('d3d_backend'),backend,'saved backend');
  ({service}=coldStart());
  assert.equal(service.getD3dBackend(),backend,'cold-launch restore through EntryAbility');
}
service.setD3dPreference('wined3d_zink');
let run=coldStart();
run.entry.publishLaunchRequest({parameters:{'winehua.d3d_backend':'dxvk_legacy_d3d9'}});
run.service.updateLaunchRequest();
assert.equal(run.service.getD3dBackend(),'dxvk_legacy_d3d9','explicit launch override');
assert.equal(disk.get('d3d_backend'),'wined3d_zink','override must not replace saved UI choice');
assert.equal(coldStart().service.getD3dBackend(),'wined3d_zink');
for (const backend of ['dxvk_legacy_d3d9','wined3d_zink','vkd3d_limited_500k']) {
  service=coldStart().service;
  service.setD3dPreference(backend);
  service.setDxvkPreference('dxvk_modern_2_6');
  assert.equal(service.getD3dBackend(),backend,'independent DXVK profile must not replace special backend');
}
service.setD3dPreference('not-a-backend');
assert.equal(service.getD3dBackend(),'wined3d','invalid input retains safe fallback');
disk.clear();storageDenied=true;
assert.doesNotThrow(() => coldStart(), 'preferences unavailable must not block launch');
console.log('PASS: six choices, cold-launch restore, temporary override, independent DXVK and storage failure');
