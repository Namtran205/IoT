#pragma once
#include <Arduino.h>

bool initializeCameraAndFaces();
int enrolledFaceCount();
const char *recognizeOneFace(int &faceId);
using CheckinFeedback = void (*)(const char *id, const char *step, const char *hint, float score);
const char *recognizeLiveFace(int &faceId, const char *checkinId, void (*whileBusy)(), CheckinFeedback feedback);
const char *enrollOneFace(void (*whileBusy)(), const char *token=nullptr);
bool clearAllFaces(const char *resetToken = nullptr);

struct FaceEngineStatus {
  bool camera_ready, storage_ready, enrollment_locked, enrolling;
  int face_count, samples, detected_faces, last_face_id, enrollment_face_id;
  float match_score;
  unsigned long frame_number, frame_at, processing_ms;
  char detection[24], result[32];
  const char *frame_light;
  const char *liveness_step;
  char enrollment_token[40], enrollment_result[32];
  char face_reset_token[33];
  char enrolled_tokens[7][40];
};
FaceEngineStatus faceEngineStatus();
void previewOneFrame();
void cancelEnrollment();
bool rotateCamera();

