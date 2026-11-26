#pragma once
#include <Arduino.h>
#include "esp_http_server.h"

struct AttendanceSession { uint32_t id, generation; };
AttendanceSession cameraAttendanceSession();
bool cameraAttendanceButtonFallback();
void cameraAttendanceResult(const char *id, AttendanceSession session, const char *status, int faceId);
esp_err_t cameraAttendanceSync(httpd_req_t *req);
