const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(process.argv[2] || 'typescript');
const root = path.resolve(__dirname, '..');
function load(relative, modules) {
  const result = { exports:{} };
  const filename = path.join(root, relative);
  const code = ts.transpileModule(fs.readFileSync(filename,'utf8'), {
    compilerOptions:{module:ts.ModuleKind.CommonJS,target:ts.ScriptTarget.ES2020}
  }).outputText;
  vm.runInNewContext(code,{module:result,exports:result.exports,Date,Map,JSON,Number,
    require:name=>{assert(name in modules,name);return modules[name];}}, {filename});
  return result.exports;
}
const events=[];
let keyboardTarget=5;
const pointer=load('entry/src/main/ets/service/DesktopPointerService.ets', {
  'libentry.so':{default:{sendPointerEvent:(...args)=>events.push(args),
    getKeyboardTarget:()=>keyboardTarget}}
});
const state=pointer.DesktopPointerService.getInstance();
let notifications=0;const listener=()=>notifications++;
state.subscribe(listener);state.subscribe(listener);
pointer.sendDesktopPointerEvent(3,2,450,320,0,4,-2,true);
assert.deepEqual(events,[[3,2,450,320,0,4,-2,true]],'preserve native coordinates, deltas and physical-mouse flag');
assert.equal(state.x,450);assert.equal(state.y,320);assert.equal(notifications,1);
state.move(450,320);state.move(NaN,2);state.move(-1,2);
assert.equal(notifications,1,'unchanged or invalid coordinates cause no UI work');
state.unsubscribe(listener);state.move(500,400);assert.equal(notifications,1);
assert.equal(pointer.desktopKeyboardTarget(1),5,'an initial key targets the visible window instead of the desktop');
keyboardTarget=7;assert.equal(pointer.desktopKeyboardTarget(1),7,'the native focused/modal target is authoritative');
keyboardTarget=0;assert.equal(pointer.desktopKeyboardTarget(1),1,'empty desktop falls back to its root');
assert.equal(pointer.desktopKeyboardTarget(0),0,'an inactive session does not receive keys');
const saved=new Map();
const prefs={getSync:(k,f)=>saved.has(k)?saved.get(k):f,
  putSync:(k,v)=>saved.set(k,v),flush(){}};
const settings=load('entry/src/main/ets/service/InputSettingsService.ets', {
  '@kit.AbilityKit':{},'@kit.ArkData':{preferences:{getPreferencesSync:()=>prefs}},
  '@kit.PerformanceAnalysisKit':{hilog:{warn(){},error(){}}},'libentry.so':{default:{}}
});
const service=new settings.InputSettingsService();service.init({});
let modeNotifications=0;service.subscribe(()=>modeNotifications++);
service.setTouchpad(true);assert.equal(saved.get('touchpadEnabled'),true);
service.setTouchpad(true);assert.equal(modeNotifications,1,'reselecting a mode is a no-op');
const relativeSettings=new settings.InputSettingsService();relativeSettings.init({});
assert.equal(relativeSettings.touchpadEnabled,true,'relative mode survives reload');
service.setTouchpad(false);
const directSettings=new settings.InputSettingsService();directSettings.init({});
assert.equal(directSettings.touchpadEnabled,false,'direct mode survives reload');
service.addMouseOp('中键',0x112,.7,.6);service.addMouseOp('右键',0x111,.8,.6);
let op=service.keyOps.at(-2);assert.equal(op.mouseButton,0x112);assert.equal(op.key,0);
service.updateKeyOpPos(op.id,.4,.5);assert.equal(service.keyOps.at(-2).mouseButton,0x112);
const reloaded=new settings.InputSettingsService();reloaded.init({});
assert.equal(reloaded.keyOps.at(-2).mouseButton,0x112,'mouse type survives move, save and reload');
assert.equal(reloaded.keyOps.at(-1).mouseButton,0x111);
for(const relative of ['pages/DesktopWindow.ets','components/DesktopLayer.ets']) {
  const source=fs.readFileSync(path.join(root,'entry/src/main/ets',relative),'utf8');
  assert.match(source,/DesktopPointerOverlay\(\)/);
  assert(!source.includes('testNapi.sendPointerEvent('),'all desktop input updates the visible cursor');
}
console.log('PASS: desktop cursor state, native pointer arguments, no idle/duplicate updates, mouse control persistence and both desktop hosts');
