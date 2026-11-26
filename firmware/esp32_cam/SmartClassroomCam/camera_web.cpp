#include "camera_web.h"
#include "camera_config.h"
#include "camera_page.h"
#include "camera_attendance.h"
#include "face_engine.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#if __has_include("camera_secrets.h")
#include "camera_secrets.h"
#endif
#include "fr_forward.h"

static httpd_handle_t server = nullptr;
static QueueHandle_t commands = nullptr;
static SemaphoreHandle_t frameMutex = nullptr;
static uint8_t *cachedFrame = nullptr;
static size_t cachedLength = 0;
static unsigned long cachedFrameAt = 0;
static SemaphoreHandle_t checkinFrameMutex = nullptr;
static uint8_t *checkinFrame = nullptr;
static size_t checkinFrameLength = 0;
static char checkinFrameId[61] = {};
static char checkinFrameStatus[24] = {};
static const char *checkinFrameLight = "waiting";
static float checkinFrameMatchScore = NAN;
static bool checkinFrameCaptured = false;
static bool checkinFramePublished = false;
static bool checkinFrameFinished = false;
static unsigned long checkinFrameFinishedAt = 0;
static portMUX_TYPE networkMux = portMUX_INITIALIZER_UNLOCKED;
static bool buttonSeen = false;
static bool directButtonReady = false;
static unsigned long lastButtonSignal = 0;
static char buttonRequestId[61] = {}, buttonStatus[24] = {};
static unsigned long lastFrameRequest = 0;
static bool previewRequested = false, radioEnabled = false;
static bool checkinFrameHeld = false;
static bool checkinChallengeActive = false;
static bool stationConfigured = false;
static unsigned long lastNetworkPoll = 0, lastReconnect = 0;
static uint32_t lastStationIP = 0;
static int lastStationStatus = -1;
static uint32_t disconnectCount = 0;
static int disconnectReason = 0;
struct QueuedCommand { CameraCommand command; char token[40]; };

static esp_err_t jsonReply(httpd_req_t *req, const char *status, const char *body) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t rootHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, CAMERA_PAGE, sizeof(CAMERA_PAGE) - 1);
}

static const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_SW: return "software_restart";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    default: return "other";
  }
}

static void faceResetDevice(char (&device)[13]) {
  snprintf(device, sizeof(device), "%012llx", static_cast<unsigned long long>(ESP.getEfuseMac()) & 0xffffffffffffULL);
}

// Call these with the corresponding frame mutex held.
static void releasePreviewFrame() {
  free(cachedFrame);
  cachedFrame = nullptr;
  cachedLength = 0;
}

static void releaseCheckinFrame() {
  free(checkinFrame);
  checkinFrame = nullptr;
  checkinFrameLength = 0;
  checkinFrameId[0] = checkinFrameStatus[0] = '\0';
  checkinFrameLight = "waiting";
  checkinFrameMatchScore = NAN;
  checkinFrameCaptured = checkinFramePublished = checkinFrameFinished = false;
  portENTER_CRITICAL(&networkMux);
  checkinFrameHeld = false;
  checkinChallengeActive = false;
  previewRequested = false;
  portEXIT_CRITICAL(&networkMux);
}

static void expirePreviewFrame() {
  if (cachedFrame && millis() - cachedFrameAt >= CAM_PREVIEW_FRAME_RETENTION_MS) releasePreviewFrame();
}

static void expireCheckinFrame() {
  if (checkinFrameFinished && millis() - checkinFrameFinishedAt >= CAM_CHECKIN_FRAME_RETENTION_MS) releaseCheckinFrame();
}

