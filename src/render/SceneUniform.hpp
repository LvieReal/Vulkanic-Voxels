#pragma once

#include <vulkan/vulkan.h>

#include <string>

#include "core/Camera.hpp"
#include "render/LightingConfig.hpp"
#include "render/SceneData.hpp"

namespace vv::render {

// Manages the uniform buffer that holds camera + lighting per frame.
// Extracted from VulkanRenderer to separate camera/lighting domain logic.
class SceneUniform final {
 public:
  SceneUniform() = default;
  ~SceneUniform();

  SceneUniform(const SceneUniform&) = delete;
  SceneUniform& operator=(const SceneUniform&) = delete;

  bool create(VkDevice device, VkPhysicalDevice physicalDevice,
              std::string& outError);
  void cleanup(VkDevice device);

  // sceneFlags: x = termination-cause visualization (VV_DEBUG_TERM),
  // y = far-field fade-in alpha (first activation only; recenters do
  // not fade - their cells are identical), z = SDF-shadow experiment.
  // These land in SceneUBO.misc.y/z/w respectively.
  void update(const vv::core::Camera& camera, float timeSeconds,
              const LightingConfig& lighting,
              const glm::vec4& sceneFlags = glm::vec4(0.0f));

  VkBuffer buffer() const { return m_buffer; }
  void* mapped() const { return m_mapped; }

 private:
  VkBuffer m_buffer = VK_NULL_HANDLE;
  VkDeviceMemory m_memory = VK_NULL_HANDLE;
  void* m_mapped = nullptr;
};

} // namespace vv::render
