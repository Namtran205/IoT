#pragma once
#include <Arduino.h>

enum class CameraCommand : uint8_t { Enroll, Test, Cancel, Rotate, Restart, Clear, RadioOff, Checkin };
bool cameraWebBegin(const char *ssid, const char *password);
void cameraWebPollNetwork();
void cameraWebPrintNetwork();
bool cameraWebPopCommand(CameraCommand &command, char *token=nullptr, size_t tokenSize=0);
bool cameraWebPreviewWanted();
bool cameraWebCheckinFrameHeld();
bool cameraWebRadioEnabled();
uint32_t cameraWebAddress();
void cameraWebRadioOff();
// Takes ownership of a malloc-allocated JPEG. Camera / inference have a single
// owner (Arduino loop); the HTTP task only serves this independent JPEG copy.
void cameraWebStoreFrame(uint8_t *jpeg, size_t length);
// Retain a copy of the sensor frame used for a check-in until live view resumes.
void cameraWebBeginCheckinFrame(const char *id);
void cameraWebStoreCheckinFrame(const char *id, const uint8_t *jpeg, size_t length);
void cameraWebFinishCheckinFrame(const char *id, const char *status, const char *light, float matchScore);
void cameraWebNoteButton(const char *id=nullptr, const char *status=nullptr);
void cameraWebEnableDirectButton();

