// Exercise the preserved main-ui keyboard's real event and lifetime methods.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(process.argv[2] || 'typescript');
const root = path.resolve(__dirname, '..');
function load(name, modules = {}, globals = {}, cut = false) {
  let source = fs.readFileSync(path.join(root, name), 'utf8');
  if (cut) {
    const builder = source.indexOf('  @Builder');
    assert(builder > 0);
    source = source.slice(0, builder) + '\n}\n';
    source = source.replace('export struct VirtualKeyboardPanel', 'export class VirtualKeyboardPanel');
  }
  const code = ts.transpileModule(source, { compilerOptions: {
    module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020,
    experimentalDecorators: true
  }}).outputText;
  const result = { exports: {} };
  vm.runInNewContext(code, { module: result, exports: result.exports, Map, Set,
    Component: value => value, State() {}, Prop() {}, px2vp: value => value,
    TouchType: { Down: 0, Up: 1, Cancel: 3 },
    require: name => { assert(name in modules, name); return modules[name]; }, ...globals });
  return result.exports;
}
const router = load('entry/src/main/ets/common/TouchKeyRouter.ts');
let focused = 7, desktopRoot = 1, callback;
const events = [];
const settings = { keyboardEnabled: true, kbRelX: 0.5, kbRelY: 0.9,
  subscribe: fn => { callback = fn; }, unsubscribe: fn => { assert.equal(fn, callback); callback = null; } };
const { VirtualKeyboardPanel } = load('entry/src/main/ets/components/VirtualKeyboardPanel.ets', {
  '@kit.ArkUI': { display: { getDefaultDisplaySync: () => ({ width: 1280, height: 800 }) } },
  '@kit.PerformanceAnalysisKit': { hilog: { error() {} } },
  'libentry.so': { default: { sendKeyEvent() { throw Error('route sink must be used'); } } },
  '../service/WineWindowManager': { WineWindowManager: { getInstance: () => ({ getDesktopRootId: () => desktopRoot }) } },
  '../service/InputSettingsService': { InputSettingsService: { getInstance: () => settings } },
  '../service/DesktopPointerService': { desktopKeyboardTarget: () => focused,
    DesktopPointerService: { getInstance: () => ({ x: 10, y: 20 }) } },
  '../common/TouchKeyRouter': router,
  './DesktopInputSink': {}
}, {}, true);
const panel = new VirtualKeyboardPanel();
panel.inputSink = { keyEvent: (...args) => events.push(args), inputReady: () => true, hitTest: () => 1 };
panel.aboutToAppear();
const key = (code, mod = 0) => ({ label: '', code, mod, w: 1, shiftLabel: '' });
const down = code => panel.onKeyTouch(key(code), { type: 0 });
const up = code => panel.onKeyTouch(key(code), { type: 1 });
const shift = () => panel.onKeyTouch(key(42, 1), { type: 0 });
shift(); down(17); focused = 9; up(17);
assert.deepEqual(events, [[42, true, 7], [17, true, 7], [17, false, 7], [42, false, 7]]);
events.length = 0; focused = 7;
shift(); down(17); shift(); down(30); up(17);
assert(!events.some(event => event[0] === 42 && !event[1]), 'shared Shift must remain held');
up(30);
assert.equal(events.filter(event => event[0] === 42 && !event[1]).length, 1);
events.length = 0;
down(18); settings.keyboardEnabled = false; callback();
assert.deepEqual(events, [[18, true, 7], [18, false, 7]]);
settings.keyboardEnabled = true; callback();
events.length = 0; desktopRoot = 0;
down(19); panel.onKeyTouch(key(19), { type: 3 });
assert.deepEqual(events, [[19, true, 1], [19, false, 1]], 'X11 root=0 must retain routed key input');
events.length = 0;
down(20); panel.aboutToDisappear();
assert.deepEqual(events, [[20, true, 1], [20, false, 1]]);
assert.equal(callback, null);
console.log('PASS: full keyboard focus capture, modifier ownership, disable/cancel/unmount releases and X11 input');
