# Giao tiếp hệ thống

## Luồng dữ liệu

```text
Nút trên DevKit
  -> UART CHECKIN:<request_id>
  -> ESP32-CAM chụp / detect / align / recognize
  -> UART RESULT:<request_id>:<status>:<face_id>
  -> ESP32-DevKit mở servo khi recognized
  -> MQTT attendance
  -> Flask kiểm tra Camera / Face ID / buổi học / sự kiện trùng
  -> SQLite
  -> Frontend lấy JSON và hiển thị
```

Backend không chạy mô hình nhận diện. Khi xem trước trên web quản lý, backend có endpoint chuyển tiếp JPEG trong bộ nhớ từ camera tới trình duyệt; không lưu ảnh hoặc vector khuôn mặt vào database.

## UART

115200 baud, 8N1, kết thúc mỗi dòng bằng `\n`.

```text
CHECKIN:smartgate01-aabbccdd00112233-1
RESULT:smartgate01-aabbccdd00112233-1:recognized:0
RESULT:smartgate01-aabbccdd00112233-2:unknown:-1
```

Status: `recognized`, `unknown`, `no_face`, `not_aligned`, `multiple_faces`, `not_enrolled`, `busy`, `error`; DevKit tự tạo `timeout` nếu quá 90 giây. `no_face` là chưa phát hiện mặt, `not_aligned` là phát hiện được nhưng chưa căn chỉnh được. Không coi chuỗi `SUCCESS` thiếu Face ID là điểm danh hợp lệ. Request ID ngăn phản hồi muộn mở cửa cho lần bấm tiếp theo.

## Trang cài đặt trên ESP32-CAM

Camera mặc định chạy **Wi-Fi STA**, kết nối hotspot laptop hoặc router 2.4 GHz bằng cấu hình trong `backend/.env` được đưa vào firmware khi nạp. Khi DHCP cấp IP, Serial in `CAMERA URL: http://<IP-camera>/`. HTTP server khởi tạo ngay. Arduino loop theo dõi IP và thử kết nối lại mỗi 20 giây khi chưa có IP. Camera và UART vẫn chạy khi mất mạng. `STATUS` in lại địa chỉ khi đang kết nối.

`CAM_SETUP_AP_ENABLED=false` trong `camera_config.h` tắt AP riêng theo mặc định. Nếu chủ động bật `true`, camera phát thêm `SmartClassroom-CAM` / `Classroom123`, IP AP `192.168.4.1`. `camera_secrets.h` là cách cấu hình bằng file riêng, chỉ được dùng nếu `ssid` trong `.ino` để trống. `CAM_SETUP_WIFI_ENABLED=false` tắt toàn bộ Wi-Fi từ lúc khởi động.

| Method / đường dẫn trên camera | Chức năng |
|---|---|
| GET `/` | Giao diện căn mặt và đăng ký, toàn bộ tài nguyên nằm trên chip |
| GET `/frame` | JPEG mới nhất, có khung phát hiện; 503 nếu chưa có ảnh |
| GET `/api/status` | Tiến độ, Face ID, detection/result, uptime, reset reason, bộ nhớ |
| POST `/api/enroll`, `/api/cancel` | Yêu cầu đăng ký một người / dừng đăng ký |
| POST `/api/test` | Thử nhận diện cục bộ; không gửi kết quả điểm danh qua UART |
| POST `/api/rotate` | Xoay ảnh 180°, lưu hướng vào NVS |
| POST `/api/restart`, `/api/clear` | Khởi động lại / xóa toàn bộ khuôn mặt rồi khởi động lại |
| POST `/api/radio/off` | Tắt Wi-Fi, giữ UART; reboot để bật lại |

POST cần `X-Camera-Control: local`, kiểm tra cùng origin nếu có header Origin. HTTP 202 chỉ xác nhận đã xếp lệnh; xem `/api/status` để biết kết quả thực thi. Đây là giao diện cài đặt trên mạng cục bộ, không có tài khoản người dùng. Không chuyển tiếp cổng camera ra Internet.

