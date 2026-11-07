// node scripts/test_mouse_modes.cjs <path/to/typescript.js>
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(process.argv[2] || 'typescript');
const root = path.resolve(__dirname, '..');
const MouseAction = { Press: 1, Move: 2, Release: 3 };
const TouchType = { Down: 0, Up: 1, Move: 2, Cancel: 3 };
let now = 1000, timerId = 0, target = 7;
const timers = new Map(), events = [], scrolls = [];
const cursor = { x: 300, y: 200, positioned: true,
  move(x, y) { this.x = x; this.y = y; this.positioned = true; } };
const input = { touchpadEnabled: false, cursorSpeed: 1, subscribe() {}, unsubscribe() {} };
const napi = { findToplevelAt: () => target, raiseToplevel() {},
  sendScrollEvent: (...args) => scrolls.push(args) };
const globals = { MouseAction, TouchType, testNapi: napi, Map, Math, Number,
  Date: { now: () => now }, vp2px: value => value * 2,
  setTimeout: (fn, ms) => { const id = ++timerId; timers.set(id, { fn, due: now + ms }); return id; },
  clearTimeout: id => timers.delete(id),
  hilog: { info() {}, warn() {}, error() {} }, DOMAIN: 0,
  mapMouseButton: button => ({ 1: 0x110, 2: 0x111, 4: 0x112 })[button],
  DesktopPointerService: { getInstance: () => cursor },
  sendDesktopPointerEvent: (...args) => { cursor.move(args[2], args[3]); events.push([now, ...args]); },
  InputSettingsService: { getInstance: () => input }, DesktopController: class {} };
function compile(source, extra = {}) {
  const result = { exports: {} };
  const code = ts.transpileModule(source, {
    compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS }
  }).outputText;
  vm.runInNewContext(code, { module: result, exports: result.exports, ...globals, ...extra,
    require: name => {
      if (name === 'libentry.so') return { default: napi };
      if (name.endsWith('/DesktopPointerService')) return globals;
      if (name === './MouseMap') return globals;
      throw new Error('Unexpected import: ' + name);
    } });
  return result.exports;
}
function source(relative) { return fs.readFileSync(path.join(root, relative), 'utf8'); }
function advance(ms) {
  now += ms;
  for (const [id, timer] of [...timers]) {
    if (timer.due <= now) { timers.delete(id); timer.fn(); }
  }
}
function touch(type, id = 1, x = 10, y = 10, touches) {
  const point = { id, x, y };
  return { type, changedTouches: [point],
    touches: touches ?? (type === TouchType.Up ? [] : [point]), stopPropagation() {} };
}
const { DesktopTouchpad } = compile(source('entry/src/main/ets/common/DesktopTouchpad.ets'));
const pad = new DesktopTouchpad(); pad.setBounds(800, 400); pad.activate();
pad.handle(touch(TouchType.Down));
assert.equal(events.length, 0, 'finger down does not click while positioning a relative cursor');
assert.deepEqual([cursor.x, cursor.y], [300, 200], 'relative down preserves the existing cursor');
pad.handle(touch(TouchType.Move, 1, 30, 20));
assert.deepEqual([cursor.x, cursor.y], [340, 220], 'relative movement applies physical-pixel deltas');
pad.handle(touch(TouchType.Up, 1, 30, 20));
const count = events.length; advance(800);
assert.equal(events.length, count, 'moving cancels the stationary hold timer and does not click');
pad.cursorSpeed = 2;
pad.handle(touch(TouchType.Down, 2, 100, 100));
pad.handle(touch(TouchType.Move, 2, 105, 103));
assert.deepEqual([cursor.x, cursor.y], [360, 232], 'speed affects deltas, not absolute placement');
pad.handle(touch(TouchType.Move, 2, 10000, 10000));
assert.deepEqual([cursor.x, cursor.y], [799, 399], 'relative cursor stays inside this surface');
pad.reset(); cursor.move(200, 150); pad.activate(); pad.cursorSpeed = 1;
pad.handle(touch(TouchType.Down)); advance(499);
const beforeHold = events.length;
advance(1);
assert.deepEqual(events.at(-1).slice(1), [7, MouseAction.Press, 200, 150, 0x110],
  'a stationary relative long press becomes a held left button without any Move');
assert.equal(events.length, beforeHold + 1); advance(1500);
assert.equal(events.length, beforeHold + 1, 'holding emits no synthetic release or repeated down');
target = 9;
pad.handle({ type: TouchType.Cancel, changedTouches: [], touches: [] });
assert.deepEqual(events.at(-1).slice(1), [7, MouseAction.Release, 200, 150, 0x110],
  'empty Cancel releases the originally captured target');
