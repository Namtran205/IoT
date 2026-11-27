#pragma once
#include <Arduino.h>
#define DEVICE_ID "smartgate01"
#define CAMERA_ID "CAM01"
#define MQTT_BASE_TOPIC "demo/smartgate01"
static const int PIN_BUTTON=13, PIN_UART_RX=25, PIN_UART_TX=33;
static const int PIN_MQ2=34, PIN_SDA=26, PIN_SCL=27, PIN_SERVO=32, PIN_LED=18;
static const int SERVO_OPEN=40, SERVO_CLOSED=120;
// Raw ADC demo thresholds above the initial bench readings (74–159 ADC).
static const int GAS_ALARM_ADC=250, GAS_CLEAR_ADC=180;
static const unsigned long GAS_LED_TOGGLE_MS=350UL;
static const unsigned long GAS_LED_MIN_BLINK_MS=15000UL;
static const unsigned long CAMERA_UART_PING_MS=2000UL;
static const unsigned long CAMERA_UART_TIMEOUT_MS=8000UL;
static const unsigned long CHECKIN_TIMEOUT_MS=90000UL;

