#include "core/Camera.h"

#include <algorithm>
#include <cmath>

namespace vv::core {

static float wrapDegrees(float a)
{
    while (a > 180.0f) {
        a -= 360.0f;
    }
    while (a < -180.0f) {
        a += 360.0f;
    }
    return a;
}

float Camera::clampPitch(float pitchDeg)
{
    return std::max(-89.9f, std::min(89.9f, pitchDeg));
}

void Camera::setYawPitchDegrees(float yawDeg, float pitchDeg)
{
    m_yawDeg = wrapDegrees(yawDeg);
    m_pitchDeg = clampPitch(pitchDeg);
}

void Camera::setFovDegrees(float fovDeg)
{
    m_fovDeg = std::max(1.0f, std::min(179.0f, fovDeg));
}

void Camera::addMouseDeltaPixels(float dx, float dy)
{
    m_yawDeg = wrapDegrees(m_yawDeg + dx * m_mouseSensitivityDegPerPixel);
    m_pitchDeg = clampPitch(m_pitchDeg - dy * m_mouseSensitivityDegPerPixel);
}

glm::vec3 Camera::forward() const
{
    const float yaw = glm::radians(m_yawDeg);
    const float pitch = glm::radians(m_pitchDeg);

    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);

    // Right-handed, +Y up, -Z forward when yaw=pitch=0.
    glm::vec3 fwd{};
    fwd.x = sy * cp;
    fwd.y = sp;
    fwd.z = -cy * cp;
    return glm::normalize(fwd);
}

glm::vec3 Camera::right() const
{
    const glm::vec3 f = forward();
    const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
    return glm::normalize(glm::cross(f, worldUp));
}

glm::vec3 Camera::up() const
{
    const glm::vec3 r = right();
    const glm::vec3 f = forward();
    return glm::normalize(glm::cross(r, f));
}

void Camera::moveLocal(const glm::vec3& localDir, float deltaSeconds, float speedMultiplier)
{
    const glm::vec3 f = forward();
    const glm::vec3 r = right();
    const glm::vec3 u = up();

    glm::vec3 dirWorld = r * localDir.x + u * localDir.y + f * localDir.z;
    const float len2 = glm::dot(dirWorld, dirWorld);
    if (len2 > 1e-6f) {
        dirWorld *= (1.0f / std::sqrt(len2));
    }

    m_position += dirWorld * (m_moveSpeed * speedMultiplier * deltaSeconds);
}

float Camera::tanHalfFovRadians() const
{
    const float fovRad = glm::radians(m_fovDeg);
    return std::tan(fovRad * 0.5f);
}

} // namespace vv::core

