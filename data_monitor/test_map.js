// Exercise map interactions without making API requests or exposing a real key.
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const elements=new Map();
function element(){return {value:'',checked:true,textContent:'',children:[],style:{},
  appendChild(x){this.children.push(x)}};}
const document={hidden:false,getElementById(id){if(!elements.has(id))elements.set(id,element());return elements.get(id)},
  createElement:element,head:{appendChild(){}}};
class Overlay{
  constructor(options){this.options=options;this.events={}}
  on(name,callback){this.events[name]=callback}
  setPosition(p){this.position=p}
  setExtData(p){this.data=p}
  getExtData(){return this.data}
}
class MapMock{
  constructor(){this.events={}}
  on(name,callback){this.events[name]=callback}
  remove(){}
  setFitView(){this.fitted=true}
  setCenter(p){this.center=p}
  lngLatToContainer(p){return {x:p[0]*1000,y:p[1]*1000}}
}
const context=vm.createContext({document,window:{addEventListener(){}},qt:{webChannelTransport:{}},
  QWebChannel:function(){},AMap:{Map:MapMock,Marker:Overlay,Polyline:Overlay,
  Pixel:class{},InfoWindow:class{setContent(p){this.content=p}open(){}close(){}}},
  setTimeout(){},setInterval(){},requestAnimationFrame(){},performance:{now:()=>0},console});
let html=fs.readFileSync(__dirname+'/trajectory_map.html','utf8');
const script=html.split('<script>')[1].split('</script>')[0].replace('__AMAP_CONFIG__','{"key":"test","securityJsCode":"test"}');
vm.runInContext(script,context);
function run(code){return vm.runInContext(code,context)}
const beijing=run('gcj(116.397,39.908)');
assert(Math.abs(beijing[0]-116.403243)<.00001);
assert(Math.abs(beijing[1]-39.909403)<.00001);
assert.equal(JSON.stringify(run('gcj(-74,40.7)')),'[-74,40.7]');
run(`startMap();draw({device:'a',boot:'b',bucket_ms:1000,points:[
  {ms:1000,time:'T+1',lat:31,lon:121,valid:true},
  {ms:2000,time:'T+2',lat:31.001,lon:121.001,valid:true},
  {ms:3000,time:'T+3',lat:null,lon:null,valid:false},
  {ms:4000,time:'T+4',lat:31.002,lon:121.002,valid:true},
  {ms:5000,time:'T+5',lat:31.003,lon:121.003,valid:true}],
  shots:[{ms:2000,time:'T+2',lat:31.001,lon:121.001,valid:true,shot:1}]});`);
assert.equal(run('lines.length'),2,'GPS outage must break the line');
assert.equal(run('markers.length'),3,'start, current, shot');
run("$('speed').value='1';$('play').onclick();lastFrame=0;animate(1000)");
assert.equal(run('playMs'),2000);
assert.equal(run("$('clock').textContent"),'T+2');
run("$('play').onclick()");assert.equal(run('playing'),false);
run("$('seek').value='3000';$('seek').oninput()");
assert.equal(run("$('clock').textContent"),'T+4');
run("$('reset').onclick()");assert.equal(run('playMs'),1000);
run("map.events.dragstart()");assert.equal(run("$('follow').checked"),false);
run("markers[2].events.click()");assert(run('info.content.textContent').includes('射击 1'));
run("select(JSON.stringify(['a','new-boot']))");assert.equal(run('points.length'),0);
assert.equal(run('replaying'),false);
run(`bridge={sessions(cb){cb(JSON.stringify([{device:'device',boot:'boot',label:'Session'}]))},
  current(cb){cb('null')},requestTrack(device,boot,generation,known){
    trackReady(JSON.stringify({device,boot,revision:[1,1,1],bucket_ms:1000,
      points:[{ms:1000,time:'T+1',lat:31,lon:121,valid:true}],shots:[]}),generation);
  }};refresh(true);`);
assert.equal(run('selected'),'["device","boot"]');
assert.equal(run('points.length'),1);
console.log('Map tests passed: coordinate conversion, gaps, markers, replay, seek, session switch.');