static esp_err_t statusHandler(httpd_req_t *req) {
  const FaceEngineStatus s = faceEngineStatus();
  String stationIP = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  char tokens[320] = "[";
  size_t offset = 1;
  for (int i=0; i<7; ++i) offset += snprintf(tokens+offset, sizeof(tokens)-offset, "%s\"%s\"", i ? "," : "", s.enrolled_tokens[i]);
  snprintf(tokens+offset, sizeof(tokens)-offset, "]");
  portENTER_CRITICAL(&networkMux);
  uint32_t dropped=disconnectCount; int reason=disconnectReason;
  const bool buttonOnline = buttonSeen && millis() - lastButtonSignal < 8000UL;
  char lastRequest[61], lastStatus[24];
  strlcpy(lastRequest, buttonRequestId, sizeof(lastRequest));
  strlcpy(lastStatus, buttonStatus, sizeof(lastStatus));
  portEXIT_CRITICAL(&networkMux);
  char snapshotId[61], snapshotStatus[24];
  xSemaphoreTake(checkinFrameMutex, portMAX_DELAY);
  expireCheckinFrame();
  strlcpy(snapshotId, checkinFrameId, sizeof(snapshotId));
  strlcpy(snapshotStatus, checkinFrameStatus, sizeof(snapshotStatus));
  const bool hasSnapshot = checkinFramePublished && checkinFrame != nullptr;
  const bool captured = checkinFramePublished && checkinFrameCaptured;
  const char *snapshotLight = checkinFrameLight;
  const float snapshotMatchScore = checkinFrameMatchScore;
  xSemaphoreGive(checkinFrameMutex);
  char matchScore[16] = "null";
  if (isfinite(snapshotMatchScore)) snprintf(matchScore, sizeof(matchScore), "%.4f", snapshotMatchScore);
  char resetDevice[13];
  faceResetDevice(resetDevice);
  char body[2048];
  snprintf(body, sizeof(body),
    "{\"camera_ready\":%s,\"storage_ready\":%s,\"enrollment_locked\":%s,\"enrolling\":%s,"
    "\"face_count\":%d,\"samples\":%d,\"detected_faces\":%d,\"last_face_id\":%d,"
    "\"frame_number\":%lu,\"frame_at\":%lu,\"processing_ms\":%lu,"
    "\"detection\":\"%s\",\"result\":\"%s\",\"frame_light\":\"%s\",\"uptime_ms\":%lu,"
    "\"free_heap\":%u,\"free_psram\":%u,\"reset_reason\":\"%s\","
    "\"station_ip\":\"%s\",\"inference\":\"ESP32-CAM\",\"enrollment_protocol\":2,\"checkin_protocol\":1,"
    "\"enrollment_token\":\"%s\",\"enrollment_result\":\"%s\",\"enrollment_face_id\":%d,\"enrolled_tokens\":%s,"
    "\"face_reset_device\":\"%s\",\"face_reset_token\":\"%s\","
    "\"wifi_rssi\":%d,\"wifi_disconnects\":%u,\"wifi_disconnect_reason\":%d,"
    "\"direct_button_gpio\":%d,\"button_link\":{\"online\":%s,\"last_id\":\"%s\",\"last_status\":\"%s\"},"
    "\"checkin_snapshot\":{\"id\":\"%s\",\"status\":\"%s\",\"captured\":%s,\"ready\":%s,"
    "\"light\":\"%s\",\"match_score\":%s,\"match_threshold\":%.2f,\"liveness_step\":\"%s\"}}",
    s.camera_ready ? "true" : "false", s.storage_ready ? "true" : "false",
    s.enrollment_locked ? "true" : "false", s.enrolling ? "true" : "false",
    s.face_count, s.samples, s.detected_faces, s.last_face_id,
    s.frame_number, s.frame_at, s.processing_ms, s.detection, s.result, s.frame_light,
    millis(), (unsigned int)ESP.getFreeHeap(), (unsigned int)ESP.getFreePsram(), resetReason(), stationIP.c_str(),
    s.enrollment_token, s.enrollment_result, s.enrollment_face_id, tokens, resetDevice, s.face_reset_token,
    WiFi.status()==WL_CONNECTED ? WiFi.RSSI() : 0, dropped, reason,
    directButtonReady ? CAM_DIRECT_BUTTON_GPIO : -1, buttonOnline ? "true" : "false", lastRequest, lastStatus,
    snapshotId, snapshotStatus, captured ? "true" : "false", hasSnapshot ? "true" : "false",
    snapshotLight, matchScore, CAM_CHECKIN_MATCH_THRESHOLD, s.liveness_step ? s.liveness_step : "");
  return jsonReply(req, "200 OK", body);
}

