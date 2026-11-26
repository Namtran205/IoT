// Uses Espressif's legacy esp-face APIs shipped in Arduino-ESP32 2.0.0.
// Inference and enrollment execute here on the ESP32-CAM.
#include <memory>
#include "face_engine.h"
#include "face_liveness.h"
#include "camera_config.h"
#include "camera_web.h"
#include "esp_system.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "fd_forward.h"
#include "fr_forward.h"
#include <Preferences.h>
#include <math.h>
#include "fb_gfx.h"

static const int MAX_FACES = 7, ENROLL_SAMPLES = 5;
static face_id_list identities = {};
static mtmn_config_t detector = {};
static Preferences preferences;
static bool storageReady = false, facesReady = false;
static bool enrollmentCancelled = false;
static FaceEngineStatus state = {}, published = {};
static portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;

static void publishStatus() {
  state.face_count = identities.count;
  state.storage_ready = facesReady;
  portENTER_CRITICAL(&statusMux); published = state; portEXIT_CRITICAL(&statusMux);
}

static void resultStatus(const char *result, int faceId=-1) {
  strlcpy(state.result, result, sizeof(state.result)); state.last_face_id = faceId; publishStatus();
}

FaceEngineStatus faceEngineStatus() {
  portENTER_CRITICAL(&statusMux); FaceEngineStatus copy = published; portEXIT_CRITICAL(&statusMux);
  return copy;
}

struct LegacyStoredFace {
  uint32_t version;
  int32_t n, w, h, c;
  float vector[FACE_ID_SIZE];
  uint32_t checksum;
};

struct StoredFace {
  uint32_t version;
  int32_t n, w, h, c;
  float vector[FACE_ID_SIZE];
  char enrollment_token[40];
  uint32_t checksum;
};

template <typename T> static uint32_t checksum(const T &face) {
  uint32_t hash = 2166136261U;
  const uint8_t *bytes = (const uint8_t *)&face;
  for (size_t i = 0; i < offsetof(T, checksum); ++i) hash = (hash ^ bytes[i]) * 16777619U;
  return hash;
}

using FaceVector = std::unique_ptr<dl_matrix3d_t, decltype(&dl_matrix3d_free)>;

static float dotProduct(const float *a, const float *b) {
  float dot = 0;
  for (int i = 0; i < FACE_ID_SIZE; ++i) dot += a[i] * b[i];
  return dot;
}

static bool normalizeEmbedding(float *vector) {
  const float normSquared = dotProduct(vector, vector);
  if (!isfinite(normSquared) || normSquared <= 1e-12f) return false;
  const float scale = 1.0f / sqrtf(normSquared);
  for (int i = 0; i < FACE_ID_SIZE; ++i) vector[i] *= scale;
  return true;
}

static FaceVector extractEmbedding(dl_matrix3du_t *aligned) {
  FaceVector face(get_face_id(aligned), dl_matrix3d_free);
  if (face && !normalizeEmbedding(face->item)) face.reset();
  return face;
}

struct FaceMatch {
  int id = -1;
  float score = NAN;

  bool accepted(float threshold = FACE_REC_THRESHOLD) const { return id >= 0 && score > threshold; }
};

static FaceMatch findClosestFace(const dl_matrix3d_t &face) {
  FaceMatch best;
  for (int id = 0; id < identities.count; ++id) {
    const float score = dotProduct(face.item, identities.id_list[id]->item);
    if (best.id < 0 || score > best.score) {
      best.id = id;
      best.score = score;
    }
  }
  return best;
}

static bool saveFace(int slot, const dl_matrix3d_t &face) {
  StoredFace stored = {};
  stored.version = 2;
  stored.n = face.n; stored.w = face.w; stored.h = face.h; stored.c = face.c;
  memcpy(stored.vector, face.item, sizeof(stored.vector));
  strlcpy(stored.enrollment_token, state.enrollment_token, sizeof(stored.enrollment_token));
  stored.checksum = checksum(stored);
  char key[12]; snprintf(key, sizeof(key), "face%d", slot);
  if (preferences.putBytes(key, &stored, sizeof(stored)) != sizeof(stored)) return false;
  if (preferences.putUInt("count", slot + 1) != sizeof(uint32_t)) return false;
  strlcpy(state.enrolled_tokens[slot], stored.enrollment_token, sizeof(state.enrolled_tokens[slot]));
  return true;
}

