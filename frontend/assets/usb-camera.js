'use strict';

(() => {
  const el = id => document.getElementById('usb-camera' + (id ? '-' + id : ''));
  const panel = el('');
  const image = el('image');
  const placeholder = el('placeholder');
  const status = el('status');
  const result = el('result');
  const connect = el('connect');
  const encoder = new TextEncoder();
  let session = null;

  function render() {
    connect.textContent = session ? 'Ngắt USB' : 'Kết nối USB';
    el('checkin').disabled = el('enroll').disabled = !session;
  }

  async function send(active, command) {
    try {
      await active.writer.write(encoder.encode(command + '\n'));
    } catch {
      status.textContent = 'Mất kết nối USB. Kiểm tra cáp DevKit.';
      await active.reader.cancel().catch(() => {});
    }
  }

  function nextFrame(active, delay) {
    clearTimeout(active.timer);
    if (session !== active || !panel.open || document.hidden) return;
    active.timer = setTimeout(() => {
      if (active.waitingForFrame) {
        status.textContent = 'Chưa nhận được ảnh. Kiểm tra UART và firmware của cả hai bo.';
      }
      active.waitingForFrame = true;
      active.jpegLength = 0;
      active.jpeg = '';
      nextFrame(active, 9500);
      send(active, 'FRAME');
    }, delay);
  }

  function handleLine(active, line) {
    if (line.startsWith('FRAME_BEGIN:')) {
      const size = Number(line.slice(12));
      active.jpegLength = Number.isInteger(size) && size > 0 && size <= 24576 ? 4 * Math.ceil(size / 3) : 0;
      active.jpeg = '';
    } else if (line.startsWith('FRAME_DATA:')) {
      const chunk = line.slice(11);
      if (active.jpegLength && active.jpeg.length + chunk.length <= active.jpegLength
          && /^[A-Za-z0-9+/=]+$/.test(chunk)) active.jpeg += chunk;
    } else if (line === 'FRAME_END') {
      active.waitingForFrame = false;
      if (active.jpegLength && active.jpeg.length === active.jpegLength && panel.open && !document.hidden) {
        image.src = 'data:image/jpeg;base64,' + active.jpeg;
      }
      active.jpegLength = 0;
      active.jpeg = '';
      nextFrame(active, 400);
    } else if (line.startsWith('FRAME_ERROR:') || line.startsWith('FRAME: camera UART offline')) {
      active.waitingForFrame = false;
      const offline = line.includes('UART offline');
      status.textContent = offline ? 'DevKit đang chờ ESP32-CAM qua UART.' : 'Đang chờ ảnh từ ESP32-CAM.';
      nextFrame(active, 2000);
    } else if (/^(CHECKIN|CAMERA[: ]|ENROLL|[123]\/3 )/.test(line)) {
      result.textContent = line;
    }
  }

  async function receive(active) {
    const decoder = new TextDecoder();
    let partial = '';
    try {
      for (;;) {
        const {value, done} = await active.reader.read();
        if (done) break;
        const lines = (partial + decoder.decode(value, {stream: true})).split('\n');
        partial = lines.pop();
        if (partial.length > 256) partial = '';
        for (const line of lines) handleLine(active, line.trim());
      }
    } catch {
      status.textContent = 'Mất kết nối USB. Kiểm tra cáp DevKit.';
    } finally {
      clearTimeout(active.timer);
      active.reader.releaseLock();
      active.writer.releaseLock();
      await active.port.close().catch(() => {});
      session = null;
      image.hidden = true;
      image.removeAttribute('src');
      placeholder.hidden = false;
      render();
    }
  }

  image.onload = () => {
    if (!session || !panel.open) return;
    image.hidden = false;
    placeholder.hidden = true;
    status.textContent = 'Đang xem ảnh từ ESP32-CAM qua USB DevKit.';
  };
  image.onerror = () => { status.textContent = 'Ảnh USB bị thiếu dữ liệu. Đang thử lấy khung tiếp theo.'; };

  connect.onclick = async () => {
    if (session) {
      status.textContent = 'Đã ngắt USB.';
      await session.reader.cancel().catch(() => {});
      return;
    }
    if (!navigator.serial) {
      status.textContent = 'Mở dashboard bằng Chrome hoặc Edge trên laptop để kết nối USB.';
      return;
    }

    connect.disabled = true;
    try {
      const port = await navigator.serial.requestPort();
      await port.open({baudRate: 115200, bufferSize: 65536});
      session = {port, reader: port.readable.getReader(), writer: port.writable.getWriter()};
      status.textContent = 'Đã mở cổng COM. Đang chờ ảnh từ camera.';
      result.textContent = '';
      render();
      receive(session);
      nextFrame(session, 500);
    } catch (error) {
      if (error.name !== 'NotFoundError') {
        status.textContent = 'Không mở được cổng COM. Đóng Arduino Serial Monitor rồi thử lại.';
      }
    } finally {
      connect.disabled = false;
    }
  };

  panel.addEventListener('toggle', () => { if (session) nextFrame(session, 300); });
  document.addEventListener('visibilitychange', () => { if (session) nextFrame(session, 300); });
  el('checkin').onclick = () => { if (session) send(session, 'CHECKIN'); };
  el('enroll').onclick = () => { if (session) send(session, 'ENROLL'); };
})();
