#pragma once

#include <math.h>

// esp-face returns (eye, mouth corner, nose, opposite eye, opposite mouth corner).
// Project the nose offset onto the eye line to normalize out image roll and size.
inline float faceYaw(const float *points) {
  const float eyeX = points[6] - points[0], eyeY = points[7] - points[1];
  const float eyeDistanceSquared = eyeX * eyeX + eyeY * eyeY;
  if (!isfinite(eyeDistanceSquared) || eyeDistanceSquared < 20.0f * 20.0f) return NAN;
  const float centerX = (points[0] + points[2] + points[6] + points[8]) * 0.25f;
  const float centerY = (points[1] + points[3] + points[7] + points[9]) * 0.25f;
  const float offset = ((points[4] - centerX) * eyeX + (points[5] - centerY) * eyeY) / eyeDistanceSquared;
  if (!isfinite(offset) || fabsf(offset) > 0.55f) return NAN;
  return eyeX >= 0 ? offset : -offset;
}

class FaceLiveness {
public:
  enum class Step { Center, Turn, Return, Passed, WrongIdentity };

  explicit FaceLiveness(bool turnRight) : direction(turnRight ? 1 : -1) {}

  Step step() const { return current; }
  int faceId() const { return identity; }

  bool needsMatch(float yaw) const {
    return isfinite(yaw) && ((current == Step::Center && fabsf(yaw) <= 0.17f)
      || (current == Step::Return && fabsf(yaw - baseline) <= 0.09f));
  }

  void observe(float yaw, int matchedId) {
    switch (current) {
      case Step::Center:
        if (matchedId >= 0 && needsMatch(yaw)) {
          baseline = yaw;
          identity = matchedId;
          current = Step::Turn;
        }
        break;
      case Step::Turn:
        if (isfinite(yaw) && direction * (yaw - baseline) >= 0.18f) {
          if (++turnedFrames == 2) current = Step::Return;
        } else {
          turnedFrames = 0;
        }
        break;
      case Step::Return:
        if (matchedId >= 0 && needsMatch(yaw))
          current = matchedId == identity ? Step::Passed : Step::WrongIdentity;
        break;
      default:
        break;
    }
  }

private:
  const int direction;
  Step current = Step::Center;
  float baseline = 0.0f;
  int identity = -1;
  int turnedFrames = 0;
};
