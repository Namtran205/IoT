# Kiểm thử Smart Classroom trên mạch thật

Làm theo thứ tự trong [checklist trên mạch thật](#checklist-trên-mạch-thật). Ghi lại số mục và kết quả quan sát được nếu cần chẩn đoán.

## Kiểm tra phần mềm đã thực hiện

Các bài test nằm tại `backend/tests/test_attendance.py`: dữ liệu đầu vào, Face ID 0, ánh xạ trùng, một lần điểm danh mỗi buổi, gửi trùng đồng thời, sự kiện cũ khi đổi buổi, bản tin retained, demo so với bản tin thật, CSV và dữ liệu qua lần khởi động lại.

`backend/tests/test_enrollment_reset.py` kiểm tra đăng ký chỉ khi đúng token đã lưu, khôi phục sau mất phản hồi, khởi động lại backend hoặc đổi IP, quét bổ sung hồ sơ đã tạo trước, từ chối sai ID hoặc lỗi NVS, firmware cũ, hủy, IP không hợp lệ, reset đúng buổi và giữ sinh viên, chặn bản tin trước reset, điểm danh lại sau reset và migration database cũ giữ dữ liệu. Bộ kiểm thử backend có **32 bài** đã đạt, gồm kiểm tra Host, cache API và việc ACK chỉ mang mã sự kiện cùng trạng thái.

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s backend\tests -v
```

Firmware đã được biên dịch cho `esp32:esp32:esp32cam` và `esp32:esp32:esp32` với Core 2.0.0, ArduinoJson 7.4.2, PubSubClient 2.8. Chưa nạp firmware hoặc đo độ chính xác trên phần cứng trong lần tạo dự án.

`scripts/browser-check.mjs` kiểm tra giao diện bằng Edge/Chromium DevTools trên backend riêng cổng 5001. Đây là công cụ QA tùy chọn; người dùng không cần Node để chạy ứng dụng.

`scripts/enrollment-ui-fixture.py` mở Flask cổng 5001 với database tạm riêng trong `artifacts/tmp` và camera giả lập. `scripts/enrollment-browser-check.mjs` dùng trình duyệt DevTools cổng 9333 để kiểm tra luồng web đầy đủ: ảnh JPEG chuyển tiếp, MSSV/họ tên + 5 mẫu, mất mạng/khôi phục, tự thêm sinh viên một lần, mở buổi, thử điểm danh, hủy/xác nhận reset, giữ dữ liệu sinh viên/khuôn mặt, điểm danh lại sau reset, dừng ảnh và bố cục desktop/mobile. Ảnh ghi rõ **UI TEST**; không thay thế thử phần cứng. Chỉ chạy fixture cho QA, không dùng thay `backend/run.py`.

`scripts/camera-preview-check.mjs` kiểm tra chính HTML được nhúng trong firmware camera bằng HTTP mock cổng 5002 và trình duyệt DevTools cổng 9333: ảnh xem trước, lỗi mạng/khôi phục và hiển thị đúng địa chỉ hiện tại, đăng ký 5 mẫu, ID0, dừng/tiếp tục xem, xoay, hủy đăng ký, khóa mẫu dở, cảnh báo brownout, xóa, tắt Wi-Fi và bố cục điện thoại. Ảnh trong bài kiểm tra ghi rõ **UI TEST**, không phải ảnh camera thật. Kiểm tra này không chứng minh kết nối hotspot, camera, AI hoặc nguồn đã chạy trên mạch.

## Thử riêng cảm biến bằng DevKit

1. Để ESP32-CAM, dây UART và servo rời trong lượt thử này. Kiểm tra dây BH1750 **SDA GPIO26**, **SCL GPIO27** và LED **GPIO18** qua điện trở. Với module MQ-2 loại 5 V, nối **AO qua bộ chia 10 kΩ/10 kΩ** vào GPIO34 và nối GND chung.
2. Nạp `SmartClassroomController.ino` cho **ESP32 Dev Module** ở cấu hình mặc định, với `secrets.example.h` có sẵn và chưa tạo `secrets.h`. Kết nối USB rồi mở Serial Monitor **115200**. DevKit in dòng `SENSORS` mỗi giây.
3. Che BH1750 vài giây rồi rọi đèn vào cảm biến. Khi che, lux giảm, phần trăm LED tăng và đèn sáng hơn. Khi rọi, các số đổi theo chiều ngược lại. `BH1750=no_reading` báo chưa đọc được cảm biến qua I²C.
4. Ghi số `MQ2=… ADC` ở không khí bình thường sau khi đã ổn định. `gas_ready=1` nghĩa là firmware đã qua thời gian chờ 30 giây và bắt đầu xét ngưỡng demo. Có thể thử việc đổi `gas_alarm` bằng tín hiệu điện mô phỏng theo mục 8. Bài thử này chạy hoàn toàn trên DevKit qua USB.

## Checklist trên mạch thật

**Chuẩn bị.** Bật Mobile Hotspot laptop ở 2,4 GHz. Chạy `start.bat` nếu web chưa mở rồi vào `http://127.0.0.1:5000/`. Dùng Serial Monitor **115200 baud**, chọn COM của board đang thử. Tạo các buổi có tên **TEST nút web**, **TEST nút CAM** và **TEST nút DevKit** khi đến từng mục để dễ kiểm tra một lần ghi mới cho cùng một sinh viên. Kiểm tra dây theo [sơ đồ chân và nguồn](HARDWARE.md#3-chân-kết-nối).

### 1. Nguồn, hotspot và xem camera

1. Thử ESP32-CAM qua đế MB, tạm cách ly tải servo và MQ-2. Mở Serial Monitor camera, bấm **RST** rồi tìm `CAMERA READY`, `WIFI CONNECTED` và `CAMERA URL`.
2. Mở đúng URL in ra trong trình duyệt laptop. Ảnh phải cập nhật khi bạn di chuyển tay. Trên web quản lý, nhập IP đó vào **Camera điểm danh**, bấm **Kết nối camera** và **Bật xem trước** nếu cần. Thử **Dừng xem** rồi bật lại.
3. Giữ camera chạy vài phút. Sau đó thử lại với nguồn USB 5 V ổn định qua đế MB. Giữ hotspot bật. Nếu IP đổi, lấy IP mới từ Mobile Hotspot hoặc đọc lại Serial, rồi cập nhật trên web.

**Đạt khi** có ảnh cập nhật trên web và camera kết nối lại sau khi cấp nguồn. Nếu lặp `Brownout detector was triggered`, xử lý nguồn và cáp trước. Nếu Serial có IP nhưng web mất kết nối, mở trực tiếp `http://<IP-camera>/api/status` để xác nhận đúng IP. [Chẩn đoán nguồn của Espressif](https://docs.espressif.com/projects/esp-faq/en/latest/development-environment/debugging.html)

### 2. Đăng ký và lưu khuôn mặt

1. Vào **Đăng ký sinh viên và khuôn mặt**. Dùng MSSV thử chưa có trong danh sách và một người đồng ý thử, chưa lưu mẫu trên camera. Kiểm tra camera còn chỗ trước khi bấm.
2. Nhập MSSV và họ tên, bấm **Đăng ký sinh viên & khuôn mặt**. Để đúng một người nhìn thẳng, đủ sáng, cách camera khoảng 30–50 cm. Giữ yên cho tới khi đủ 5 mẫu và web báo đã đăng ký.
3. Kiểm tra MSSV và họ tên xuất hiện một lần trong **Danh sách sinh viên**. Ghi Face ID từ thông báo hoặc log `ENROLL SAVED`.
4. Bấm RST camera, kết nối lại IP nếu cần, rồi xem số khuôn mặt. Xác nhận người đó vẫn được nhận diện bằng mục 3.

**Đạt khi** web có đúng hồ sơ, mẫu vẫn còn sau khởi động lại. Có thể thử dùng một MSSV thử khác với cùng khuôn mặt dưới cùng điều kiện sáng. Kỳ vọng camera báo khuôn mặt đã được đăng ký và danh sách giữ nguyên. Nếu nhận dạng trùng thất bại, ghi lại ánh sáng, góc và kết quả để đánh giá độ tin cậy.

### 3. Tạo buổi và điểm danh bằng nút trên web

1. Vào **Mở lượt điểm danh**. Nhập tên **TEST nút web**, chọn ngày, bấm **Tạo và mở buổi học**. Chọn đúng buổi đang mở. Kiểm tra số có mặt ban đầu bằng 0.
2. Trong **Camera điểm danh**, đứng sao cho toàn bộ mặt rõ trong ảnh và bấm **Chụp và điểm danh** một lần. Nhìn thẳng, xoay nhẹ về phía web yêu cầu rồi nhìn thẳng lại. Giữ mặt trong khung đến khi thấy **Đã chụp**.
3. Đối chiếu ảnh dừng và thông báo nguồn **Nút web**. Sau khi hiện kết quả, đợi ảnh tự quay lại live khoảng 3 giây. Kiểm tra đúng MSSV chuyển sang **Đã điểm danh** và số có mặt tăng một.
4. Chờ live trở lại, bấm lần hai với cùng người, cùng buổi. Số có mặt và giờ đến ban đầu giữ nguyên.

**Kiểm tra hết hạn ảnh chụp.** Đóng các tab xem ảnh, mở Serial Monitor của camera ở 115200 baud rồi bấm nút vật lý. Làm theo dòng `LIVENESS` trên Serial và đợi 16 giây sau khi nhận diện xong. Khi mở lại trang, kiểm tra ảnh chụp cũ đã hết hạn và bản ghi điểm danh vẫn tra được.

**Nếu nút bị mờ**, kiểm tra đã chọn buổi mở, camera online, có ít nhất một mẫu đã lưu và đã kết thúc lượt đăng ký trước đó. Nếu web báo firmware cũ, thực hiện lại [các bước nạp camera](HARDWARE.md#1-esp32-cam).

### 4. Nút đỏ nối trực tiếp ESP32-CAM

1. Tạo và mở **TEST nút CAM**. Kiểm tra nút nối **IO13 và GND trên ESP32-CAM**.
2. Bấm nút đỏ đúng một lần rồi làm theo hướng dẫn xoay mặt trên web. Đọc **Nút vật lý trên ESP32-CAM · Đã chụp**, đợi ảnh tự quay lại trực tiếp rồi kiểm tra dòng sinh viên đã điểm danh.
3. Nếu web chưa có phản hồi, xem Serial camera. `CAM BUTTON GPIO13` xác nhận camera đã đọc nút. Nếu không có dòng này, tắt nguồn và kiểm tra hai tiếp điểm của nút.

**Đạt khi** bấm nút đỏ tạo ảnh chụp và một bản ghi mới. Lệnh điều khiển servo được thử ở mục 5 và 7.

### 5. Nút DevKit, UART và servo

1. Kiểm tra dây **CAM IO15 → DevKit GPIO25 (D25)**, **DevKit GPIO33 (D33) → CAM IO14** và **GND chung**. Nút DevKit nối **GPIO13 và GND trên DevKit**. Servo nhận tín hiệu từ **GPIO32 (D32)**. Thử servo khi tay đòn rời cơ cấu cửa.
2. Tạo và mở **TEST nút DevKit**. Mở Serial Monitor DevKit, đợi `CAMERA UART: connected`. Nếu DevKit dùng MQTT, đợi thêm `SESSION:` có mã buổi khác 0.
3. Cho người đã đăng ký nhìn thẳng, bấm nút DevKit và làm theo hướng dẫn xoay mặt trên web. Đọc lần lượt `CHECKIN request=`, `CAMERA ACK: capture requested` và `CAMERA: recognized Face ID=…`.
4. Kiểm tra servo quay về vị trí mở và dòng sinh viên được ghi. Khi dùng MQTT, đối chiếu thêm `BACKEND ACK` với `accepted` cho lần đầu hoặc `duplicate` khi bấm lại.

**Khoanh lỗi theo mốc cuối.** Có `CHECKIN request=` mà thiếu `CAMERA ACK` thì kiểm tra UART. Có `recognized` mà servo đứng thì kiểm tra tín hiệu GPIO32, điện áp nguồn chung tại servo và GND. Servo mở mà web chưa ghi thì kiểm tra đúng buổi, Face ID đã liên kết và đường truyền kết quả. DevKit dùng HTTP qua camera ở cấu hình nội bộ hoặc dùng MQTT khi đã cài broker.

### 6. Thử từ chối và đo độ ổn định của AI

1. Mở một buổi thử. Bấm nút khi khung trống, với một người chưa đăng ký và với hai người cùng đứng trước camera. Mỗi lần chờ kết quả và ảnh live trở lại.
2. Kỳ vọng `no_face` khi không có mặt, `unknown` khi camera thấy mặt lạ đủ rõ, và `multiple_faces` khi phát hiện được cả hai mặt. `not_aligned` báo đã thấy mặt nhưng chưa căn được. Kiểm tra số có mặt giữ nguyên. Khi muốn thử phản hồi của cửa, đưa servo về đóng trước mỗi lượt.
3. Với cùng một người đã đăng ký, thử 10 lần ở chỗ đủ sáng rồi lặp lại tại vị trí dự định demo. Ghi số lần `recognized`, `no_face`, `not_aligned`, `unknown` hoặc trạng thái khác và thời gian từ lúc bấm tới lúc có kết quả.
4. Nếu hay trượt dù ảnh rõ, xem điểm khớp trên ảnh chụp của web hoặc lưu dòng `CHECKIN DETECTION` từ Serial camera. Dòng này có số khung đã thử, thời gian xử lý, kết quả và điểm khớp. `similarity=nan` cho biết lượt đó chưa tính được điểm. Dùng tỷ lệ nhận đúng trên số lượt bấm để đánh giá. Số có mặt chỉ tăng một lần trong mỗi buổi.

**Thử chống ảnh tĩnh.** Dùng một buổi thử và ảnh của chính người đồng ý tham gia đã đăng ký. Đóng servo trước khi thử nút DevKit. Đặt ảnh in hoặc ảnh tĩnh trên màn hình trước camera và bấm nút. Nếu ảnh được nhận diện ban đầu, chương trình đợi động tác xoay rồi báo **Chưa vượt qua xác minh người thật**. Kiểm tra số có mặt không tăng và cửa không mở. Sau đó để chính người đó làm đủ ba bước. Kiểm tra kết quả nhận diện, bản ghi điểm danh và servo. Ghi lại tỷ lệ qua thử thách của người thật và tỷ lệ ảnh tĩnh bị chặn.

### 7. MQTT, mở/đóng cửa trên web và BH1750/LED

1. Cấu hình theo [mục MQTT](HARDWARE.md#5-bật-mqtt-để-giám-sát-cảm-biến-và-điều-khiển-cửa-trên-web). Đợi đầu trang báo **MQTT đã kết nối** và khung **Cửa ra vào** báo **Trực tuyến**. Nếu chỉ có dấu hiệu kết nối MQTT, xem Wi-Fi và log DevKit.
2. Khi cảnh báo gas đang tắt, bấm **Mở cửa**, đợi servo quay và web hiện **Cửa đang mở**. Bấm **Đóng cửa**, đợi servo quay về và web hiện **Cửa đã đóng**.
3. Che BH1750 vài giây. Quan sát LED vàng sáng hơn, số lux giảm và phần trăm LED trên web tăng. Rọi đèn điện thoại vào BH1750 vài giây. LED phải giảm sáng, lux tăng và phần trăm LED giảm.

**Nếu LED đứng yên ở cả hai lần**, kiểm tra BH1750 ở địa chỉ `0x23`, SDA GPIO26, SCL GPIO27 và LED GPIO18. LED có thể thử bằng cách che/rọi ngay cả khi chưa có MQTT.

### 8. MQ-2 và ưu tiên mở cửa khi cảnh báo

1. Nạp `SmartClassroomController.ino` vào DevKit. Ở chế độ USB nội bộ, mở Serial Monitor 115200 để xem `MQ2=… ADC`, `gas_alarm` và `LED_mode`. Khi chạy MQTT, xem trạng thái cảnh báo trên web và quan sát LED trực tiếp. Xác nhận LED nằm ở **D18** và dây tín hiệu servo ở **D32**.
2. Thử bằng tín hiệu điện mô phỏng. Tắt nguồn để tách AO của MQ-2 khỏi đầu trên bộ chia. Giữ GPIO34 ở điểm giữa hai điện trở 10 kΩ. Gắn dây thử vào vị trí vừa tháo AO và nối đầu còn lại của dây thử vào GND. Dùng tín hiệu điện cho bài demo thay cho lửa, khói hoặc xả khí.
3. Cấp nguồn, chờ `gas_ready=1`, `gas_alarm=0` và `LED_mode=auto`. Servo ở **120°**. Chuyển đầu dây thử từ GND sang **3V3**. Sau hai mẫu cao, kiểm tra `gas_alarm=1`, `LED_mode=blink`, LED sáng 350 ms rồi tắt 350 ms và servo quay về **40°**.
4. Chuyển đầu thử về GND ngay sau khi cảnh báo bật. Sau năm mẫu thấp, kiểm tra `gas_alarm=0` trong khi LED vẫn còn nháy. Tính từ lúc đèn bắt đầu nháy, đợi đủ 15 giây rồi kiểm tra `LED_mode=auto`. Che và rọi BH1750 để thấy độ sáng LED thay đổi lại. Servo giữ vị trí mở. Giữ dây thử ở GND rồi bấm EN/RST để quay về vị trí đóng và diễn lại. Tắt nguồn trước khi nối lại AO của MQ-2.

Nếu thấy `gas_alarm=1` mà servo đứng, kiểm tra bằng Serial Monitor ở chế độ USB nội bộ. Chọn **115200** và **New Line**, đợi `gas_alarm=0` rồi gửi `SERVO_CLOSE` và `SERVO_OPEN` lần lượt. Log `SERVO command | GPIO32 | 120 -> 40 deg` báo firmware đã đặt góc đích mở. Nếu log đổi góc mà tay servo đứng, kiểm tra dây D32, GND chung và điện áp nguồn tại servo lúc bắt đầu quay. Để tay servo rời cơ cấu trong lúc thử.

**Nếu lắp lại MQ-2 và cửa tự mở**, so sánh ADC nền với `GAS_ALARM_ADC` và `GAS_CLEAR_ADC` trong `config.h`. Các ngưỡng trong firmware là giá trị ADC của mô hình thử nghiệm.

### 9. Kết thúc buổi, xuất CSV và reset một buổi

1. Chọn một buổi thử đã có điểm danh, bấm **Xuất CSV** rồi mở file `attendance-<id>.csv`. So khớp MSSV, họ tên, trạng thái và giờ đến với dòng trên web.
2. Chọn buổi đang mở và bấm **Kết thúc buổi**. Kiểm tra **Chụp và điểm danh** bị khóa, dữ liệu cũ vẫn xem được. Bấm **Mở buổi** trên buổi đã chọn nếu cần thử tiếp.
3. Chọn đúng một buổi thử có dữ liệu. Bấm **Reset điểm danh**, thử **Hủy** để kiểm tra số có mặt giữ nguyên. Bấm lại rồi xác nhận. Kiểm tra số có mặt về 0, danh sách sinh viên và số mặt giữ nguyên. Đổi sang buổi thử khác để xác nhận buổi đó vẫn còn dữ liệu.
4. Mở lại buổi vừa reset nếu cần, điểm danh lại bằng nút web và xem có bản ghi mới. Khi thử bằng nút DevKit qua MQTT, đợi Serial báo `SESSION:` với `generation` mới rồi mới bấm.

### 10. Xóa dữ liệu thử nghiệm

Chỉ dùng bước này khi cả database và dữ liệu khuôn mặt trên camera đều có thể bỏ. Nút **Xóa dữ liệu thử nghiệm** xóa sinh viên, buổi học, lịch sử và mẫu mặt đã lưu.

1. Kết nối camera và ghi số sinh viên, số buổi và số khuôn mặt trước khi thử.
2. Mở **Quản lý dữ liệu thử nghiệm**, bấm **Xóa dữ liệu thử nghiệm**, đọc hộp xác nhận rồi bấm **Hủy**. Kiểm tra các số giữ nguyên.
3. Lặp lại và xác nhận khi sẵn sàng xóa bộ dữ liệu thử. Đợi báo hoàn tất. Làm mới web rồi kiểm tra danh sách sinh viên và buổi học rỗng, số khuôn mặt về 0. Thử đăng ký một người mới để xác nhận khởi tạo lại được.

## Kiểm tra khi mất kết nối

- Sau khi chạy đạt mục 1–6, tắt rồi bật lại hotspot. Xem camera tự kết nối lại, cập nhật IP trên web nếu DHCP cấp IP khác. Kiểm tra UART và nhận diện tại chỗ trong thời gian mất Wi-Fi.
- Với dữ liệu thử, có thể tắt backend trong một buổi MQTT đang mở rồi bấm nút DevKit. Giữ điện cho DevKit, bật lại backend trong cùng buổi và đối chiếu `BACKEND ACK` cùng bản ghi. Hàng đợi RAM chứa tối đa 8 kết quả.
- Nếu kiểm tra gián đoạn lúc đăng ký, dùng người thử chưa lưu mẫu và chờ hệ thống đối chiếu lượt cũ sau khi kết nối lại. Kiểm tra web tạo tối đa một hồ sơ đúng MSSV. Nếu hủy sau khi đã thu một phần mẫu và camera báo khóa, khởi động lại camera để thử lượt mới.

## Cách demo xử lý tại biên

### Demo giữ Wi-Fi và xem ảnh trên web

1. Kết nối hai bo với hotspot, mở một buổi thử và bật ảnh xem trước trong **Camera điểm danh**. Mở **F12 → Network**, xóa danh sách cũ rồi bấm **Chụp và điểm danh** và làm theo hướng dẫn xoay mặt.
2. Lọc `checkin-frame` và xem **Headers**. `GET /api/camera/checkin-frame/<event_id>` trả `Content-Type: image/jpeg` để hiển thị ảnh vừa chụp. `GET /api/camera/frame` phục vụ ảnh trực tiếp. Trình duyệt tải cả hai qua `fetch` nên có thể thấy chúng dưới **Fetch/XHR**.
3. Lọc `camera/status` và xem **Response** của lượt tương ứng. Đối chiếu `camera.inference`, `camera.last_face_id` và `camera.checkin_snapshot.status`. Nút web bắt đầu bằng `POST /api/checkins` trả `event_id`, rồi hỏi `GET /api/checkins/<event_id>` để lấy `status` và `message`.
4. Thử nút DevKit ở một buổi thử khác. Khi `CAMERA UART: connected`, bấm một lần và xem `CAMERA ACK: capture requested`, `CAMERA: recognized Face ID=0` và `BACKEND ACK: ... accepted` ở Serial Monitor DevKit **115200 baud**. `Face ID=0` chỉ là ví dụ. Đường này dùng UART giữa hai bo và MQTT từ DevKit tới broker. Xem [định dạng JSON MQTT của dự án](PROTOCOL.md#mqtt).
5. Chỉ đoạn nhận diện trong `face_engine.cpp` và endpoint chuyển tiếp JPEG trong `backend/classroom/camera.py`. Giải thích chip ESP32 xử lý khung hình và so khớp đặc trưng. PSRAM cấp bộ nhớ làm việc. Flask nhận kết quả để ghi điểm danh và chuyển JPEG cho trình duyệt hiển thị.

Network của trình duyệt chỉ quan sát các yêu cầu của trang. Dùng log DevKit hoặc giao diện broker để đối chiếu bản tin MQTT. Biểu đồ lưu lượng mạng tổng không xác định được vai trò của các gói dữ liệu.

### Kiểm tra khi camera tắt Wi-Fi

1. Sau khi đăng ký một người, mở Serial Monitor của **ESP32-CAM** ở 115200 baud và chọn **New Line**. Gửi `STATUS` để xác nhận `camera=ready` và `face_count` lớn hơn 0. Thử `TEST` một lần với người đã đăng ký.
2. Gửi `WIFI_OFF`, rồi gửi `STATUS` để hiện `setup_WiFi=OFF`. Cho người đã đăng ký đứng trước camera và gửi `TEST`. Tìm `LOCAL TEST status=recognized face_id=… | inference=ESP32-CAM`. Đưa mặt ra khỏi khung và gửi `TEST` lần nữa. Kỳ vọng `no_face` để đối chiếu với lượt có mặt. Gửi `RESTART` hoặc bấm RST để camera vào hotspot lại.
3. Mở `face_engine.cpp` và chỉ các bước `face_detect()`, `align_face()`, `get_face_id()` và `findClosestFace()`. Log và mã nguồn cho thấy firmware camera chạy từ khung hình đến Face ID.
4. Nối UART với DevKit và tắt hotspot hoặc Flask. Mở Serial Monitor của DevKit ở 115200 baud. Bấm nút một lần và làm theo `1/3 NHIN THANG`, `2/3 XOAY NHE` và `3/3 NHIN THANG LAI`. Kiểm tra `CAMERA: recognized` và servo quay. Thử thêm một lượt giữ mặt bất động sau khi vào bước 2 để xác nhận `CAMERA: liveness_failed` và cửa giữ nguyên trạng thái.
5. Chạy backend, mở buổi học, xem message `/attendance` và dòng sinh viên trên web. Message chỉ chứa ID/trạng thái; không có ảnh.

Thời gian nhận diện và sai số phụ thuộc ánh sáng, góc mặt và giới hạn ESP32 đời đầu. Đo ít nhất 10 lần cho mỗi người, thêm người chưa đăng ký và ghi lại thời gian / số lần đúng. Nêu rõ loại ảnh đã thử khi báo cáo hiệu quả chống giả mạo.

