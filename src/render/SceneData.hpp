#pragma once

#include <glm/glm.hpp>
#include <cstdint>

namespace vv::render {

// Push constants sent to the compute shader per dispatch.
struct PushConstants final {
  glm::uvec4 screen{};    // x=width, y=height, z=bgra, w=frame
  glm::vec4 camera{};     // x=tanHalfFov
  glm::uvec4 chunkSize{}; // xyz=chunk voxel dims
  glm::vec4 voxelSize{};  // xyz=voxel size in world units
};

// Uniform buffer updated each frame with camera and lighting.
struct SceneUBO final {
  glm::vec4 camPos{};
  glm::vec4 camForward{};
  glm::vec4 camRight{};
  glm::vec4 camUp{};
  glm::vec4 lightDir{};
  glm::vec4 lightColor{};
  glm::vec4 skyLow{};
  glm::vec4 skyHigh{};
  glm::vec4 misc{}; // x = timeSeconds
};

} // namespace vv::render
