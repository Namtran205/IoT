# Nguồn và quyết định tích hợp

- Các tài liệu và mã nguồn người dùng đã gửi trong hội thoại chia sẻ được đối chiếu tại `shared-chat-review/review-context.json`.
- Hệ thống mới dùng kiến trúc chốt ở cuối hội thoại: nhận diện trên camera, UART sang ESP32 trung tâm, MQTT kết quả, backend lưu điểm danh.
- `face_engine.cpp` dùng mô hình và API esp-face đi kèm [Arduino-ESP32 2.0.0](https://github.com/espressif/arduino-esp32/releases/tag/2.0.0), cùng chuỗi detect/align/recognize đã thử trong [CameraWebServer-YAF](https://github.com/sigmdel/CameraWebServer-YAF). Mô hình có sẵn; enrollment không phải tự huấn luyện lại mô hình bằng Edge Impulse.
- [Flask SQLite pattern](https://flask.palletsprojects.com/en/stable/patterns/sqlite3/), [Paho MQTT Python](https://github.com/eclipse-paho/paho.mqtt.python).
- [Thay đổi LEDC từ Core 2.x sang 3.0](https://docs.espressif.com/projects/arduino-esp32/en/latest/migration_guides/2.x_to_3.0.html) giải thích hai nhánh PWM trong controller.
- Trang cài đặt camera dùng SoftAP của [Arduino-ESP32 Wi-Fi API](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/wifi.html) và `esp_http_server`; phần chuyển ảnh/vẽ khung dùng API có trong [CameraWebServer Core 2.0.0](https://github.com/espressif/arduino-esp32/blob/2.0.0/libraries/ESP32/examples/Camera/CameraWebServer/app_httpd.cpp). Giao diện và hàng đợi lệnh được viết cho dự án này; AI vẫn chạy trên chip.
- ArduinoJson và PubSubClient được đặt nguyên cùng license trong `firmware/libraries/` để có thể kiểm tra bằng CLI trong dự án.

Không sao chép mật khẩu từ tài liệu cũ vào cấu hình mẫu. Dự án gốc ở ngoài thư mục IOT được giữ nguyên.
