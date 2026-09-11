#pragma once

#include <glm/glm.hpp>

namespace vv::core {

class Camera final {
public:
  void setPosition(const glm::vec3 &p) { m_position = p; }
  const glm::vec3 &position() const { return m_position; }

  void setYawPitchDegrees(float yawDeg, float pitchDeg);
  float yawDegrees() const { return m_yawDeg; }
  float pitchDegrees() const { return m_pitchDeg; }

  void setFovDegrees(float fovDeg);
  float fovDegrees() const { return m_fovDeg; }

  void setMoveSpeed(float unitsPerSecond) { m_moveSpeed = unitsPerSecond; }
  float moveSpeed() const { return m_moveSpeed; }

  void setMouseSensitivity(float degreesPerPixel) {
    m_mouseSensitivityDegPerPixel = degreesPerPixel;
  }
  float mouseSensitivity() const { return m_mouseSensitivityDegPerPixel; }

  void addMouseDeltaPixels(float dx, float dy);
  void moveLocal(const glm::vec3 &localDir, float deltaSeconds,
                 float speedMultiplier = 1.0f);

  glm::vec3 forward() const;
  glm::vec3 right() const;
  glm::vec3 up() const;

  float tanHalfFovRadians() const;

private:
  static float clampPitch(float pitchDeg);

  glm::vec3 m_position{0.0f};
  float m_yawDeg = 0.0f;   // +yaw turns right around +Y
  float m_pitchDeg = 0.0f; // +pitch looks up
  float m_fovDeg = 70.0f;
  float m_moveSpeed = 20.0f;
  float m_mouseSensitivityDegPerPixel = 0.12f;
};

} // namespace vv::core
