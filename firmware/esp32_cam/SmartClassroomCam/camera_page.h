#pragma once
#include <Arduino.h>

static const char CAMERA_PAGE[] PROGMEM = R"CAMHTML(<!doctype html>
<html lang="vi"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Camera · Smart Classroom</title>
<style>
:root{font-family:"Segoe UI",Arial,sans-serif;color:#19333f;background:#f1f5f7;font-size:16px}*{box-sizing:border-box}body{margin:0}button{font:inherit;cursor:pointer}button:disabled{opacity:.45;cursor:not-allowed}[hidden]{display:none!important}header{background:#142f3b;color:#fff;padding:20px max(20px,calc((100vw - 1160px)/2));display:flex;align-items:center;justify-content:space-between;gap:15px}header strong{font-size:20px}header small{display:block;color:#b3cbd3;margin-top:5px;font-size:13px}.badge{font-size:14px;padding:7px 11px;border-radius:20px;background:#e7f5ed;color:#286445;white-space:nowrap}main{max-width:1160px;margin:auto;padding:25px 20px}h1{font-size:28px;letter-spacing:-.6px;margin:0 0 8px}h2{font-size:20px;margin:0 0 15px}p{line-height:1.65;margin:8px 0;color:#59717d}.grid{display:grid;grid-template-columns:minmax(0,1.6fr) minmax(290px,1fr);gap:22px;margin-top:20px;align-items:start}.panel{background:#fff;border:1px solid #dfe7eb;border-radius:16px;padding:20px;min-width:0}.stage{position:relative;background:#0d1d26;aspect-ratio:4/3;border-radius:12px;overflow:hidden;display:grid;place-items:center}.stage img{width:100%;height:100%;object-fit:contain;position:absolute;inset:0}.guide{position:absolute;width:58%;height:84%;border:2px dashed #fff7;border-radius:46%;pointer-events:none}.placeholder{color:#bbced7;padding:20px;text-align:center;position:absolute;inset:0;display:grid;place-items:center;font-size:15px}.placeholder span{background:#0d1d26e8;border-radius:10px;padding:18px}.row{display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap}.controls{display:flex;gap:10px;flex-wrap:wrap;margin:16px 0 12px}.btn{background:#087969;color:white;border:1px solid transparent;border-radius:9px;padding:11px 15px;font-size:15px;font-weight:600}.btn.secondary{color:#294958;border-color:#cfdde2;background:white}.btn.danger{color:#9b3440;border-color:#e5cbd0;background:white}.btn.full{width:100%;margin:8px 0}.hint{font-size:14px;color:#6e838c;line-height:1.7}.notice{border:1px solid #cbdde8;background:#eaf3fa;color:#335c77;border-radius:10px;padding:13px 16px;font-size:15px;line-height:1.7;margin:16px 0}.notice.error{background:#fff0ef;border-color:#edc4c0;color:#8e3e36}.summary{display:flex;gap:13px;margin:12px 0 20px}.metric{flex:1;background:#f1f6f5;padding:14px;border-radius:10px}.metric small{display:block;font-size:13px;color:#637e82}.metric strong{display:block;font-size:26px;margin-top:8px}.progress{height:8px;background:#e6efeb;border-radius:10px;overflow:hidden;margin:12px 0}.progress span{height:100%;display:block;background:#159a72;width:0;transition:width .2s}.state{font-size:16px;font-weight:600;line-height:1.7;min-height:54px;color:#1b6251}ol{margin:14px 0;padding-left:24px;color:#4d6571;font-size:15px;line-height:1.8}details{margin-top:20px;border-top:1px solid #e1e9ed;padding-top:16px}summary{cursor:pointer;font-size:15px;font-weight:600}.tools{display:flex;gap:10px;flex-wrap:wrap;margin-top:15px}dl{font-size:14px;display:grid;grid-template-columns:1fr 1fr;gap:10px}dt{color:#6b818c}dd{margin:0;text-align:right}footer{font-size:14px;color:#637c86;line-height:1.8;margin:20px 0}label{font-size:14px;display:flex;gap:7px;align-items:center}button:focus-visible,summary:focus-visible{outline:3px solid #69b49b;outline-offset:3px}@media(max-width:760px){.grid{grid-template-columns:1fr}header{align-items:flex-start}header .badge{font-size:12px}h1{font-size:24px}main{padding:20px 14px}.panel{padding:16px}.btn{font-size:14px;padding:10px 12px}}
</style></head><body>
<header><div><strong>Smart Classroom · Camera</strong><small>Căn mặt và đăng ký sinh viên</small></div><span class="badge">AI trên ESP32-CAM</span></header>
<main><h1>Nhìn thấy khung hình, đăng ký đúng người.</h1><p>Ảnh lấy trực tiếp từ ESP32-CAM. Đăng ký khuôn mặt một lần. Khi điểm danh, sinh viên nhấn nút vật lý trên ESP32 trung tâm.</p>
<div id="notice" class="notice" role="status">Đang kết nối camera…</div>
<p class="hint">Để nhập mã sinh viên, họ tên và tự cập nhật danh sách, mở <a href="http://127.0.0.1:5000/#enrollment">web quản lý trên laptop</a>, nhập IP camera ở đó rồi đăng ký. Các nút bên dưới dùng để kiểm tra camera riêng.</p>
<div class="grid"><section class="panel"><div class="row"><h2>Hình xem trước</h2><label><input id="show-guide" type="checkbox" checked>Vùng căn mặt</label></div>
<div class="stage"><img id="image" alt="Hình xem trước từ ESP32-CAM" hidden><div id="guide" class="guide"></div><div id="placeholder" class="placeholder"><span>Đang chờ ảnh đầu tiên…</span></div></div>
<div class="controls"><button id="preview" class="btn secondary" type="button">Dừng xem</button><button id="rotate" class="btn secondary" type="button">Xoay ảnh 180°</button></div>
<p id="detection" class="state">Đang chờ camera phát hiện khuôn mặt…</p><p class="hint">Khung nét đứt là vùng gợi ý để căn mặt. Khung nét liền vẽ trên ảnh mới là kết quả phát hiện từ camera. Ảnh cập nhật từng khung; có thể chậm hơn khi AI đang xử lý.</p>
<p class="hint" id="frame-info">Chưa có khung hình được xử lý.</p></section>
<section class="panel"><h2>Đăng ký khuôn mặt</h2><div class="summary"><div class="metric"><small>Người đã lưu</small><strong id="face-count">— / 7</strong></div><div class="metric"><small>Mẫu đang thu</small><strong id="sample-count">0 / 5</strong></div></div>
<div class="progress"><span id="progress"></span></div><p id="result" class="state">Giữ một khuôn mặt trong vùng nhìn thấy.</p>
<button id="enroll" type="button" class="btn full">Đăng ký riêng trên camera</button><button id="cancel" type="button" class="btn secondary full" hidden>Dừng đăng ký</button><button id="test" type="button" class="btn secondary full">Thử nhận diện trên camera</button>
<ol><li>Căn toàn bộ khuôn mặt trong ảnh, nơi đủ sáng.</li><li>Nhấn “Đăng ký người mới”, giữ mặt cho đủ 5 mẫu.</li><li>Ghi lại Face ID được lưu và liên kết đúng sinh viên trên web quản lý.</li><li>Khi điểm danh, dùng nút trên ESP32 trung tâm.</li></ol>
<p class="hint">“Thử nhận diện” chỉ kiểm tra camera; không tạo bản ghi điểm danh hoặc ra lệnh mở cửa.</p>
<details><summary>Thông tin và công cụ</summary><dl><dt>Uptime</dt><dd id="uptime">—</dd><dt>Lần khởi động</dt><dd id="reset">—</dd><dt>Heap trống</dt><dd id="heap">—</dd><dt>IP mạng nhà / hotspot</dt><dd id="station">Chưa kết nối</dd></dl><div class="tools"><button id="restart" class="btn secondary" type="button">Khởi động lại</button><button id="clear" class="btn danger" type="button">Xóa tất cả khuôn mặt</button><button id="radio-off" class="btn secondary" type="button">Tắt Wi-Fi camera</button></div><p class="hint">Tắt Wi-Fi sẽ đóng trang này. Nút điểm danh qua UART vẫn hoạt động. Nhấn Reset trên mạch để kết nối lại Wi-Fi đã cấu hình.</p></details>
</section></div><footer>Trang này do ESP32-CAM cung cấp. Khi xem trước, ảnh được gửi tới trình duyệt để bạn căn mặt; phát hiện và nhận diện vẫn chạy trên chip. Dừng xem hoặc tắt Wi-Fi sau khi đăng ký nếu không cần truyền ảnh.</footer></main>
<script>
'use strict';
const $=id=>document.getElementById(id);
let preview=true,stopped=false,actionPending=false,hasImage=false,objectUrl=null,lastUptime=null,failures=0;
let checkinSnapshot=null;
const lightAdvice={dark:'Vùng mặt tối. Chiếu thêm ánh sáng dịu từ phía trước.',bright:'Vùng mặt quá sáng. Tránh đèn chiếu thẳng hoặc nắng gắt.',uneven:'Ánh sáng không đều. Xoay về phía đèn và tránh để cửa sổ sáng sau lưng.'};
const words={waiting:'Đang chờ ảnh.',ok:'Đã thấy một khuôn mặt và căn chỉnh được. Có thể đăng ký.',no_face:'Chưa phát hiện khuôn mặt. Đưa toàn bộ mặt vào ảnh và điều chỉnh khoảng cách / ánh sáng.',not_aligned:'Đã thấy mặt nhưng chưa căn chỉnh được. Nhìn thẳng và giữ đủ mắt, mũi, miệng trong ảnh.',multiple_faces:'Có nhiều khuôn mặt. Chỉ để một người trong khung.',capture_error:'Chưa lấy được ảnh. Kiểm tra nguồn và cáp camera.',decode_error:'Không chuyển đổi được ảnh camera.',memory_error:'Camera thiếu bộ nhớ.',error:'Camera báo lỗi; kiểm tra nguồn và thông tin khởi động.'};
const messages={idle:'Sẵn sàng. Đăng ký người mới hoặc dùng nút điểm danh trên ESP32 trung tâm.',initializing:'Camera đang khởi tạo.',enrolling:'Đang đăng ký. Giữ mặt trong khung, chờ đủ 5 mẫu.',enrolled:'Đã lưu khuôn mặt vào bộ nhớ.',recognizing:'Đang nhận diện ngay trên camera…',recognized:'Đã nhận ra người đã đăng ký.',unknown:'Chưa nhận ra danh tính. Đây có thể là người chưa đăng ký.',not_enrolled:'Chưa có khuôn mặt được lưu. Đăng ký người đầu tiên.',enrollment_timeout:'Đăng ký hết thời gian. Khởi động lại camera rồi thử lại.',enrollment_cancelled:'Đã dừng đăng ký.',enrollment_locked:'Cần khởi động lại camera trước khi đăng ký tiếp.',full:'Đã đủ 7 người. Không thể thêm ID mới.',storage_error:'Không đọc/ghi được dữ liệu khuôn mặt. Kiểm tra thông tin trước khi dùng công cụ xóa.',psram_error:'Không thấy PSRAM. Kiểm tra board và phần cứng.',camera_error:'Khởi tạo camera thất bại. Kiểm tra nguồn và cáp camera.',rotated:'Đã xoay ảnh camera 180°.'};
messages.enrollment_duplicate='Camera đã lưu khuôn mặt này. Kiểm tra Face ID của người đã đăng ký.';
messages.liveness_failed='Chưa vượt qua xác minh người thật. Bấm lại và làm theo hướng dẫn xoay mặt.';
const livenessSteps={center:'1/3 · Nhìn thẳng.',turn_left:'2/3 · Xoay nhẹ về phía trái của ảnh.',turn_right:'2/3 · Xoay nhẹ về phía phải của ảnh.',return:'3/3 · Nhìn thẳng lại.'};
function notice(message,error=false){$('notice').textContent=message;$('notice').classList.toggle('error',error);}
async function statusTick(){
  if(stopped)return;
  try{
    const response=await fetch('/api/status',{cache:'no-store',signal:AbortSignal.timeout(8000)});if(!response.ok)throw new Error('Không đọc được trạng thái.');const s=await response.json();
    if(stopped)return;
    checkinSnapshot=s.checkin_snapshot?.id?s.checkin_snapshot:null;
    const challenge=checkinSnapshot?.status==='challenge';
    $('guide').hidden=(!!checkinSnapshot&&!challenge)||!$('show-guide').checked;
    $('preview').textContent=checkinSnapshot?'Xem trực tiếp':preview?'Dừng xem':'Tiếp tục xem';
    $('preview').disabled=challenge||['capturing','recognizing'].includes(checkinSnapshot?.status);
    const restarted=lastUptime!==null&&s.uptime_ms<lastUptime;lastUptime=s.uptime_ms;failures=0;
    if(restarted)notice('Camera vừa khởi động lại. Đối chiếu số người đã lưu và kiểm tra nguồn nếu việc này lặp lại.',true);
    else if(s.reset_reason==='brownout')notice('Lần khởi động này có báo sụt áp (brownout). Kiểm tra củ sạc 5V và dây USB.',true);
    else if(!s.camera_ready)notice('Camera chưa sẵn sàng. Xem thông báo lỗi ở phần đăng ký.',true);
    else if(s.enrollment_locked)notice('Lượt đăng ký trước bị dở. Nhấn “Khởi động lại” để làm lại, dữ liệu đã lưu được giữ.',true);
    else notice('Đã kết nối camera. Nguồn ngoài vẫn chạy được; không cần mở Arduino IDE.');
    $('face-count').textContent=s.face_count+' / 7';$('sample-count').textContent=s.samples+' / 5';$('progress').style.width=(s.samples*20)+'%';
    $('detection').textContent=challenge ? livenessSteps[checkinSnapshot.liveness_step]||livenessSteps.center : checkinSnapshot
      ? (checkinSnapshot.captured?'Đã chụp ảnh điểm danh.':checkinSnapshot.status==='capturing'?'Đang chụp ảnh điểm danh…':'Chưa chụp được ảnh.')
      : [words[s.detection]||s.detection,lightAdvice[s.frame_light]].filter(Boolean).join(' ');
    let result=messages[s.result]||words[s.result]||s.result;
    if(s.result==='enrollment_timeout'&&!s.enrollment_locked)result='Chưa thu được mẫu hợp lệ. Căn mặt theo ảnh rồi đăng ký lại.';
    if(s.result==='enrollment_error')result='Camera gặp lỗi khi xử lý mẫu. Khởi động lại camera rồi thử tiếp.';
    $('result').textContent=result+(s.last_face_id>=0?' Face ID: '+s.last_face_id+'.':'');
    $('frame-info').textContent=s.frame_number?'Khung '+s.frame_number+' · xử lý '+s.processing_ms+' ms · '+Math.round((s.uptime_ms-s.frame_at)/1000)+' giây trước.':'Chưa có khung hình được xử lý.';
    const busy=s.enrolling||s.result==='recognizing';
    $('enroll').disabled=actionPending||busy||!s.camera_ready||!s.storage_ready||s.enrollment_locked||s.face_count>=7;
    $('test').disabled=actionPending||busy||!s.camera_ready||!s.storage_ready||s.enrollment_locked||!s.face_count;
    $('cancel').hidden=!s.enrolling;$('cancel').disabled=actionPending;
    for(const id of ['rotate','restart','clear','radio-off'])$(id).disabled=actionPending||busy;
    $('uptime').textContent=Math.floor(s.uptime_ms/1000)+' giây';$('reset').textContent=s.reset_reason;$('heap').textContent=Math.round(s.free_heap/1024)+' KB';$('station').textContent=s.station_ip||'Chưa kết nối';
  }catch(error){if(++failures>1)notice('Mất kết nối camera. Kiểm tra nguồn, Mobile Hotspot và đúng IP camera. Địa chỉ trang hiện tại: '+location.origin+'.',true);}
  finally{if(!stopped)setTimeout(statusTick,850);}
}
async function frameTick(){
  if(stopped)return;
  const showLive=preview||checkinSnapshot?.status==='challenge';
  if(showLive&&!document.hidden){
    try{
      const response=await fetch('/frame?t='+Date.now(),{cache:'no-store',signal:AbortSignal.timeout(12000)});
      if(response.ok){const blob=await response.blob();const next=URL.createObjectURL(blob);const image=$('image');image.onload=()=>{if(objectUrl)URL.revokeObjectURL(objectUrl);objectUrl=next;hasImage=true;image.hidden=false;$('placeholder').hidden=showLive;};image.onerror=()=>URL.revokeObjectURL(next);image.src=next;}
      else if(!hasImage){$('placeholder').hidden=false;$('placeholder').querySelector('span').textContent='Đang chờ camera xử lý ảnh đầu tiên…';}
    }catch(error){if(!hasImage){$('placeholder').hidden=false;$('placeholder').querySelector('span').textContent='Chưa lấy được ảnh. Kiểm tra Wi-Fi và nguồn camera.';}}
  }
  if(!stopped)setTimeout(frameTick,550);
}
async function act(path){
  if(actionPending||stopped)return;actionPending=true;
  for(const id of ['enroll','test','rotate','restart','clear','radio-off'])$(id).disabled=true;
  try{const r=await fetch('/api/'+path,{method:'POST',headers:{'X-Camera-Control':'local'},signal:AbortSignal.timeout(8000)});const body=await r.json();if(!r.ok)throw new Error(body.error==='busy'?'Camera đang xử lý. Chờ hoàn tất.':body.error==='command_pending'?'Có yêu cầu đang chờ. Thử lại sau.':'Chưa gửi được yêu cầu.');
    if(path==='radio/off'){stopped=true;preview=false;notice('Đã yêu cầu tắt Wi-Fi. Dùng nút điểm danh qua UART; nhấn Reset để kết nối lại Wi-Fi đã cấu hình rồi mở đúng IP camera.');}
    else notice(path==='restart'||path==='clear'?'Camera đang khởi động lại. Chờ và kết nối lại Wi-Fi nếu cần.':'Đã gửi yêu cầu. Xem trạng thái camera bên dưới.');
  }catch(e){notice(e.message,true);}finally{actionPending=false;}
}
$('preview').onclick=async()=>{
  if(checkinSnapshot){
    try{
      const response=await fetch('/api/checkin/resume-preview',{
        method:'POST',headers:{'X-Camera-Control':'local','X-Checkin-Id':checkinSnapshot.id},signal:AbortSignal.timeout(4000)
      });
      if(!response.ok)throw new Error('Chưa thể mở lại ảnh trực tiếp. Chờ camera xử lý xong.');
      checkinSnapshot=null;
      preview=true;
    }catch(error){notice(error.message,true);return;}
  }else preview=!preview;
  $('preview').textContent=preview?'Dừng xem':'Tiếp tục xem';
  if(!preview){
    $('placeholder').hidden=false;
    $('placeholder').querySelector('span').textContent='Đã dừng xem. Nút điểm danh vẫn hoạt động.';
  }else if(hasImage)$('placeholder').hidden=true;
};
$('show-guide').onchange=e=>$('guide').hidden=!!checkinSnapshot||!e.target.checked;
$('enroll').onclick=()=>act('enroll');$('cancel').onclick=()=>act('cancel');$('test').onclick=()=>act('test');$('rotate').onclick=()=>act('rotate');
$('restart').onclick=()=>{if(confirm('Khởi động lại camera? Các khuôn mặt đã lưu được giữ.'))act('restart');};
$('clear').onclick=()=>{if(confirm('Xóa TẤT CẢ khuôn mặt đã lưu? Bạn phải đăng ký và kiểm tra lại liên kết Face ID trên web quản lý.'))act('clear');};
$('radio-off').onclick=()=>{if(confirm('Tắt Wi-Fi camera và đóng kết nối trang này? Nút UART vẫn dùng được. Nhấn Reset để kết nối lại Wi-Fi đã cấu hình.'))act('radio/off');};
statusTick();frameTick();
</script></body></html>)CAMHTML";
