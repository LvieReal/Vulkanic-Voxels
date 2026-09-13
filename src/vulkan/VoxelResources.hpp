#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

#include "voxel/Chunk.hpp"
#include "voxel/VoxelConfig.hpp"
#include "voxel/VoxelTextures.hpp"

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
//    from vv::voxel::kVoxelTypeInfo. Now the FALLBACK look: types with
//    texture files (resources/textures/voxels) use those instead (see
//    createVoxelTextures + binding 10's per-type face table).
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

	// Bindless voxel textures (bindings 8/9/10): one mip-mapped RGBA8
	// image per texture FILE (no atlas; a 1x1 white dummy keeps the
	// array non-empty), uploaded + mip-chained with one one-time
	// submit. `sets` (from vv::render::loadVoxelTextureFiles) maps each
	// type's faces to image indices; it is published to the shader via
	// a small per-type SSBO (binding 10). Nearest texel sampling by
	// default. Idempotent.
	bool createVoxelTextures(
			VkDevice device, VkPhysicalDevice physicalDevice,
			VkCommandPool commandPool, VkQueue queue,
			const std::vector<vv::voxel::VoxelTextureImage>& images,
			const std::vector<vv::voxel::VoxelTextureSet>& sets,
			std::string& outError);

	// Batch-uploads chunk data into their slots (single staging buffer + one
	// one-time submit; waits for the queue so in-flight frames never read a
	// half-uploaded atlas). Synchronous - used for the initial region and
	// genuine teleports.
	bool uploadChunks(VkDevice device, VkPhysicalDevice physicalDevice,
										VkCommandPool commandPool, VkQueue queue,
										const std::vector<ChunkUpload>& uploads,
										std::string& outError);

	// Streaming upload (one chunk per call): persistent staging buffer +
	// command buffer + fence; waits ONLY for the previous streaming submit
	// (a frame old by then) - never the device or queue. The synchronous
	// path above stalls on vkDeviceWaitIdle + vkQueueWaitIdle every
	// streaming frame, which read as per-frame stutter during border
	// crossings; this path removes the stalls.
	// Safety: only upload slots no uploaded chunk table references (spare
	// ring); the region table swap (finishRegionMove) drains everything
	// with its own full device wait before publishing.
	bool uploadChunksStreaming(VkDevice device, VkPhysicalDevice physicalDevice,
															VkCommandPool commandPool, VkQueue queue,
															const ChunkUpload& upload,
															std::string& outError);
	// The chunk table is TABLE-HALVES-buffered and the far field is
	// FAR-HALVES-buffered: writers always target the half no in-flight
	// frame reads, then the renderer flips a push-constant half index.
	// This removes every device/queue wait from the per-crossing region
	// swap (the sprint hitch). Three table halves are needed because with
	// kMaxFramesInFlight == 2 a half being rewritten must have been last
	// read by a frame whose fence has since been waited; two halves cannot
	// guarantee that when swaps happen on consecutive frames.
	static constexpr std::uint32_t kTableHalves = 3;
	static constexpr std::uint32_t kFarHalves = 2;

	// Blocks until the last fence-scoped streaming chunk upload has fully
	// landed (sub-millisecond; the publisher calls it before making a table
	// that references freshly uploaded slots active).
	void waitStreamingUploadIdle(VkDevice device);

	// Uploads a complete far field into the given half of the far buffer.
	// Uses a persistent staging buffer + fence (no device/queue waits);
	// waits for its own copy to land before returning, so the caller can
	// flip the far half index immediately.
	bool uploadFarFieldHalf(VkDevice device, VkPhysicalDevice physicalDevice,
													VkCommandPool commandPool, VkQueue queue,
													const std::vector<std::uint32_t>& cells,
													std::uint32_t half, std::string& outError);

	// Async partial upload into the given half: cellRuns are (cellOffset,
	// count) ranges whose values are concatenated in `values` (in run
	// order). Fence-scoped (the next far upload waits it); returns without
	// waiting. Used for far seam patches - a torn frame can only show a
	// few seam cells with the previous few-voxels-lower estimate.
	bool uploadFarFieldDelta(VkDevice device, VkPhysicalDevice physicalDevice,
												 VkCommandPool commandPool, VkQueue queue,
												 std::uint32_t half,
												 const std::vector<std::pair<std::uint32_t, std::uint32_t>>& cellRuns,
												 const std::vector<std::uint32_t>& values,
												 std::string& outError);

	// Rewrites one half of the chunk table (one u32 slot index per region
	// grid cell, row-major over gridWidth x gridHeight). Plain mapped
	// write to the inactive half - no GPU synchronization needed.
	bool writeChunkTable(const std::vector<std::uint32_t>& slotPerCell,
											std::uint32_t half);

	// Per-slot fade-in alphas (binding 7; one float per atlas slot, 1.0 =
	// fully opaque). The renderer writes the whole array each frame from
	// mapped memory; the shader composites hits in fading chunks against
	// whatever is behind them (far LOD / sky / another chunk).
	void writeChunkFade(const std::vector<float>& alphas);

	void cleanup(VkDevice device);

	VkBuffer voxelBuffer() const { return m_voxelBuffer; }
	VkBuffer chunkTableBuffer() const { return m_chunkTableBuffer; }
	VkBuffer heightBuffer() const { return m_heightBuffer; }
	VkBuffer fadeBuffer() const { return m_fadeBuffer; }

	// Bindless texture array (one image per texture FILE plus the white
	// dummy at index 0; see createVoxelTextures). Empty until created.
	std::uint32_t voxelTextureCount() const {
		return static_cast<std::uint32_t>(m_voxelTextureViews.size());
	}
	VkBuffer voxelTexInfoBuffer() const { return m_texInfoBuffer; }
	VkImageView voxelTextureView(std::uint32_t type) const {
		return type < m_voxelTextureViews.size() ? m_voxelTextureViews[type]
																					 : VK_NULL_HANDLE;
	}
	VkSampler voxelSampler() const { return m_voxelSampler; }
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

	// Per-slot fade-in alphas (see writeChunkFade).
	VkBuffer m_fadeBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_fadeMemory = VK_NULL_HANDLE;
	void* m_mappedFade = nullptr;

	// Bindless voxel textures (see createVoxelTextures): one image per
	// texture FILE plus a shared REPEAT/nearest-texel sampler.
	std::vector<VkImage> m_voxelTextureImages;
	std::vector<VkDeviceMemory> m_voxelTextureMemory;
	std::vector<VkImageView> m_voxelTextureViews;
	VkSampler m_voxelSampler = VK_NULL_HANDLE;

	// Per-type face table for the shader (binding 10): 8 u32 per
	// VoxelType - words 0..5 = face image indices (0xFFFFFFFF = plain
	// palette colors), word 6 = nominal texture size (explicit-LOD
	// estimate), word 7 unused. Written once at creation.
	VkBuffer m_texInfoBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_texInfoMemory = VK_NULL_HANDLE;

	VkBuffer m_farBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_farMemory = VK_NULL_HANDLE;
	std::uint64_t m_farCellsPerHalf = 0;  // far dim * far dim

	// --- Far upload path (double-buffered: staging + cmd + fence x2) ---
	// Each upload uses the slot NOT used by the previous one, so the wait
	// before writing targets a submit TWO uploads old - always retired.
	// (A single buffer made the wait trail the GPU by a full frame, since
	// the copy is queued behind the previous frame's compute work - a
	// frame-time stall per streaming frame at high load.)
	VkBuffer m_farStaging[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	VkDeviceMemory m_farStagingMemory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	void* m_farStagingMapped[2] = {nullptr, nullptr};
	VkCommandPool m_farCommandPool = VK_NULL_HANDLE;
	VkCommandBuffer m_farCmd[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	VkFence m_farFence[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	bool m_farFencePending[2] = {false, false};
	std::uint32_t m_farParity = 0;

	bool ensureFarUploadResources(VkDevice device,
																VkPhysicalDevice physicalDevice,
																VkCommandPool commandPool,
																std::string& outError);
	void waitPreviousFarUpload(VkDevice device);

	VkBuffer m_paletteBuffer = VK_NULL_HANDLE;
	VkDeviceMemory m_paletteMemory = VK_NULL_HANDLE;

	// --- Streaming upload path (double-buffered, see far above) ---
	VkBuffer m_streamStaging[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	VkDeviceMemory m_streamStagingMemory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	void* m_streamStagingMapped[2] = {nullptr, nullptr};
	VkCommandPool m_streamCommandPool = VK_NULL_HANDLE;
	VkCommandBuffer m_streamCmd[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	VkFence m_streamFence[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
	bool m_streamFencePending[2] = {false, false};
	std::uint32_t m_streamParity = 0;
	// Uploads the palette built from vv::voxel::kVoxelTypeInfo.
	bool createPalette(VkDevice device, VkPhysicalDevice physicalDevice,
										 std::string& outError);

	std::uint32_t m_slotCount = 0;
	std::uint64_t m_slotByteStride = 0;
	std::uint64_t m_heightSlotWords = 0;
	std::uint64_t m_tableElements = 0;
};

}  // namespace vv::vulkan
