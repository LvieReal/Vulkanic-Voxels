#include "vulkan/VoxelResources.hpp"

#include <cstring>

#include "voxel/World.hpp"
#include "vulkan/VulkanUtils.hpp"

namespace vv::vulkan {

VoxelResources::~VoxelResources() {
  // Requires device for cleanup; caller should invoke cleanup().
}

bool VoxelResources::createAndUpload(VkDevice device,
                                     VkPhysicalDevice physicalDevice,
                                     VkCommandPool commandPool, VkQueue queue,
                                     const vv::voxel::VoxelConfig& config,
                                     std::string& outError) {
  m_config = config;

  vv::voxel::World world(
      vv::voxel::Extent3u{config.chunkSizeVoxels.x, config.chunkSizeVoxels.y,
                          config.chunkSizeVoxels.z});
  const vv::voxel::Chunk& chunk = world.chunk0();
  const auto& voxels = chunk.rawVoxelsU32();
  const VkDeviceSize voxelBytes =
      static_cast<VkDeviceSize>(voxels.size() * sizeof(uint32_t));
  if (voxelBytes == 0) {
    outError = "World produced an empty chunk.";
    return false;
  }

  VkResult r = VK_SUCCESS;
  VkBuffer stagingBuffer = VK_NULL_HANDLE;
  VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;

  auto cleanupTemp = [&]() {
    if (cmd) {
      vkFreeCommandBuffers(device, commandPool, 1, &cmd);
      cmd = VK_NULL_HANDLE;
    }
    if (stagingBuffer) {
      vkDestroyBuffer(device, stagingBuffer, nullptr);
      stagingBuffer = VK_NULL_HANDLE;
    }
    if (stagingMemory) {
      vkFreeMemory(device, stagingMemory, nullptr);
      stagingMemory = VK_NULL_HANDLE;
    }
  };

  VkBufferCreateInfo voxelBuf{};
  voxelBuf.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  voxelBuf.size = voxelBytes;
  voxelBuf.usage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  voxelBuf.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  r = vkCreateBuffer(device, &voxelBuf, nullptr, &m_buffer);
  if (r != VK_SUCCESS) {
    outError =
        "Failed to create voxel buffer (" + utils::vkResultToString(r) + ").";
    cleanupTemp();
    return false;
  }

  VkMemoryRequirements voxelReq{};
  vkGetBufferMemoryRequirements(device, m_buffer, &voxelReq);
  uint32_t voxelMemType = utils::findMemoryTypeIndex(
      physicalDevice, voxelReq.memoryTypeBits,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (voxelMemType == UINT32_MAX) {
    outError = "No suitable device-local memory type found for voxel buffer.";
    cleanupTemp();
    return false;
  }

  VkMemoryAllocateInfo voxelAlloc{};
  voxelAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  voxelAlloc.allocationSize = voxelReq.size;
  voxelAlloc.memoryTypeIndex = voxelMemType;

  r = vkAllocateMemory(device, &voxelAlloc, nullptr, &m_memory);
  if (r != VK_SUCCESS) {
    outError = "Failed to allocate voxel buffer memory (" +
               utils::vkResultToString(r) + ").";
    cleanupTemp();
    return false;
  }

  r = vkBindBufferMemory(device, m_buffer, m_memory, 0);
  if (r != VK_SUCCESS) {
    outError =
        "Failed to bind voxel buffer memory (" + utils::vkResultToString(r) + ").";
    cleanupTemp();
    return false;
  }

  VkBufferCreateInfo stagingInfo{};
  stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  stagingInfo.size = voxelBytes;
  stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  r = vkCreateBuffer(device, &stagingInfo, nullptr, &stagingBuffer);
  if (r != VK_SUCCESS) {
    outError = "Failed to create voxel staging buffer (" +
               utils::vkResultToString(r) + ").";
    cleanupTemp();
    return false;
  }

  VkMemoryRequirements stagingReq{};
  vkGetBufferMemoryRequirements(device, stagingBuffer, &stagingReq);
  uint32_t stagingType = utils::findMemoryTypeIndex(
      physicalDevice, stagingReq.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (stagingType == UINT32_MAX) {
    outError =
        "No suitable host-visible memory type found for voxel staging buffer.";
    cleanupTemp();
    return false;
  }

  VkMemoryAllocateInfo stagingAlloc{};
  stagingAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  stagingAlloc.allocationSize = stagingReq.size;
  stagingAlloc.memoryTypeIndex = stagingType;

  r = vkAllocateMemory(device, &stagingAlloc, nullptr, &stagingMemory);
  if (r != VK_SUCCESS) {
    outError = "Failed to allocate voxel staging memory (" +
               utils::vkResultToString(r) + ").";
    cleanupTemp();
    return false;
  }

  r = vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);
  if (r != VK_SUCCESS) {
    outError =
        "Failed to bind voxel staging memory (" + utils::vkResultToString(r) + ").";
    cleanupTemp();
    return false;
  }

  void* mapped = nullptr;
  r = vkMapMemory(device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
  if (r != VK_SUCCESS || !mapped) {
    outError = "Failed to map voxel staging memory.";
    cleanupTemp();
    return false;
  }
  std::memcpy(mapped, voxels.data(), static_cast<size_t>(voxelBytes));
  vkUnmapMemory(device, stagingMemory);

  VkCommandBufferAllocateInfo cmdAlloc{};
  cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cmdAlloc.commandPool = commandPool;
  cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmdAlloc.commandBufferCount = 1;

  r = vkAllocateCommandBuffers(device, &cmdAlloc, &cmd);
  if (r != VK_SUCCESS) {
    outError = "Failed to allocate voxel upload command buffer (" +
               utils::vkResultToString(r) + ").";
    cleanupTemp();
    return false;
  }

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
    outError = "Failed to begin voxel upload command buffer.";
    cleanupTemp();
    return false;
  }

  VkBufferCopy copy{};
  copy.srcOffset = 0;
  copy.dstOffset = 0;
  copy.size = voxelBytes;
  vkCmdCopyBuffer(cmd, stagingBuffer, m_buffer, 1, &copy);

  if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
    outError = "Failed to end voxel upload command buffer.";
    cleanupTemp();
    return false;
  }

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;

  r = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
  if (r != VK_SUCCESS) {
    outError = "Failed to submit voxel upload (" + utils::vkResultToString(r) + ").";
    cleanupTemp();
    return false;
  }
  vkQueueWaitIdle(queue);

  cleanupTemp();
  return true;
}

void VoxelResources::cleanup(VkDevice device) {
  if (m_buffer) {
    vkDestroyBuffer(device, m_buffer, nullptr);
    m_buffer = VK_NULL_HANDLE;
  }
  if (m_memory) {
    vkFreeMemory(device, m_memory, nullptr);
    m_memory = VK_NULL_HANDLE;
  }
}

} // namespace vv::vulkan
