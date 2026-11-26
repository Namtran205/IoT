# Chạy web và kết nối MQTT

## 1. Khởi động

Chạy `start.bat`, hoặc từ thư mục gốc:

```powershell
.\.venv\Scripts\python.exe backend\run.py
```

Mở `http://127.0.0.1:5000`. Dừng bằng `Ctrl+C`. Database tự tạo tại `backend/instance/classroom.sqlite3`, được giữ khi khởi động lại.

Không mở `frontend/index.html` trực tiếp bằng `file://`: giao diện cần API của backend.

## 2. Thử khi chưa có mạch

`backend/.env` mặc định `MQTT_ENABLED=false`, `DEMO_MODE=true`. Thêm sinh viên, mở buổi học rồi chọn **Thử điểm danh**. Giao diện gắn nhãn rõ nguồn `demo`; không tạo số liệu cảm biến giả.

Web có hai cách thêm sinh viên: **Đăng ký sinh viên & khuôn mặt** điều khiển camera lấy 5 mẫu và tự liên kết khi đã lưu thành công; **Liên kết Face ID có sẵn** dành cho người đã được quét riêng hoặc thử phần mềm. Cặp Camera / Face ID đã dùng không thể thêm lần nữa. Bản hiện tại chưa cung cấp sửa/xóa sinh viên trên web để tránh vô tình thay ánh xạ khi đã có lịch sử.

Điền `CAM_WIFI_SSID` và `CAM_WIFI_PASSWORD` của Mobile Hotspot laptop (2.4 GHz) trong `backend/.env`, mở web bằng `start.bat` rồi Upload firmware camera trong Arduino IDE (enrollment protocol 2). Lấy IP từ `CAMERA URL`, nhập vào phần đăng ký trên **http://127.0.0.1:5000**, bấm Kết nối camera, nhập MSSV/họ tên và đăng ký. Hotspot cấp IP cho camera. Dùng `Ctrl+F5` để cập nhật giao diện. Backend tự thêm bảng và cột cần thiết khi khởi động, giữ dữ liệu cũ. Có thể cấu hình IP khởi tạo bằng `CAMERA_URL` trong `.env`, hoặc lưu trực tiếp từ form web. Sau khi nạp, camera có thể dùng củ sạc 5V ≥2A qua đế MB. Xem [hướng dẫn phần cứng](HARDWARE.md).

Chọn buổi học trong danh sách để xem lịch sử. **Mở buổi đã chọn** cho phép tiếp tục buổi cũ; **Tạo và mở buổi học** sẽ mở một buổi mới. Điểm danh trùng trong một buổi chỉ ghi một lần.

**Reset điểm danh** xóa kết quả của riêng buổi đang chọn sau khi xác nhận; giữ sinh viên, khuôn mặt và các buổi khác. Để bấm điểm danh lại sau reset, cập nhật firmware DevKit hỗ trợ generation mới. Backend giữ biên nhận cũ để bản tin gửi lại không khôi phục dữ liệu vừa xóa.

## 3. Kết nối HiveMQ Cloud

Điền thông tin thật trong `backend/.env`:

```dotenv
MQTT_ENABLED=true
DEMO_MODE=false
MQTT_HOST=YOUR_CLUSTER.s1.eu.hivemq.cloud
MQTT_PORT=8883
MQTT_USERNAME=YOUR_MQTT_USERNAME
MQTT_PASSWORD=YOUR_MQTT_PASSWORD
MQTT_TLS=true
MQTT_BASE_TOPIC=demo/smartgate01
DEVICE_ID=smartgate01
CAMERA_ID=CAM01
```

Khởi động lại backend sau khi thay `.env`. Tên broker, base topic, Device ID và Camera ID phải giống cấu hình firmware. Không dùng lẫn địa chỉ broker cũ trong README của bộ tài liệu trước.

Backend dùng chứng chỉ CA của hệ điều hành để xác thực TLS. Thông tin MQTT chỉ nằm ở backend và firmware, không gửi xuống JavaScript. Tài khoản broker cần có quyền publish/subscribe trên các topic trong [PROTOCOL.md](PROTOCOL.md).

ESP32 trung tâm cũng cần cấu hình chứng chỉ CA đúng với broker. Điền PEM vào `MQTT_ROOT_CA` trong `secrets.h`; firmware chờ SNTP trước khi kết nối TLS. `MQTT_ALLOW_INSECURE=true` chỉ là tùy chọn thử nghiệm có chủ đích khi chưa cấu hình CA, mặc định là `false`.

## 4. Cấu hình chính

| Biến | Ý nghĩa |
|---|---|
| `HOST`, `PORT` | Mặc định `127.0.0.1:5000` |
| `DATABASE_PATH` | Đường dẫn SQLite; đường dẫn tương đối tính từ `backend/` |
| `DEMO_MODE` | Cho phép nút thử phần mềm, mặc định true |
| `MQTT_ENABLED` | Bật kết nối broker, mặc định false |
| `DEVICE_ID` | Chỉ nhận dữ liệu từ ESP32 có ID này |
| `CAMERA_ID` | Namespace của Face ID, mặc định CAM01 |

Thời gian ghi điểm danh lấy khi backend nhận bản tin, theo UTC+7. Không dùng `uptime_ms` của ESP32 làm đồng hồ lịch.

Nếu tạm mất mạng, ESP32 giữ tối đa 8 kết quả trong RAM, gửi lại mỗi 5 giây đến khi nhận ACK. Khởi động lại ESP32 làm mất hàng đợi này. Khi bản tin đến mà buổi tương ứng đã đóng, backend từ chối thay vì gán sang buổi mới. Đây chưa phải hệ thống đồng bộ offline có lưu bền và đồng hồ thiết bị.

## 5. Kiểm tra

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s backend\tests -v
```

Không cần pytest. Các bài kiểm tra dùng database tạm riêng, không sửa dữ liệu đang dùng.

