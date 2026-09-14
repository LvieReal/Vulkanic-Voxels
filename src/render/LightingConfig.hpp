#pragma once

#include <glm/glm.hpp>

namespace vv::render {

struct LightingConfig final {
  // Pass 3.5 look (ported from the user's WGSL reference renderer):
  // bright daylight sky instead of the old near-black values. skyLow is the
  // horizon color, skyHigh the zenith (the shader adds horizon haze and sun
  // glow/core on top - see skyColor in pixels_rgba.comp).
  glm::vec3 lightDir = glm::normalize(glm::vec3(0.5f, 1.0f, 0.5f));
  glm::vec3 lightColor = glm::vec3(1.0f, 0.95f, 0.85f);  // warm sun
  glm::vec3 skyLow = glm::vec3(0.8f, 0.9f, 1.0f);        // horizon
  glm::vec3 skyHigh = glm::vec3(0.45f, 0.62f, 0.95f);    // zenith
};

} // namespace vv::render