static bool loadFaces() {
  unsigned int count = preferences.getUInt("count", 0);
  if (count > MAX_FACES) return false;
  for (unsigned int slot = 0; slot < count; ++slot) {
    StoredFace stored = {};
    char key[12]; snprintf(key, sizeof(key), "face%u", slot);
    size_t length = preferences.getBytesLength(key);
    if (length == sizeof(LegacyStoredFace)) {
      LegacyStoredFace old = {};
      if (preferences.getBytes(key, &old, sizeof(old)) != sizeof(old) || old.version != 1 || old.checksum != checksum(old)) return false;
      stored.n=old.n; stored.w=old.w; stored.h=old.h; stored.c=old.c;
      memcpy(stored.vector, old.vector, sizeof(stored.vector));
    } else if (length == sizeof(stored)) {
      if (preferences.getBytes(key, &stored, sizeof(stored)) != sizeof(stored) ||
          stored.version != 2 || stored.checksum != checksum(stored) || stored.enrollment_token[39] != '\0') return false;
      for (int i=0; stored.enrollment_token[i]; ++i)
        if (!isalnum((unsigned char)stored.enrollment_token[i]) && stored.enrollment_token[i]!='-' && stored.enrollment_token[i]!='_') return false;
    } else return false;
    if (stored.n != 1 || stored.w < 1 || stored.w > FACE_ID_SIZE ||
        stored.h < 1 || stored.h > FACE_ID_SIZE || stored.c < 1 || stored.c > FACE_ID_SIZE ||
        (int64_t)stored.n * stored.w * stored.h * stored.c != FACE_ID_SIZE) return false;
    // The legacy SDK stored an arithmetic mean but matched as if it were a unit vector.
    if (!normalizeEmbedding(stored.vector)) return false;
    dl_matrix3d_t *face = dl_matrix3d_alloc(stored.n, stored.w, stored.h, stored.c);
    if (!face) return false;
    memcpy(face->item, stored.vector, sizeof(stored.vector));
    if (identities.id_list[slot]) dl_matrix3d_free(identities.id_list[slot]);
    identities.id_list[slot] = face;
    identities.count = slot + 1;
    identities.tail = (slot + 1) % MAX_FACES;
    strlcpy(state.enrolled_tokens[slot], stored.enrollment_token, sizeof(state.enrolled_tokens[slot]));
  }
  return true;
}

