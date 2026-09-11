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
//
//  - column height atlas: per chunk slot, one u16 per voxel column (highest
//    solid voxel Y + 1, 0 = all-air), packed two-per-u32, row-major over
//    X + Z*chunkSizeX. The compute shader's air-skip uses it to step over
//    empty columns in O(1); the bound is conservative, so it stays correct
//    for any future voxel content (overhangs, edits).
//
//  - voxel palette: small buffer with per-type, per-face base colors, filled
//    from vv::voxel::kVoxelTypeInfo. Placeholder until the texturing pass;
//    the plan is a bindless texture array with real per-type albedo textures.
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

	// Uploads a freshly built far-LOD field (vv::terrain::FarField::cells)
	// into the far buffer. Rare (far-field recenters), so a queue idle is
	// acceptable.
	bool uploadFarField(VkDevice device, VkPhysicalDevice physicalDevice,
											VkCommandPool commandPool, VkQueue queue,
											const std::vector<std::uint32_t>& cells,
											std::string& outError);

	// Rewrites the whole chunk table (one u32 slot index per region grid
	// cell, row-major over gridWidth x gridHeight).
	bool writeChunkTable(const std::vector<std::uint32_t>& slotPerCell);

	void cleanup(VkDevice device);

	VkBuffer voxelBuffer() const { return m_voxelBuffer; }
	VkBuffer chunkTableBuffer() const { return m_chunkTableBuffer; }
	VkBuffer heightBuffer() const { return m_heightBuffer; }
	VkBuffer farBuffer() const { return m_farBuffer; }
	VkBuffer paletteBuffer() const { return m_paletteBuffer; }

	std::uint32_t slotCount() const { return m_slotCount; }
	std::uint64_t slotByteStride() const { return m_slotByteStride; }
	std::uint64_t slotWordStride() const { return m_slotByteStride / 4u; }
	// Heightmap slot stride in u32 words (sync contract with the shader's
	// (chunkSizeX * chunkSizeZ + 1) / 2).
	std::uint64_t heightSlotWordStride() const { return m_heightSlotWords; }

 private:
	VkBuffer m_voxelBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_voxelMemory = VK_NULL_HANDLE;

	VkBuffer m_chunkTableBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_chunkTableMemory = VK_NULL_HANDLE;
	void* m_mappedTable = nullptr;

	VkBuffer m_heightBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_heightMemory = VK_NULL_HANDLE;

	VkBuffer m_farBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_farMemory = VK_NULL_HANDLE;

	VkBuffer m_paletteBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_paletteMemory = VK_NULL_HANDLE;

	// Uploads the palette built from vv::voxel::kVoxelTypeInfo.
	bool createPalette(VkDevice device, VkPhysicalDevice physicalDevice,
										 std::string& outError);

	std::uint32_t m_slotCount = 0;
	std::uint64_t m_slotByteStride = 0;
	std::uint64_t m_heightSlotWords = 0;
	std::uint64_t m_tableElements = 0;
};

}  // namespace vv::vulkan
