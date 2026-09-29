const fs = require('fs');
const vm = require('vm');
const assert = require('assert');
const ts = require('/apps/harmony/sdk/default/openharmony/ets/build-tools/ets-loader/node_modules/typescript');
const root = '/data/src/winehua';
const source = fs.readFileSync(`${root}/entry/src/main/ets/service/ScreenAwake.ets`, 'utf8').replace(/^import .*;\r?\n/gm, '');
const compiled = ts.transpileModule(source, { compilerOptions: { target: ts.ScriptTarget.ES2020, module: ts.ModuleKind.CommonJS } });
const sandbox = { exports: {}, hilog: { info() {}, warn() {} } };
vm.runInNewContext(compiled.outputText, sandbox);
const { ScreenAwake } = sandbox.exports;
class FakeWindow {
  constructor() { this.calls = []; this.pending = []; this.applied = undefined; }
  setWindowKeepScreenOn(value) {
    this.calls.push(value);
    return new Promise((resolve, reject) => this.pending.push({ value, resolve, reject }));
  }
  async finish(ok = true) {
    const request = this.pending.shift();
    assert(request);
    if (ok) { this.applied = request.value; request.resolve(); }
    else request.reject(new Error('expected window failure'));
    await Promise.resolve();
    await Promise.resolve();
  }
}
async function main() {
  const a = new FakeWindow(), ctl = new ScreenAwake('lifecycle');
  ctl.setKeepScreenOn(true); ctl.attach(a); ctl.setKeepScreenOn(false);
  assert.deepStrictEqual(a.calls, [true]);
  await a.finish(); assert.deepStrictEqual(a.calls, [true, false]);
  await a.finish(); assert.strictEqual(a.applied, false);

  const entry = new ScreenAwake('entry'), desktop = new ScreenAwake('desktop');
  const e = new FakeWindow(), d = new FakeWindow();
  entry.setKeepScreenOn(true); desktop.setKeepScreenOn(true); entry.attach(e); desktop.attach(d);
  entry.setKeepScreenOn(false); await e.finish(); await e.finish(); await d.finish();
  assert.strictEqual(e.applied, false); assert.strictEqual(d.applied, true); assert.deepStrictEqual(d.calls, [true]);

  const replacement = new ScreenAwake('replace'), old = new FakeWindow(), next = new FakeWindow();
  replacement.setKeepScreenOn(true); replacement.attach(old); replacement.attach(next);
  await old.finish(); await old.finish(); await next.finish();
  assert.strictEqual(old.applied, false); assert.strictEqual(next.applied, true);
  replacement.detach(); await next.finish(); assert.strictEqual(next.applied, false);

  const failed = new ScreenAwake('failure'), f = new FakeWindow();
  failed.setKeepScreenOn(true); failed.attach(f); await f.finish(false);
  assert.deepStrictEqual(f.calls, [true]); assert.strictEqual(f.pending.length, 0);
  failed.setKeepScreenOn(true); await f.finish(); assert.strictEqual(f.applied, true);

  const pendingFailure = new ScreenAwake('changed-failure'), pf = new FakeWindow();
  pendingFailure.setKeepScreenOn(true); pendingFailure.attach(pf); pendingFailure.setKeepScreenOn(false);
  await pf.finish(false); await pf.finish(); assert.strictEqual(pf.applied, false);
  const result = { status: 'PASS', scenarios: ['pending foreground/background order', 'independent Ability windows', 'replacement and detach', 'no unbounded error retry', 'changed state after failed request'],
    scope: 'Exact ETS implementation transpiled with bundled TypeScript; mocked Window promises. HAP compile validates ArkTS; device keep-awake is separate.' };
  fs.writeFileSync(`${root}/build/direct-loader-screenawake-20260929/screen-awake-host-regression.json`, JSON.stringify(result, null, 2) + '\n');
  console.log(JSON.stringify(result));
}
main().catch(error => { console.error(error); process.exit(1); });