bool initializeCameraAndFaces() {
  state.frame_light = "waiting";
  state.match_score = NAN;
  state.last_face_id = state.enrollment_face_id = -1;
  strlcpy(state.detection, "waiting", sizeof(state.detection)); resultStatus("initializing");
  if (!psramFound()) { Serial.println("ERROR: PSRAM not found"); resultStatus("psram_error"); return false; }
  camera_config_t camera = {};
  camera.ledc_channel = LEDC_CHANNEL_0; camera.ledc_timer = LEDC_TIMER_0;
  camera.pin_d0 = 5; camera.pin_d1 = 18; camera.pin_d2 = 19; camera.pin_d3 = 21;
  camera.pin_d4 = 36; camera.pin_d5 = 39; camera.pin_d6 = 34; camera.pin_d7 = 35;
  camera.pin_xclk = 0; camera.pin_pclk = 22; camera.pin_vsync = 25; camera.pin_href = 23;
  camera.pin_sscb_sda = 26; camera.pin_sscb_scl = 27; camera.pin_pwdn = 32; camera.pin_reset = -1;
  camera.xclk_freq_hz = 20000000; camera.pixel_format = PIXFORMAT_JPEG;
  camera.frame_size = FRAMESIZE_QVGA; camera.jpeg_quality = 12; camera.fb_count = 1;
  camera.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  esp_err_t error = esp_camera_init(&camera);
  if (error != ESP_OK) { Serial.printf("ERROR: camera init 0x%x\n", error); resultStatus("camera_error"); return false; }
  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor) {
    sensor->set_exposure_ctrl(sensor, 1);
    sensor->set_gain_ctrl(sensor, 1);
  }
  Preferences settings;
  if (settings.begin("classroom-cam", true)) {
    bool rotated = settings.getBool("rotated", false);
    settings.end();
    if (sensor && rotated) { sensor->set_vflip(sensor, 1); sensor->set_hmirror(sensor, 1); }
  }
  detector.type = FAST; detector.min_face = 54; detector.pyramid = 0.707; detector.pyramid_times = 4;
  detector.p_threshold.score = 0.6; detector.p_threshold.nms = 0.7; detector.p_threshold.candidate_number = 20;
  detector.r_threshold.score = 0.7; detector.r_threshold.nms = 0.7; detector.r_threshold.candidate_number = 10;
  // Report a second credible face so attendance can reject group shots.
  detector.o_threshold.score = 0.7; detector.o_threshold.nms = 0.7; detector.o_threshold.candidate_number = 2;
  face_id_init(&identities, MAX_FACES, ENROLL_SAMPLES);
  if (!identities.id_list) { resultStatus("memory_error"); return false; }
  storageReady = preferences.begin("classroom-face", false);
  facesReady = storageReady && loadFaces();
  if (storageReady) preferences.getString("reset-token", state.face_reset_token, sizeof(state.face_reset_token));
  Serial.printf("CAMERA READY | saved faces=%d | storage=%s\n", identities.count, facesReady ? "OK" : "ERROR");
  if (!facesReady) Serial.println("Face storage invalid. Use CLEAR_FACES before enrollment.");
  state.camera_ready = true;
  resultStatus(facesReady ? "idle" : "storage_error");
  return true;
}

int enrolledFaceCount() { return identities.count; }

struct Capture {
  dl_matrix3du_t *rgb = nullptr;
  dl_matrix3du_t *aligned = nullptr;
  camera_fb_t *snapshot = nullptr;
  const char *status = "error";
  const char *light = "waiting";
  const char *framing = nullptr;
  float yaw = NAN;
  unsigned long started = millis();
};

static const char *faceFraming(const box_t &box, int width, int height) {
  const float faceWidth = box.box_p[2] - box.box_p[0] + 1;
  if (faceWidth < width * 0.22f) return "closer";
  if (faceWidth > width * 0.70f) return "farther";
  const float centerX = (box.box_p[0] + box.box_p[2]) * 0.5f / width;
  const float centerY = (box.box_p[1] + box.box_p[3]) * 0.5f / height;
  if (centerX < 0.25f || centerX > 0.75f || centerY < 0.24f || centerY > 0.76f) return "center_face";
  return nullptr;
}

static const char *frameLighting(const dl_matrix3du_t *rgb, const box_array_t *boxes) {
  int left = rgb->w / 4, right = rgb->w * 3 / 4;
  int top = rgb->h / 4, bottom = rgb->h * 3 / 4;
  if (boxes && boxes->len == 1) {
    const auto &box = boxes->box[0];
    left = constrain(static_cast<int>(box.box_p[0]), 0, rgb->w - 1);
    top = constrain(static_cast<int>(box.box_p[1]), 0, rgb->h - 1);
    right = constrain(static_cast<int>(box.box_p[2]) + 1, left + 1, rgb->w);
    bottom = constrain(static_cast<int>(box.box_p[3]) + 1, top + 1, rgb->h);
  }
  unsigned int total = 0, count = 0, dark = 0, bright = 0;
  for (int y = top; y < bottom; y += 4) {
    for (int x = left; x < right; x += 4) {
      const uint8_t *p = rgb->item + (y * rgb->w + x) * 3;
      const unsigned int light = (29U * p[0] + 150U * p[1] + 77U * p[2]) >> 8;
      total += light;
      ++count;
      dark += light < 55;
      bright += light > 225;
    }
  }
  if (total < 55 * count || dark * 3 > count * 2) return "dark";
  if (total > 215 * count || bright * 3 > count * 2) return "bright";
  if (dark * 4 > count && bright * 4 > count) return "uneven";
  return "ok";
}

