#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

#include "voxel/Chunk.hpp"
#include "voxel/VoxelConfig.hpp"

namespace vv::vulkan {

// GPU storage for the voxel world:
//
//  - voxel atlas: one device-local storage buffer holding every loaded chunk.
//    Each slot holds one chunk's voxel types (one byte per VoxelType) packed
//    4-per-uint32, in the chunk's X + Y*sizeX + Z*sizeX*sizeY order.
//
//  - chunk table: one host-visible storage buffer mapping region grid cells
//    (chunk coords relative to the region origin) to atlas slot indices;
//    kEmptySlot marks "not loaded" (treated as air by the shader).
class VoxelResources final {
 public:
	struct ChunkUpload {
		std::uint32_t slot = 0;
		const vv::voxel::Chunk* chunk = nullptr;
	};

	static constexpr std::uint32_t kEmptySlot = 0xFFFFFFFFu;

	VoxelResources() = default;
	~VoxelResources();

	VoxelResources(const VoxelResources&) = delete;
	VoxelResources& operator=(const VoxelResources&) = delete;

	bool create(VkDevice device, VkPhysicalDevice physicalDevice,
							const vv::voxel::VoxelConfig& config, std::string& outError);

	// Batch-uploads chunk data into their slots (single staging buffer + one
	// one-time submit; waits for the queue so in-flight frames never read a
	// half-updated atlas).
	bool uploadChunks(VkDevice device, VkPhysicalDevice physicalDevice,
										VkCommandPool commandPool, VkQueue queue,
										const std::vector<ChunkUpload>& uploads,
										std::string& outError);

	// Rewrites the whole chunk table (one u32 slot index per region grid
	// cell, row-major over gridWidth x gridHeight).
	bool writeChunkTable(const std::vector<std::uint32_t>& slotPerCell);

	void cleanup(VkDevice device);

	VkBuffer voxelBuffer() const { return m_voxelBuffer; }
	VkBuffer chunkTableBuffer() const { return m_chunkTableBuffer; }

	std::uint32_t slotCount() const { return m_slotCount; }
	std::uint64_t slotByteStride() const { return m_slotByteStride; }
	std::uint64_t slotWordStride() const { return m_slotByteStride / 4u; }

 private:
	VkBuffer m_voxelBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_voxelMemory = VK_NULL_HANDLE;

	VkBuffer m_chunkTableBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_chunkTableMemory = VK_NULL_HANDLE;
	void* m_mappedTable = nullptr;

	std::uint32_t m_slotCount = 0;
	std::uint64_t m_slotByteStride = 0;
	std::uint64_t m_tableElements = 0;
};

}  // namespace vv::vulkan
