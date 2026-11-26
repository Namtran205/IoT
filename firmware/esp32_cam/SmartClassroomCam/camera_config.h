#pragma once

// Mac dinh: ket noi Wi-Fi Mobile Hotspot tu backend/.env qua camera_secrets.h.
#define CAM_SETUP_WIFI_ENABLED true
// TUY CHON nang cao: true de camera phat them mang rieng. Mac dinh TAT.
#define CAM_SETUP_AP_ENABLED false
#define CAM_AP_SSID "SmartClassroom-CAM"
#define CAM_AP_PASSWORD "Classroom123"
#define CAM_PREVIEW_INTERVAL_MS 500UL
#define CAM_PREVIEW_CLIENT_TIMEOUT_MS 6000UL
#define CAM_PREVIEW_FRAME_RETENTION_MS 6000UL
#define CAM_CHECKIN_FRAME_RETENTION_MS 15000UL
constexpr unsigned long CAM_LIVENESS_TIMEOUT_MS = 18000UL;

// Set false for an existing installation with CAM IO13 wired to the DevKit UART RX.
// With the direct button enabled, switch IO13 to GND and use IO15 for UART TX.
constexpr bool CAM_DIRECT_BUTTON_ENABLED = true;
constexpr int CAM_DIRECT_BUTTON_GPIO = 13;
constexpr int CAM_UART_RX_GPIO = 14;
constexpr int CAM_UART_TX_GPIO = CAM_DIRECT_BUTTON_ENABLED ? 15 : 13;