static void freeFaceBoxes(box_array_t *boxes) {
  if (!boxes) return;
  dl_lib_free(boxes->score);
  dl_lib_free(boxes->box);
  if (boxes->landmark) dl_lib_free(boxes->landmark);
  dl_lib_free(boxes);
}

static Capture captureAligned(int retry=0, bool keepSnapshot=false, bool discardStaleFrame=true) {
  Capture capture;
  state.detected_faces = 0;
  state.frame_light = "waiting";
  if (discardStaleFrame) {
    camera_fb_t *old = esp_camera_fb_get();
    if (old) esp_camera_fb_return(old);
  }
  camera_fb_t *frame = esp_camera_fb_get();
  if (!frame) {
    capture.status = "capture_error";
    strlcpy(state.detection,capture.status,sizeof(state.detection)); publishStatus(); return capture;
  }
  if (keepSnapshot && frame->format == PIXFORMAT_JPEG) capture.snapshot = frame;
  capture.rgb = dl_matrix3du_alloc(1, frame->width, frame->height, 3);
  if (!capture.rgb) {
    if (!capture.snapshot) esp_camera_fb_return(frame);
    capture.status = "memory_error";
    strlcpy(state.detection,capture.status,sizeof(state.detection)); publishStatus(); return capture;
  }
  bool converted = fmt2rgb888(frame->buf, frame->len, frame->format, capture.rgb->item);
  if (!capture.snapshot) esp_camera_fb_return(frame);
  if (!converted) {
    dl_matrix3du_free(capture.rgb); capture.rgb=nullptr;
    capture.status = "decode_error";
    strlcpy(state.detection,capture.status,sizeof(state.detection)); publishStatus(); return capture;
  }
  mtmn_config_t captureDetector = detector;
  if (retry) captureDetector.min_face = 48;
  if (retry >= 2) {
    captureDetector.type = NORMAL;
    captureDetector.min_face = 42;
  }
  box_array_t *boxes = face_detect(capture.rgb, &captureDetector);
  capture.light = frameLighting(capture.rgb, boxes);
  state.frame_light = capture.light;
  capture.status = "no_face";
  state.detected_faces = boxes ? boxes->len : 0;
  if (boxes) {
    if (boxes->len > 1) capture.status = "multiple_faces";
    else if (boxes->len == 0) capture.status = "no_face";
    else {
      capture.framing = faceFraming(boxes->box[0], capture.rgb->w, capture.rgb->h);
      if (boxes->landmark) capture.yaw = faceYaw(boxes->landmark[0].landmark_p);
      capture.aligned = dl_matrix3du_alloc(1, FACE_WIDTH, FACE_HEIGHT, 3);
      if (!capture.aligned) capture.status = "error";
      else if (align_face(boxes, capture.rgb, capture.aligned) != ESP_OK) {
        dl_matrix3du_free(capture.aligned); capture.aligned = nullptr; capture.status = "not_aligned";
      } else capture.status = "ok";
    }
    // Align BEFORE drawing; overlays must never become input to recognition.
    if (cameraWebPreviewWanted()) {
      fb_data_t image = {};
      image.width=capture.rgb->w; image.height=capture.rgb->h; image.data=capture.rgb->item;
      image.bytes_per_pixel=3; image.format=FB_BGR888;
      uint32_t color = !strcmp(capture.status,"ok") ? 0x0000FF00 : 0x0000FFFF;
      for (int i=0; i<boxes->len; ++i) {
        int x=constrain((int)boxes->box[i].box_p[0],0,capture.rgb->w-1);
        int y=constrain((int)boxes->box[i].box_p[1],0,capture.rgb->h-1);
        int right=constrain((int)boxes->box[i].box_p[2],x,capture.rgb->w-1);
        int bottom=constrain((int)boxes->box[i].box_p[3],y,capture.rgb->h-1);
        fb_gfx_drawFastHLine(&image,x,y,right-x+1,color); fb_gfx_drawFastHLine(&image,x,bottom,right-x+1,color);
        fb_gfx_drawFastVLine(&image,x,y,bottom-y+1,color); fb_gfx_drawFastVLine(&image,right,y,bottom-y+1,color);
      }
    }
    freeFaceBoxes(boxes);
  }
  strlcpy(state.detection,capture.status,sizeof(state.detection)); publishStatus();
  return capture;
}

