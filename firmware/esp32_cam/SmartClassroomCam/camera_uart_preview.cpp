#include "camera_uart_preview.h"
#include "img_converters.h"
#include "mbedtls/base64.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

namespace {
struct PreviewFrame {
  uint8_t *jpeg;
  size_t length;
};

Stream *output = nullptr;
QueueHandle_t frames = nullptr;
SemaphoreHandle_t slot = nullptr;
bool requested = false; // Only accessed by the Arduino loop.

void sendFrames(void *) {
  for (;;) {
    PreviewFrame frame = {};
    xQueueReceive(frames, &frame, portMAX_DELAY);
    output->printf("FRAME_BEGIN:%u\n", static_cast<unsigned>(frame.length));
    for (size_t offset = 0; offset < frame.length; offset += 96) {
      unsigned char encoded[129];
      size_t written = 0;
      mbedtls_base64_encode(encoded, sizeof(encoded), &written, frame.jpeg + offset,
                            min(size_t(96), frame.length - offset));
      output->printf("FRAME_DATA:%.*s\n", static_cast<int>(written), reinterpret_cast<const char *>(encoded));
      vTaskDelay(1);
    }
    output->printf("FRAME_END\n");
    free(frame.jpeg);
    xSemaphoreGive(slot);
  }
}
}

void cameraUartPreviewBegin(Stream &port) {
  output = &port;
  frames = xQueueCreate(1, sizeof(PreviewFrame));
  slot = xSemaphoreCreateBinary();
  if (frames && slot && xTaskCreate(sendFrames, "uart-preview", 3072, nullptr, 1, nullptr) == pdPASS) {
    xSemaphoreGive(slot);
    return;
  }
  if (frames) vQueueDelete(frames);
  if (slot) vSemaphoreDelete(slot);
  frames = nullptr;
  slot = nullptr;
}

void cameraUartPreviewRequest() {
  if (!slot) output->printf("FRAME_ERROR:preview_unavailable\n");
  else if (xSemaphoreTake(slot, 0) == pdTRUE) requested = true;
}

bool cameraUartPreviewPending() { return requested; }

void cameraUartPreviewFrame(const camera_fb_t *frame) {
  if (!requested) return;
  requested = false;
  PreviewFrame preview = {};
  if (frame && frame->format == PIXFORMAT_JPEG && frame->width == 320 && frame->height == 240) {
    constexpr uint16_t previewWidth = 160;
    constexpr uint16_t previewHeight = 120;
    constexpr size_t rgb565Bytes = previewWidth * previewHeight * 2;
    auto *rgb565 = static_cast<uint8_t *>(ps_malloc(rgb565Bytes));
    if (rgb565 && jpg2rgb565(frame->buf, frame->len, rgb565, JPG_SCALE_2X)) {
      // The decoder writes RGB565 low byte first. The encoder reads high byte first.
      for (size_t i = 0; i < rgb565Bytes; i += 2) {
        const uint8_t lowByte = rgb565[i];
        rgb565[i] = rgb565[i + 1];
        rgb565[i + 1] = lowByte;
      }
      fmt2jpg(rgb565, rgb565Bytes, previewWidth, previewHeight, PIXFORMAT_RGB565, 55,
              &preview.jpeg, &preview.length);
    }
    free(rgb565);
  }
  if (preview.jpeg && preview.length && preview.length <= 24576 && xQueueSend(frames, &preview, 0) == pdTRUE) return;
  free(preview.jpeg);
  output->printf("FRAME_ERROR:capture_failed\n");
  xSemaphoreGive(slot);
}
