#include "camera_attendance.h"

namespace {
struct Result {
  char id[61];
  AttendanceSession session;
  char status[20];
  int faceId;
};
constexpr size_t capacity = 16;
Result results[capacity] = {};
size_t head = 0, count = 0;
AttendanceSession current = {};
uint32_t renewedAt = 0;
bool buttonFallback = false;
portMUX_TYPE attendanceMux = portMUX_INITIALIZER_UNLOCKED;

bool readNumber(httpd_req_t *req, const char *header, uint32_t &out) {
  char value[11];
  if (httpd_req_get_hdr_value_str(req, header, value, sizeof(value)) != ESP_OK || !value[0]) return false;
  uint64_t number = 0;
  for (const char *p = value; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    number = number * 10 + (*p - '0');
  }
  if (number > UINT32_MAX) return false;
  out = static_cast<uint32_t>(number);
  return true;
}
}

AttendanceSession cameraAttendanceSession() {
  portENTER_CRITICAL(&attendanceMux);
  AttendanceSession snapshot = millis() - renewedAt < 12000UL ? current : AttendanceSession{};
  portEXIT_CRITICAL(&attendanceMux);
  return snapshot;
}

bool cameraAttendanceButtonFallback() {
  portENTER_CRITICAL(&attendanceMux);
  const bool enabled = buttonFallback && millis() - renewedAt < 12000UL;
  portEXIT_CRITICAL(&attendanceMux);
  return enabled;
}

void cameraAttendanceResult(const char *id, AttendanceSession session, const char *status, int faceId) {
  portENTER_CRITICAL(&attendanceMux);
  if (count < capacity) {
    Result &entry = results[(head + count++) % capacity];
    strlcpy(entry.id, id, sizeof(entry.id));
    entry.session = session;
    strlcpy(entry.status, status, sizeof(entry.status));
    entry.faceId = faceId;
  } else {
    portEXIT_CRITICAL(&attendanceMux);
    Serial.println("ATTENDANCE: camera result queue full");
    return;
  }
  portEXIT_CRITICAL(&attendanceMux);
}

esp_err_t cameraAttendanceSync(httpd_req_t *req) {
  AttendanceSession session = {};
  if (!readNumber(req, "X-Session-Id", session.id) || !readNumber(req, "X-Session-Generation", session.generation))
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid session");
  char ack[61] = {};
  httpd_req_get_hdr_value_str(req, "X-Checkin-Ack", ack, sizeof(ack));
  char fallback[2] = {};
  httpd_req_get_hdr_value_str(req, "X-Button-Fallback", fallback, sizeof(fallback));
  Result entry = {};
  portENTER_CRITICAL(&attendanceMux);
  current = session;
  buttonFallback = fallback[0] == '1';
  renewedAt = millis();
  if (count && !strcmp(ack, results[head].id)) {
    head = (head + 1) % capacity;
    --count;
  }
  if (count) entry = results[head];
  portEXIT_CRITICAL(&attendanceMux);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (!entry.id[0]) return httpd_resp_sendstr(req, "{\"result\":null}");
  char body[260];
  snprintf(body, sizeof(body),
           "{\"result\":{\"event_id\":\"%s\",\"session_id\":%lu,\"session_generation\":%lu,\"status\":\"%s\",\"face_id\":%d}}",
           entry.id, (unsigned long)entry.session.id, (unsigned long)entry.session.generation, entry.status, entry.faceId);
  return httpd_resp_sendstr(req, body);
}