static void finishCapture(Capture &capture, const char *label=nullptr) {
  if (capture.snapshot) { esp_camera_fb_return(capture.snapshot); capture.snapshot=nullptr; }
  if (capture.rgb) {
    if (cameraWebPreviewWanted()) {
      if (label) {
        fb_data_t image = {}; image.width=capture.rgb->w; image.height=capture.rgb->h;
        image.data=capture.rgb->item; image.bytes_per_pixel=3; image.format=FB_BGR888;
        fb_gfx_print(&image,5,5,0x00FFFFFF,label);
      }
      uint8_t *jpeg=nullptr; size_t length=0;
      if (fmt2jpg(capture.rgb->item,capture.rgb->w*capture.rgb->h*3,capture.rgb->w,capture.rgb->h,PIXFORMAT_RGB888,65,&jpeg,&length))
        cameraWebStoreFrame(jpeg,length);
      else free(jpeg);
    }
    dl_matrix3du_free(capture.rgb); capture.rgb=nullptr;
    state.frame_number++; state.frame_at=millis();
  }
  if (capture.aligned) { dl_matrix3du_free(capture.aligned); capture.aligned=nullptr; }
  state.processing_ms=millis()-capture.started; publishStatus();
}

void previewOneFrame() {
  if (!state.camera_ready) return;
  Capture capture=captureAligned(); finishCapture(capture,capture.status);
}

bool rotateCamera() {
  if (!state.camera_ready || state.enrolling) return false;
  sensor_t *sensor=esp_camera_sensor_get(); if (!sensor) return false;
  int value=!sensor->status.vflip;
  bool ok=sensor->set_vflip(sensor,value)==0 && sensor->set_hmirror(sensor,value)==0;
  if (ok) {
    Preferences settings;
    ok = settings.begin("classroom-cam", false);
    if (ok) { ok = settings.putBool("rotated", value) == sizeof(bool); settings.end(); }
  }
  resultStatus(ok ? "rotated" : "error"); return ok;
}

static int recognitionPriority(const char *status) {
  if (!strcmp(status, "recognized") || !strcmp(status, "multiple_faces")) return 3;
  if (!strcmp(status, "unknown") || !strcmp(status, "not_enrolled")) return 2;
  if (!strcmp(status, "not_aligned")) return 1;
  if (!strcmp(status, "no_face")) return 0;
  return -1;
}

static FaceMatch matchCapture(const Capture &capture) {
  const FaceVector face = extractEmbedding(capture.aligned);
  return face ? findClosestFace(*face) : FaceMatch{};
}

