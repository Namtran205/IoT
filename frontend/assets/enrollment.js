'use strict';
(()=>{
  let camera=null,job=null,configured=false,connected=false,preview=false;
  let reading=false,fetchingFrame=false,submitting=false,objectUrl=null,lastCompleted=null;
  const form=$('enrollment-form');
  const previews=[
    {image:$('camera-image'), placeholder:$('camera-placeholder'), button:$('camera-preview'), detection:$('camera-detection')},
    {image:$('checkin-camera-image'), placeholder:$('checkin-camera-placeholder'), button:$('checkin-camera-preview'), detection:$('checkin-camera-detection')}
  ];
  let previewVersion=0, jobVersion=0, checkinShot=null, resumedShotId='', resuming=false;
  let autoResume=null;
  let resetPending=false, requestingReset=false;
  const isChallenge=shot=>shot?.status==='challenge';
  const processingShot=()=>isChallenge(checkinShot)||['capturing','recognizing'].includes(checkinShot?.status);
  const livenessSteps={
    center:'1/3 · Nhìn thẳng và giữ toàn bộ mặt trong khung.',
    turn_left:'2/3 · Xoay nhẹ mặt về phía trái của ảnh. Giữ đến khi hướng dẫn đổi.',
    turn_right:'2/3 · Xoay nhẹ mặt về phía phải của ảnh. Giữ đến khi hướng dẫn đổi.',
    return:'3/3 · Nhìn thẳng lại vào camera để hoàn tất.'
  };
  const livenessFailure='Chưa vượt qua xác minh người thật. Bấm lại và làm theo hướng dẫn xoay mặt.';
  const pending=()=>job&&['starting','enrolling','uncertain'].includes(job.state);
  const detection={waiting:'Đang chờ ảnh.',ok:'Đã phát hiện một khuôn mặt và căn chỉnh được. Giữ mặt yên.',no_face:'Chưa thấy mặt. Nhìn thẳng, giữ toàn bộ mặt trong ảnh và đợi ô xanh.',not_aligned:'Đã thấy mặt nhưng chưa căn chỉnh được. Nhìn thẳng, giữ đủ mắt / mũi / miệng trong ảnh.',multiple_faces:'Có nhiều mặt trong ảnh. Chỉ để một người trước camera.',inconsistent_face:'Mẫu vừa chụp lệch so với các mẫu trước. Giữ cùng một người, nhìn thẳng và giữ mặt yên.',capture_error:'Chưa lấy được ảnh. Kiểm tra nguồn và cáp camera.',memory_error:'Camera thiếu bộ nhớ.',decode_error:'Chưa giải mã được ảnh camera.'};
  const lightAdvice={
    dark:'Vùng mặt tối. Chiếu thêm ánh sáng dịu từ phía trước.',
    bright:'Vùng mặt quá sáng. Tránh đèn chiếu thẳng hoặc nắng gắt.',
    uneven:'Ánh sáng không đều. Xoay về phía đèn và tránh để cửa sổ sáng sau lưng.'
  };
  function describeDetection(){
    const message=detection[camera.detection]||'Đang chờ ảnh.';
    return [message,lightAdvice[camera.frame_light]].filter(Boolean).join(' ');
  }
  function enrollmentMessage(samples){
    const student=job&&`${job.student_code} — ${job.full_name}`;
    if(job?.state==='completed')return `Đã lưu ${student} vào danh sách (Face ID ${job.expected_face_id}).`;
    if(job?.state==='failed' || job?.state==='needs_link'){
      const detail=job.error?.includes('enrollment_timeout')
        ? 'Camera hết thời gian thu mẫu. Bật xem trước, đứng nơi đủ sáng và căn mặt đến khi camera báo đã phát hiện khuôn mặt.'
        : job.error;
      const restart=connected&&camera.enrollment_locked&&!detail?.includes('Khởi động lại')
        ?' Bấm Khởi động lại camera trước khi thử tiếp.':'';
      return `${student}: ${detail || 'Lượt đăng ký chưa hoàn tất.'}${restart}`;
    }
    if(pending()){
      if(!connected)return `Mất kết nối trong lượt đăng ký ${student}. Kết nối lại đúng IP để web đối chiếu kết quả.`;
      if(camera.enrollment_token!==job.id || !camera.enrolling){
        return `Đang chờ camera xác nhận lượt đăng ký ${student}. Theo dõi hình xem trước.`;
      }
      if(samples===5)return `Đã thu 5/5 mẫu của ${student}. Đang chờ camera xác nhận đã lưu.`;
      return `${student} · Đã thu ${samples}/5 mẫu. ${describeDetection()}`;
    }
    if(!connected)return 'Kết nối camera để xem ảnh và đăng ký.';
    if(resetPending)return 'Chờ camera xác nhận đã xóa dữ liệu trước khi đăng ký.';
    if(camera.enrollment_protocol!==2)return 'Nạp firmware SmartClassroomCam mới để đăng ký kèm thông tin sinh viên.';
    if(!camera.camera_ready||!camera.storage_ready)return 'Camera hoặc bộ nhớ chưa sẵn sàng. Kiểm tra nguồn và trạng thái khởi động.';
    if(camera.enrollment_locked)return 'Lượt thu mẫu trước bị dở. Khởi động lại camera trước khi đăng ký tiếp.';
    if(checkinShot)return 'Chờ ảnh trực tiếp mở lại để đăng ký.';
    if(camera.face_count>=7)return 'Camera đã lưu đủ 7 người.';
    return 'Nhập thông tin, căn mặt trong ảnh và bấm đăng ký.';
  }
  function setDetection(message){
    for(const view of previews)view.detection.textContent=message;
  }
  function showPlaceholder(message){
    if(objectUrl)URL.revokeObjectURL(objectUrl);
    objectUrl=null;
    for(const view of previews){
      view.image.hidden=true;
      view.image.removeAttribute('src');
      view.placeholder.textContent=message;
      view.placeholder.hidden=false;
    }
  }
  function syncCheckinShot(shot){
    if(shot?.id===resumedShotId&&resumedShotId)return;
    const next=shot?.id?shot:null;
    if(next?.id!==checkinShot?.id || next?.ready!==checkinShot?.ready || next?.captured!==checkinShot?.captured || isChallenge(next)!==isChallenge(checkinShot)){
      previewVersion++;
      if(!next)showPlaceholder(preview?'Đang chờ ảnh từ ESP32-CAM…':'Đã dừng xem camera.');
      else if(next.captured)showPlaceholder(next.ready?'Đã chụp. Đang tải ảnh điểm danh…':'Đã chụp. Camera chưa trả được ảnh xem lại.');
      else if(isChallenge(next))showPlaceholder('Đang lấy ảnh để xác minh khuôn mặt…');
      else if(next.status==='liveness_failed')showPlaceholder(livenessFailure);
      else showPlaceholder(next.status==='capturing'?'ESP32-CAM đang chụp và nhận diện…':'Camera chưa chụp được ảnh.');
    }
    checkinShot=next;
  }
  function cancelAutoResume(){
    if(autoResume)clearTimeout(autoResume.timer);
    autoResume=null;
  }
  function updateAutoResume(){
    if(!checkinShot||!connected||resuming||processingShot()||document.hidden){
      cancelAutoResume();
      return;
    }
    const id=checkinShot.id;
    // Give the browser time to load the JPEG, even if recognition finished first.
    const delay=checkinShot.ready&&!objectUrl?8000:3000;
    if(autoResume?.id===id&&autoResume.delay===delay)return;
    cancelAutoResume();
    const timer=setTimeout(()=>{
      autoResume=null;
      if(checkinShot?.id===id&&!document.hidden)resumeCheckinPreview(true);
    },delay);
    autoResume={id,delay,timer};
  }
  function renderCheckinShot(){
    updateAutoResume();
    $('checkin-shot-badge').hidden=!checkinShot?.captured||!objectUrl;
    if(!checkinShot)return;
    const source=checkinShot.id.startsWith('web-')?'Nút web':checkinShot.id.startsWith('cam-button-')?'Nút vật lý trên ESP32-CAM':'Nút vật lý';
    if(isChallenge(checkinShot)){
      const step=livenessSteps[checkinShot.liveness_step]||livenessSteps.center;
      const advice=lightAdvice[camera?.frame_light]||'';
      setDetection(connected?`${source} · Xác minh người thật · ${step} ${advice}`:'Đang chờ kết nối lại với ESP32-CAM.');
      return;
    }
    let message='Ảnh đã chụp. Bạn có thể rời vị trí.';
    if(!checkinShot.captured){
      message=checkinShot.status==='capturing'?'Đang chụp và nhận diện. Giữ mặt trước camera đến khi hiện ảnh.':'Chưa chụp được ảnh. Chờ ảnh trực tiếp rồi thử lại.';
      if(checkinShot.status==='liveness_failed')message=livenessFailure;
      else if(checkinShot.status==='unknown')message='Khuôn mặt chưa khớp với người đã đăng ký.';
      else if(detection[checkinShot.status])message=detection[checkinShot.status];
      $('camera-placeholder').textContent=message;
      $('checkin-camera-placeholder').textContent=message;
    }else if(checkinShot.status==='recognizing'){
      message=connected?'Đã chụp · Đang nhận diện. Bạn có thể rời vị trí.'
        :'Đã chụp · Chờ kết nối lại để nhận kết quả.';
    }else if(checkinShot.status==='recognized'){
      message='Đã chụp · ESP32-CAM đã nhận diện. Xem kết quả điểm danh bên dưới.';
    }else if(checkinShot.status==='unknown'){
      message='Đã chụp · Khuôn mặt chưa khớp với người đã đăng ký.';
    }else if(checkinShot.status==='not_enrolled'){
      message='Đã chụp · Camera chưa có khuôn mặt đăng ký.';
    }else if(detection[checkinShot.status]){
      message='Đã chụp · '+detection[checkinShot.status];
    }
    const {match_score:score,match_threshold:threshold}=checkinShot;
    if(Number.isFinite(score)&&Number.isFinite(threshold)){
      message+=` Điểm khớp ${score.toFixed(3)} (ngưỡng ${threshold.toFixed(2)}).`;
    }
    const advice=lightAdvice[checkinShot.light];
    if(advice&&checkinShot.status!=='recognized')message+=' '+advice;
    $('checkin-camera-detection').textContent=source+' · '+message;
    $('camera-detection').textContent=checkinShot.captured?'Đang giữ ảnh điểm danh. Web tự mở lại sau khi có kết quả.':message;
  }
  function renderButtonLink(){
    const label=$('button-link-status');
    label.hidden=!connected;
    if(!connected)return;
    const link=camera.button_link;
    const gpio=camera.direct_button_gpio;
    if(Number.isInteger(gpio)&&gpio>=0&&!link?.online){
      label.textContent=`Nút trên ESP32-CAM đã sẵn sàng ở IO${gpio}.`;
      return;
    }
    if(!link){
      label.textContent=camera.checkin_snapshot
        ? 'Nạp bản mới cho ESP32-CAM và DevKit để kiểm tra kết nối nút vật lý.'
        : 'Nạp SmartClassroomCam mới để giữ ảnh đã chụp và kiểm tra nút vật lý.';
    }else if(!link.online){
      label.textContent='Chưa thấy tín hiệu DevKit. Kiểm tra nguồn, dây UART và firmware DevKit.';
    }else if(link.last_status==='busy'&&camera.enrolling){
      label.textContent='Đã nhận nút vật lý. Camera đang đăng ký mặt, chờ xong rồi bấm lại.';
    }else if(link.last_status==='capturing'){
      label.textContent='Đã nhận nút vật lý. Đang chờ ESP32-CAM chụp.';
    }else{
      label.textContent=link.last_id?'DevKit đã gửi lệnh từ nút vật lý.':'Đã thấy tín hiệu DevKit. Bấm nút vật lý để thử.';
    }
  }
  async function resumeCheckinPreview(automatic=false){
    if(!checkinShot||!connected||resuming||processingShot())return;
    const id=checkinShot.id;
    cancelAutoResume();
    resuming=true;
    render();
    try{
      await api('/camera/resume-preview',{event_id:id});
      resumedShotId=id;
      if(camera?.checkin_snapshot?.id===id)camera.checkin_snapshot=null;
      if(checkinShot?.id===id){
        checkinShot=null;
        setPreview(true);
        frameTick();
      }
    }catch(error){if(!automatic)toast(error.message,true);}
    finally{resuming=false;render();}
  }
  function setPreview(enabled){
    preview=enabled;
    previewVersion++;
    showPlaceholder(preview?'Đang chờ ảnh từ ESP32-CAM…':'Đã dừng xem camera.');
    render();
  }
  function render(){
    const resettingData=resetPending||requestingReset;
    window.dispatchEvent(new CustomEvent('camera-status',{detail:connected&&!resettingData?camera:null}));
    const waiting=pending(),busy=camera?.enrolling||camera?.result==='recognizing';
    $('camera-badge').textContent=connected?'Camera đã kết nối':configured?'Camera chưa phản hồi':'Chưa kết nối camera';
    $('camera-badge').className='pill '+(connected?'success':'neutral');
    $('start-enrollment').disabled=resettingData||submitting||!connected||!camera?.camera_ready||!camera?.storage_ready||camera?.enrollment_protocol!==2||camera?.enrollment_locked||camera?.face_count>=7||busy||waiting||!!checkinShot;
    $('data-reset').disabled=requestingReset||!connected||busy||processingShot()||!camera?.face_reset_device;
    $('data-reset').textContent=resetPending?'Gửi lại lệnh xóa':'Xóa dữ liệu thử nghiệm';
    $('data-reset-status').textContent=requestingReset?'Đang gửi lệnh đến camera…':resetPending
      ?'Đang chờ ESP32-CAM xác nhận. Nếu camera đổi IP, kết nối lại rồi kiểm tra trạng thái.'
      :connected&&!camera?.face_reset_device?'Nạp firmware SmartClassroomCam mới để dùng nút xóa.':'';
    form.elements.student_code.disabled=!!waiting;form.elements.full_name.disabled=!!waiting;
    $('cancel-enrollment').hidden=!waiting;$('cancel-enrollment').disabled=!connected||submitting;
    $('camera-rotate').disabled=resettingData||!connected||busy||!!waiting||!!checkinShot;
    $('camera-restart').disabled=resettingData||!connected||busy;
    for(const view of previews){
      view.button.disabled=!configured||(!!checkinShot&&(!connected||resuming||processingShot()));
      view.button.textContent=checkinShot?(resuming?'Đang mở lại…':'Xem trực tiếp'):preview?'Dừng xem':'Bật xem trước';
    }
    $('camera-face-count').textContent=camera
      ? `Đã lưu ${camera.face_count} người · Tối đa 7`
      : 'Sức chứa camera: tối đa 7 người';
    const samples=job?.state==='completed'?5:job&&camera?.enrollment_token===job.id?camera.samples:0;
    renderButtonLink();
    $('camera-samples').textContent=samples+' / 5 mẫu';
    $('camera-enroll-progress').style.width=(samples*20)+'%';
    $('enrollment-result').textContent=enrollmentMessage(samples);
    $('enrolled-student').hidden=job?.state!=='completed';
    if(job?.state==='completed')$('enrolled-student').textContent=`Xem ${job.student_code} trong danh sách`;
    if(!connected){renderCheckinShot();return;}
    const recognizing=camera.result==='recognizing';
    setDetection(recognizing?'ESP32-CAM đang nhận diện…':preview?describeDetection():'Bật xem trước để căn mặt và kiểm tra ánh sáng.');
    renderCheckinShot();
    $('camera-diagnostics').textContent=`Uptime ${Math.floor(camera.uptime_ms/1000)} giây · khởi động: ${camera.reset_reason} · Wi-Fi ${camera.wifi_rssi??'—'} dBm · ngắt Wi-Fi ${camera.wifi_disconnects??0} lần (mã ${camera.wifi_disconnect_reason??0}).`;
  }
  function syncReset(pendingReset){
    const finished=resetPending&&!pendingReset;
    resetPending=pendingReset;
    if(finished){
      lastCompleted=null;
      form.reset();
      $('search').value='';
      $('attendance-filter').value='all';
      delete $('web-checkin-status').dataset.result;
      $('button-checkin-status').textContent='Nhấn nút vật lý khi đã căn mặt trong ảnh.';
      toast('Đã xóa dữ liệu thử nghiệm và khuôn mặt. Bạn có thể đăng ký lại.');
      refresh();
    }else if(pendingReset){
      $('data-reset').closest('details').open=true;
    }
  }
  async function statusTick(){
    if(!configured||reading||document.hidden)return;
    reading=true;
    const version=jobVersion;
    try{
      const result=await api('/camera/status');
      camera=result.camera;
      syncCheckinShot(camera.checkin_snapshot);
      if(camera.checkin_snapshot?.id===resumedShotId&&resumedShotId)camera.checkin_snapshot=null;
      if(version===jobVersion){job=result.job;syncReset(result.reset_pending===true);}
      connected=true;
      if(isChallenge(checkinShot)||(checkinShot?.ready&&!objectUrl))frameTick();
      if(job?.state==='completed'&&job.id!==lastCompleted){
        lastCompleted=job.id;form.reset();toast(`Đã lưu ${job.student_code} và khuôn mặt Face ID ${job.expected_face_id}.`);window.dispatchEvent(new Event('enrollment-completed'));
      }
      render();
    }catch(e){
      connected=false;
      if(!checkinShot){
        setDetection(e.message);
        if(preview)showPlaceholder('Mất kết nối camera. Kiểm tra nguồn, hotspot và địa chỉ IP.');
      }
      render();
    }
    finally{reading=false;}
  }
  async function frameTick(){
    const challenge=isChallenge(checkinShot);
    const snapshotId=challenge?null:checkinShot?.id;
    if(!configured||fetchingFrame||document.hidden)return;
    if(snapshotId?(!checkinShot.ready||objectUrl||!connected):!preview&&!challenge)return;
    fetchingFrame=true;
    const version=previewVersion;
    let next;
    try{
      const path=snapshotId?'/api/camera/checkin-frame/'+encodeURIComponent(snapshotId):'/api/camera/frame';
      const response=await fetch(path,{cache:'no-store',signal:AbortSignal.timeout(6000)});
      if(!response.ok)throw new Error('waiting');
      next=URL.createObjectURL(await response.blob());
      const frame=new Image();
      frame.src=next;
      await frame.decode();
      if(version!==previewVersion)return;
      for(const view of previews){
        view.image.toggleAttribute('data-frozen',!!snapshotId);
        view.image.src=next;
        view.image.hidden=false;
        view.placeholder.hidden=true;
      }
      if(objectUrl)URL.revokeObjectURL(objectUrl);
      objectUrl=next;
      next=null;
      if(snapshotId)renderCheckinShot();
    }catch(e){
      if(version===previewVersion&&!objectUrl&&connected)showPlaceholder(snapshotId?'Đã chụp. Đang tải lại ảnh điểm danh…':'Đang chờ ảnh từ ESP32-CAM…');
    }finally{
      if(next)URL.revokeObjectURL(next);
      fetchingFrame=false;
    }
  }
  $('camera-connect').onsubmit=async e=>{
    e.preventDefault();const button=e.currentTarget.querySelector('button');button.disabled=true;
    try{const result=await api('/camera',{camera_url:$('camera-url').value});$('camera-url').value=result.camera_url;configured=true;connected=false;checkinShot=null;setPreview(true);await statusTick();await frameTick();}
    catch(error){toast(error.message,true);}finally{button.disabled=false;}
  };
  form.onsubmit=async e=>{
    e.preventDefault();if(resetPending||requestingReset||submitting||pending()||checkinShot)return;
    submitting=true;render();
    try{
      const submitted=await api('/enrollments',{student_code:form.elements.student_code.value,full_name:form.elements.full_name.value});
      jobVersion++;
      if(job?.id!==submitted.id||pending())job=submitted;
      if(!preview)setPreview(true);
      await statusTick();
    }
    catch(error){toast(error.message,true);}finally{submitting=false;render();}
  };
  $('data-reset').onclick=async()=>{
    if($('data-reset').disabled)return;
    if(!resetPending&&prompt('Xóa vĩnh viễn toàn bộ sinh viên, buổi học, lịch sử điểm danh và khuôn mặt trên camera. Nhập XÓA để xác nhận.')?.trim()!=='XÓA')return;
    requestingReset=true;
    render();
    try{
      const result=await api('/data-reset',{confirm:'XÓA'});
      jobVersion++;
      syncReset(result.reset_pending);
      await statusTick();
    }catch(error){toast(error.message,true);}
    finally{requestingReset=false;render();}
  };
  $('enrolled-student').onclick=()=>{
    if(job?.state!=='completed')return;
    $('search').value=job.student_code;
    $('attendance-filter').value='all';
    renderStudents();
    refresh();
  };
  for(const view of previews)view.button.onclick=()=>{
    if(checkinShot){resumeCheckinPreview();return;}
    setPreview(!preview);
    if(preview)frameTick();
  };
  for(const [id,action]of [['camera-rotate','rotate'],['cancel-enrollment','cancel'],['camera-restart','restart']])$(id).onclick=async()=>{
    if(action==='restart'&&!confirm('Khởi động lại ESP32-CAM? Lượt thu mẫu đang dở sẽ dừng; khuôn mặt đã lưu vẫn được giữ.'))return;
    try{await api('/camera/'+action,{});toast(action==='restart'?'Đã yêu cầu khởi động lại camera.':'Đã gửi lệnh camera.');await statusTick();}catch(e){toast(e.message,true);}
  };
  (async()=>{try{const result=await api('/camera');job=result.job;syncReset(result.reset_pending===true);lastCompleted=job?.state==='completed'?job.id:null;$('camera-url').value=result.camera_url||'http://192.168.137.165';configured=!!result.camera_url;if(configured)setPreview(true);else render();await statusTick();await frameTick();}catch(e){setDetection(e.message);}})();
  setInterval(statusTick,1400);setInterval(frameTick,900);
})();
