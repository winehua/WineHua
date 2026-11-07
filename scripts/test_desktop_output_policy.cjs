// node scripts/test_desktop_output_policy.cjs <path/to/typescript.js>
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(process.argv[2] || 'typescript');
const root = path.resolve(__dirname, '..');
const manager = fs.readFileSync(path.join(root, 'entry/src/main/ets/service/WineWindowManager.ets'), 'utf8');
function method(signature) {
  const start = manager.indexOf(signature);
  assert(start >= 0, signature);
  let end = manager.indexOf('{', start) + 1, depth = 1;
  while (depth && end < manager.length) {
    depth += (manager[end] === '{') - (manager[end] === '}');
    end++;
  }
  assert.equal(depth, 0);
  return manager.slice(start, end);
}
const storage = new Map();
const outputs = [];
let screen = { width: 2380, height: 1100, densityPixels: 1.75 };
const deviceInfo = { deviceType: 'phone' };
const displaySettings = { scale: 2, init() {} };
const sandbox = {
  exports: {}, deviceInfo, DOMAIN: 0, TAG: 'test',
  PHONE_DESKTOP_BOTTOM_VP: 16,
  WineWindowManager: { DESKTOP_PRESENT_SCALE: 2 },
  DisplaySettingsService: { getInstance: () => displaySettings },
  AppStorage: { get: key => storage.get(key) },
  display: { getDefaultDisplaySync: () => screen },
  hilog: { info() {}, warn() {} },
  testNapi: { setDisplayScale() {}, setOutputSize: (w, h) => outputs.push([w, h]) }
};
const code = `class Manager {
  private displayScale = 1;
  private scaleBoost = 1;
  private fixedDesktopResolution = false;
  private nextDesktopResolution = '';
  private activeDesktopResolution = '';
  ${method('applyOutputPolicy(): void')}
  ${method('setNextDesktopResolution(resolution: string): void')}
  ${method('prepareSessionOutputPolicy(): void')}
  ${method('hasFixedDesktopResolution(): boolean')}
}
exports.Manager = Manager;`;
vm.runInNewContext(ts.transpileModule(code, {
  compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
}).outputText, sandbox);
const m = new sandbox.exports.Manager();
const latest = () => outputs[outputs.length - 1];
storage.set('winehua.phone.scale.preference', 3);
m.applyOutputPolicy();
const landscape = latest();
assert.equal(m.hasFixedDesktopResolution(), false);
assert.deepEqual(landscape, [793, 357]);
screen = { width: 1100, height: 2380, densityPixels: 1.75 };
m.applyOutputPolicy();
assert.deepEqual(latest(), landscape, 'pending orientation must not reverse the output');
storage.set('winehua.phone.desktop.preference', '1024x768');
assert.equal(m.hasFixedDesktopResolution(), false, 'editing preferences does not change the active snapshot');
m.applyOutputPolicy();
assert.deepEqual(latest(), [1024, 768]);
assert.equal(m.hasFixedDesktopResolution(), true);
storage.set('winehua.phone.desktop.preference', 'auto');
assert.equal(m.hasFixedDesktopResolution(), true, 'changes during startup remain pending');
m.applyOutputPolicy();
assert.deepEqual(latest(), landscape);
assert.equal(m.hasFixedDesktopResolution(), false);
storage.set('winehua.phone.desktop.preference', 'auto');
m.setNextDesktopResolution('1024x768');
m.prepareSessionOutputPolicy();
assert.deepEqual(latest(), [1024, 768], 'game override must apply at the session boundary');
assert.equal(storage.get('winehua.phone.desktop.preference'), 'auto');
m.applyOutputPolicy();
assert.deepEqual(latest(), [1024, 768], 'recreating a host window must retain the active session override');
m.prepareSessionOutputPolicy();
assert.deepEqual(latest(), landscape, 'a later global session must not inherit a previous game override');
storage.set('winehua.phone.desktop.preference', '1024x768');
deviceInfo.deviceType = '2in1';
sandbox.PHONE_DESKTOP_BOTTOM_VP = 0;
displaySettings.scale = 4;
m.applyOutputPolicy();
assert.equal(m.hasFixedDesktopResolution(), false, 'phone fixed-size preference must not affect PC');
assert.deepEqual(latest(), [275, 595], 'main-ui PC output must retain the user-selected scale');
const service = fs.readFileSync(path.join(root, 'entry/src/main/ets/service/WineEnvService.ets'), 'utf8');
const session = service.slice(service.indexOf('startSession = async'));
assert(session.indexOf('.prepareSessionOutputPolicy()') >= 0);
assert(session.indexOf('.prepareSessionOutputPolicy()') < session.indexOf('testNapi.startServer('));
console.log('PASS: fixed/automatic output, portrait startup, session snapshot and pre-launch wiring');