static esp_err_t sendCheckinFrame(httpd_req_t *req, const char *expectedId) {
  // Caller holds checkinFrameMutex while the HTTP task sends the buffer.
  if (expectedId && strcmp(expectedId, checkinFrameId))
    return jsonReply(req, "404 Not Found", "{\"error\":\"snapshot_changed\"}");
  if (!checkinFramePublished || !checkinFrame)
    return jsonReply(req, "503 Service Unavailable", "{\"error\":\"snapshot_unavailable\"}");
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, (const char *)checkinFrame, checkinFrameLength);
}

static esp_err_t checkinFrameHandler(httpd_req_t *req) {
  char expectedId[61] = {};
  if (httpd_req_get_hdr_value_str(req, "X-Checkin-Id", expectedId, sizeof(expectedId)) != ESP_OK || !expectedId[0])
    return jsonReply(req, "400 Bad Request", "{\"error\":\"checkin_id_required\"}");
  xSemaphoreTake(checkinFrameMutex, portMAX_DELAY);
  expireCheckinFrame();
  const esp_err_t result = sendCheckinFrame(req, expectedId);
  xSemaphoreGive(checkinFrameMutex);
  return result;
}

static esp_err_t resumePreview(httpd_req_t *req) {
  char expectedId[61] = {};
  if (httpd_req_get_hdr_value_str(req, "X-Checkin-Id", expectedId, sizeof(expectedId)) != ESP_OK || !expectedId[0])
    return jsonReply(req, "400 Bad Request", "{\"error\":\"checkin_id_required\"}");
  xSemaphoreTake(checkinFrameMutex, portMAX_DELAY);
  expireCheckinFrame();
  const bool busy = checkinFrameId[0] && (strcmp(expectedId, checkinFrameId) || !checkinFrameFinished);
  if (!busy && checkinFrameId[0]) {
    xSemaphoreTake(frameMutex, portMAX_DELAY);
    releasePreviewFrame();
    xSemaphoreGive(frameMutex);
    releaseCheckinFrame();
  }
  xSemaphoreGive(checkinFrameMutex);
  if (busy) return jsonReply(req, "409 Conflict", "{\"error\":\"checkin_snapshot_busy\"}");
  return jsonReply(req, "200 OK", "{\"ready\":true}");
}

