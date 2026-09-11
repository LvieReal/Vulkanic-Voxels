#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

namespace vv::vulkan::utils {

// Helpers for Vulkan buffer creation and upload.

bool createBuffer(VkDevice device, VkPhysicalDevice physicalDevice,
                  VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags memProps, VkBuffer& outBuffer,
                  VkDeviceMemory& outMemory, std::string& outError);

} // namespace vv::vulkan::utils