Đăng ký liên kết dùng thêm header `X-Enrollment-Token` gồm tối đa 39 ký tự chữ/số/`-`/`_`. Firmware protocol 2 trả `enrollment_token`, `enrollment_result`, `enrolled_tokens[7]` và `enrollment_protocol:2` trong status. Mỗi token đã hoàn tất được lưu **cùng blob đặc trưng** trong NVS; backend chỉ thêm sinh viên khi tìm đúng token ở đúng Face ID và bộ nhớ camera xác nhận sẵn sàng. Kết quả được đối chiếu lại sau khi mất mạng, tải lại trang hoặc khởi động lại. Không lấy `last_face_id` hay số lượng người tăng lên làm bằng chứng thành công. Firmware vẫn đọc được blob v1 cũ, token trống; dùng liên kết Face ID có sẵn cho những người đó.

Arduino loop chụp và xử lý ảnh. HTTP task đọc JPEG và đưa lệnh vào hàng đợi. Khi đăng ký, UART trả `busy` và web có thể đọc tiến độ hoặc gửi lệnh hủy. Camera cập nhật ảnh xem trước khi có yêu cầu `/frame` trong 6 giây gần nhất. Đăng ký cần 5 mẫu, dừng sau khoảng 30 giây không có mẫu mới và giới hạn tổng thời gian ở 120 giây. Mẫu thu dở nằm trong RAM và được bỏ khi kết thúc lượt.

Ở web quản lý, JPEG qua Flask để hiển thị cùng origin. ESP32-CAM phát hiện, căn chỉnh và so khớp đặc trưng. Hai nút điểm danh dùng chung mô hình và backend đối chiếu buổi học, Face ID cùng biên nhận trước khi ghi. Status có uptime, reset reason, RSSI và số lần ngắt Wi-Fi.

Backend đồng bộ buổi học với camera qua `POST /api/checkin/sync`. Nút web yêu cầu `POST /api/checkin` kèm mã lượt. DevKit chạy đường nội bộ gửi `CHECKIN_LOCAL:<request_id>` qua UART và camera dùng buổi đã đồng bộ. Camera trả kết quả đang chờ qua HTTP. Backend xác nhận sau khi xử lý và dùng mã lượt để tránh ghi lặp. DevKit có `secrets.h` mặc định dùng đường MQTT với lệnh UART `CHECKIN:<request_id>`.

Trong thử thách xoay đầu, `checkin_snapshot.status` là `challenge`, kèm `liveness_step` bằng `center`, `turn_left`, `turn_right` hoặc `return`. `GET /frame` phục vụ ảnh xem trước cho đến khi kết thúc. Camera chỉ phát hành `recognized` khi hoàn thành thử thách và khớp cùng Face ID ban đầu. Camera trả `liveness_failed` qua HTTP; đường UART dùng `unknown` cho kết quả này để tương thích DevKit hiện có.

## MQTT

Base mặc định `demo/smartgate01`, kết nối MQTT TLS 8883.

| Topic sau base | Bên gửi | Nội dung | Retain |
|---|---|---|---|
| `/session` | Backend | `{session_id:1,session_generation:0,active:true}` | Có |
| `/attendance` | DevKit | Kết quả dưới đây | Không |
| `/attendance/ack` | Backend | `{event_id,status}` | Không |
| `/telemetry` | DevKit | lux, mq2_raw, led_percent, door, gas_alarm, station_ip | Không |
| `/state` | DevKit | door, moving, gas_alarm, led_percent | Có |
| `/availability` | DevKit | online / offline, LWT | Có |
| `/command` | Backend | `{door:"open"}` hoặc `{door:"close"}` | Không |

Ví dụ bản tin điểm danh:

```json
{
  "device_id": "smartgate01",
  "camera_id": "CAM01",
  "event": "attendance_result",
  "event_id": "smartgate01-aabbccdd00112233-1",
  "request_id": "smartgate01-aabbccdd00112233-1",
  "session_id": 1,
  "session_generation": 0,
  "status": "recognized",
  "face_id": 0,
  "uptime_ms": 123456
}
```