const char *recognizeOneFace(int &faceId) {
  faceId = -1;
  state.match_score = NAN;
  const char *unavailable = !facesReady ? "error" : !identities.count ? "not_enrolled" : nullptr;
  if (unavailable) { resultStatus(unavailable); return unavailable; }
  const unsigned long started = millis();
  resultStatus("recognizing");
  const char *status = "error";
  const char *detection = "waiting";
  const char *light = "waiting";
  int detectedFaces = 0;
  int bestPriority = -2;
  int attempts = 0;
  while (attempts < 3) {
    Capture capture = captureAligned(attempts++);
    const char *attemptStatus = capture.status;
    FaceMatch match;
    if (!strcmp(attemptStatus, "ok")) {
      match = matchCapture(capture);
      attemptStatus = match.accepted() ? "recognized" : match.id >= 0 ? "unknown" : "embedding_error";
      if (match.accepted()) faceId = match.id;
    }
    const int priority = recognitionPriority(attemptStatus);
    if (priority > bestPriority || (priority == bestPriority && match.score > state.match_score)) {
      bestPriority = priority;
      status = priority < 0 ? "error" : attemptStatus;
      detection = capture.status;
      light = capture.light;
      detectedFaces = state.detected_faces;
      state.match_score = match.score;
    }
    char label[48];
    snprintf(label, sizeof(label), "%s ID=%d", attemptStatus, faceId);
    finishCapture(capture, label);
    // Retry only recoverable frames. Reject when more than one face is seen.
    if (priority < 0 || priority > 2) break;
    yield();
  }
  strlcpy(state.detection, detection, sizeof(state.detection));
  state.frame_light = light;
  state.detected_faces = detectedFaces;
  state.processing_ms = millis() - started;
  Serial.printf("CHECKIN DETECTION | frames=%d | total_ms=%lu | %s | similarity=%.3f | threshold=%.2f\n",
                attempts, state.processing_ms, status, state.match_score, FACE_REC_THRESHOLD);
  resultStatus(status,faceId); return status;
}

static const char *checkinHint(const Capture &capture, const FaceLiveness &challenge, bool lowMatch) {
  if (strcmp(capture.light, "ok")) return capture.light;
  if (capture.framing) return capture.framing;
  if (!strcmp(capture.status, "no_face")) return "no_face";
  if (challenge.turnedOpposite(capture.yaw)) return "opposite";
  if (lowMatch) return "low_match";
  if (challenge.step() != FaceLiveness::Step::Turn && !challenge.needsMatch(capture.yaw)) return "straight";
  if (!isfinite(capture.yaw) || (challenge.step() != FaceLiveness::Step::Turn && !capture.aligned)) return "straight";
  return "hold";
}

const char *recognizeLiveFace(int &faceId, const char *checkinId, void (*whileBusy)(), CheckinFeedback feedback) {
  faceId = -1;
  state.match_score = NAN;
  if (!facesReady || !identities.count) {
    const char *status = facesReady ? "not_enrolled" : "error";
    resultStatus(status);
    return status;
  }

  using Step = FaceLiveness::Step;
  const bool turnRight = (esp_random() & 1U) != 0;
  FaceLiveness challenge(turnRight);
  state.liveness_step = "center";
  resultStatus("recognizing");
  Serial.println("LIVENESS 1/3 | Look straight at the camera");
  const unsigned long started = millis();
  unsigned long lastFeedback = started;
  if (feedback) feedback(checkinId, state.liveness_step, "start", NAN);
  unsigned long lastSeen = started;
  const char *status = "no_face";
  int attempts = 0, retry = 0;
  while (millis() - started < CAM_LIVENESS_TIMEOUT_MS) {
    whileBusy();
    Capture capture = captureAligned(retry, true, attempts == 0);
    ++attempts;
    const Step before = challenge.step();
    const bool multipleFaces = state.detected_faces > 1;
    const bool frameError = !capture.rgb || !strcmp(capture.status, "error");
    const bool faceFound = state.detected_faces == 1 && isfinite(capture.yaw);
    if (faceFound) lastSeen = millis();
    const bool needsMatch = capture.aligned && challenge.needsMatch(capture.yaw);
    FaceMatch match;
    if (needsMatch) match = matchCapture(capture);
    if (isfinite(match.score)) state.match_score = match.score;
    const bool faceLost = before != Step::Center && millis() - lastSeen > CAM_LIVENESS_FACE_LOST_MS;
    const bool expired = millis() - started >= CAM_LIVENESS_TIMEOUT_MS;
    if (multipleFaces || frameError) {
      status = multipleFaces ? "multiple_faces" : "error";
    } else if (before != Step::Center) {
      status = "liveness_failed";
    } else if (strcmp(capture.status, "no_face")) {
      status = needsMatch && match.id >= 0 ? "unknown" : "not_aligned";
    }

    const bool fatal = multipleFaces || frameError || faceLost || expired;
    const bool matched = match.accepted(CAM_CHECKIN_MATCH_THRESHOLD);
    if (!fatal) challenge.observe(capture.yaw, matched ? match.id : -1);
    const Step after = challenge.step();
    if (after == Step::Turn && before != after) {
      state.liveness_step = turnRight ? "turn_right" : "turn_left";
      status = "liveness_failed";
      Serial.printf("LIVENESS 2/3 | Turn towards %s of the image\n", turnRight ? "RIGHT" : "LEFT");
    } else if (after == Step::Return && before != after) {
      state.liveness_step = "return";
      Serial.println("LIVENESS 3/3 | Look straight again");
    } else if (after == Step::Passed) {
      faceId = challenge.faceId();
      status = "recognized";
      if (capture.snapshot)
        cameraWebStoreCheckinFrame(checkinId, capture.snapshot->buf, capture.snapshot->len);
    }
    if (feedback && !fatal && after != Step::Passed && after != Step::WrongIdentity
        && (after != before || millis() - lastFeedback >= 1400UL)) {
      lastFeedback = millis();
      feedback(checkinId, state.liveness_step,
               after != before ? "start" : checkinHint(capture, challenge, needsMatch && !matched), match.score);
    }
    const char *label = after == Step::Turn ? (turnRight ? "TURN RIGHT" : "TURN LEFT")
      : after == Step::Return ? "LOOK STRAIGHT AGAIN" : "LOOK STRAIGHT";
    finishCapture(capture, label);
    if (fatal || after == Step::Passed || after == Step::WrongIdentity) break;
    if (!faceFound) retry = (retry + 1) % 3;
    delay(35);
  }
  state.liveness_step = nullptr;
  state.processing_ms = millis() - started;
  Serial.printf("CHECKIN DETECTION | frames=%d | total_ms=%lu | %s | similarity=%.3f | threshold=%.2f\n",
                attempts, state.processing_ms, status, state.match_score, CAM_CHECKIN_MATCH_THRESHOLD);
  resultStatus(status, faceId);
  return status;
}

