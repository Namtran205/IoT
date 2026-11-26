/* Smart Classroom: ESP32-CAM AI-Thinker, Arduino ESP32 core 2.0.0.
   Board: AI Thinker ESP32-CAM, Huge APP (3MB No OTA).
   Button: CAM GPIO13 to GND. UART: CAM RX14 <- DevKit TX33,
   CAM TX15 -> DevKit RX25, common GND. See camera_config.h for legacy wiring.
   Wi-Fi: joins the laptop's 2.4 GHz Mobile Hotspot. Inference stays on chip.
   USB Serial commands (115200, newline): STATUS, ENROLL, TEST, CLEAR_FACES.

   Cau hinh Wi-Fi trong backend/.env. Mo web bang start.bat truoc khi Upload.
   Sau Upload, mo Serial Monitor 115200 va Reset; mo dia chi CAMERA URL
   tren LAPTOP dang phat hotspot. Hinh anh den tu ESP32-CAM.
*/
#include <Arduino.h>
#include "face_engine.h"
#include "camera_web.h"
#include "camera_attendance.h"
#include "camera_config.h"

// DIEN WI-FI MOBILE HOTSPOT CUA LAPTOP TAI DAY (bang tan 2.4 GHz).
// Khong phai mat khau dang nhap Windows. Giu thong tin trong dau ngoac kep.
const char *ssid = "";
const char *password = "";

HardwareSerial controller(1);
static String uartLine, usbLine;
static const char *uartEnrollmentId = nullptr;
static int uartEnrollmentSamples = 0;
static bool cameraReady = false, overflowUART = false, overflowUSB = false;
static unsigned long lastPreview=0;
static portMUX_TYPE buttonMux = portMUX_INITIALIZER_UNLOCKED;
static bool directPressPending = false;
static uint32_t buttonBootId, buttonSequence = 0;