DevKit chụp `session_id` và `session_generation` đã nhận từ backend tại lúc nhấn nút. Backend chỉ ghi khi ID đó vẫn là buổi đang mở và generation khớp. Reset buổi tăng generation, xóa attendance và giữ receipts; cả bản tin đã xử lý lẫn bản tin chưa từng nhận của lượt trước đều không thể khôi phục kết quả. Bản tin thiếu generation mặc định 0 để tương thích trước lần reset đầu; DevKit cần cập nhật để tiếp tục sau reset. Nếu chưa nhận buổi học, session_id=0 sẽ bị từ chối.

ACK chỉ mang `event_id` và một trong ba trạng thái `accepted`, `duplicate`, `rejected`. DevKit dùng hai trường này để bỏ bản tin khỏi RAM. Nội dung thông báo được lưu ở backend để hiển thị trên web.

Mỗi `(camera_id,face_id)` có một sinh viên. Mỗi `(session_id,student_id)` có một bản ghi. Event ID được lưu riêng để phát lại qua mạng không tạo dữ liệu ở buổi mới. Không gửi ảnh JPEG, base64 hoặc vector khuôn mặt qua MQTT.

## HTTP API

| Method / đường dẫn | Chức năng |
|---|---|
| GET `/api/health` | Backend, MQTT, thiết bị và số liệu mới nhất |
| GET / POST `/api/students` | Danh sách / thêm sinh viên |
| GET / POST `/api/sessions` | Danh sách / tạo và mở buổi |
| POST `/api/sessions/<id>/activate` | Mở lại buổi |
| POST `/api/sessions/close` | Kết thúc buổi đang mở |
| POST `/api/sessions/<id>/reset` | JSON `{confirm:true}`, xóa kết quả đúng buổi và tăng generation; giữ sinh viên / receipts |
| GET / POST `/api/camera` | Đọc / lưu IP nội bộ của camera và lượt đăng ký gần nhất |
| GET `/api/camera/status` | Đọc trạng thái camera, đối chiếu token, thêm sinh viên khi xác nhận lưu thành công |
| GET `/api/camera/frame` | Chuyển JPEG tạm thời, không lưu file và không chạy AI |
| POST `/api/enrollments` | JSON `{student_code,full_name}`, tạo lượt đăng ký và gửi token tới camera |
| POST `/api/camera/cancel`, `/rotate`, `/restart`, `/test` | Lệnh chẩn đoán / điều khiển camera |
| GET `/api/attendance?session_id=1` | Danh sách có mặt/chưa có mặt |
| GET `/api/attendance/export.csv?session_id=1` | CSV UTF-8 cho Excel |
| GET `/api/events` | 30 sự kiện gần nhất |
| POST `/api/demo/checkin` | Thử phần mềm, cần DEMO_MODE=true |
| POST `/api/device/door` | Gửi lệnh cửa qua MQTT |

POST dùng JSON và header `X-Classroom-Client: dashboard`. API không bật CORS; trang web dùng cùng origin. Header này ngăn ghi từ trang web khác, không thay thế đăng nhập người dùng. Chỉ chạy local theo mặc định.

Khi backend chạy ở chế độ localhost, Flask chỉ chấp nhận tên host `127.0.0.1` và `localhost`. Phản hồi `/api/` đặt `Cache-Control: no-store`. Camera cũng đặt `no-store` cho JPEG. Firmware đặt thời hạn ảnh xem trước là 6 giây theo tuổi của khung và dọn ảnh điểm danh sau 15 giây tính từ lúc nhận diện xong nếu chưa tiếp tục xem trực tiếp.

Transport camera chỉ dùng HTTP tới địa chỉ IPv4 thuộc 10/8, 172.16/12 hoặc 192.168/16, không theo redirect, không dùng proxy hệ thống, không nhận URL tùy ý trong endpoint chuyển ảnh. Thông tin MSSV/họ tên nằm ở SQLite, không gửi lên camera; camera nhận mã lượt đăng ký. Lệnh mất phản hồi không được gửi lại tự động. Thay IP trên web có thể khôi phục một lượt đang chờ, nhưng vẫn cần đúng token đã lưu để liên kết.

Xem [Edge AI, dữ liệu khuôn mặt và bảo mật](EDGE_AI_SECURITY.md) để phân biệt dữ liệu lưu, HTTP nội bộ, MQTT qua TLS và các cơ chế xác thực của bản demo.