static void enrollmentResult(const char *result, int faceId=-1) {
  strlcpy(state.enrollment_result, result, sizeof(state.enrollment_result));
  state.enrollment_face_id = faceId;
  resultStatus(result, faceId);
}

static const char *duplicateEnrollment(int id) {
  state.enrolling = false;
  enrollmentResult("enrollment_duplicate", id);
  Serial.printf("ENROLL DUPLICATE | existing Face ID=%d\n", id);
  return "ERROR: face already registered";
}

static const char *commitEnrollment(FaceVector &sum, int slot) {
  state.enrolling = false;
  // L2-normalizing the sum is equivalent to normalizing the arithmetic mean.
  if (!normalizeEmbedding(sum->item)) {
    enrollmentResult("enrollment_error");
    return "ERROR: invalid enrollment embedding";
  }
  const FaceMatch match = findClosestFace(*sum);
  if (match.accepted()) return duplicateEnrollment(match.id);
  if (!saveFace(slot, *sum)) {
    facesReady = false;
    enrollmentResult("storage_error");
    return "ERROR: cannot save NVS; reboot camera and check storage";
  }
  if (identities.id_list[slot]) dl_matrix3d_free(identities.id_list[slot]);
  identities.id_list[slot] = sum.release();
  identities.count = slot + 1;
  identities.tail = (slot + 1) % MAX_FACES;
  Serial.printf("ENROLL SAVED | Face ID=%d | survives reboot\n", slot);
  enrollmentResult("enrolled", slot);
  return "OK";
}

