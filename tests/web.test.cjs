const {test}=require('node:test');
const assert=require('node:assert/strict');
const vm=require('node:vm');
const fs=require('node:fs');
const source=fs.readFileSync('src/esp32-rfid/web/app.js','utf8');
function context(fetch=async()=>new Response('{}')){
 const banner={textContent:'',classList:{remove(){}}};
 const c=vm.createContext({fetch,Response,AbortController,setTimeout,clearTimeout,window:{addEventListener(){}},document:{hidden:false,getElementById(){return banner;}}});
 vm.runInContext(source.slice(0,source.indexOf('async function loadStatus')),c);
 return {c,banner};
}
test('escapes_user_and_log_html',()=>{
 const {c}=context();const payload='<svg/onload=alert(1)>';const name='" onmouseover="bad';
 const output=c.renderUsers([{uid:'ABCDEF',name,relay1:true,relay2:false}]);
 assert.ok(!output.includes('data-name=""'));assert.ok(output.includes('&quot;'));
 const logs=c.renderLogs([{ts:1,msg:payload}]);assert.ok(logs.includes('&lt;svg'));assert.ok(!logs.includes('<svg'));
});
test('rejects_http_and_application_failures',async()=>{
 for(const status of [200,400,500]){
  const {c}=context(async()=>new Response('{"ok":false,"error":"save_failed"}',{status,headers:{'Content-Type':'application/json'}}));
  await assert.rejects(c.fetchWithAuth('/settings',{method:'POST'}),/save_failed/);
 }
 const {c}=context(async()=>new Response('{"ok":true}',{headers:{'Content-Type':'application/json'}}));
 assert.equal((await c.fetchWithAuth('/status')).status,200);
});
test('polling_does_not_overlap_or_run_during_upload',async()=>{
 const {c}=context();vm.runInContext('const runningPolls=new Set();',c);
 const start=source.indexOf('async function poll(fn)');const end=source.indexOf('\n}',start)+2;vm.runInContext(source.slice(start,end),c);
 let resolve,calls=0;const job=()=>{++calls;return new Promise(r=>{resolve=r;});};
 const one=c.poll(job);await c.poll(job);assert.equal(calls,1);resolve();await one;
 vm.runInContext('uploadingFirmware=true',c);await c.poll(job);assert.equal(calls,1);
});
