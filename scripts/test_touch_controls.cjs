// Run with Node and an installed TypeScript compiler: node scripts/test_touch_controls.cjs <typescript.js>
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(process.argv[2] || 'typescript');
function source(name) {
  const filename = path.resolve(__dirname, '../entry/src/main/ets/common', name + '.ts');
  const code = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 }
  }).outputText;
  const result = { exports: {} };
  vm.runInNewContext(code, { exports: result.exports, module: result, Map, Math }, { filename });
  return result.exports;
}
const { clampControlCenter, sampleKeyboardStick } = source('TouchControlMath');
const { TouchKeyRouter } = source('TouchKeyRouter');
const { TouchMouseRouter } = source('TouchMouseRouter');
const codes = (x, y, arrows = false) => Array.from(sampleKeyboardStick(x, y, 144, arrows).codes);
assert.deepEqual(codes(72, 72), [], 'touch at center is neutral');
assert.deepEqual(codes(73, 71), [], 'dead zone suppresses small movement');
assert.deepEqual(codes(72, 0), [17]);
assert.deepEqual(codes(0, 72), [30]);
assert.deepEqual(codes(72, 144), [31]);
assert.deepEqual(codes(144, 72), [32]);
assert.deepEqual(codes(144, 0), [32, 17]);
assert.deepEqual(codes(144, 144, true), [106, 108]);
assert.deepEqual(codes(144, 75), [32], 'near-horizontal movement is not diagonal');
for (let x = -500; x < 650; x += 19) {
  for (let y = -500; y < 650; y += 23) {
    const sample = sampleKeyboardStick(x, y, 144, false);
    assert.ok(Math.hypot(sample.x, sample.y) <= 38.00001, 'thumb stays inside circular base');
    assert.ok(!(sample.codes.includes(17) && sample.codes.includes(31)));
    assert.ok(!(sample.codes.includes(30) && sample.codes.includes(32)));
    for (const size of [56, 130, 144]) {
      const p = clampControlCenter(x, y, size, 821, 363);
      assert.ok(p.x >= size / 2 && p.x <= 821 - size / 2);
      assert.ok(p.y >= size / 2 && p.y <= 363 - size / 2);
    }
  }
}
const events = [];
const router = new TouchKeyRouter((target, code, down) => events.push([target, code, down]));
router.set('stick', 4, [17]);
router.set('button', 4, [17]);
assert.deepEqual(events, [[4, 17, true]], 'overlapping controls share key-down');
router.set('stick', 4, []);
assert.equal(events.length, 1, 'releasing one owner keeps other owner pressed');
router.set('button', 9, []);
assert.deepEqual(events.at(-1), [4, 17, false], 'release goes to original target');
router.set('combo', 4, [29, 31, 31]);
router.releaseAll();
assert.deepEqual(events.slice(-2), [[4, 31, false], [4, 29, false]], 'combo releases in reverse order');
router.set('dpad:103', 4, [103]);
router.set('dpad:106', 4, [106]);
router.releasePrefix('dpad');
assert.deepEqual(events.slice(-2), [[4, 103, false], [4, 106, false]]);
const length = events.length;
router.releaseAll();
assert.equal(events.length, length, 'releasing twice emits no duplicate key-up');
const mouseEvents = [];
const mouse = new TouchMouseRouter((...event) => mouseEvents.push(event));
mouse.set('middle-one',4,0x112,true);
mouse.set('middle-two',4,0x112,true);
mouse.set('right',4,0x111,true);
assert.deepEqual(mouseEvents,[[4,0x112,true],[4,0x111,true]],'mouse button owners are independent and reference counted');
mouse.set('middle-one',9,0x112,false);
assert.equal(mouseEvents.length,2,'one middle owner remains pressed');
mouse.set('middle-two',9,0x112,false);
assert.deepEqual(mouseEvents.at(-1),[4,0x112,false],'release follows the original mouse target after focus changes');
mouse.releaseAll();
assert.deepEqual(mouseEvents.at(-1),[4,0x111,false]);
const mouseLength=mouseEvents.length;
mouse.releaseAll();
assert.equal(mouseEvents.length,mouseLength,'duplicate teardown sends no second release');
mouse.set('layout:key',4,0x111,true);
mouse.releasePrefix('layout');
assert.deepEqual(mouseEvents.at(-1),[4,0x111,false],'removing a control releases its held button');
mouse.set('invalid',4,0x113,true);
assert.deepEqual(mouseEvents.at(-1),[4,0x111,false],'only supported pointer buttons are accepted');
console.log('PASS: stick directions/bounds, shared keyboard and mouse ownership, middle/right buttons, cancellation and original-target releases');
