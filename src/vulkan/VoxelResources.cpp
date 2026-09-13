#include "vulkan/VoxelResources.hpp"

#include <algorithm>
#include <cstring>

#include "vulkan/BufferUtils.hpp"
#include "vulkan/VulkanUtils.hpp"

namespace vv::vulkan {

VoxelResources::~VoxelResources() {
	// Requires a device for cleanup; callers must invoke cleanup() first.
}

bool VoxelResources::create(VkDevice device, VkPhysicalDevice physicalDevice,
														const vv::voxel::VoxelConfig& config,
														std::string& outError) {
	const std::uint64_t voxelsPerChunk =
			static_cast<std::uint64_t>(config.chunkSizeX) * config.worldHeight *
			config.chunkSizeZ;
	m_slotByteStride = (voxelsPerChunk + 3u) / 4u * 4u;
	m_slotCount = static_cast<std::uint32_t>(config.slotCount());
	m_tableElements =
			static_cast<std::uint64_t>(config.gridWidth()) * config.gridHeight();

	if (m_slotCount == 0 || m_slotByteStride == 0) {
		outError = "Invalid voxel world configuration (zero-sized atlas).";
		return false;
	}

	const VkDeviceSize atlasBytes =
			static_cast<VkDeviceSize>(m_slotCount) * m_slotByteStride;
	if (!utils::createBuffer(device, physicalDevice, atlasBytes,
														VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
																VK_BUFFER_USAGE_TRANSFER_DST_BIT,
														VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
														m_voxelBuffer, m_voxelMemory, outError)) {
		return false;
	}

	// Column height atlas (see class comment). One slot per chunk, u16 per
	// column packed two-per-u32.
	m_heightSlotWords =
			(static_cast<std::uint64_t>(config.chunkSizeX) * config.chunkSizeZ +
			 1u) /
			2u;
	const VkDeviceSize heightBytes =
			static_cast<VkDeviceSize>(m_slotCount) * m_heightSlotWords * 4u;
	if (!utils::createBuffer(device, physicalDevice, heightBytes,
														VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
																VK_BUFFER_USAGE_TRANSFER_DST_BIT,
														VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
														m_heightBuffer, m_heightMemory, outError)) {
		cleanup(device);
		return false;
	}

	// Far-LOD height field (binding 6): one u32 per coarse cell, dim =
	// 2 * farLodRadiusChunks * chunkSizeX / farLodCellVoxels (square). The
	// buffer always exists (tiny dummy when far LOD is disabled) so the
	// binding-6 descriptor is always valid; the shader never reads it when
	// the push-constant far dims are 0.
	{
		const std::uint64_t farDim =
				std::max<std::uint64_t>(config.farLodDim(), 1u);
		m_farCellsPerHalf = farDim * farDim;
		// Double-buffered (kFarHalves): uploads target the inactive half.
		const VkDeviceSize farBytes =
				static_cast<VkDeviceSize>(farDim) * farDim * 4u * kFarHalves;
		if (!utils::createBuffer(device, physicalDevice, farBytes,
															VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
																	VK_BUFFER_USAGE_TRANSFER_DST_BIT,
															VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
															m_farBuffer, m_farMemory, outError)) {
			cleanup(device);
			return false;
		}
	}

	// Triple-buffered (kTableHalves, see header): region swaps write the
	// half no in-flight frame reads, then flip a push-constant index.
	const VkDeviceSize tableBytes = static_cast<VkDeviceSize>(m_tableElements) *
																	sizeof(std::uint32_t) * kTableHalves;
	if (!utils::createBuffer(device, physicalDevice, tableBytes,
													 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
													 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
															 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
													 m_chunkTableBuffer, m_chunkTableMemory,
													 outError)) {
		cleanup(device);
		return false;
	}

	VkResult r = vkMapMemory(device, m_chunkTableMemory, 0, VK_WHOLE_SIZE, 0,
													 &m_mappedTable);
	if (r != VK_SUCCESS || m_mappedTable == nullptr) {
		outError = "Failed to map the chunk table memory.";
		cleanup(device);
		return false;
	}
	std::memset(m_mappedTable, 0xFF, static_cast<std::size_t>(tableBytes));

	if (!createPalette(device, physicalDevice, outError)) {
		cleanup(device);
		return false;
	}

	return true;
}

bool VoxelResources::createPalette(VkDevice device,
																		VkPhysicalDevice physicalDevice,
																		std::string& outError) {
	// Flat per-type, per-face color palette (see vv::voxel::buildVoxelPalette;
	// layout mirrors the shader's VoxelPalette SSBO). Placeholder until the
	// texturing pass; the plan is a bindless texture array with real per-type
	// albedo textures.
	const std::vector<float> palette = vv::voxel::buildVoxelPalette();

	const VkDeviceSize paletteBytes = static_cast<VkDeviceSize>(palette.size()) *
																		sizeof(float);
	if (!utils::createBuffer(device, physicalDevice, paletteBytes,
													 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
													 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
															 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
													 m_paletteBuffer, m_paletteMemory,
													 outError)) {
		return false;
	}

	void* mapped = nullptr;
	VkResult r = vkMapMemory(device, m_paletteMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
	if (r != VK_SUCCESS || mapped == nullptr) {
		outError = "Failed to map the voxel palette memory.";
		return false;
	}
	std::memcpy(mapped, palette.data(), palette.size() * sizeof(float));
	vkUnmapMemory(device, m_paletteMemory);
	return true;
}

bool VoxelResources::uploadChunks(VkDevice device,
																	VkPhysicalDevice physicalDevice,
																	VkCommandPool commandPool, VkQueue queue,
																	const std::vector<ChunkUpload>& uploads,
																	std::string& outError) {
	if (uploads.empty()) {
		return true;
	}

	VkDeviceSize totalBytes = 0;
	for (const ChunkUpload& upload : uploads) {
		if (upload.chunk == nullptr || upload.slot >= m_slotCount) {
			outError = "Invalid chunk upload request.";
			return false;
		}
		if (upload.chunk->heightMapWordStride() != m_heightSlotWords) {
			outError = "Chunk heightmap stride does not match the atlas.";
			return false;
		}
		totalBytes += static_cast<VkDeviceSize>(m_slotByteStride) +
		              static_cast<VkDeviceSize>(m_heightSlotWords) * 4u;
	}

	// Wait for in-flight frames before mutating the atlas; region updates are
	// rare (chunk border crossings), so a queue idle here is acceptable.
	vkDeviceWaitIdle(device);

	VkBuffer stagingBuffer = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
	if (!utils::createBuffer(device, physicalDevice, totalBytes,
													 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
													 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
															 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
													 stagingBuffer, stagingMemory, outError)) {
		return false;
	}

	void* mapped = nullptr;
	VkResult r = vkMapMemory(device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
	if (r != VK_SUCCESS || mapped == nullptr) {
		outError = "Failed to map chunk staging memory.";
		vkDestroyBuffer(device, stagingBuffer, nullptr);
		vkFreeMemory(device, stagingMemory, nullptr);
		return false;
	}

	std::memset(mapped, 0, static_cast<std::size_t>(totalBytes));
	{
		// Staging layout: [voxel data for all uploads][heightmaps for all
		// uploads], so each section is copied with contiguous per-upload
		// regions.
		std::size_t voxelOffset = 0;
		std::size_t heightOffset =
				static_cast<std::size_t>(m_slotByteStride) * uploads.size();
		const std::size_t heightBytesPerSlot =
				static_cast<std::size_t>(m_heightSlotWords) * 4u;
		for (const ChunkUpload& upload : uploads) {
			const auto& types = upload.chunk->voxelTypes();
			std::memcpy(static_cast<std::uint8_t*>(mapped) + voxelOffset,
									types.data(), types.size());
			voxelOffset += static_cast<std::size_t>(m_slotByteStride);

			const auto& heights = upload.chunk->heightMapWords();
			std::memcpy(static_cast<std::uint8_t*>(mapped) + heightOffset,
									heights.data(), heights.size() * sizeof(std::uint32_t));
			heightOffset += heightBytesPerSlot;
		}
	}
	vkUnmapMemory(device, stagingMemory);

	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkCommandBufferAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	alloc.commandPool = commandPool;
	alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc.commandBufferCount = 1;
	r = vkAllocateCommandBuffers(device, &alloc, &cmd);
	if (r != VK_SUCCESS) {
		outError = "Failed to allocate chunk upload command buffer.";
		vkDestroyBuffer(device, stagingBuffer, nullptr);
		vkFreeMemory(device, stagingMemory, nullptr);
		return false;
	}

	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
		outError = "Failed to begin chunk upload command buffer.";
		vkFreeCommandBuffers(device, commandPool, 1, &cmd);
		vkDestroyBuffer(device, stagingBuffer, nullptr);
		vkFreeMemory(device, stagingMemory, nullptr);
		return false;
	}

	// Two SEPARATE copy commands: voxel data -> voxel atlas, heightmaps ->
	// height atlas. The regions must never be mixed into one call: a region's
	// destination BUFFER is chosen by the call, not by the region struct.
	// (Pass 3.0 shipped them mixed into one vkCmdCopyBuffer targeting the
	// voxel atlas: heightmap bytes landed in the first 2 KB of every chunk
	// slot and the height buffer stayed uninitialized -> every column read
	// bound 0 -> every ray was air-skipped -> "voxels disappeared".)
	std::vector<VkBufferCopy> voxelRegions;
	std::vector<VkBufferCopy> heightRegions;
	voxelRegions.reserve(uploads.size());
	heightRegions.reserve(uploads.size());
	{
		VkDeviceSize voxelOffset = 0;
		VkDeviceSize heightOffset =
				static_cast<VkDeviceSize>(m_slotByteStride) *
				static_cast<VkDeviceSize>(uploads.size());
		const VkDeviceSize heightBytesPerSlot =
				static_cast<VkDeviceSize>(m_heightSlotWords) * 4u;
		for (const ChunkUpload& upload : uploads) {
			VkBufferCopy voxelRegion{};
			voxelRegion.srcOffset = voxelOffset;
			voxelRegion.dstOffset =
					static_cast<VkDeviceSize>(upload.slot) * m_slotByteStride;
			voxelRegion.size = static_cast<VkDeviceSize>(m_slotByteStride);
			voxelRegions.push_back(voxelRegion);
			voxelOffset += static_cast<VkDeviceSize>(m_slotByteStride);

			VkBufferCopy heightRegion{};
			heightRegion.srcOffset = heightOffset;
			heightRegion.dstOffset =
					static_cast<VkDeviceSize>(upload.slot) * heightBytesPerSlot;
			heightRegion.size = heightBytesPerSlot;
			heightRegions.push_back(heightRegion);
			heightOffset += heightBytesPerSlot;
		}
	}
	vkCmdCopyBuffer(cmd, stagingBuffer, m_voxelBuffer,
								 static_cast<std::uint32_t>(voxelRegions.size()),
								 voxelRegions.data());
	vkCmdCopyBuffer(cmd, stagingBuffer, m_heightBuffer,
								 static_cast<std::uint32_t>(heightRegions.size()),
								 heightRegions.data());

	if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
		outError = "Failed to end chunk upload command buffer.";
		vkFreeCommandBuffers(device, commandPool, 1, &cmd);
		vkDestroyBuffer(device, stagingBuffer, nullptr);
		vkFreeMemory(device, stagingMemory, nullptr);
		return false;
	}

	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	r = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
	if (r == VK_SUCCESS) {
		vkQueueWaitIdle(queue);
	}

	vkFreeCommandBuffers(device, commandPool, 1, &cmd);
	vkDestroyBuffer(device, stagingBuffer, nullptr);
	vkFreeMemory(device, stagingMemory, nullptr);

	if (r != VK_SUCCESS) {
		outError = "Failed to submit chunk uploads.";
		return false;
	}
	return true;
}

bool VoxelResources::uploadChunksStreaming(
		VkDevice device, VkPhysicalDevice physicalDevice,
		VkCommandPool commandPool, VkQueue queue, const ChunkUpload& upload,
		std::string& outError) {
	if (upload.chunk == nullptr || upload.slot >= m_slotCount) {
		outError = "Invalid streaming chunk upload request.";
		return false;
	}
	if (upload.chunk->heightMapWordStride() != m_heightSlotWords) {
		outError = "Chunk heightmap stride does not match the atlas.";
		return false;
	}

	// Lazily create the persistent staging (exactly one chunk's worth:
	// voxel bytes + height words), command buffer and fence.
	if (m_streamStaging == VK_NULL_HANDLE) {
		const VkDeviceSize bytes = static_cast<VkDeviceSize>(m_slotByteStride) +
				static_cast<VkDeviceSize>(m_heightSlotWords) * 4u;
		if (!utils::createBuffer(device, physicalDevice, bytes,
														 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
														 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
																 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
														 m_streamStaging, m_streamStagingMemory,
														 outError)) {
			return false;
		}
		VkResult r = vkMapMemory(device, m_streamStagingMemory, 0, VK_WHOLE_SIZE, 0,
														 &m_streamStagingMapped);
		if (r != VK_SUCCESS || m_streamStagingMapped == nullptr) {
			outError = "Failed to map streaming staging memory.";
			return false;
		}
		VkCommandBufferAllocateInfo alloc{};
		alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		alloc.commandPool = commandPool;
		alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		alloc.commandBufferCount = 1;
		r = vkAllocateCommandBuffers(device, &alloc, &m_streamCmd);
		if (r != VK_SUCCESS) {
			outError = "Failed to allocate streaming command buffer.";
			return false;
		}
		m_streamCommandPool = commandPool;
		VkFenceCreateInfo fence{};
		fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		r = vkCreateFence(device, &fence, nullptr, &m_streamFence);
		if (r != VK_SUCCESS) {
			outError = "Failed to create streaming upload fence.";
			return false;
		}
	}

	// Wait for the PREVIOUS streaming submit only (not the device/queue):
	// by the next frame it is long done, so this normally costs nothing.
	if (m_streamFencePending) {
		vkWaitForFences(device, 1, &m_streamFence, VK_TRUE, UINT64_MAX);
		vkResetFences(device, 1, &m_streamFence);
		m_streamFencePending = false;
	}

	// Staging layout: [voxel bytes][heightmap words].
	const auto& types = upload.chunk->voxelTypes();
	std::memcpy(m_streamStagingMapped, types.data(), types.size());
	const auto& heights = upload.chunk->heightMapWords();
	std::memcpy(static_cast<std::uint8_t*>(m_streamStagingMapped) +
								static_cast<std::size_t>(m_slotByteStride),
							heights.data(), heights.size() * sizeof(std::uint32_t));

	VkResult r = vkResetCommandBuffer(m_streamCmd, 0);
	if (r != VK_SUCCESS) {
		outError = "Failed to reset streaming command buffer.";
		return false;
	}
	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	r = vkBeginCommandBuffer(m_streamCmd, &begin);
	if (r != VK_SUCCESS) {
		outError = "Failed to begin streaming command buffer.";
		return false;
	}
	// Two SEPARATE copy commands (voxels -> voxel atlas, heights -> height
	// atlas): one vkCmdCopyBuffer must never span destination buffers.
	VkBufferCopy voxelRegion{};
	voxelRegion.size = static_cast<VkDeviceSize>(m_slotByteStride);
	voxelRegion.dstOffset =
			static_cast<VkDeviceSize>(upload.slot) * m_slotByteStride;
	vkCmdCopyBuffer(m_streamCmd, m_streamStaging, m_voxelBuffer, 1, &voxelRegion);
	VkBufferCopy heightRegion{};
	heightRegion.srcOffset = static_cast<VkDeviceSize>(m_slotByteStride);
	heightRegion.size = static_cast<VkDeviceSize>(m_heightSlotWords) * 4u;
	heightRegion.dstOffset = static_cast<VkDeviceSize>(upload.slot) *
														static_cast<VkDeviceSize>(m_heightSlotWords) * 4u;
	vkCmdCopyBuffer(m_streamCmd, m_streamStaging, m_heightBuffer, 1,
									&heightRegion);
	r = vkEndCommandBuffer(m_streamCmd);
	if (r != VK_SUCCESS) {
		outError = "Failed to end streaming command buffer.";
		return false;
	}

	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &m_streamCmd;
	r = vkQueueSubmit(queue, 1, &submit, m_streamFence);
	if (r != VK_SUCCESS) {
		outError = "Failed to submit streaming chunk upload.";
		return false;
	}
	m_streamFencePending = true;
	return true;
}

void VoxelResources::waitStreamingUploadIdle(VkDevice device) {
	if (m_streamFencePending) {
		vkWaitForFences(device, 1, &m_streamFence, VK_TRUE, UINT64_MAX);
		vkResetFences(device, 1, &m_streamFence);
		m_streamFencePending = false;
	}
}

bool VoxelResources::ensureFarUploadResources(
		VkDevice device, VkPhysicalDevice physicalDevice, VkCommandPool commandPool,
		std::string& outError) {
	if (m_farStaging != VK_NULL_HANDLE) {
		return true;
	}
	const VkDeviceSize bytes =
			static_cast<VkDeviceSize>(m_farCellsPerHalf) * 4u;
	if (bytes == 0) {
		outError = "Far LOD is not enabled on this resource set.";
		return false;
	}
	if (!utils::createBuffer(device, physicalDevice, bytes,
													 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
													 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
															 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
													 m_farStaging, m_farStagingMemory,
													 outError)) {
		return false;
	}
	VkResult r = vkMapMemory(device, m_farStagingMemory, 0, VK_WHOLE_SIZE, 0,
													 &m_farStagingMapped);
	if (r != VK_SUCCESS || m_farStagingMapped == nullptr) {
		outError = "Failed to map far staging memory.";
		return false;
	}
	VkCommandBufferAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	alloc.commandPool = commandPool;
	alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc.commandBufferCount = 1;
	r = vkAllocateCommandBuffers(device, &alloc, &m_farCmd);
	if (r != VK_SUCCESS) {
		outError = "Failed to allocate far upload command buffer.";
		return false;
	}
	m_farCommandPool = commandPool;
	VkFenceCreateInfo fence{};
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	r = vkCreateFence(device, &fence, nullptr, &m_farFence);
	if (r != VK_SUCCESS) {
		outError = "Failed to create far upload fence.";
		return false;
	}
	return true;
}

void VoxelResources::waitPreviousFarUpload(VkDevice device) {
	if (m_farFencePending) {
		vkWaitForFences(device, 1, &m_farFence, VK_TRUE, UINT64_MAX);
		vkResetFences(device, 1, &m_farFence);
		m_farFencePending = false;
	}
}

bool VoxelResources::uploadFarFieldHalf(
		VkDevice device, VkPhysicalDevice physicalDevice, VkCommandPool commandPool,
		VkQueue queue, const std::vector<std::uint32_t>& cells,
		std::uint32_t half, std::string& outError) {
	if (m_farBuffer == VK_NULL_HANDLE || half >= kFarHalves) {
		outError = "Far LOD is not enabled on this resource set.";
		return false;
	}
	if (cells.size() != m_farCellsPerHalf) {
		outError = "Far field size does not match the far buffer.";
		return false;
	}
	if (!ensureFarUploadResources(device, physicalDevice, commandPool,
																outError)) {
		return false;
	}
	waitPreviousFarUpload(device);

	std::memcpy(m_farStagingMapped, cells.data(),
							cells.size() * sizeof(std::uint32_t));

	VkResult r = vkResetCommandBuffer(m_farCmd, 0);
	if (r != VK_SUCCESS) {
		outError = "Failed to reset far upload command buffer.";
		return false;
	}
	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	r = vkBeginCommandBuffer(m_farCmd, &begin);
	if (r != VK_SUCCESS) {
		outError = "Failed to begin far upload command buffer.";
		return false;
	}
	VkBufferCopy region{};
	region.dstOffset =
			static_cast<VkDeviceSize>(half) * m_farCellsPerHalf * 4u;
	region.size = static_cast<VkDeviceSize>(m_farCellsPerHalf) * 4u;
	vkCmdCopyBuffer(m_farCmd, m_farStaging, m_farBuffer, 1, &region);
	r = vkEndCommandBuffer(m_farCmd);
	if (r != VK_SUCCESS) {
		outError = "Failed to end far upload command buffer.";
		return false;
	}
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &m_farCmd;
	r = vkQueueSubmit(queue, 1, &submit, m_farFence);
	if (r != VK_SUCCESS) {
		outError = "Failed to submit far field upload.";
		return false;
	}
	// Wait for THIS copy so the caller can flip the half index immediately
	// (a frame reading the new half must not race the copy). This waits a
	// single 4 MB transfer, not the device.
	vkWaitForFences(device, 1, &m_farFence, VK_TRUE, UINT64_MAX);
	vkResetFences(device, 1, &m_farFence);
	return true;
}

bool VoxelResources::uploadFarFieldDelta(
		VkDevice device, VkPhysicalDevice physicalDevice, VkCommandPool commandPool,
		VkQueue queue, std::uint32_t half,
		const std::vector<std::pair<std::uint32_t, std::uint32_t>>& cellRuns,
		const std::vector<std::uint32_t>& values, std::string& outError) {
	if (m_farBuffer == VK_NULL_HANDLE || half >= kFarHalves) {
		outError = "Far LOD is not enabled on this resource set.";
		return false;
	}
	std::size_t total = 0;
	for (const auto& run : cellRuns) {
		total += run.second;
	}
	if (total == 0) {
		return true;
	}
	if (total != values.size() || total > m_farCellsPerHalf) {
		outError = "Invalid far delta upload ranges.";
		return false;
	}
	if (!ensureFarUploadResources(device, physicalDevice, commandPool,
																outError)) {
		return false;
	}
	waitPreviousFarUpload(device);

	std::size_t src = 0;
	auto* staging = static_cast<std::uint32_t*>(m_farStagingMapped);
	for (const auto& run : cellRuns) {
		std::memcpy(staging + src, values.data() + src,
								static_cast<std::size_t>(run.second) * sizeof(std::uint32_t));
		src += run.second;
	}

	VkResult r = vkResetCommandBuffer(m_farCmd, 0);
	if (r != VK_SUCCESS) {
		outError = "Failed to reset far upload command buffer.";
		return false;
	}
	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	r = vkBeginCommandBuffer(m_farCmd, &begin);
	if (r != VK_SUCCESS) {
		outError = "Failed to begin far upload command buffer.";
		return false;
	}
	std::vector<VkBufferCopy> regions;
	regions.reserve(cellRuns.size());
	src = 0;
	const VkDeviceSize base =
			static_cast<VkDeviceSize>(half) * m_farCellsPerHalf;
	for (const auto& run : cellRuns) {
		VkBufferCopy region{};
		region.srcOffset = static_cast<VkDeviceSize>(src) * 4u;
		region.dstOffset =
				(base + static_cast<VkDeviceSize>(run.first)) * 4u;
		region.size = static_cast<VkDeviceSize>(run.second) * 4u;
		regions.push_back(region);
		src += run.second;
	}
	vkCmdCopyBuffer(m_farCmd, m_farStaging, m_farBuffer,
									static_cast<std::uint32_t>(regions.size()), regions.data());
	r = vkEndCommandBuffer(m_farCmd);
	if (r != VK_SUCCESS) {
		outError = "Failed to end far upload command buffer.";
		return false;
	}
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &m_farCmd;
	r = vkQueueSubmit(queue, 1, &submit, m_farFence);
	if (r != VK_SUCCESS) {
		outError = "Failed to submit far delta upload.";
		return false;
	}
	m_farFencePending = true;  // async; the next far upload waits it
	return true;
}

bool VoxelResources::writeChunkTable(
		const std::vector<std::uint32_t>& slotPerCell, std::uint32_t half) {
	if (slotPerCell.size() != m_tableElements || m_mappedTable == nullptr ||
			half >= kTableHalves) {
		return false;
	}
	auto* dst = static_cast<std::uint32_t*>(m_mappedTable) +
							static_cast<std::size_t>(half) * m_tableElements;
	std::memcpy(dst, slotPerCell.data(),
							slotPerCell.size() * sizeof(std::uint32_t));
	return true;
}

void VoxelResources::cleanup(VkDevice device) {
	if (m_streamFence != VK_NULL_HANDLE) {
		vkDestroyFence(device, m_streamFence, nullptr);
		m_streamFence = VK_NULL_HANDLE;
	}
	if (m_streamCmd != VK_NULL_HANDLE && m_streamCommandPool != VK_NULL_HANDLE) {
		vkFreeCommandBuffers(device, m_streamCommandPool, 1, &m_streamCmd);
		m_streamCmd = VK_NULL_HANDLE;
	}
	m_streamCommandPool = VK_NULL_HANDLE;
	if (m_streamStagingMapped != nullptr) {
		vkUnmapMemory(device, m_streamStagingMemory);
		m_streamStagingMapped = nullptr;
	}
	if (m_streamStaging != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, m_streamStaging, nullptr);
		m_streamStaging = VK_NULL_HANDLE;
	}
	if (m_streamStagingMemory != VK_NULL_HANDLE) {
		vkFreeMemory(device, m_streamStagingMemory, nullptr);
		m_streamStagingMemory = VK_NULL_HANDLE;
	}
	m_streamFencePending = false;
	if (m_mappedTable != nullptr && m_chunkTableMemory != VK_NULL_HANDLE) {
		vkUnmapMemory(device, m_chunkTableMemory);
		m_mappedTable = nullptr;
	}
	if (m_heightBuffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, m_heightBuffer, nullptr);
		m_heightBuffer = VK_NULL_HANDLE;
	}
	if (m_heightMemory != VK_NULL_HANDLE) {
		vkFreeMemory(device, m_heightMemory, nullptr);
		m_heightMemory = VK_NULL_HANDLE;
	}
	if (m_farFence != VK_NULL_HANDLE) {
		vkDestroyFence(device, m_farFence, nullptr);
		m_farFence = VK_NULL_HANDLE;
	}
	if (m_farCmd != VK_NULL_HANDLE && m_farCommandPool != VK_NULL_HANDLE) {
		vkFreeCommandBuffers(device, m_farCommandPool, 1, &m_farCmd);
		m_farCmd = VK_NULL_HANDLE;
	}
	m_farCommandPool = VK_NULL_HANDLE;
	if (m_farStagingMapped != nullptr) {
		vkUnmapMemory(device, m_farStagingMemory);
		m_farStagingMapped = nullptr;
	}
	if (m_farStaging != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, m_farStaging, nullptr);
		m_farStaging = VK_NULL_HANDLE;
	}
	if (m_farStagingMemory != VK_NULL_HANDLE) {
		vkFreeMemory(device, m_farStagingMemory, nullptr);
		m_farStagingMemory = VK_NULL_HANDLE;
	}
	m_farFencePending = false;
	if (m_farBuffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, m_farBuffer, nullptr);
		m_farBuffer = VK_NULL_HANDLE;
	}
	if (m_farMemory != VK_NULL_HANDLE) {
		vkFreeMemory(device, m_farMemory, nullptr);
		m_farMemory = VK_NULL_HANDLE;
	}
	if (m_paletteBuffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, m_paletteBuffer, nullptr);
		m_paletteBuffer = VK_NULL_HANDLE;
	}
	if (m_paletteMemory != VK_NULL_HANDLE) {
		vkFreeMemory(device, m_paletteMemory, nullptr);
		m_paletteMemory = VK_NULL_HANDLE;
	}
	if (m_chunkTableBuffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, m_chunkTableBuffer, nullptr);
		m_chunkTableBuffer = VK_NULL_HANDLE;
	}
	if (m_chunkTableMemory != VK_NULL_HANDLE) {
		vkFreeMemory(device, m_chunkTableMemory, nullptr);
		m_chunkTableMemory = VK_NULL_HANDLE;
	}
	if (m_voxelBuffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, m_voxelBuffer, nullptr);
		m_voxelBuffer = VK_NULL_HANDLE;
	}
	if (m_voxelMemory != VK_NULL_HANDLE) {
		vkFreeMemory(device, m_voxelMemory, nullptr);
		m_voxelMemory = VK_NULL_HANDLE;
	}
	m_slotCount = 0;
	m_slotByteStride = 0;
	m_heightSlotWords = 0;
	m_tableElements = 0;
	m_farCellsPerHalf = 0;
}

}  // namespace vv::vulkan