const cancelled = events.length; pad.reset(); advance(600);
assert.equal(events.length, cancelled, 'reset is idempotent and clears timers');
target = 7;
pad.handle(touch(TouchType.Down)); pad.handle(touch(TouchType.Up)); advance(100);
pad.handle(touch(TouchType.Down, 2));
assert.equal(events.at(-1)[2], MouseAction.Press, 'the second tap starts a hold immediately');
const doubled = events.length; advance(700);
assert.equal(events.length, doubled, 'double-tap holding does not require motion');
pad.handle(touch(TouchType.Up, 99));
assert.equal(events.length, doubled, 'another finger cannot release the captured hold');
pad.handle(touch(TouchType.Up, 2));
assert.equal(events.at(-1)[2], MouseAction.Release);
pad.reset();
pad.handle(touch(TouchType.Down));
pad.handle(touch(TouchType.Down, 2, 30, 30, [{ id: 1, x: 10, y: 10 }, { id: 2, x: 30, y: 30 }]));
const multiCount = events.length; advance(600);
assert.equal(events.length, multiCount, 'two-finger scrolling cancels a pending hold');
pad.handle(touch(TouchType.Move, 2, 30, 0, [{ id: 1, x: 10, y: -20 }, { id: 2, x: 30, y: 0 }]));
assert.ok(scrolls.length > 0, 'two-finger movement still scrolls');
pad.reset();
assert.ok(events.every(event => event[5] === 0 || event[5] === 0x110),
  'relative gestures never invent a right or middle button');

// Execute the actual touch handlers and lifecycle methods from both hosts.
function block(text, marker) {
  const start = text.indexOf(marker); assert.ok(start >= 0, marker);
  const open = text.indexOf('{', start); let end = open + 1, depth = 1;
  while (depth && end < text.length) { depth += (text[end] === '{') - (text[end] === '}'); end++; }
  assert.equal(depth, 0); return text.slice(open + 1, end - 1);
}
const { GameTouchRouter } = compile(source('entry/src/main/ets/common/GameTouchRouter.ts'));
const desktopWindow = source('entry/src/main/ets/pages/DesktopWindow.ets');
assert.match(desktopWindow, /DesktopLayer\(\{ floating: false \}\)/,
  'the phone/tablet host delegates to the shared layer instead of a second gesture copy');
const routedSink = {
  hitTest: () => target, beginStroke: () => target,
  pressAt: (tl, x, y, button) => globals.sendDesktopPointerEvent(tl, MouseAction.Press, x, y, button),
  releaseAt: (tl, x, y, button) => globals.sendDesktopPointerEvent(tl, MouseAction.Release, x, y, button),
  strokeMove: (tl, x, y) => globals.sendDesktopPointerEvent(tl, MouseAction.Move, x, y, 0),
  scrollAt: (...args) => scrolls.push(args), inputReady: () => true
};
for (const relative of ['components/DesktopLayer.ets']) {
  const text = source('entry/src/main/ets/' + relative);
  const fields = text.slice(text.indexOf('  controller: DesktopController'), text.indexOf('  build()'))
    .replace(/@State\s+/g, '');
  const { Host } = compile('export class Host { floating = false; ' + fields +
    '\nhandleTouch(ev) {' + block(text, '.onTouch((ev: TouchEvent) =>') + '} }',
    { DesktopTouchpad, GameTouchRouter });
  const host = new Host(); host.sink = routedSink; host.touchpad = new DesktopTouchpad(routedSink);
  input.touchpadEnabled = false; host.aboutToAppear();
  events.length = 0; target = 7;
  host.handleTouch(touch(TouchType.Down, 1, 40, 30));
  assert.deepEqual(events.at(-1).slice(1), [7, MouseAction.Press, 80, 60, 0x110],
    relative + ': direct touch presses at Down');
  advance(1500); assert.equal(events.length, 1, 'stationary direct hold needs no movement');
  host.handleTouch(touch(TouchType.Up, 99)); assert.equal(events.length, 1);
  host.handleTouch(touch(TouchType.Up)); assert.equal(events.at(-1)[2], MouseAction.Release);
  host.handleTouch(touch(TouchType.Down));
  input.touchpadEnabled = true; host.mirrorInput();
  assert.equal(events.at(-1)[2], MouseAction.Release, 'switching mode releases the old hold');
  const switched = events.length; host.handleTouch(touch(TouchType.Up));
  assert.equal(events.length, switched, 'an old Up does not become a click in the new mode');
  host.handleTouch(touch(TouchType.Down)); advance(500);
  assert.equal(events.at(-1)[2], MouseAction.Press);
  host.aboutToDisappear();
  assert.equal(events.at(-1)[2], MouseAction.Release, 'teardown releases relative hold');
  assert.equal(timers.size, 0, 'teardown clears deferred work');
}
console.log('PASS: both desktop mouse modes, cursor continuity/bounds/speed, stationary holds, double-tap holds, scroll, owner capture, mode switching and teardown');