const char *enrollOneFace(void (*whileBusy)(), const char *token) {
  if (token && token[0]) {
    for (int i=0; i<identities.count; ++i) if (!strcmp(token, state.enrolled_tokens[i])) return "Already saved";
    strlcpy(state.enrollment_token, token, sizeof(state.enrollment_token));
  } else snprintf(state.enrollment_token, sizeof(state.enrollment_token), "local-%08x%08x", esp_random(), esp_random());
  if (!facesReady) { enrollmentResult("storage_error"); return "ERROR: face storage is unavailable"; }
  if (identities.count >= MAX_FACES) { enrollmentResult("full"); return "ERROR: maximum 7 faces; clear all before replacing IDs"; }
  FaceVector sum(nullptr, dl_matrix3d_free);
  const int slot = identities.tail;
  uint8_t duplicateVotes[MAX_FACES] = {};
  state.enrolling=true; state.samples=0; enrollmentCancelled=false; enrollmentResult("enrolling");
  constexpr unsigned long maxDurationMs = 120000;
  constexpr unsigned long maxTimeWithoutSampleMs = 30000;
  const unsigned long started = millis();
  unsigned long lastSampleAt = started;
  unsigned long lastLog=0;
  int detectionRetry = 0;
  Serial.printf("ENROLL START | Face ID=%d | keep one face centered\n", slot);
  while (millis() - started < maxDurationMs && millis() - lastSampleAt < maxTimeWithoutSampleMs) {
    whileBusy();
    if (enrollmentCancelled) {
      state.enrolling=false; enrollmentResult("enrollment_cancelled");
      return "Cancelled";
    }
    Capture capture=captureAligned(detectionRetry);
    if (strcmp(capture.status,"ok")) {
      if (!strcmp(capture.status, "no_face") || !strcmp(capture.status, "not_aligned"))
        detectionRetry = min(detectionRetry + 1, 2);
      else
        detectionRetry = 0;
      if (!lastLog || millis()-lastLog>1500) { Serial.printf("ENROLL WAIT: %s\n",capture.status); lastLog=millis(); }
      finishCapture(capture,capture.status); delay(120); continue;
    }
    detectionRetry = 0;
    FaceVector face = extractEmbedding(capture.aligned);
    if (!face) {
      finishCapture(capture, "EMBEDDING ERROR");
      state.enrolling = false;
      enrollmentResult("enrollment_error");
      return "ERROR: cannot extract face embedding";
    }
    const FaceMatch match = findClosestFace(*face);
    if (match.accepted()) {
      finishCapture(capture, "CHECKING EXISTING FACE");
      if (++duplicateVotes[match.id] >= 2) return duplicateEnrollment(match.id);
      delay(150);
      continue;
    }
    if (sum) {
      const float similarity = dotProduct(face->item, sum->item) / sqrtf(dotProduct(sum->item, sum->item));
      if (similarity <= FACE_REC_THRESHOLD) {
        strlcpy(state.detection, "inconsistent_face", sizeof(state.detection));
        if (!lastLog || millis() - lastLog > 1500) {
          Serial.printf("ENROLL WAIT: inconsistent_face | similarity=%.3f\n", similarity);
          lastLog = millis();
        }
        finishCapture(capture, "KEEP ONE FACE CENTERED");
        delay(150);
        continue;
      }
      for (int i = 0; i < FACE_ID_SIZE; ++i) sum->item[i] += face->item[i];
    } else {
      sum = std::move(face);
    }
    ++state.samples;
    lastSampleAt = millis();
    char label[48]; snprintf(label,sizeof(label),"ENROLL ID=%d SAMPLE=%d/5",slot,state.samples);
    finishCapture(capture,label);
    Serial.printf("ENROLL Face ID=%d Sample=%d/5\n", slot, state.samples);
    if (state.samples == ENROLL_SAMPLES) return commitEnrollment(sum, slot);
    delay(150);
  }
  state.enrolling=false; enrollmentResult("enrollment_timeout");
  return "ERROR: enrollment timeout; adjust face position or lighting and retry";
}

void cancelEnrollment() { enrollmentCancelled=true; }

bool clearAllFaces(const char *resetToken) {
  if (!storageReady || !preferences.clear() ||
      (resetToken && resetToken[0] && preferences.putString("reset-token", resetToken) != strlen(resetToken))) {
    facesReady = false;
    resultStatus("storage_error");
    return false;
  }
  // The caller restarts to reload the empty list from NVS.
  return true;
}

