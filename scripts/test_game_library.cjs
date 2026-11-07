// node scripts/test_game_library.cjs /path/to/typescript.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const ts = require(process.argv[2] || 'typescript');
const root = path.resolve(__dirname, '..');
function load(relative, modules = {}, globals = {}) {
  const filename = path.join(root, relative);
  const result = { exports: {} };
  const code = ts.transpileModule(fs.readFileSync(filename, 'utf8'), {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2020 }
  }).outputText;
  vm.runInNewContext(code, { module: result, exports: result.exports, Map, Set, Date,
    Promise, setTimeout, clearTimeout, require: name => {
      assert(name in modules, `Unexpected import: ${name}`); return modules[name];
    }, ...globals }, { filename });
  return result.exports;
}
const model = load('entry/src/main/ets/common/GameLibraryModel.ts');
const launchOptions = load('entry/src/main/ets/common/WineProgramOptions.ts');
const box64 = load('entry/src/main/ets/service/Box64Dynarec.ets');
const plain = value => JSON.parse(JSON.stringify(value));
assert.equal(model.relativeGamePath('中文游戏\\Binaries/Win64/game.exe'), '中文游戏/Binaries/Win64/game.exe');
for (const bad of ['../x.exe', 'game/../../x.exe', '/etc/x', 'C:\\x.exe', 'bad\0.exe'])
  assert.throws(() => model.relativeGamePath(bad));
const entries = model.buildGameEntries(['A/unins000.exe', 'A/A.exe', 'A/Binaries/Win64/A-Win64.exe',
  'B/launcher.exe', 'B/DLC/dlc.exe', 'setupOnly/Setup_x64.exe', 'standalone.EXE', 'A/A.exe'], ['A', 'B', 'empty', 'setupOnly']);
assert.equal(entries.length, 5);
const a = entries.find(g => g.id === 'folder:A');
assert.equal(a.suggestedExecutable, 'A/A.exe');
assert.equal(a.executables.length, 3);
assert.equal(entries.find(g => g.id === 'folder:B').executables.length, 2);
assert.equal(entries.find(g => g.id === 'folder:setupOnly').suggestedExecutable, '');
assert.equal(entries.find(g => g.id === 'folder:empty').executables.length, 0);
assert.equal(entries.find(g => g.id === 'file:standalone.EXE').directory, '');
const programs = model.expandGameEntries(entries);
assert.equal(programs.length, 7);
assert.equal(programs.find(g => g.id === 'exe:B/DLC/dlc.exe').directory, 'B/DLC');
assert.equal(programs.find(g => g.id === 'exe:setupOnly/Setup_x64.exe').suggestedExecutable, '');
const darkest=model.buildGameEntries([
  'DarkestDungeon/DarkestDungeon/_windows/steam_workshop_upload.exe',
  'DarkestDungeon/DarkestDungeon/_windows/win32/localization.exe',
  'DarkestDungeon/DarkestDungeon/_windows/win64/SaveGameTransfer.exe',
  'DarkestDungeon/DarkestDungeon/_windows/win32/Darkest.exe',
  'DarkestDungeon/DarkestDungeon/_windows/win64/Darkest.exe'
],['DarkestDungeon'])[0];
assert.equal(darkest.suggestedExecutable,'DarkestDungeon/DarkestDungeon/_windows/win64/Darkest.exe',
  'prefer the actual 64-bit game to Steam workshop/upload/save/localization helpers');
assert.deepEqual(plain(model.parseGameArguments('-windowed --name "中文 游戏" ""')), ['-windowed','--name','中文 游戏','']);
assert.deepEqual(plain(model.parseGameArguments(String.raw`--root "C:\Games\A B\\"`)), ['--root','C:\\Games\\A B\\']);
assert.deepEqual(plain(model.parseGameArguments(String.raw`--title "say \"hello\""`)), ['--title','say "hello"']);
assert.deepEqual(plain(model.parseGameArguments('"a""b"')), ['a"b']);
assert.deepEqual(plain(model.parseGameArguments('C:\\Games\\file.exe')), ['C:\\Games\\file.exe']);
assert.throws(() => model.parseGameArguments('"missing close'));
assert.throws(() => model.parseGameArguments('"bad|value"'));
assert.deepEqual(plain(model.parseGameEnvironment('# comment\nFOO=a=b\nEMPTY=\n')), { FOO:'a=b', EMPTY:'' });
for (const text of ['no equals', '9INVALID=x', '__proto__=x', 'WINEPREFIX=other', 'WINEDEBUG=+all', 'X=bad|value'])
  assert.throws(() => model.parseGameEnvironment(text));