static void readDirectButton(void *) {
  int raw = digitalRead(CAM_DIRECT_BUTTON_GPIO), stable = raw;
  unsigned long changedAt = millis();
  for (;;) {
    const int reading = digitalRead(CAM_DIRECT_BUTTON_GPIO);
    const unsigned long now = millis();
    if (reading != raw) {
      raw = reading;
      changedAt = now;
    } else if (raw != stable && now - changedAt >= 35) {
      stable = raw;
      if (stable == LOW) {
        portENTER_CRITICAL(&buttonMux);
        directPressPending = true;
        portEXIT_CRITICAL(&buttonMux);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static bool takeDirectPress() {
  portENTER_CRITICAL(&buttonMux);
  const bool pending = directPressPending;
  directPressPending = false;
  portEXIT_CRITICAL(&buttonMux);
  return pending;
}
static bool validRequest(const String &id) {
  if (!id.length() || id.length() > 60) return false;
  for (unsigned int i = 0; i < id.length(); ++i)
    if (!isalnum((unsigned char)id[i]) && id[i] != '-' && id[i] != '_') return false;
  return true;
}

static bool readButtonCommand(const String &line, String &id, bool &local) {
  if (line == "BUTTON_PING") {
    cameraWebNoteButton();
    controller.println("BUTTON_PONG");
    return false;
  }
  local = line.startsWith("CHECKIN_LOCAL:");
  if (!local && !line.startsWith("CHECKIN:")) return false;
  id = line.substring(local ? 14 : 8);
  if (!validRequest(id)) return false;
  local = local || cameraAttendanceButtonFallback();
  return true;
}

static bool readEnrollmentCommand(const String &line, String &id) {
  if (!line.startsWith("ENROLL:")) return false;
  id = line.substring(7);
  return id.length() < sizeof(FaceEngineStatus::enrollment_token) && validRequest(id);
}

static void replyEnrollment(const char *id, const char *status, int number) {
  controller.printf("ENROLL:%s:%s:%d\n", id, status, number);
}

static void reply(const String &requestId, const char *status, int faceId=-1) {
  controller.printf("RESULT:%s:%s:%d\n", requestId.c_str(), status, faceId);
  Serial.printf("RESULT request=%s status=%s face_id=%d | inference=ESP32-CAM\n", requestId.c_str(), status, faceId);
}

static void replyBusy();

static void sendCheckinGuide(const char *id, const char *step, const char *hint, float score) {
  controller.printf("CHECKIN_GUIDE:%s:%s:%s:%.3f\n", id, step, hint, score);
}

static const char *captureCheckin(const char *id, int &faceId, CheckinFeedback feedback) {
  cameraWebBeginCheckinFrame(id);
  const char *status = cameraReady ? recognizeLiveFace(faceId, id, replyBusy, feedback) : "error";
  const FaceEngineStatus result = faceEngineStatus();
  cameraWebFinishCheckinFrame(id, status, result.frame_light, result.match_score);
  if (feedback) feedback(id, "result", status, cameraReady ? result.match_score : NAN);
  return status;
}

static void handleDirectButton(bool busy) {
  if (!takeDirectPress()) return;
  char id[40];
  snprintf(id, sizeof(id), "cam-button-%08lx-%lu", (unsigned long)buttonBootId, (unsigned long)++buttonSequence);
  const AttendanceSession session = cameraAttendanceSession();
  int faceId = -1;
  const char *status = "busy";
  if (!busy) {
    digitalWrite(33, LOW);
    status = captureCheckin(id, faceId, nullptr);
    digitalWrite(33, HIGH);
  }
  cameraAttendanceResult(id, session, status, faceId);
  Serial.printf("CAM BUTTON GPIO%d request=%s status=%s face_id=%d\n", CAM_DIRECT_BUTTON_GPIO, id, status, faceId);
}

static bool readLine(Stream &stream, String &buffer, bool &overflow, String &line) {
  while (stream.available()) {
    char c = stream.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (overflow) { overflow = false; buffer = ""; continue; }
      line = buffer; buffer = ""; line.trim(); if (line.length()) return true;
    } else if (buffer.length() < 100 && c >= 32 && c <= 126) buffer += c;
    else { overflow = true; buffer = ""; }
  }
  return false;
}

static void replyBusy() {
  cameraWebPollNetwork();
  handleDirectButton(true);
  if (uartEnrollmentId) {
    const int samples = faceEngineStatus().samples;
    if (samples != uartEnrollmentSamples) {
      uartEnrollmentSamples = samples;
      replyEnrollment(uartEnrollmentId, "progress", samples);
    }
  }
  String command;
  while (readLine(controller, uartLine, overflowUART, command)) {
    String id;
    bool local;
    if (readButtonCommand(command, id, local)) {
      cameraWebNoteButton(id.c_str(), "busy");
      if (local) cameraAttendanceResult(id.c_str(), cameraAttendanceSession(), "busy", -1);
      reply(id, "busy");
    } else if (readEnrollmentCommand(command, id)) {
      replyEnrollment(id.c_str(), "busy", -1);
    }
  }
  CameraCommand action;
  char token[40] = {};
  while (cameraWebPopCommand(action, token, sizeof(token))) {
    if (action==CameraCommand::Cancel) cancelEnrollment();
    if (action==CameraCommand::Checkin) cameraAttendanceResult(token, {}, "busy", -1);
    // A command may have been queued just before enrollment started.
    // Ignore it while enrollment owns the camera; never reenter inference.
  }
}

static void enrollFromController(const String &id) {
  if (!cameraReady) {
    replyEnrollment(id.c_str(), "camera_error", -1);
    return;
  }
  uartEnrollmentSamples = 0;
  uartEnrollmentId = id.c_str();
  replyEnrollment(uartEnrollmentId, "started", 0);
  Serial.println(enrollOneFace(replyBusy, uartEnrollmentId));
  uartEnrollmentId = nullptr;

  const FaceEngineStatus result = faceEngineStatus();
  for (int slot = 0; slot < result.face_count; ++slot) {
    if (!strcmp(result.enrolled_tokens[slot], id.c_str())) {
      replyEnrollment(id.c_str(), "enrolled", slot);
      return;
    }
  }
  replyEnrollment(id.c_str(), result.enrollment_result, result.enrollment_face_id);
}

void setup() {
  Serial.begin(115200); delay(800);
  controller.begin(115200, SERIAL_8N1, CAM_UART_RX_GPIO, CAM_UART_TX_GPIO);
  if (CAM_DIRECT_BUTTON_ENABLED) {
    pinMode(CAM_DIRECT_BUTTON_GPIO, INPUT_PULLUP);
    buttonBootId = esp_random();
    if (xTaskCreate(readDirectButton, "camera-button", 2048, nullptr, 1, nullptr) == pdPASS) {
      cameraWebEnableDirectButton();
      Serial.printf("CAM BUTTON READY | GPIO%d to GND\n", CAM_DIRECT_BUTTON_GPIO);
    } else Serial.println("CAM BUTTON ERROR: cannot start reader task");
  }
  pinMode(33, OUTPUT); digitalWrite(33, HIGH);
  pinMode(4,OUTPUT); digitalWrite(4,LOW); // Keep the high-power flash off.
  Serial.println("Smart Classroom CAM | on-device face inference | setup preview available");
  Serial.printf("BOOT reset_reason=%d | uptime_ms=%lu\n", (int)esp_reset_reason(), millis());
  cameraReady = initializeCameraAndFaces();
  if (!cameraWebBegin(ssid, password)) Serial.println("Camera web unavailable; see Wi-Fi error above. UART / USB still work.");
  Serial.println("USB commands: STATUS | ENROLL | TEST | CLEAR_FACES | WIFI_OFF | RESTART");
}

void loop() {
  cameraWebPollNetwork();
  handleDirectButton(false);
  String command;
  while (readLine(controller, uartLine, overflowUART, command)) {
    String id;
    bool local;
    if (readButtonCommand(command, id, local)) {
      const AttendanceSession session = local ? cameraAttendanceSession() : AttendanceSession{};
      cameraWebNoteButton(id.c_str(), "capturing");
      controller.printf("CHECKIN_ACK:%s\n", id.c_str());
      int faceId = -1;
      digitalWrite(33, LOW);
      const char *status = captureCheckin(id.c_str(), faceId, sendCheckinGuide);
      cameraWebNoteButton(id.c_str(), status);
      if (local) cameraAttendanceResult(id.c_str(), session, status, faceId);
      // Map the challenge rejection for controllers with the original UART protocol.
      reply(id, !strcmp(status, "liveness_failed") ? "unknown" : status, faceId);
      digitalWrite(33, HIGH);
      break;
    } else if (readEnrollmentCommand(command, id)) {
      enrollFromController(id);
      break;
    }
  }
  if (readLine(Serial, usbLine, overflowUSB, command)) {
    if (command == "STATUS") {
      Serial.printf("camera=%s | face_count=%d | setup_WiFi=%s\n", cameraReady?"ready":"error", enrolledFaceCount(),cameraWebRadioEnabled()?"ON":"OFF");
      cameraWebPrintNetwork();
    }
    else if (command == "ENROLL") {
      if (cameraReady) Serial.println(enrollOneFace(replyBusy));
      else Serial.println("ERROR: camera not initialized");
    } else if (command == "TEST") {
      int faceId = -1; const char *status = cameraReady ? recognizeOneFace(faceId) : "error";
      Serial.printf("LOCAL TEST status=%s face_id=%d | inference=ESP32-CAM\n", status, faceId);
    } else if (command == "CLEAR_FACES") {
      if (clearAllFaces()) { Serial.println("All faces cleared. Rebooting."); delay(300); ESP.restart(); }
      else Serial.println("ERROR: cannot clear face storage");
    } else if (command=="WIFI_OFF") cameraWebRadioOff();
    else if (command=="RESTART") { delay(200); ESP.restart(); }
    else Serial.println("Unknown command. Use STATUS, ENROLL, TEST, CLEAR_FACES, WIFI_OFF, RESTART.");
  }
  CameraCommand action;
  char enrollmentToken[40] = {};
  if (cameraWebPopCommand(action, enrollmentToken, sizeof(enrollmentToken))) {
    if (action==CameraCommand::Enroll) {
      if (cameraReady) Serial.println(enrollOneFace(replyBusy, enrollmentToken));
    } else if (action==CameraCommand::Checkin) {
      int faceId=-1;
      const char *status=cameraWebCheckinFrameHeld() ? "busy" : captureCheckin(enrollmentToken, faceId, nullptr);
      cameraAttendanceResult(enrollmentToken, {}, status, faceId);
      Serial.printf("WEB CHECKIN request=%s status=%s face_id=%d\n",enrollmentToken,status,faceId);
    } else if (action==CameraCommand::Test) {
      int id=-1; const char *status=cameraReady?recognizeOneFace(id):"error";
      Serial.printf("LOCAL WEB TEST status=%s face_id=%d | inference=ESP32-CAM\n",status,id);
    } else if (action==CameraCommand::Rotate) rotateCamera();
    else if (action==CameraCommand::Restart) { delay(400); ESP.restart(); }
    else if (action==CameraCommand::Clear) {
      if (clearAllFaces(enrollmentToken)) { delay(400); ESP.restart(); }
    } else if (action==CameraCommand::RadioOff) { delay(500); cameraWebRadioOff(); }
  }
  if (cameraReady && cameraWebPreviewWanted() && millis()-lastPreview>=CAM_PREVIEW_INTERVAL_MS) {
    previewOneFrame(); lastPreview=millis();
  }
  delay(10);
}

