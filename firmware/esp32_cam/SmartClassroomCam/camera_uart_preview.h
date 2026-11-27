#pragma once

#include <Arduino.h>
#include "esp_camera.h"

void cameraUartPreviewBegin(Stream &port);
void cameraUartPreviewRequest();
bool cameraUartPreviewPending();
void cameraUartPreviewFrame(const camera_fb_t *frame);
