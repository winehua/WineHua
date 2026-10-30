const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');
const ts=require(process.argv[2]||'typescript');
const root=path.resolve(__dirname,'..');
let now=0,nextTimer=1;const timers=new Map();
const context={Date:{now:()=>now},setTimeout:(fn,ms)=>{const id=nextTimer++;timers.set(id,{fn,due:now+ms});return id;},
  clearTimeout:id=>timers.delete(id)};
function compile(source,extra={}) {
  const result={exports:{}};
  const code=ts.transpileModule(source,{compilerOptions:{module:ts.ModuleKind.CommonJS,target:ts.ScriptTarget.ES2020}}).outputText;
  vm.runInNewContext(code,{module:result,exports:result.exports,...context,...extra});return result.exports;
}
const {MinimumKeyHold}=compile(fs.readFileSync(path.join(root,'entry/src/main/ets/common/MinimumKeyHold.ts'),'utf8'));
const view=fs.readFileSync(path.join(root,'entry/src/main/ets/components/TouchControlView.ets'),'utf8');
function method(signature) {
  const start=view.indexOf(signature);assert(start>=0,signature);
  let end=view.indexOf('{',start)+1,depth=1;
  while(depth&&end<view.length){depth+=(view[end]==='{')-(view[end]==='}');end++;}
  assert.equal(depth,0);return view.slice(start,end);
}
const events=[],mouseEvents=[];
const plain=value=>JSON.parse(JSON.stringify(value));
const {Control}=compile(`export class Control {
  controlId='test'; kind='key'; code=28; mods=0; mouseButton=0; finger=-1;
  thumbX=0;thumbY=0;pressed=false;dragging=false;
  keyHold=new MinimumKeyHold(100);
  onKeys=(id,codes)=>events.push([Date.now(),id,Array.from(codes)]);
  onMouseButton=(id,button,down)=>mouseEvents.push([Date.now(),id,button,down]);syncPosition(){};
  ${method('private changedTouch(')}
  ${method('private cancelInput(')}
  ${method('private playTouch(')}
}`,{MinimumKeyHold,events,mouseEvents,TouchType:{Down:0,Up:1,Move:2,Cancel:3}});
function touch(type,id=7,remaining=[]) {
  return {type,changedTouches:[{id,x:10,y:10}],touches:type===0?[{id,x:10,y:10}]:remaining.map(id=>({id,x:10,y:10})),stopPropagation(){}};
}
function advance(time) {
  now=time;
  for(const [id,entry] of [...timers]) if(entry.due<=now){timers.delete(id);entry.fn();}
}
let control=new Control();control.playTouch(touch(0));now=25;control.playTouch(touch(1));
assert.equal(events.length,1,'a fast virtual tap must remain down across a game polling interval');
advance(99);assert.equal(events.length,1);advance(100);
assert.deepEqual(plain(events.at(-1)),[100,'test',[]]);
now=200;control.playTouch(touch(0));now=500;control.playTouch(touch(1));
assert.deepEqual(plain(events.at(-1)),[500,'test',[]],'long holds release immediately');
now=600;control.playTouch(touch(0));now=620;control.playTouch(touch(1));
now=630;control.playTouch(touch(3));const count=events.length;advance(900);
assert.equal(events.length,count,'cancel cancels the pending timer and cannot release a future owner');
assert.deepEqual(plain(events.at(-1)),[630,'test',[]]);
now=1000;control.playTouch(touch(0,7));
const beforeOtherFinger=events.length;control.playTouch(touch(1,9,[7]));
assert.equal(control.finger,7);assert.equal(events.length,beforeOtherFinger,'lifting a different finger must not release this key');
now=1200;control.playTouch(touch(1,7));
now=1300;control.playTouch(touch(0,7));now=1320;control.playTouch(touch(1,7));
now=1340;control.playTouch(touch(0,8));
assert.deepEqual(plain(events.slice(-2)),[[1340,'test',[]],[1340,'test',[28]]],'a new tap flushes the old release before the new down');
control.playTouch(touch(3));advance(1600);assert.equal(timers.size,0);
for(const button of [0x110,0x112,0x111]) {
  control=new Control();control.mouseButton=button;mouseEvents.length=0;
  now+=100;const pressedAt=now;control.playTouch(touch(0,7));
  assert.deepEqual(plain(mouseEvents),[[pressedAt,'test',button,true]],'each mouse button presses immediately without motion');
  advance(now+1500);
  assert.equal(mouseEvents.length,1,'a stationary mouse-button hold never becomes a click or another button');
  control.playTouch(touch(1,9,[7]));assert.equal(mouseEvents.length,1,'another finger cannot release a mouse button');
  control.playTouch(touch(1,7));
  assert.deepEqual(plain(mouseEvents.at(-1)),[now,'test',button,false],'Up immediately releases the same button');
  assert.equal(timers.size,0,'mouse releases do not use keyboard minimum-hold timers');
  control.playTouch(touch(0,7));control.playTouch({type:3,changedTouches:[],touches:[],stopPropagation(){}});
  assert.equal(mouseEvents.at(-1)[3],false,'empty Cancel releases a held mouse button');
}
console.log('PASS: actual touch-control fast tap, immediate long-hold release, cancel/hide timer cleanup, rapid retap and multi-finger ownership');
console.log('PASS: immediate stationary left/middle/right holds and same-button Up/Cancel releases');
