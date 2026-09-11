#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

#include "voxel/VoxelConfig.hpp"

namespace vv::vulkan {

// Manages voxel storage buffer and upload.
// Extracted from VulkanRenderer to separate voxel domain logic.
class VoxelResources final {
 public:
  VoxelResources() = default;
  ~VoxelResources();

  VoxelResources(const VoxelResources&) = delete;
  VoxelResources& operator=(const VoxelResources&) = delete;

  bool createAndUpload(VkDevice device, VkPhysicalDevice physicalDevice,
                       VkCommandPool commandPool, VkQueue queue,
                       const vv::voxel::VoxelConfig& config,
                       std::string& outError);

  void cleanup(VkDevice device);

  VkBuffer buffer() const { return m_buffer; }

  const vv::voxel::VoxelConfig& config() const { return m_config; }

 private:
  VkBuffer m_buffer = VK_NULL_HANDLE;
  VkDeviceMemory m_memory = VK_NULL_HANDLE;
  vv::voxel::VoxelConfig m_config{};
};

} // namespace vv::vulkan
