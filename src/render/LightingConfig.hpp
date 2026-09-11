#pragma once

#include <glm/glm.hpp>

namespace vv::render {

struct LightingConfig final {
  glm::vec3 lightDir = glm::vec3(0.3f, 0.9f, 0.2f);
  glm::vec3 lightColor = glm::vec3(1.0f);
  glm::vec3 skyLow = glm::vec3(0.05f, 0.08f, 0.12f);
  glm::vec3 skyHigh = glm::vec3(0.2f, 0.3f, 0.5f);
};

} // namespace vv::render
