const fs = require('fs');
const vm = require('vm');
const assert = require('assert');

const elements = new Map();
function element() {
  return {value:'',checked:true,textContent:'',children:[],appendChild(node){this.children.push(node)}};
}
const document = {
  hidden:false,
  getElementById(id){if(!elements.has(id))elements.set(id,element());return elements.get(id)},
  createElement:element,
  head:{appendChild(){}},
};
class Overlay {
  constructor(options){this.options=options;this.events={}}
  on(name,callback){this.events[name]=callback}
  setPosition(position){this.position=position}
  setExtData(data){this.data=data}
  getExtData(){return this.data}
}
class MapMock {
  constructor(){this.events={}}
  on(name,callback){this.events[name]=callback}
  remove(){}
  setFitView(){this.fitted=true}
  setCenter(position){this.center=position}
  lngLatToContainer(position){return {x:position[0]*1000,y:position[1]*1000}}
}
const context = vm.createContext({document,window:{addEventListener(){}},
  AMap:{Map:MapMock,Marker:Overlay,Polyline:Overlay,Pixel:class{},
    InfoWindow:class{setContent(node){this.content=node}open(){}close(){}}},
  setTimeout(){},setInterval(){},requestAnimationFrame(){},performance:{now:()=>0},
  URLSearchParams,fetch:async()=>({ok:true,json:async()=>({items:[]})}),console});
const html = fs.readFileSync(__dirname+'/../viewer_map.html','utf8');
const script = html.split('<script>')[1].split('</script>')[0]
  .replace('__AMAP_CONFIG__','{"key":"test","securityJsCode":"test"}');
vm.runInContext(script,context);
const run = code => vm.runInContext(code,context);

const beijing=run('gcj(116.397,39.908)');
assert(Math.abs(beijing[0]-116.403243)<.00001);
assert(Math.abs(beijing[1]-39.909403)<.00001);
assert.equal(JSON.stringify(run('gcj(-74,40.7)')),'[-74,40.7]');
run(`startMap();draw({device:'device',epoch:'epoch',boot:'boot',raw_count:3,bucket_ms:1000,
  points:[{ms:1000,time:'T+1',lat:31,lon:121,valid:true},
    {ms:2000,time:'T+2',lat:31.001,lon:121.001,valid:true},
    {ms:4000,time:'T+4',lat:31.002,lon:121.002,valid:true}],
  shots:[{ms:2000,time:'T+2',lat:31.001,lon:121.001,valid:true,shot:7},
    {ms:3000,time:'T+3',lat:null,lon:null,valid:false,shot:8}]});`);
assert.equal(run('lines.length'),1);
assert.equal(run('lines[0].options.path.length'),3);
assert.equal(run('markers.length'),3);
assert(run("$('message').textContent").includes('1 次射击无有效定位'));
run("$('speed').value='1';$('play').onclick();lastFrame=0;animate(1000)");
assert.equal(run('playMs'),2000);
assert.equal(run("$('clock').textContent"),'T+2');
run("$('seek').value='3000';$('seek').oninput()");
assert.equal(run("$('clock').textContent"),'T+4');
run("$('reset').onclick()");
assert.equal(run('playMs'),1000);
run('map.events.dragstart()');
assert.equal(run("$('follow').checked"),false);
run('markers[2].events.click()');
assert(run('info.content.textContent').includes('射击 7'));
console.log('Map display, coordinates, markers and replay passed.');