async function serviceTests() {
  const saved = new Map();
  const store = { getSync: (key, fallback) => saved.has(key) ? saved.get(key) : fallback,
    putSync: (key,value) => saved.set(key,value), flushSync() {} };
  const nodes = new Map([
    ['/games', { type:'dir', ino:101n, names:['A','B','empty','loop','root.EXE'] }],
    ['/games/A', { type:'dir', names:['A.exe','unins000.exe','Binaries'] }],
    ['/games/A/Binaries', { type:'dir', names:['Win64'] }],
    ['/games/A/Binaries/Win64', { type:'dir', names:['A-Win64.exe'] }],
    ['/games/A/A.exe', { type:'file' }], ['/games/A/unins000.exe', { type:'file' }],
    ['/games/A/Binaries/Win64/A-Win64.exe', { type:'file' }],
    ['/games/B', { type:'dir', names:['launcher.exe'] }], ['/games/B/launcher.exe', { type:'file' }],
    ['/games/empty', { type:'dir', names:[] }], ['/games/loop', { type:'link' }],
    ['/games/root.EXE', { type:'file' }]
  ]);
  const stat = p => {
    assert(nodes.has(p), `Unexpected file access: ${p}`);
    const type = nodes.get(p).type;
    return { ino:nodes.get(p).ino || 0n,
      isDirectory:()=>type==='dir', isFile:()=>type==='file', isSymbolicLink:()=>type==='link' };
  };
  const fileIo = { lstatSync: stat, statSync: stat, listFile: async p => {
    assert(nodes.has(p) && nodes.get(p).type==='dir'); return nodes.get(p).names;
  } };
  let restarts = 0, shown = 0, nextResolution = '', fixed = false, markSurface = true, blockCaller = false;
  const requests = [];
  const context = { identity:'foreground-library-window' };
  const env = { engineState:'ready', desktopMode:true, desktopSurfaceReady:false, initFailed:false,
    upgradePending:false, isBusy:()=>env.engineState==='starting'||env.engineState==='stopping', getGamesDirectory:async()=>'/games',
    ensureEngineReady:async()=>{ env.engineState='ready'; return true; },
    ensureDesktop:async()=>{assert(shown>0,'request the foreground window before waiting for it');return true;},
    restartEngine:()=>{ restarts++; fixed=nextResolution==='1024x768'; env.engineState='ready'; }
  };
  const wm = { hasFixedDesktopResolution:()=>fixed, setNextDesktopResolution:r=>{nextResolution=r;},
    showDesktopForCaller:async caller=>{
      assert.equal(caller,context);
      if (blockCaller) throw {code:201};
      shown++;if(markSurface)env.desktopSurfaceReady=true;
    } };
  const storage = new Map([['winehua.phone.desktop.preference','auto'],['winehua.d3d.backend','wined3d']]);
  const napi = { runWineProgram: options => { requests.push(plain(options)); return { pid:321, found:true }; } };
  const { GameLibraryService } = load('entry/src/main/ets/service/GameLibraryService.ets', {
    '@kit.AbilityKit':{}, '@kit.ArkData':{ preferences:{ getPreferencesSync:()=>store } },
    '@kit.CoreFileKit':{fileIo}, '@kit.BasicServicesKit':{deviceInfo:{deviceType:'phone'}},
    '@kit.PerformanceAnalysisKit':{hilog:{info(){},warn(){},error(){}}}, 'libentry.so':{default:napi},
    '../common/GameLibraryModel':model, '../common/GraphicsDefaults':{DEFAULT_D3D_BACKEND:'wined3d'},
    '../common/WineProgramOptions':launchOptions,
    './Box64Dynarec':box64,
    './WineEnvService':{DRIVE_C:'/prefix/drive_c',WineEnvService:{getInstance:()=>env}},
    './WineWindowManager':{WineWindowManager:{getInstance:()=>wm}}
  }, {AppStorage:{get:key=>storage.get(key)}});
  const service = new GameLibraryService();
  service.init(context);
  await service.refresh();
  assert.equal(service.entries.length, 4);
  assert(!service.entries.some(g=>g.title==='loop'));
  const gameA = service.entries.find(g=>g.id==='folder:A');
  const gameB = service.entries.find(g=>g.id==='folder:B');
  let profile = service.getProfile(gameA);
  Object.assign(profile,{name:'游戏 A', argumentsText:'--name "中文 空格"',d3dBackend:'dxvk_legacy_d3d9',
    compatMode:'conservative',environmentText:'CUSTOM=one=two\nBOX64_DYNAREC_BIGBLOCK=0'});
  service.saveProfile(gameA,profile);
  assert.equal(service.getProfile(gameB).d3dBackend,'inherit','settings must remain per game');
  const reloaded = new GameLibraryService();reloaded.init({});
  assert.equal(reloaded.getProfile(gameA).name,'游戏 A','settings must survive a new service');
  const launched = await service.launch(gameA);
  assert.equal(launched.accepted,true,JSON.stringify(launched));
  assert.equal(shown,1);
  assert.equal(requests[0].windowsExePath,'/games/A/A.exe');
  assert.equal(requests[0].workingDirectory,'/games/A');
  assert.deepEqual(requests[0].argv,['--name','中文 空格']);
  assert.equal(requests[0].d3dBackend,'dxvk_legacy_d3d9');
  assert.equal(requests[0].dxvkBackend,'dxvk_legacy');
  assert.equal(requests[0].environment.BOX64_DYNAREC_BIGBLOCK,'0');
  assert.equal(requests[0].environment.BOX64_DYNAREC_SAFEFLAGS,'2');
  assert.equal(requests[0].environment.CUSTOM,'one=two');
  assert.equal(requests[0].openglRenderer,'auto');
  assert(service.getProfile(gameA).lastLaunchedAt>0);
  profile=service.getProfile(gameA);profile.resolution='1024x768';service.saveProfile(gameA,profile);
  const mismatch=await service.launch(gameA);
  assert.equal(mismatch.requiresRestart,true);
  assert.equal(restarts,0,'resolution mismatch must not terminate a session automatically');
  assert.equal(requests.length,1);
  const restarted=await service.launch(gameA,true);
  assert.equal(restarted.accepted,true);assert.equal(restarts,1);
  assert.equal(nextResolution,'1024x768');
  assert.equal(storage.get('winehua.phone.desktop.preference'),'auto','per-game resolution must not overwrite global preference');
  env.engineState='starting';
  setTimeout(()=>{env.engineState='ready';},5);
  const queued=await service.launch(gameA);
  assert.equal(queued.accepted,true,'a click during startup should wait rather than restart or silently fail');
  assert.equal(restarts,1);
  markSurface=false;env.desktopSurfaceReady=false;
  const manualPhone=await service.launch(gameA);
  assert.equal(manualPhone.accepted,true,'manual phone desktops must not acquire a second surface-ready gate');
  blockCaller=true;
  const denied=await service.launch(gameA);
  assert.equal(denied.accepted,false);assert.match(denied.message,/201/);
  blockCaller=false;
  service.scanning=true;
  const busy=await service.launch(gameA);assert.equal(busy.accepted,false);service.scanning=false;
  assert.equal(requests.length,4);
  nodes.delete('/games/A/A.exe');
  const missing=await service.launch(gameA);assert.equal(missing.accepted,false);
  assert.equal(requests.length,4,'removed executables must not reach Wine');
  assert.throws(()=>service.saveProfile(gameA,{...profile,executable:'../outside.exe'}));
  fixed=false;
  nodes.set('/prefix/drive_c/games',{type:'dir',ino:101n});
  const defaultLaunch=await service.launch(gameB);
  assert.equal(defaultLaunch.accepted,true);
  const baseline=launchOptions.wineProgramOptions('/prefix/drive_c/games/B/launcher.exe',
    '/prefix/drive_c/games/B','wined3d','dxvk_legacy');
  assert.deepEqual(requests.at(-1),plain(baseline),
    'unconfigured profiles must preserve native launch defaults and empty CPU overrides');
  assert.deepEqual(requests.at(-1).environment,{},'no generic CPU preset may replace native optimizations');
  nodes.set('/prefix/drive_c/games',{type:'dir',ino:202n});
  await service.launch(gameB);
  assert.equal(requests.at(-1).windowsExePath,'/games/B/launcher.exe',
    'a different C: copy must not replace the indexed game');
  storage.set('winehua.d3d.backend','dxvk_modern_2_6');
  await service.launch(gameB);
  assert.equal(requests.at(-1).d3dBackend,'dxvk_modern_2_6');
  assert.equal(requests.at(-1).dxvkBackend,'dxvk_modern_2_6');
  const gpuProfile=service.getProfile(gameB);gpuProfile.openglRenderer='zink';service.saveProfile(gameB,gpuProfile);
  await service.launch(gameB);
  assert.equal(requests.at(-1).openglRenderer,'zink','per-game OpenGL selection must reach native');
  assert.equal(service.getProfile(gameB).openglRenderer,'zink','OpenGL selection must persist');
  console.log('PASS: recursive indexing, persistence, scoped profiles, launch arguments/environment, safe restart, removed files and native launch defaults');
}
serviceTests().catch(err=>{console.error(err);process.exitCode=1;});
