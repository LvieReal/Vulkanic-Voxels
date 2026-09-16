#include "render/SceneUniform.hpp"

#include <cstring>

#include "vulkan/VulkanUtils.hpp"

namespace vv::render {

SceneUniform::~SceneUniform() {
  // Requires device for cleanup; caller should invoke cleanup().
}

bool SceneUniform::create(VkDevice device, VkPhysicalDevice physicalDevice,
                          std::string& outError) {
  VkBufferCreateInfo buf{};
  buf.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buf.size = static_cast<VkDeviceSize>(sizeof(SceneUBO));
  buf.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  buf.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkResult r = vkCreateBuffer(device, &buf, nullptr, &m_buffer);
  if (r != VK_SUCCESS) {
    outError = "Failed to create scene uniform buffer (" +
               vv::vulkan::utils::vkResultToString(r) + ").";
    return false;
  }

  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(device, m_buffer, &req);
  uint32_t memType = vv::vulkan::utils::findMemoryTypeIndex(
      physicalDevice, req.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (memType == UINT32_MAX) {
    outError =
        "No suitable host-visible memory type found for scene uniform buffer.";
    return false;
  }

  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = memType;

  r = vkAllocateMemory(device, &alloc, nullptr, &m_memory);
  if (r != VK_SUCCESS) {
    outError = "Failed to allocate scene uniform buffer memory (" +
               vv::vulkan::utils::vkResultToString(r) + ").";
    return false;
  }

  r = vkBindBufferMemory(device, m_buffer, m_memory, 0);
  if (r != VK_SUCCESS) {
    outError = "Failed to bind scene uniform buffer memory (" +
               vv::vulkan::utils::vkResultToString(r) + ").";
    return false;
  }

  void* mapped = nullptr;
  r = vkMapMemory(device, m_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
  if (r != VK_SUCCESS || !mapped) {
    outError = "Failed to map scene uniform buffer memory.";
    return false;
  }

  m_mapped = mapped;
  std::memset(m_mapped, 0, sizeof(SceneUBO));
  return true;
}

void SceneUniform::cleanup(VkDevice device) {
  if (m_mapped) {
    vkUnmapMemory(device, m_memory);
    m_mapped = nullptr;
  }
  if (m_buffer) {
    vkDestroyBuffer(device, m_buffer, nullptr);
    m_buffer = VK_NULL_HANDLE;
  }
  if (m_memory) {
    vkFreeMemory(device, m_memory, nullptr);
    m_memory = VK_NULL_HANDLE;
  }
}

void SceneUniform::update(const vv::core::Camera& camera, float timeSeconds,
                          const LightingConfig& lighting,
                          const glm::vec4& sceneFlags,
                          const glm::vec4& ambientParams) {
  if (!m_mapped) {
    return;
  }
  SceneUBO ubo{};
  const glm::vec3 f = camera.forward();
  const glm::vec3 r = camera.right();
  const glm::vec3 u = camera.up();

  ubo.camPos = glm::vec4(camera.position(), 1.0f);
  ubo.camForward = glm::vec4(f, 0.0f);
  ubo.camRight = glm::vec4(r, 0.0f);
  ubo.camUp = glm::vec4(u, 0.0f);

  ubo.lightDir = glm::vec4(glm::normalize(lighting.lightDir), 0.0f);
  ubo.lightColor = glm::vec4(lighting.lightColor, 0.0f);
  ubo.skyLow = glm::vec4(lighting.skyLow, 0.0f);
  ubo.skyHigh = glm::vec4(lighting.skyHigh, 0.0f);
  ubo.misc = glm::vec4(timeSeconds, sceneFlags.x, sceneFlags.y,
                       sceneFlags.z);
  ubo.ambient = ambientParams;

  std::memcpy(m_mapped, &ubo, sizeof(ubo));
}

} // namespace vv::render