static esp_err_t frameHandler(httpd_req_t *req) {
  xSemaphoreTake(checkinFrameMutex, portMAX_DELAY);
  expireCheckinFrame();
  if (checkinFrameId[0] && strcmp(checkinFrameStatus, "challenge")) {
    const esp_err_t result = sendCheckinFrame(req, nullptr);
    xSemaphoreGive(checkinFrameMutex);
    return result;
  }
  portENTER_CRITICAL(&networkMux);
  previewRequested = true; lastFrameRequest = millis();
  portEXIT_CRITICAL(&networkMux);
  xSemaphoreGive(checkinFrameMutex);
  if (xSemaphoreTake(frameMutex, pdMS_TO_TICKS(25)) != pdTRUE)
    return jsonReply(req, "503 Service Unavailable", "{\"error\":\"frame_busy\"}");
  expirePreviewFrame();
  if (!cachedFrame) {
    xSemaphoreGive(frameMutex);
    return jsonReply(req, "503 Service Unavailable", "{\"error\":\"waiting_for_first_frame\"}");
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t result = httpd_resp_send(req, (const char *)cachedFrame, cachedLength);
  xSemaphoreGive(frameMutex);
  return result;
}

static esp_err_t actionHandler(httpd_req_t *req) {
  char header[32] = {}, host[80] = {}, origin[100] = {};
  httpd_req_get_hdr_value_str(req, "X-Camera-Control", header, sizeof(header));
  if (strcmp(header, "local")) return jsonReply(req, "403 Forbidden", "{\"error\":\"invalid_client\"}");
  if (httpd_req_get_hdr_value_len(req, "Origin")) {
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
        String(origin) != String("http://") + host)
      return jsonReply(req, "403 Forbidden", "{\"error\":\"invalid_origin\"}");
  }
  CameraCommand command;
  if (!strcmp(req->uri, "/api/checkin/sync")) return cameraAttendanceSync(req);
  if (!strcmp(req->uri, "/api/checkin/resume-preview")) return resumePreview(req);
  if (!strcmp(req->uri, "/api/enroll")) command = CameraCommand::Enroll;
  else if (!strcmp(req->uri, "/api/checkin")) command = CameraCommand::Checkin;
  else if (!strcmp(req->uri, "/api/test")) command = CameraCommand::Test;
  else if (!strcmp(req->uri, "/api/cancel")) command = CameraCommand::Cancel;
  else if (!strcmp(req->uri, "/api/rotate")) command = CameraCommand::Rotate;
  else if (!strcmp(req->uri, "/api/restart")) command = CameraCommand::Restart;
  else if (!strcmp(req->uri, "/api/clear")) command = CameraCommand::Clear;
  else if (!strcmp(req->uri, "/api/radio/off")) command = CameraCommand::RadioOff;
  else return jsonReply(req, "404 Not Found", "{\"error\":\"unknown_action\"}");
  QueuedCommand queued = {}; queued.command = command;
  if (command == CameraCommand::Enroll || command == CameraCommand::Checkin) {
    const char *tokenHeader = command == CameraCommand::Checkin ? "X-Checkin-Id" : "X-Enrollment-Token";
    size_t length = httpd_req_get_hdr_value_len(req, tokenHeader);
    if (command == CameraCommand::Checkin && (!length || !cameraAttendanceSession().id))
      return jsonReply(req, "409 Conflict", "{\"error\":\"no_active_session\"}");
    if (length >= sizeof(queued.token)) return jsonReply(req, "400 Bad Request", "{\"error\":\"invalid_enrollment_token\"}");
    if (length) {
      if (httpd_req_get_hdr_value_str(req, tokenHeader, queued.token, sizeof(queued.token)) != ESP_OK)
        return jsonReply(req, "400 Bad Request", "{\"error\":\"invalid_enrollment_token\"}");
      for (int i=0; queued.token[i]; ++i)
        if (!isalnum((unsigned char)queued.token[i]) && queued.token[i]!='-' && queued.token[i]!='_')
          return jsonReply(req, "400 Bad Request", "{\"error\":\"invalid_enrollment_token\"}");
    }
  }
  const FaceEngineStatus s = faceEngineStatus();
  if (command == CameraCommand::Clear && httpd_req_get_hdr_value_len(req, "X-Face-Reset-Token")) {
    char device[13], expectedDevice[13];
    faceResetDevice(expectedDevice);
    if (httpd_req_get_hdr_value_str(req, "X-Face-Reset-Device", device, sizeof(device)) != ESP_OK ||
        strcmp(device, expectedDevice) ||
        httpd_req_get_hdr_value_str(req, "X-Face-Reset-Token", queued.token, sizeof(queued.token)) != ESP_OK ||
        strlen(queued.token) != 32 || strspn(queued.token, "0123456789abcdef") != 32)
      return jsonReply(req, "400 Bad Request", "{\"error\":\"invalid_face_reset\"}");
    if (!strcmp(s.face_reset_token, queued.token) && s.storage_ready && !s.face_count)
      return jsonReply(req, "200 OK", "{\"already_cleared\":true}");
  }
  if (command == CameraCommand::Enroll && queued.token[0]) {
    for (int i=0; i<s.face_count; ++i) if (!strcmp(queued.token, s.enrolled_tokens[i]))
      return jsonReply(req, "200 OK", "{\"already_saved\":true}");
    if (s.enrolling && !strcmp(queued.token, s.enrollment_token))
      return jsonReply(req, "200 OK", "{\"already_queued\":true}");
  }
  if ((s.enrolling || !strcmp(s.result, "recognizing")) && command != CameraCommand::Cancel)
    return jsonReply(req, "409 Conflict", "{\"error\":\"busy\"}");
  if ((command == CameraCommand::Checkin || command == CameraCommand::Enroll) && cameraWebCheckinFrameHeld())
    return jsonReply(req, "409 Conflict", "{\"error\":\"checkin_snapshot_busy\"}");
  if (xQueueSend(commands, &queued, 0) != pdTRUE)
    return jsonReply(req, "409 Conflict", "{\"error\":\"command_pending\"}");
  return jsonReply(req, "202 Accepted", "{\"queued\":true}");
}

