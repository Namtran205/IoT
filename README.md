# IoT Smart Classroom

Dự án chạy cục bộ gồm giao diện quản lý, Flask API, SQLite và firmware cho ESP32-DevKit / ESP32-CAM. Camera nhận diện trên chip; backend quản lý sinh viên và điểm danh. Ảnh xem trước được chuyển tiếp tạm thời, không lưu ảnh hoặc chạy mô hình trên laptop.

## Bắt đầu ngay

Mở Terminal trong `F:\Downloads\IOT`, chạy:

```powershell
.\.venv\Scripts\python.exe backend\run.py
```

Môi trường `.venv` đã được cài trong lần tạo dự án này. Mở **http://127.0.0.1:5000**. Bạn cũng có thể chạy `start.bat`.

Nếu sao chép dự án sang máy khác, cài Python 3.10 trở lên rồi chạy:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r backend\requirements-lock.txt
Copy-Item backend\.env.example backend\.env
.\.venv\Scripts\python.exe backend\run.py
```

Không cần Node.js, npm hoặc chạy frontend bằng Live Server. Flask cung cấp các file trong `frontend/` và API trên cùng một địa chỉ.

## Việc bạn nên làm theo thứ tự

1. Chạy web, thêm một sinh viên với `Camera = CAM01`, `Face ID = 0`.
2. Tạo và mở một buổi học. Nhấn **Thử điểm danh** trên dòng sinh viên. Bản ghi hiện nhãn **Thử phần mềm**; đây là kiểm tra web/database, không gọi AI hoặc mở cửa.
3. Điền `CAM_WIFI_SSID` và `CAM_WIFI_PASSWORD` của **Mobile Hotspot laptop (2.4 GHz)** trong `backend/.env`. Mở web bằng `start.bat` rồi Upload bản camera mới. Lấy IP từ `CAMERA URL` và nhập trong khung **Camera điểm danh** trên web `127.0.0.1:5000`. Sau khi kết nối, vào **Đăng ký sinh viên và khuôn mặt** để nhập MSSV/họ tên và thu đủ 5 mẫu. Sinh viên tự xuất hiện khi camera xác nhận đã lưu ID 0–6 vào NVS. Nếu đã quét riêng, dùng **Liên kết Face ID có sẵn**. [Hướng dẫn phần cứng](docs/HARDWARE.md)
4. Điền MQTT vào `backend/.env` và `firmware/esp32_devkit/SmartClassroomController/secrets.h`; nạp firmware ESP32 trung tâm.
5. Nối UART, mở một buổi học và nhấn nút GPIO13 trên ESP32 trung tâm. Xem kết quả trên web.
6. Tắt `DEMO_MODE` sau khi thử; dùng buổi học mới cho dữ liệu thiết bị thật.

Xem [chạy và cấu hình](docs/SETUP.md), [UART / MQTT / API](docs/PROTOCOL.md), [Edge AI và bảo mật](docs/EDGE_AI_SECURITY.md) hoặc [kịch bản kiểm thử](docs/TESTING.md).

## Cấu trúc

```text
IOT/
├── frontend/
│   ├── index.html
│   └── assets/                # CSS và JavaScript gọi API
├── backend/
│   ├── classroom/             # API, MQTT, xử lý điểm danh, SQLite
│   ├── instance/              # Database tự tạo, không đưa lên Git
│   ├── tests/                 # Kiểm tra logic và API
│   ├── .env.example
│   ├── requirements.txt
│   ├── requirements-lock.txt
│   └── run.py
├── firmware/
│   ├── esp32_cam/SmartClassroomCam/
│   ├── esp32_devkit/SmartClassroomController/
│   └── libraries/             # ArduinoJson và PubSubClient cho CLI
├── docs/
├── scripts/
├── start.bat
└── shared-chat-review/        # Tư liệu đối chiếu hội thoại, bỏ qua trong Git
```

## Phần hiện có

- Thêm sinh viên và liên kết `(camera_id, face_id)`; mỗi liên kết là duy nhất.
- Nhập thông tin và đăng ký khuôn mặt trên cùng web; lưu yêu cầu để đối chiếu lại sau mất kết nối, chỉ liên kết đúng token camera xác nhận.
- Tạo / mở / kết thúc buổi học, xem lịch sử và xuất CSV.
- Reset kết quả của riêng buổi được chọn, giữ sinh viên và khuôn mặt; chặn bản tin cũ sau reset bằng generation (cần nạp DevKit mới).
- Nhận kết quả qua MQTT TLS, chống ghi trùng theo sự kiện và theo sinh viên/buổi.
- Gắn sự kiện với buổi học tại thời điểm nhấn nút; từ chối bản tin cũ đến khi buổi đã kết thúc.
- Hiển thị cảm biến và trạng thái cửa; điều khiển mở/đóng khi thiết bị có dữ liệu mới.
- Firmware camera kết nối Mobile Hotspot / Wi-Fi 2.4 GHz để cung cấp trang xem ảnh và đăng ký; nhận diện vẫn chạy trên chip, nhận yêu cầu điểm danh qua UART. Có thể tắt Wi-Fi sau khi đăng ký.
- Firmware trung tâm điều chỉnh LED, mở cửa khi cảnh báo gas, gửi MQTT và giữ tối đa 8 kết quả trong RAM để chờ ACK từ backend.

Hai firmware đã được biên dịch với Core 2.0.0. Phép nhận diện và thử thách xoay mặt chạy trên ESP32-CAM. Việc nhận diện, hiệu quả chống ảnh tĩnh, NVS, nối dây và điều khiển servo cần kiểm tra trên mạch thật. Xem [cách kiểm tra](docs/TESTING.md).

Ứng dụng hiện dành cho demo trên máy cá nhân, bind `127.0.0.1`. Chưa có tài khoản đăng nhập hoặc cấu hình triển khai Internet.

