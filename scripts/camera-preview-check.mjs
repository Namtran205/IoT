// UI QA only: an HTTP mock replaces the ESP32-CAM, never the real inference.
// Start a temporary Chromium/Edge with --remote-debugging-port=9333 first.
import {readFile, writeFile, mkdir} from 'node:fs/promises';
import {createServer} from 'node:http';
import assert from 'node:assert/strict';

const source = await readFile('firmware/esp32_cam/SmartClassroomCam/camera_page.h', 'utf8');
const html = source.match(/R"CAMHTML\(([\s\S]*?)\)CAMHTML";/)?.[1];
assert.ok(html, 'Camera HTML must be extracted from the compiled source');
const start = Date.now(), requests = [];
let frameRequests = 0, enrollmentTimer = null, statusUnavailable = false;
const state = {camera_ready:true, storage_ready:true, enrollment_locked:false, enrolling:false,
  face_count:0, samples:0, detected_faces:0, last_face_id:-1, frame_number:1,
  frame_at:0, processing_ms:780, detection:'no_face', result:'idle', free_heap:105000,
  reset_reason:'power_on', station_ip:'', inference:'ESP32-CAM'};
const image = `<svg xmlns="http://www.w3.org/2000/svg" width="640" height="480"><rect width="640" height="480" fill="#183641"/><rect x="195" y="65" width="250" height="335" fill="#325963" stroke="#56cf94" stroke-width="4"/><text x="320" y="235" text-anchor="middle" fill="white" font-family="Arial" font-size="24">UI TEST</text><text x="320" y="270" text-anchor="middle" fill="#c5e0da" font-family="Arial" font-size="16">Mock frame / no camera connected</text></svg>`;
const server = createServer((req,res) => {
  const path = new URL(req.url, 'http://localhost').pathname;
  requests.push({method:req.method,path});
  res.setHeader('Cache-Control','no-store');
  if (path === '/') {res.setHeader('Content-Type','text/html; charset=utf-8');return res.end(html);}
  if (path === '/frame') {frameRequests++;state.frame_number++;state.frame_at=Date.now()-start;res.setHeader('Content-Type','image/svg+xml');return res.end(image);}
  res.setHeader('Content-Type','application/json');
  if (path === '/api/status') {
    if (statusUnavailable) {res.statusCode=503;return res.end('{"error":"test_network_failure"}');}
    return res.end(JSON.stringify({...state,uptime_ms:Date.now()-start}));
  }
  if (req.method !== 'POST' || req.headers['x-camera-control'] !== 'local') {res.statusCode=403;return res.end('{"error":"invalid_client"}');}
  res.statusCode=202;
  if (path === '/api/enroll') {
    Object.assign(state,{enrolling:true,samples:0,result:'enrolling',detection:'ok',detected_faces:1});
    enrollmentTimer=setInterval(()=>{state.samples++;if(state.samples===5){clearInterval(enrollmentTimer);Object.assign(state,{enrolling:false,face_count:state.face_count+1,result:'enrolled',last_face_id:state.face_count});}},900);
  } else if (path === '/api/cancel') {
    clearInterval(enrollmentTimer);Object.assign(state,{enrolling:false,enrollment_locked:state.samples>0,result:'enrollment_cancelled'});
  } else if (path === '/api/test') Object.assign(state,{result:'recognized',last_face_id:0});
  else if (path === '/api/rotate') state.result='rotated';
  else if (path === '/api/restart') Object.assign(state,{result:'idle',samples:0,enrollment_locked:false});
  else if (path === '/api/clear') Object.assign(state,{result:'idle',samples:0,enrollment_locked:false,face_count:0,last_face_id:-1});
  else if (path !== '/api/radio/off') {res.statusCode=404;return res.end('{"error":"unknown_action"}');}
  res.end('{"queued":true}');
});
await new Promise(resolve=>server.listen(5002,'127.0.0.1',resolve));
let ws, target;
const pending = new Map(), errors = [];
try {
  target=await (await fetch('http://127.0.0.1:9333/json/new?about:blank',{method:'PUT'})).json();
  ws=new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
  let sequence=0;
  ws.onmessage=event=>{const message=JSON.parse(event.data);if(message.id){const call=pending.get(message.id);if(call){pending.delete(message.id);clearTimeout(call.timer);message.error?call.reject(new Error(message.error.message)):call.resolve(message.result);}}else if(message.method==='Runtime.exceptionThrown')errors.push(message.params.exceptionDetails.text);};
  const cdp=(method,params={})=>new Promise((resolve,reject)=>{const id=++sequence;const timer=setTimeout(()=>{pending.delete(id);reject(new Error('CDP timeout: '+method));},15000);pending.set(id,{resolve,reject,timer});ws.send(JSON.stringify({id,method,params}));});
  const evaluate=async expression=>{const r=await cdp('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true});if(r.exceptionDetails)throw new Error(r.exceptionDetails.text);return r.result.value;};
  const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));
  const waitFor=async expression=>{const end=Date.now()+12000;while(Date.now()<end){if(await evaluate(expression))return;await pause(120);}throw new Error('UI condition timed out: '+expression);};
  const click=async id=>evaluate(`document.getElementById(${JSON.stringify(id)}).click()`);
  await cdp('Runtime.enable');await cdp('Page.enable');
  await cdp('Emulation.setDeviceMetricsOverride',{width:1440,height:1120,deviceScaleFactor:1,mobile:false});
  await cdp('Page.navigate',{url:'http://127.0.0.1:5002'});
  await waitFor("document.getElementById('notice')?.textContent.includes('Đã kết nối')");
  await waitFor("document.getElementById('image').naturalWidth===640");
  assert.equal(await evaluate("document.getElementById('test').disabled"),true);
  assert.ok(await evaluate("document.getElementById('detection').textContent.includes('Chưa phát hiện')"));
  statusUnavailable=true;
  await waitFor("document.getElementById('notice').textContent.includes('Mobile Hotspot')");
  assert.ok(await evaluate("document.getElementById('notice').textContent.includes(location.origin)"));
  statusUnavailable=false;
  await waitFor("document.getElementById('notice').textContent.includes('Đã kết nối')");
  state.detection='not_aligned';
  await waitFor("document.getElementById('detection').textContent.includes('chưa căn chỉnh')");
  await click('enroll');
  await waitFor("document.getElementById('sample-count').textContent==='2 / 5'");
  assert.equal(await evaluate("document.getElementById('enroll').disabled"),true);
  assert.equal(await evaluate("document.getElementById('cancel').hidden"),false);
  await waitFor("document.getElementById('result').textContent.includes('Face ID: 0')");
  assert.equal(await evaluate("document.getElementById('face-count').textContent"),'1 / 7');
  await click('test');await waitFor("document.getElementById('result').textContent.includes('Đã nhận ra')");
  await click('preview');await pause(800);const pausedCount=frameRequests;await pause(1200);
  assert.equal(frameRequests,pausedCount,'Pausing preview must stop frame requests');
  assert.equal(await evaluate("document.getElementById('placeholder').hidden"),false);
  await click('preview');await pause(900);assert.ok(frameRequests>pausedCount);
  await click('rotate');await waitFor("document.getElementById('result').textContent.includes('180°')");
  state.result='enrolled';state.last_face_id=0;
  await waitFor("document.getElementById('result').textContent.includes('Đã lưu')");
  await mkdir('artifacts',{recursive:true});
  let shot=await cdp('Page.captureScreenshot',{format:'png',captureBeyondViewport:true});
  await writeFile('artifacts/camera-preview-desktop.png',Buffer.from(shot.data,'base64'));
  await cdp('Emulation.setDeviceMetricsOverride',{width:390,height:844,deviceScaleFactor:1,mobile:true});
  await waitFor('document.documentElement.clientWidth===390');
  assert.ok(await evaluate('document.documentElement.scrollWidth<=document.documentElement.clientWidth'));
  shot=await cdp('Page.captureScreenshot',{format:'png',captureBeyondViewport:true});
  await writeFile('artifacts/camera-preview-mobile.png',Buffer.from(shot.data,'base64'));
  await click('enroll');await waitFor("document.getElementById('sample-count').textContent==='1 / 5'");
  await click('cancel');await waitFor("document.getElementById('notice').textContent.includes('bị dở')");
  assert.equal(await evaluate("document.getElementById('test').disabled"),true);
  await evaluate('window.confirm=()=>true');
  await click('restart');await waitFor("!document.getElementById('test').disabled");
  state.reset_reason='brownout';await waitFor("document.getElementById('notice').textContent.includes('sụt áp')");
  state.reset_reason='power_on';await click('clear');
  await waitFor("document.getElementById('face-count').textContent==='0 / 7'");
  await click('radio-off');await waitFor("document.getElementById('notice').textContent.includes('Đã yêu cầu tắt Wi-Fi')");
  await pause(1100);const stoppedCount=requests.length;await pause(1300);assert.equal(requests.length,stoppedCount);
  assert.ok(!requests.some(r=>r.path.includes('attendance')||r.path.includes('checkin')));
  assert.deepEqual(errors,[]);
  console.log('PASS: real camera HTML with mocked hardware; preview, network loss/recovery with current origin, no_face/not_aligned messages, enrollment 5/5, Face ID 0, test, pause/resume, rotate, cancel lock, restart, brownout, clear, radio off, mobile overflow, no JS exceptions.');
  console.log('Screenshots use a clearly labelled mock frame, not real camera output.');
} finally {
  clearInterval(enrollmentTimer);
  for(const call of pending.values())clearTimeout(call.timer);
  ws?.close();
  if(target)await fetch('http://127.0.0.1:9333/json/close/'+target.id).catch(()=>{});
  await new Promise(resolve=>server.close(resolve));
}