bool cameraWebBegin(const char *ssid, const char *password) {
  if (!CAM_SETUP_WIFI_ENABLED) { Serial.println("WIFI DISABLED in camera_config.h"); return false; }
#if defined(CAM_WIFI_SSID) && defined(CAM_WIFI_PASSWORD)
  // Preserve separate-file configuration, but explicit .ino values win.
  if (!ssid || !ssid[0]) { ssid = CAM_WIFI_SSID; password = CAM_WIFI_PASSWORD; }
#endif
  stationConfigured = ssid && ssid[0];
  if (!stationConfigured && !CAM_SETUP_AP_ENABLED) {
    Serial.println("WIFI CONFIG MISSING: set CAM_WIFI_SSID and CAM_WIFI_PASSWORD in backend/.env, run start.bat, then upload.");
    return false;
  }
  commands = xQueueCreate(1, sizeof(QueuedCommand));
  frameMutex = xSemaphoreCreateMutex();
  checkinFrameMutex = xSemaphoreCreateMutex();
  if (!commands || !frameMutex || !checkinFrameMutex) { Serial.println("CAMERA WEB ERROR: cannot allocate command queue / frame mutex"); return false; }
  wifi_mode_t mode = CAM_SETUP_AP_ENABLED ? (stationConfigured ? WIFI_AP_STA : WIFI_AP) : WIFI_STA;
  if (!WiFi.mode(mode)) { Serial.println("WIFI ERROR: cannot start radio"); return false; }
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    portENTER_CRITICAL(&networkMux);
    disconnectCount++; disconnectReason=info.wifi_sta_disconnected.reason;
    portEXIT_CRITICAL(&networkMux);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  if (CAM_SETUP_AP_ENABLED) {
    IPAddress ap(192,168,4,1), mask(255,255,255,0);
    if (!WiFi.softAPConfig(ap, ap, mask) || !WiFi.softAP(CAM_AP_SSID, CAM_AP_PASSWORD, 1, false, 2)) {
      Serial.println("WIFI ERROR: cannot start optional setup AP"); return false;
    }
    Serial.printf("OPTIONAL AP: %s | http://192.168.4.1/\n", CAM_AP_SSID);
  }
  if (stationConfigured) {
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid, password ? password : "");
    lastReconnect = millis();
    Serial.printf("WIFI CONNECTING: %s | use 2.4 GHz | waiting for CAMERA URL...\n", ssid);
  }
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 14;
  config.stack_size = 6144;
  config.recv_wait_timeout = 2; config.send_wait_timeout = 2;
  esp_err_t error = httpd_start(&server, &config);
  if (error != ESP_OK) { Serial.printf("CAMERA WEB ERROR: HTTP server start 0x%x\n", error); return false; }
  httpd_uri_t route = {};
  route.uri = "/"; route.method = HTTP_GET; route.handler = rootHandler;
  httpd_register_uri_handler(server, &route);
  route.uri = "/api/status"; route.handler = statusHandler;
  httpd_register_uri_handler(server, &route);
  route.uri = "/frame"; route.handler = frameHandler;
  httpd_register_uri_handler(server, &route);
  route.uri = "/checkin/frame"; route.handler = checkinFrameHandler;
  httpd_register_uri_handler(server, &route);
  const char *actions[] = {"/api/enroll", "/api/test", "/api/cancel", "/api/rotate", "/api/restart", "/api/clear", "/api/radio/off", "/api/checkin", "/api/checkin/sync", "/api/checkin/resume-preview"};
  route.method = HTTP_POST; route.handler = actionHandler;
  for (const char *uri : actions) { route.uri = uri; httpd_register_uri_handler(server, &route); }
  radioEnabled = true;
  Serial.println("CAMERA WEB SERVER: ready | camera sensor and AI are on ESP32-CAM");
  return true;
}

