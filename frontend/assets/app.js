'use strict';
const $ = id => document.getElementById(id);
let health = null, students = [], roster = [], selectedSession = '', selected = null;
let syncing = false, refreshRequested = false, toastTimer, resetting = false;
let cameraStatus = null, webCheckinPending = false;
const sourceLabels = {demo:'Thử phần mềm',web:'Nút web',button:'Nút vật lý',mqtt:'Nút vật lý'};
const timeFormat = value => value ? new Date(value).toLocaleTimeString('vi-VN', {timeZone:'Asia/Ho_Chi_Minh', hour:'2-digit', minute:'2-digit', second:'2-digit'}) : '—';
const dateFormat = new Intl.DateTimeFormat('vi-VN', {dateStyle:'full',timeZone:'Asia/Ho_Chi_Minh'});
const today = () => new Intl.DateTimeFormat('en-CA', {timeZone:'Asia/Ho_Chi_Minh',year:'numeric',month:'2-digit',day:'2-digit'}).format(new Date());
function toast(message, error=false) { $('toast').textContent=message; $('toast').classList.toggle('error',error); $('toast').hidden=false; clearTimeout(toastTimer); toastTimer=setTimeout(()=>$('toast').hidden=true,5000); }
async function api(path, data) {
  const response=await fetch('/api'+path,{method:data===undefined?'GET':'POST',headers:data===undefined?{}:{'Content-Type':'application/json','X-Classroom-Client':'dashboard'},body:data===undefined?undefined:JSON.stringify(data),signal:AbortSignal.timeout(8000)});
  const body=await response.json(); if(!response.ok) throw new Error(body.error||body.message||'Yêu cầu chưa thành công.'); return body;
}
function node(tag, text, className) {const el=document.createElement(tag); if(text!==undefined)el.textContent=text; if(className)el.className=className;return el;}
function renderStudents() {
  const query=$('search').value.trim().toLocaleLowerCase('vi');
  const filter=$('attendance-filter');
  filter.disabled=!selected;
  const listed=selected?roster:students;
  const rows=listed.filter(s=>
    (!selected||filter.value==='all'||s.status===filter.value) &&
    (s.student_code+' '+s.full_name).toLocaleLowerCase('vi').includes(query));
  const fragment=document.createDocumentFragment();
  for(const s of rows){
    const tr=node('tr'), name=node('td'), status=node('td');
    name.append(node('strong',s.full_name),node('small',`${s.camera_id} · Face ID ${s.face_id}`));
    const checkedIn=s.status==='present';
    if(selected)status.append(node('span',checkedIn?'Đã điểm danh':'Chưa điểm danh','pill '+(checkedIn?'success':'neutral')));
    else status.textContent='—';
    tr.append(node('td',s.student_code),name,status,node('td',timeFormat(s.checked_in_at)),node('td',sourceLabels[s.source]||'—'));
    if(health?.demo_mode){const td=node('td');const button=node('button','Thử điểm danh','button small secondary');button.type='button';button.disabled=!selected?.active||s.camera_id!==health.camera_id;button.onclick=async()=>{button.disabled=true;try{const result=await api('/demo/checkin',{face_id:s.face_id});toast(result.message);await refresh();}catch(e){toast(e.message,true);}finally{button.disabled=!selected?.active;}};td.append(button);tr.append(td);}
    fragment.append(tr);
  }
  $('student-rows').replaceChildren(fragment);
  $('empty-students').hidden=listed.length>0;
  $('empty-filter').hidden=!listed.length||rows.length>0;
  $('row-count').textContent=`${rows.length} / ${listed.length} sinh viên`;
}
function renderCheckinAvailability() {
  const button = $('web-checkin');
  const status = $('web-checkin-status');
  const cameraReady = cameraStatus?.camera_ready && cameraStatus.storage_ready &&
    !cameraStatus.enrollment_locked && cameraStatus.face_count > 0;
  const active = selected?.active && String(selected.id) === selectedSession;
  const needsSession = !selectedSession;
  const cameraBusy = cameraStatus?.enrolling || cameraStatus?.result === 'recognizing';
  const holdingPhoto = !!cameraStatus?.checkin_snapshot?.id;
  button.textContent = needsSession ? 'Tạo buổi để điểm danh' : 'Chụp và điểm danh';
  button.disabled = webCheckinPending || resetting || (!needsSession &&
    (!active || !cameraReady || cameraStatus?.checkin_protocol !== 1 || cameraBusy || holdingPhoto));
  if (webCheckinPending) return;
  if (resetting) status.textContent = 'Đang reset kết quả của buổi học.';
  else if (needsSession) status.textContent = 'Tạo và mở một buổi học để lưu kết quả điểm danh.';
  else if (!active) status.textContent = 'Chọn buổi đang mở hoặc bấm Mở buổi ở bên cạnh.';
  else if (!cameraStatus) status.textContent = 'Nhập IP và kết nối ESP32-CAM để điểm danh.';
  else if (cameraStatus.checkin_protocol !== 1) status.textContent = 'Nạp firmware ESP32-CAM mới để dùng nút web.';
  else if (cameraStatus.enrolling) status.textContent = 'Camera đang đăng ký khuôn mặt. Chờ thu mẫu xong rồi chụp.';
  else if (!cameraReady) status.textContent = 'Kiểm tra trạng thái camera và đăng ký khuôn mặt trước.';
  else {
    if (!status.dataset.result) status.textContent = cameraBusy || holdingPhoto
      ? 'Chờ lượt chụp hiện tại rồi bấm lại.'
      : 'Nhìn thẳng, bấm nút và làm theo hướng dẫn dưới ảnh camera.';
    return;
  }
  delete status.dataset.result;
}
window.addEventListener('camera-status', event => {
  cameraStatus = event.detail;
  renderCheckinAvailability();
});
$('web-checkin').onclick = async () => {
  if ($('web-checkin').disabled) return;
  if (!selectedSession) {
    window.location.hash = 'session-form';
    $('session-form').elements.name.focus({preventScroll:true});
    return;
  }
  if (!selected?.active || String(selected.id) !== selectedSession) return;
  const status = $('web-checkin-status');
  webCheckinPending = true;
  delete status.dataset.result;
  status.textContent = 'Đang yêu cầu ESP32-CAM chụp. Giữ mặt trong khung và làm theo hướng dẫn.';
  renderCheckinAvailability();
  try {
    const {event_id} = await api('/checkins', {session_id:selected.id, generation:selected.generation});
    const deadline = Date.now() + 72000;
    while (Date.now() < deadline) {
      let result;
      try { result = await api('/checkins/' + encodeURIComponent(event_id)); }
      catch (error) {
        status.textContent = 'Đang chờ kết nối để kiểm tra kết quả.';
      }
      if (result && result.status !== 'pending') {
        status.textContent = result.message;
        await refresh();
        return;
      }
      await new Promise(resolve => setTimeout(resolve, 1000));
    }
    status.textContent = 'Chưa nhận được kết quả. Kiểm tra camera rồi thử lại.';
  } catch (error) {
    status.textContent = error.message;
  } finally {
    status.dataset.result = 'true';
    webCheckinPending = false;
    renderCheckinAvailability();
  }
};
function renderHealth() {
  const d=health.device,t=d.telemetry,m=health.mqtt;
  $('connection').textContent=m.enabled?(m.connected?'MQTT đã kết nối':'MQTT chưa kết nối'):'Backend sẵn sàng · MQTT đang tắt';
  $('connection-dot').classList.toggle('online',!m.enabled||m.connected);$('demo-banner').hidden=!health.demo_mode;$('demo-heading').hidden=!health.demo_mode;
  $('device-badge').textContent=d.online?'Trực tuyến':m.enabled?'Chưa trực tuyến':'MQTT đang tắt';$('device-badge').className='pill '+(d.online?'success':'neutral');
  const ipLabel = d.online || !t.station_ip ? 'IP DevKit' : 'IP DevKit gần nhất';
  $('devkit-ip').textContent = `${ipLabel} · ${t.station_ip ?? 'Chờ thiết bị gửi IP'}`;
  $('lux').textContent=t.lux===undefined?'—':Number(t.lux).toFixed(1);$('gas').textContent=t.mq2_raw===undefined?'—':t.mq2_raw;$('led').textContent=t.led_percent===undefined?'—':t.led_percent;
  $('gas-status').textContent=t.gas_alarm?'Đang cảnh báo':t.gas_sensor_ready?'Cảm biến đã sẵn sàng':t.gas_sensor_ready===false?'Đang chờ cảm biến ổn định':'Chưa có dữ liệu MQ-2';
  $('gas-banner').hidden=!t.gas_alarm;$('door').textContent=t.door==='open'?'Cửa đang mở':t.door==='closed'?'Cửa đã đóng':'Chưa rõ trạng thái';
  $('door-detail').textContent=t.moving?'Servo đang chuyển động':d.online?'Đã nhận phản hồi từ ESP32':d.updated_at?`Dữ liệu cuối lúc ${timeFormat(d.updated_at)}`:'Chờ phản hồi từ ESP32';
  $('open-door').disabled=!d.online||!m.connected;$('close-door').disabled=!d.online||!m.connected||!!t.gas_alarm;
  if(m.error){$('error-banner').textContent=m.error;$('error-banner').hidden=false;}
}
async function refresh() {
  if(syncing){refreshRequested=true;return;}syncing=true;
  try{
    const [h,s,ss,events]=await Promise.all([api('/health'),api('/students'),api('/sessions'),api('/events')]);health=h;students=s;
    if(!selectedSession||!ss.some(x=>String(x.id)===selectedSession))selectedSession=String(ss.find(x=>x.active)?.id||ss[0]?.id||'');
    const options=ss.map(x=>{const el=node('option',`${x.name} · ${x.date}${x.active?' · Đang mở':''}`);el.value=x.id;return el;});
    if(!options.length){const el=node('option','Chưa có buổi học');el.value='';options.push(el);}$('session-select').replaceChildren(...options);$('session-select').value=selectedSession;
    const requestedSession=selectedSession;
    const attendance=await api('/attendance'+(requestedSession?'?session_id='+encodeURIComponent(requestedSession):''));
    if(requestedSession!==selectedSession)return;
    selected=attendance.session;roster=attendance.students;
    $('present').textContent=selected?attendance.present:'—';$('total').textContent=selected?`/ ${attendance.total} sinh viên có mặt`:'Chưa xem kết quả';$('attendance-progress').style.width=(attendance.total?100*attendance.present/attendance.total:0)+'%';
    $('session-caption').textContent=selected?dateFormat.format(new Date(selected.date)):'Chưa chọn buổi học';$('active-session').textContent=selected?.active?'Buổi đang mở':selected?'Đang xem lịch sử':'Chưa mở điểm danh';$('active-session').className='pill '+(selected?.active?'success':'neutral');
    $('activate-session').disabled=!selected||!!selected.active;$('close-session').disabled=!selected?.active;$('export').disabled=!selected;
    $('reset-attendance').disabled=!selected||resetting;
    renderCheckinAvailability();
    const lastButton=events.find(e=>e.kind.startsWith('button:')||e.kind.startsWith('mqtt:accepted')||e.kind.startsWith('mqtt:rejected')||e.kind.startsWith('mqtt:duplicate'));
    if(lastButton)$('button-checkin-status').textContent='Nút vật lý lúc '+timeFormat(lastButton.created_at)+' · '+lastButton.message;
    $('events').replaceChildren(...(events.length?events.map(e=>{const li=node('li',e.message);const label=sourceLabels[e.kind.split(':')[0]];if(label)li.prepend(node('span',label+' · ','muted'));li.append(node('time',timeFormat(e.created_at)));return li;}):[node('li','Chưa có hoạt động.','muted')]));
    $('error-banner').hidden=true;renderHealth();renderStudents();$('updated').textContent='Cập nhật '+timeFormat(h.time);
  }catch(e){$('error-banner').textContent='Không cập nhật được dữ liệu: '+e.message;$('error-banner').hidden=false;$('connection').textContent='Mất kết nối backend';$('connection-dot').classList.remove('online');$('open-door').disabled=true;$('close-door').disabled=true;}
  finally{syncing=false;if(refreshRequested){refreshRequested=false;refresh();}}
}
function formHandler(id,path,transform=x=>x){$(id).addEventListener('submit',async event=>{event.preventDefault();const form=event.currentTarget,button=form.querySelector('button[type="submit"]');button.disabled=true;try{const body=Object.fromEntries(new FormData(form));const result=await api(path,transform(body));if(id==='student-form'){form.reset();form.elements.camera_id.value=health?.camera_id||'CAM01';}else selectedSession=String(result.id);toast(id==='student-form'?'Đã thêm sinh viên.':'Đã mở buổi học.');await refresh();}catch(e){toast(e.message,true);}finally{button.disabled=false;}});}
formHandler('student-form','/students',x=>({...x,face_id:Number(x.face_id)}));formHandler('session-form','/sessions');
$('search').oninput=renderStudents;
$('attendance-filter').onchange=renderStudents;
$('clear-filters').onclick=()=>{$('search').value='';$('attendance-filter').value='all';renderStudents();$('search').focus();};
$('session-select').onchange=async e=>{
  selectedSession=e.target.value;
  $('attendance-filter').value='all';
  delete $('web-checkin-status').dataset.result;
  for(const id of ['activate-session','close-session','reset-attendance','export','attendance-filter'])$(id).disabled=true;
  renderCheckinAvailability();
  await refresh();
};
$('activate-session').onclick=async()=>{try{await api(`/sessions/${selectedSession}/activate`,{});toast('Đã mở buổi học đã chọn.');await refresh();}catch(e){toast(e.message,true);}};
$('close-session').onclick=async()=>{try{await api('/sessions/close',{});toast('Đã kết thúc điểm danh.');await refresh();}catch(e){toast(e.message,true);}};
$('reset-attendance').onclick=async()=>{
  if(!selected||resetting)return;
  const session=selected;
  if(!confirm(`Reset điểm danh buổi “${session.name}” (${session.date})? Kết quả của riêng buổi này sẽ bị xóa. Danh sách sinh viên và khuôn mặt trên camera được giữ nguyên.`))return;
  resetting=true;$('reset-attendance').disabled=true;renderCheckinAvailability();
  try{const result=await api(`/sessions/${session.id}/reset`,{confirm:true});toast(result.message);await refresh();}catch(e){toast(e.message,true);}finally{resetting=false;$('reset-attendance').disabled=!selected;renderCheckinAvailability();}
};
window.addEventListener('enrollment-completed',()=>refresh());
$('export').onclick=()=>{if(selectedSession)window.location.assign('/api/attendance/export.csv?session_id='+encodeURIComponent(selectedSession));};
for(const action of ['open','close'])$(action+'-door').onclick=async()=>{try{const result=await api('/device/door',{door:action});toast(result.message);}catch(e){toast(e.message,true);}};
$('session-date').value=today();setInterval(()=>$('clock').textContent=timeFormat(new Date().toISOString()),1000);refresh();setInterval(()=>{if(!syncing)refresh();},2500);
