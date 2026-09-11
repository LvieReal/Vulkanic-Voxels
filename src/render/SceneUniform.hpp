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

  // debugFlags: x = termination-cause visualization (VV_DEBUG_TERM),
  // y = 4x supersampling (VV_DEBUG_SSAA); both land in SceneUBO.misc.yz,
  // which the shader consumes. Defaults keep the plain path.
  void update(const vv::core::Camera& camera, float timeSeconds,
              const LightingConfig& lighting,
              const glm::vec2& debugFlags = glm::vec2(0.0f));

  VkBuffer buffer() const { return m_buffer; }
  void* mapped() const { return m_mapped; }

 private:
  VkBuffer m_buffer = VK_NULL_HANDLE;
  VkDeviceMemory m_memory = VK_NULL_HANDLE;
  void* m_mapped = nullptr;
};

} // namespace vv::render
