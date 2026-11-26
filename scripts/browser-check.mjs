// Optional local UI smoke test. Start backend on :5001 with a test database,
// then start a temporary Chromium/Edge with --remote-debugging-port=9333.
import {writeFile, mkdir} from 'node:fs/promises';
import assert from 'node:assert/strict';
const base='http://127.0.0.1:5001';
const target=await (await fetch('http://127.0.0.1:9333/json/new?about:blank',{method:'PUT'})).json();
const ws=new WebSocket(target.webSocketDebuggerUrl);
await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
let sequence=0;const pending=new Map(),errors=[];
ws.onmessage=event=>{const message=JSON.parse(event.data);if(message.id){const call=pending.get(message.id);if(call){pending.delete(message.id);clearTimeout(call.timer);message.error?call.reject(new Error(message.error.message)):call.resolve(message.result);}}else if(message.method==='Runtime.exceptionThrown')errors.push(message.params.exceptionDetails.text);};
const cdp=(method,params={})=>new Promise((resolve,reject)=>{const id=++sequence;const timer=setTimeout(()=>{pending.delete(id);reject(new Error('CDP timeout: '+method));},15000);pending.set(id,{resolve,reject,timer});ws.send(JSON.stringify({id,method,params}));});
const evaluate=async expression=>{const r=await cdp('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw new Error(r.exceptionDetails.text);return r.result.value;};
const waitFor=async expression=>{const end=Date.now()+12000;while(Date.now()<end){if(await evaluate(expression))return;await new Promise(r=>setTimeout(r,120));}throw new Error('UI condition timed out: '+expression);};
try{
  await cdp('Runtime.enable');await cdp('Page.enable');
  await cdp('Emulation.setDeviceMetricsOverride',{width:1440,height:1160,deviceScaleFactor:1,mobile:false});
  await cdp('Page.navigate',{url:base});
  await waitFor("document.getElementById('connection')?.textContent.includes('Backend sẵn sàng')");
  assert.equal(await evaluate("document.getElementById('empty-students').hidden"),false);
  assert.equal(await evaluate("document.getElementById('open-door').disabled"),true);
  await evaluate(`(()=>{const f=document.getElementById('student-form');f.elements.student_code.value='SV_TEST_01';f.elements.full_name.value='Sinh viên kiểm thử';f.elements.camera_id.value='CAM01';f.elements.face_id.value='0';f.requestSubmit();})()`);
  await waitFor("document.getElementById('student-rows').textContent.includes('SV_TEST_01')");
  await evaluate(`(()=>{const f=document.getElementById('session-form');f.elements.name.value='Buổi kiểm tra giao diện';f.elements.date.value='2026-11-23';f.requestSubmit();})()`);
  await waitFor("document.getElementById('active-session').textContent==='Buổi đang mở'");
  await evaluate("document.querySelector('#student-rows button').click()");
  await waitFor("document.getElementById('present').textContent==='1'");
  assert.ok(await evaluate("document.getElementById('student-rows').textContent.includes('Thử phần mềm')"));
  await evaluate("document.querySelector('#student-rows button').click()");
  await waitFor("document.getElementById('toast').textContent.includes('đã điểm danh')");
  assert.equal(await evaluate("document.getElementById('present').textContent"),'1');
  await evaluate("document.getElementById('search').value='không tìm thấy';document.getElementById('search').dispatchEvent(new Event('input'))");
  assert.equal(await evaluate("document.querySelectorAll('#student-rows tr').length"),0);
  await evaluate("document.getElementById('search').value='';document.getElementById('search').dispatchEvent(new Event('input'))");
  await mkdir('artifacts',{recursive:true});
  await evaluate("document.getElementById('toast').hidden=true");
  await new Promise(r=>setTimeout(r,300));
  await evaluate("document.getElementById('toast').hidden=true");
  const desktop=await cdp('Page.captureScreenshot',{format:'png',captureBeyondViewport:true});await writeFile('artifacts/dashboard-desktop.png',Buffer.from(desktop.data,'base64'));
  await cdp('Emulation.setDeviceMetricsOverride',{width:390,height:844,deviceScaleFactor:1,mobile:true});
  await waitFor('document.documentElement.clientWidth===390');
  assert.ok(await evaluate('document.documentElement.scrollWidth <= document.documentElement.clientWidth'), JSON.stringify(await evaluate('({width:document.documentElement.clientWidth,scroll:document.documentElement.scrollWidth})')));
  const mobile=await cdp('Page.captureScreenshot',{format:'png',captureBeyondViewport:true});await writeFile('artifacts/dashboard-mobile.png',Buffer.from(mobile.data,'base64'));
  await evaluate("document.getElementById('close-session').click()");
  await waitFor("document.getElementById('active-session').textContent==='Đang xem lịch sử'");
  assert.deepEqual(errors,[]);
  console.log('PASS: empty state, add student, create session, demo, duplicate prevention, filter, close session, mobile overflow, no JS exceptions.');
  console.log('Screenshots: artifacts/dashboard-desktop.png; artifacts/dashboard-mobile.png');
}finally{ws.close();await fetch('http://127.0.0.1:9333/json/close/'+target.id);}

