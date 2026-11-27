# Edge AI, dữ liệu khuôn mặt và bảo mật

## 1. Thiết bị nào thực sự chạy AI?

**Vi điều khiển ESP32 trên bo ESP32-CAM** là thiết bị xử lý AI tại biên. “Tại biên” nghĩa là xử lý ở thiết bị gần nơi thu dữ liệu. Cảm biến OV2640 chỉ tạo ảnh. Vi điều khiển trên cùng bo chạy mô hình trong firmware, trích vector và quyết định Face ID. ESP32-DevKit là vi điều khiển thứ hai, phụ trách nút nhấn, cảm biến và servo. Flask trên laptop điều phối lệnh chụp, chuyển ảnh xem trước, kiểm tra kết quả nghiệp vụ và lưu điểm danh. EMQX chuyển tiếp bản tin MQTT.

Mô hình đã được huấn luyện trước và liên kết vào firmware từ thư viện esp-face của Arduino-ESP32 Core 2.0.0. Quá trình đăng ký giữ nguyên trọng số mô hình và thêm mẫu tham chiếu để chip so khớp ở lượt chụp sau. [ESP-WHO của Espressif](https://github.com/espressif/esp-who/releases/tag/v0.9.0)

| Công đoạn | Nơi xử lý | Đầu ra |
|---|---|---|
| Chụp QVGA 320 × 240 và giải mã JPEG | OV2640 và ESP32-CAM | Khung màu trong bộ nhớ tạm |
| Dò mặt và tìm điểm mốc bằng MTMN | ESP32-CAM | Vùng khuôn mặt và các điểm mốc |
| Căn chỉnh theo điểm mốc | ESP32-CAM | Ảnh mặt 56 × 56 × 3 |
| Trích đặc trưng bằng MobileFace/FRMN | ESP32-CAM | Vector 512 số float |
| Chuẩn hóa và so khớp với các mẫu | ESP32-CAM | Face ID, trạng thái và điểm khớp nếu tính được |
| Ánh xạ ID, kiểm tra buổi và ghi điểm danh | Flask + SQLite trên laptop | Bản ghi điểm danh |

### Đăng ký khuôn mặt

1. Nhập MSSV và họ tên trên dashboard. Flask tạo lượt đăng ký và gửi một mã lượt cho ESP32-CAM.
2. ESP32-CAM tự chụp, căn mặt và trích vector cho từng mẫu hợp lệ. Firmware kiểm tra mặt đăng ký trùng và độ nhất quán giữa các mẫu.
3. Sau 5 mẫu, camera lấy trung bình các vector rồi chuẩn hóa L2. Một mẫu tham chiếu chuẩn hóa được lưu cho mỗi Face ID.
4. Camera xác nhận đã ghi đúng mã lượt vào bộ lưu trữ. Flask kiểm tra đúng Face ID, mã lượt và trạng thái bộ nhớ rồi mới liên kết sinh viên.

### Kho vector nằm ở đâu?

Kho vector là **NVS trong flash trên ESP32-CAM**, namespace `classroom-face`, lưu bằng thư viện `Preferences`. Mỗi khóa `face0` đến `face6` là một bản ghi nhị phân gồm 512 số float, phiên bản và kích thước vector, mã lượt đăng ký và checksum. Khóa `count` ghi số bản đã lưu. Mỗi vector float chiếm khoảng 2 KiB trước phần metadata. Khi khởi động, firmware đọc các bản ghi, kiểm tra định dạng và checksum, chuẩn hóa vector và đưa vào RAM để so khớp. NVS giữ dữ liệu qua khởi động lại và mất điện. Đây là kho key–value nhỏ dành cho tối đa 7 người. [Arduino-ESP32 Preferences và NVS](https://docs.espressif.com/projects/arduino-esp32/en/latest/tutorials/preferences.html)

SQLite `backend/instance/classroom.sqlite3` lưu MSSV, tên, ánh xạ `(camera_id, face_id)`, buổi, lịch sử điểm danh và biên nhận. Ví dụ, `CAM01 / Face ID 0` là khóa để Flask tra sinh viên. Ảnh và vector không được ghi vào SQLite trong luồng hiện tại. Trọng số model nằm ở phân vùng ứng dụng của firmware, còn vector của người dùng nằm ở NVS.

### Khi chụp điểm danh

ESP32-CAM chạy chuỗi chụp, dò mặt, căn chỉnh và trích vector. Với các vector đã chuẩn hóa L2, tích vô hướng tương ứng với cosine similarity. Camera so vector mới với các mẫu trong RAM và chọn Face ID có điểm cao nhất nếu điểm **lớn hơn 0,52** khi điểm danh. Lệnh chẩn đoán `TEST` và kiểm tra mẫu đăng ký dùng **0,55**. Trước khi trả `recognized` cho lượt điểm danh, camera kiểm tra thử thách chuyển động đầu trên các khung liên tiếp.

1. **Nhìn thẳng.** Camera xác định Face ID ban đầu và ghi lại vị trí tương đối của mũi so với mắt và khóe miệng.
2. **Xoay nhẹ theo yêu cầu.** Camera chọn trái hoặc phải ngẫu nhiên. Hướng dẫn hiện trên web và trên Serial Monitor của DevKit khi dùng nút DevKit. Để qua bước này, vị trí tương đối của mũi phải chuyển theo hướng đó ở hai khung liên tiếp. Phép tính theo trục mắt giảm ảnh hưởng của dịch chuyển, thay đổi kích thước và nghiêng ảnh trong mặt phẳng.
3. **Nhìn thẳng lại.** Camera so khớp lại và yêu cầu cùng Face ID ban đầu. Khung cuối được giữ để xem lại. Mỗi bước có thời hạn riêng. Camera chờ tới 25 giây để khớp mặt ban đầu, 16 giây để xoay và 12 giây để nhìn thẳng lại. Sau khi khớp mặt, camera cho phép mất dấu tạm thời tới 8 giây. Khi lượt đã khớp mặt ở bước đầu nhưng không hoàn thành thử thách hoặc khớp người khác ở bước cuối, kết quả là `liveness_failed`. Lượt đó không ghi điểm danh hay mở cửa qua nhận diện.

- **Nút web:** Flask gửi mã lượt qua HTTP nội bộ. Camera xử lý và trả kết quả cho Flask. Ảnh xem trước và ảnh giữ lại của lượt chụp là JPEG đi theo đường HTTP riêng để hiển thị.
- **Nút DevKit:** DevKit gửi mã yêu cầu qua UART và nhận trạng thái cùng Face ID. Servo có thể mở theo phản hồi hợp lệ trước khi Flask ghi nhận điểm danh. Khi dùng MQTT, DevKit gửi sự kiện đến broker rồi Flask xử lý. Chế độ nội bộ dùng HTTP từ camera để chuyển kết quả về laptop.
- **Nút trên ESP32-CAM:** nếu lắp, camera tự nhận nút, suy luận và trả kết quả qua HTTP nội bộ.

Việc thiết bị dùng Wi-Fi để nhận lệnh hoặc gửi kết quả vẫn phù hợp với Edge AI. Mất Internet và mất Wi-Fi là hai tình huống khác nhau. Nếu LAN còn hoạt động, luồng web qua HTTP nội bộ có thể vẫn chạy khi Internet hoặc broker ngắt. Nếu Wi-Fi camera tắt, có thể chứng minh suy luận bằng lệnh `TEST` trên USB. Ghi điểm danh lên dashboard vẫn cần một đường truyền kết quả và một buổi hợp lệ.

## 2. Ranh giới bảo mật của bản demo

| Rủi ro | Cơ chế đang có | Giới hạn |
|---|---|---|
| Lộ ảnh khuôn mặt qua mạng | JPEG dùng `Cache-Control: no-store`. Khi firmware mới được nạp, ảnh xem trước hết hạn theo tuổi khung sau 6 giây. Ảnh điểm danh được dọn khi mở lại live hoặc sau 15 giây kể từ lúc có kết quả. | HTTP camera ↔ laptop hiện chưa có TLS. Thiết bị có đường truy cập camera có thể xem ảnh nếu endpoint đang cung cấp khung. |
| Đọc hoặc điều khiển từ tài khoản trái phép | Ở cấu hình localhost, Flask chấp nhận `127.0.0.1` và `localhost` trong Host, với `no-store` cho phản hồi API. API ghi kiểm tra header `X-Classroom-Client` và cùng origin nếu request có Origin. Camera kiểm tra tương tự với `X-Camera-Control`. Backend giới hạn địa chỉ camera ở IPv4 nội bộ và đường dẫn cho phép, chặn chuyển hướng và proxy, giới hạn kích thước phản hồi. | Header này là giá trị quy ước công khai, phục vụ chặn lệnh từ web khác trong trình duyệt. Camera và dashboard chưa có đăng nhập hoặc phân quyền người dùng. Giới hạn địa chỉ làm giảm bề mặt truy cập thay mặt backend, nhưng các IP nội bộ vẫn cần kiểm soát. |
| Đọc hoặc gửi bản tin MQTT ngoài quyền | MQTT tới EMQX Cloud dùng TLS ở cổng 8883, username/password và xác minh chứng chỉ broker. Cấu hình dự án đặt `MQTT_TLS=true` và `MQTT_ALLOW_INSECURE=false`. | TLS bảo vệ các kết nối tới broker. Broker xử lý payload sau khi kết thúc TLS. Chính sách cấp quyền publish/subscribe của tài khoản phải được cấu hình và kiểm tra tại EMQX. Mỗi chuỗi `device_id` hay `camera_id` là định danh ứng dụng. |
| Gửi lại kết quả, ghi điểm danh sai buổi | Đăng ký đối chiếu mã lượt và Face ID. DevKit đối chiếu request ID. Backend kiểm tra thiết bị, camera, Face ID, buổi đang mở và `generation`, từ chối điểm danh từ bản tin retained, chống ghi lặp bằng `event_id` và ràng buộc duy nhất theo sinh viên/buổi. | Các kiểm tra này bảo vệ luồng nghiệp vụ trước lặp và phản hồi cũ. Bản tin, UART và các ID chưa có chữ ký hoặc cơ chế xác thực riêng ở tầng ứng dụng. Servo có thể mở theo phản hồi UART trước xác nhận của Flask. |
| Trộm thiết bị, đọc ổ đĩa hoặc sao chép bản lưu | Mẫu sinh trắc ở flash camera, SQLite ở laptop. Bản ghi NVS có checksum để phát hiện dữ liệu lỗi. `.gitignore` có quy tắc loại `.env`, `secrets.h`, `camera_secrets.h` và dữ liệu SQLite khỏi mã nguồn được chia sẻ. | Cấu hình demo chưa triển khai hoặc xác nhận NVS encryption, flash encryption và Secure Boot, còn SQLite dùng kiểu lưu mặc định. Checksum là phép kiểm tra dữ liệu, không dùng khóa mật mã. Cần bảo vệ quyền truy cập thiết bị, file, CSV và bản sao. |
| Dùng ảnh hoặc màn hình để giả danh | Thử thách xoay đầu ngẫu nhiên chạy trên ESP32-CAM trước khi chấp nhận điểm danh qua web hoặc nút vật lý. Firmware yêu cầu cùng Face ID trước và sau chuyển động. | Camera RGB một mắt dùng ước lượng từ các điểm mốc. Cần đo hiệu quả trên mạch thật. Video phát lại, biến dạng ảnh hoặc mô hình 3D cần đánh giá riêng. Đăng ký vẫn dùng quy trình 5 mẫu và cần người phụ trách giám sát. |

**Dữ liệu trên MQTT.** Bản tin `/attendance` do DevKit gửi chứa trạng thái, Face ID và các mã nghiệp vụ. Bản tin `/attendance/ack` chỉ chứa `event_id` và `status`. Nội dung thông báo có MSSV và họ tên được xử lý ở backend để hiển thị trên web. MQTT chỉ mang dữ liệu nghiệp vụ và số đo. [Kết nối EMQX Serverless qua TLS](https://docs.emqx.com/en/cloud/latest/deployments/port_guide_serverless.html) · [Phân quyền topic EMQX](https://docs.emqx.com/en/cloud/latest/deployments/authz_overview.html)

**Xóa và lưu giữ dữ liệu.** Vector vẫn là dữ liệu sinh trắc vì dùng để nhận lại người đã đăng ký. Reset một buổi xóa kết quả buổi đó, tăng `generation` và giữ hồ sơ cùng mẫu mặt. Xóa dữ liệu thử nghiệm là thao tác khác. Backend yêu cầu camera xác nhận xóa đúng thiết bị rồi dọn dữ liệu nghiệp vụ. Bản CSV và bản sao đã xuất cần quản lý riêng. Trong demo, dùng hồ sơ thử nghiệm và người đồng ý tham gia.

**Chia sẻ mã nguồn.** Cấu hình Wi-Fi camera nằm trong `backend/.env`. Khi khởi động, backend tự tạo `camera_secrets.h` cho Arduino. Git bỏ qua cả hai file. Khi chia sẻ mã nguồn, giữ lại `.env.example` và header mẫu MQTT.

**Ưu tiên nếu triển khai thật.** Thêm đăng nhập và phân quyền cho thao tác quản trị. Cô lập camera trên mạng riêng và bảo vệ HTTP bằng xác thực thiết bị cùng kênh mã hóa phù hợp. Tách tài khoản MQTT của DevKit và Flask, giới hạn quyền theo topic. Cấu hình và kiểm chứng mã hóa NVS, Secure Boot và bảo vệ ổ đĩa laptop. Đánh giá kết quả với người thật, ảnh và video phát lại. Dùng thêm yếu tố xác thực trước khi dùng nhận diện để điều khiển cửa thật. [Mã hóa NVS của Espressif](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/storage/nvs_encryption.html) · [NIST về nhận diện sinh trắc và PAD](https://pages.nist.gov/800-63-4/sp800-63b.html#presentation-attack-detection)

## 3. Cách trình bày và kiểm chứng

1. Đăng ký một khuôn mặt thử trong dashboard và tìm `ENROLL SAVED | Face ID=...` trong Serial Monitor của camera. Khởi động lại để kiểm tra `saved faces` và nhận diện lại.
2. Chụp bằng nút web, đối chiếu log `CHECKIN DETECTION` trên camera với thời điểm Flask ghi điểm danh. Nêu rõ ảnh dùng để xem trước có thể đi qua mạng nội bộ trong khi vector và phép so khớp ở trên chip.
3. Với mẫu đã lưu, chọn **COM của camera, 115200 baud, New Line**. Gửi `WIFI_OFF` rồi `TEST`. Xem trạng thái và Face ID qua Serial ngay khi Wi-Fi camera tắt. Khởi động lại camera để trở về chế độ kết nối. Bài này minh họa suy luận trên chip, còn điểm danh qua web cần kết nối.
4. Cho người đã đăng ký và người chưa đăng ký thử mỗi nhóm nhiều lượt, đổi điều kiện sáng và góc mặt. Ghi số nhận đúng, nhầm, từ chối và thời gian xử lý. Ghi rõ số mẫu thử và cách tính. Dùng tình huống khung trống hoặc nhiều mặt để phân biệt lỗi dò/căn với điểm khớp thấp.
5. Kiểm tra bấm hai lần trong cùng buổi chỉ sinh một bản ghi. Các bài test backend đã xác nhận kiểm tra origin, gửi lặp, bản tin retained, token đăng ký và kết quả cũ sau reset. Đây là kiểm thử nghiệp vụ, còn chất lượng model và an toàn khi mở cửa cần kiểm tra riêng trên mạch.

Trong `face_engine.cpp`, xem `enrollOneFace`, `recognizeOneFace`, `saveFace` và `loadFaces` để đối chiếu xử lý AI và lưu mẫu. Phần còn lại nằm ở `camera_web.cpp`, `SmartClassroomController.ino`, `backend/classroom/camera.py`, `checkin.py`, `mqtt.py` và `service.py`. Xem [giao tiếp hệ thống](PROTOCOL.md) và [kịch bản demo](TESTING.md).
