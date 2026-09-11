#include "vulkan/BufferUtils.hpp"

#include "vulkan/VulkanUtils.hpp"

#include <cstring>

namespace vv::vulkan::utils {

bool createBuffer(VkDevice device, VkPhysicalDevice physicalDevice,
                  VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags memProps, VkBuffer& outBuffer,
                  VkDeviceMemory& outMemory, std::string& outError) {
  VkBufferCreateInfo buf{};
  buf.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buf.size = size;
  buf.usage = usage;
  buf.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkResult r = vkCreateBuffer(device, &buf, nullptr, &outBuffer);
  if (r != VK_SUCCESS) {
    outError = "Failed to create buffer (" + vkResultToString(r) + ").";
    return false;
  }

  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(device, outBuffer, &req);
  uint32_t memType =
      findMemoryTypeIndex(physicalDevice, req.memoryTypeBits, memProps);
  // Fallback for device-local: try host-visible if device-local not found.
  if (memType == UINT32_MAX && (memProps & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
    memType = findMemoryTypeIndex(
        physicalDevice, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  }
  if (memType == UINT32_MAX) {
    outError = "No suitable memory type found for buffer.";
    vkDestroyBuffer(device, outBuffer, nullptr);
    outBuffer = VK_NULL_HANDLE;
    return false;
  }

  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = memType;

  r = vkAllocateMemory(device, &alloc, nullptr, &outMemory);
  if (r != VK_SUCCESS) {
    outError = "Failed to allocate buffer memory (" + vkResultToString(r) + ").";
    vkDestroyBuffer(device, outBuffer, nullptr);
    outBuffer = VK_NULL_HANDLE;
    return false;
  }

  r = vkBindBufferMemory(device, outBuffer, outMemory, 0);
  if (r != VK_SUCCESS) {
    outError = "Failed to bind buffer memory (" + vkResultToString(r) + ").";
    vkFreeMemory(device, outMemory, nullptr);
    vkDestroyBuffer(device, outBuffer, nullptr);
    outBuffer = VK_NULL_HANDLE;
    outMemory = VK_NULL_HANDLE;
    return false;
  }

  return true;
}

} // namespace vv::vulkan::utils
