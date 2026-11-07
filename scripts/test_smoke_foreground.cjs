// Run with Node and TypeScript. Exercises the actual smoke orchestration.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const ts = require(process.argv[2] || 'typescript');
const root = path.resolve(__dirname, '..');
let now = 0, rootId = 0, surfaceReady = false, shown = 0, rejected = false;
let x11 = false;
const caller = { identity: 'foreground-entry' };
const manager = {
  isDesktopMode: true, getDesktopRootId: () => rootId,
  get desktopSurfaceReady() { return surfaceReady; },
  getDesktopCanvas: () => surfaceReady ? { w: 800, h: 600 } : null,
  async showDesktopForCaller(context) {
    assert.equal(context, caller);
    shown++;
    if (rejected) throw new Error('foreground rejected');
    surfaceReady = true;
  }
};
function load(relative, modules, extra = {}) {
  const result = { exports: {} };
  const source = fs.readFileSync(path.join(root, relative), 'utf8');
  const code = ts.transpileModule(source, {
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
  }).outputText;
  vm.runInNewContext(code, { module: result, exports: result.exports, Promise, Error, Map, Set,
    Date: { now: () => now },
    setTimeout(fn, ms) { now += ms; if (now >= 300) rootId = 1; fn(); },
    require: name => { assert(name in modules, name); return modules[name]; }, ...extra });
  return result.exports;
}
const { SmokeRunner } = load('entry/src/main/ets/smoke/SmokeRunner.ets', {
  'libentry.so': { default: {} }, 'libsmoke_napi.so': { default: {} },
  'libdisplay_route_napi.so': { default: {} },
  '@kit.PerformanceAnalysisKit': { hilog: { info() {}, warn() {}, error() {} } },
  '../service/WineEnvService': {}, '../common/MouseMap': {},
  '../service/DisplayRouteService': { DisplayRouteService: { getInstance: () => ({ isX11: () => x11 }) } },
  '../service/WineWindowManager': { WineWindowManager: { getInstance: () => manager } }
});
async function main() {
  const runner = new SmokeRunner();
  await runner.prepareDesktop(caller);
  assert(now >= 300 && shown === 1 && surfaceReady, 'wait for root, then request visible desktop');
  surfaceReady = false;
  await runner.prepareDesktop(caller);
  assert.equal(shown, 2, 'each new Want must restore the desktop even when root already exists');
  rejected = true;
  await assert.rejects(runner.prepareDesktop(caller), /foreground rejected/);
  rejected = false;
  await assert.rejects(runner.prepareDesktop(), /caller context/);
  manager.isDesktopMode = false;
  await runner.prepareDesktop();
  assert.equal(shown, 3, 'managed windows need no desktop foreground operation');
  manager.isDesktopMode = true; x11 = true; rootId = 0;
  await runner.prepareDesktop(caller);
  assert.equal(shown, 4, 'X11 root=0 uses canvas readiness instead of waiting for a Wayland root');

  let starts = 0, seeded = 0;
  const env = { prefixReady: true, engineState: 'ready', upgradePending: false,
    doReset() { throw new Error('must never reset a reuse prefix'); } };
  let startupError;
  const { SmokeHook } = load('entry/src/main/ets/smoke/SmokeHook.ets', {
    '@kit.PerformanceAnalysisKit': { hilog: { info() {}, warn() {}, error() {} } },
    '@kit.CoreFileKit': { fileIo: {} }, '../service/WineEnvService': {},
    './SmokeRunner': { SmokeRunner: { getInstance: () => ({ start(req, context, error) {
      assert.equal(context, caller); startupError = error; starts++;
    } }) } }
  });
  const hook = new SmokeHook(); hook.env = env; hook.seed = () => { seeded++; };
  hook.onEngineReady();
  assert.equal(starts, 0, 'normal engine readiness must not open a desktop or start smoke');
  hook.caller = caller; hook.pending = { runId: 'cold' }; hook.onEngineReady();
  assert.equal(starts, 1);
  hook.pending = { runId: 'warm' }; hook.consume = () => {}; hook.onNewWant();
  assert.equal(starts, 2, 'cold and warm requests pass their foreground caller to the same runner');
  env.upgradePending = true;
  hook.pending = { runId: 'reuse-upgrade', prefix: 'reuse' }; hook.onNewWant();
  assert.equal(starts, 3);
  assert.match(startupError, /prefix=reuse preserves/);
  assert.equal(seeded, 2);
  console.log('PASS: cold/warm smoke desktop gating, rejected callers, normal startup and reuse data protection');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