void cameraWebPrintNetwork() {
  if (!radioEnabled) { Serial.println("WIFI OFF / WEB NOT STARTED"); return; }
  if (stationConfigured && WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0) {
    Serial.printf("WIFI CONNECTED: %s\nCAMERA URL: http://%s/\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  } else if (stationConfigured) Serial.println("WIFI NOT CONNECTED: check laptop Mobile Hotspot, SSID, password and 2.4 GHz band");
  if (CAM_SETUP_AP_ENABLED) Serial.printf("OPTIONAL AP: %s | http://%s/\n", CAM_AP_SSID, WiFi.softAPIP().toString().c_str());
}

void cameraWebPollNetwork() {
  if (!radioEnabled || millis() - lastNetworkPoll < 1000UL) return;
  lastNetworkPoll = millis();
  if (xSemaphoreTake(checkinFrameMutex, 0) == pdTRUE) {
    expireCheckinFrame();
    xSemaphoreGive(checkinFrameMutex);
  }
  if (xSemaphoreTake(frameMutex, 0) == pdTRUE) {
    expirePreviewFrame();
    xSemaphoreGive(frameMutex);
  }
  if (!stationConfigured) return;
  wl_status_t status = WiFi.status();
  uint32_t ip = status == WL_CONNECTED ? (uint32_t)WiFi.localIP() : 0;
  if (ip && (ip != lastStationIP || lastStationStatus != WL_CONNECTED)) cameraWebPrintNetwork();
  if (!ip && lastStationIP) Serial.println("WIFI DISCONNECTED: waiting for laptop Mobile HotSpot; UART still works");
  lastStationIP = ip; lastStationStatus = status;
  if (!ip && millis() - lastReconnect >= 20000UL) {
    Serial.printf("WIFI RETRY: status=%d | check hotspot ON, 2.4 GHz, SSID / password\n", (int)status);
    WiFi.reconnect(); lastReconnect = millis();
  }
}

bool cameraWebPopCommand(CameraCommand &command, char *token, size_t tokenSize) {
  QueuedCommand queued = {};
  if (!commands || xQueueReceive(commands, &queued, 0) != pdTRUE) return false;
  command=queued.command;
  if (token && tokenSize) strlcpy(token, queued.token, tokenSize);
  return true;
}

bool cameraWebCheckinFrameHeld() {
  portENTER_CRITICAL(&networkMux);
  const bool held = checkinFrameHeld;
  portEXIT_CRITICAL(&networkMux);
  return held;
}

void cameraWebNoteButton(const char *id, const char *status) {
  portENTER_CRITICAL(&networkMux);
  buttonSeen = true;
  lastButtonSignal = millis();
  if (id && status) {
    strlcpy(buttonRequestId, id, sizeof(buttonRequestId));
    strlcpy(buttonStatus, status, sizeof(buttonStatus));
  }
  portEXIT_CRITICAL(&networkMux);
}

void cameraWebEnableDirectButton() { directButtonReady = true; }

bool cameraWebPreviewWanted() {
  portENTER_CRITICAL(&networkMux);
  bool wanted = (!checkinFrameHeld || checkinChallengeActive) && previewRequested
    && millis() - lastFrameRequest < CAM_PREVIEW_CLIENT_TIMEOUT_MS;
  portEXIT_CRITICAL(&networkMux);
  return radioEnabled && wanted;
}

bool cameraWebRadioEnabled() { return radioEnabled; }

void cameraWebStoreFrame(uint8_t *jpeg, size_t length) {
  if (!frameMutex || xSemaphoreTake(frameMutex, pdMS_TO_TICKS(20)) != pdTRUE) { free(jpeg); return; }
  free(cachedFrame); cachedFrame = jpeg; cachedLength = length;
  cachedFrameAt = millis();
  xSemaphoreGive(frameMutex);
}

void cameraWebBeginCheckinFrame(const char *id) {
  if (!radioEnabled || !checkinFrameMutex) return;
  xSemaphoreTake(checkinFrameMutex, portMAX_DELAY);
  free(checkinFrame);
  checkinFrame = nullptr;
  checkinFrameLength = 0;
  checkinFrameCaptured = checkinFramePublished = checkinFrameFinished = false;
  checkinFrameLight = "waiting";
  checkinFrameMatchScore = NAN;
  strlcpy(checkinFrameId, id, sizeof(checkinFrameId));
  strlcpy(checkinFrameStatus, "challenge", sizeof(checkinFrameStatus));
  portENTER_CRITICAL(&networkMux);
  checkinFrameHeld = true;
  checkinChallengeActive = true;
  portEXIT_CRITICAL(&networkMux);
  xSemaphoreGive(checkinFrameMutex);
}

void cameraWebStoreCheckinFrame(const char *id, const uint8_t *jpeg, size_t length) {
  if (!radioEnabled || !checkinFrameMutex) return;
  uint8_t *copy = length && length <= 256 * 1024 ? static_cast<uint8_t *>(ps_malloc(length)) : nullptr;
  if (copy) memcpy(copy, jpeg, length);
  xSemaphoreTake(checkinFrameMutex, portMAX_DELAY);
  if (!strcmp(id, checkinFrameId)) {
    free(checkinFrame);
    checkinFrame = copy;
    checkinFrameLength = copy ? length : 0;
    checkinFrameCaptured = length != 0;
  } else free(copy);
  xSemaphoreGive(checkinFrameMutex);
}

void cameraWebFinishCheckinFrame(const char *id, const char *status, const char *light, float matchScore) {
  if (!radioEnabled || !checkinFrameMutex) return;
  xSemaphoreTake(checkinFrameMutex, portMAX_DELAY);
  if (!strcmp(id, checkinFrameId)) {
    strlcpy(checkinFrameStatus, status, sizeof(checkinFrameStatus));
    checkinFrameFinishedAt = millis();
    checkinFrameFinished = checkinFramePublished = true;
    checkinFrameLight = light;
    checkinFrameMatchScore = matchScore;
    portENTER_CRITICAL(&networkMux);
    checkinChallengeActive = previewRequested = false;
    portEXIT_CRITICAL(&networkMux);
  }
  xSemaphoreGive(checkinFrameMutex);
}

void cameraWebRadioOff() {
  if (server) { httpd_stop(server); server = nullptr; }
  WiFi.softAPdisconnect(true); WiFi.disconnect(true); WiFi.mode(WIFI_OFF);
  radioEnabled = false;
  if (checkinFrameMutex) {
    xSemaphoreTake(checkinFrameMutex, portMAX_DELAY);
    releaseCheckinFrame();
    xSemaphoreGive(checkinFrameMutex);
  }
  if (frameMutex && xSemaphoreTake(frameMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    releasePreviewFrame();
    xSemaphoreGive(frameMutex);
  }
  Serial.println("WIFI OFF | UART and local inference remain available | reboot to reopen setup");
}
