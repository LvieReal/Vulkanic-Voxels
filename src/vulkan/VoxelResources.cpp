#include "vulkan/VoxelResources.hpp"

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

	const VkDeviceSize tableBytes =
			static_cast<VkDeviceSize>(m_tableElements) * sizeof(std::uint32_t);
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
	// Layout mirrors the shader's VoxelPalette block: three vec4 arrays of
	// kPaletteCapacity entries (top, side, bottom), entry order = VoxelType
	// enum order. Placeholder flat-color palette; the texturing pass will
	// replace it with a bindless texture array of real per-type albedo
	// textures.
	constexpr std::size_t kFloatsPerVec4 = 4;
	constexpr std::size_t kFaces = 3;
	std::vector<float> palette(kFaces * kPaletteCapacity * kFloatsPerVec4, 0.0f);

	const auto writeFace = [&](std::size_t face, const float* source) {
		for (std::uint32_t type = 0; type < vv::voxel::kVoxelTypeCount &&
																	type < kPaletteCapacity;
				 ++type) {
			const std::size_t dst =
					(face * kPaletteCapacity + type) * kFloatsPerVec4;
			for (std::size_t c = 0; c < 3; ++c) {
				palette[dst + c] = source[type * 3 + c];
			}
			palette[dst + 3] = 1.0f;
		}
	};
	writeFace(0, &vv::voxel::kVoxelTypeInfo[0].top[0]);
	writeFace(1, &vv::voxel::kVoxelTypeInfo[0].side[0]);
	writeFace(2, &vv::voxel::kVoxelTypeInfo[0].bottom[0]);

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
		totalBytes += static_cast<VkDeviceSize>(m_slotByteStride);
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
		std::size_t offset = 0;
		for (const ChunkUpload& upload : uploads) {
			const auto& types = upload.chunk->voxelTypes();
			std::memcpy(static_cast<std::uint8_t*>(mapped) + offset, types.data(),
									types.size());
			offset += static_cast<std::size_t>(m_slotByteStride);
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

	std::vector<VkBufferCopy> regions;
	regions.reserve(uploads.size());
	{
		VkDeviceSize stagingOffset = 0;
		for (const ChunkUpload& upload : uploads) {
			VkBufferCopy region{};
			region.srcOffset = stagingOffset;
			region.dstOffset =
					static_cast<VkDeviceSize>(upload.slot) * m_slotByteStride;
			region.size = static_cast<VkDeviceSize>(m_slotByteStride);
			regions.push_back(region);
			stagingOffset += static_cast<VkDeviceSize>(m_slotByteStride);
		}
	}
	vkCmdCopyBuffer(cmd, stagingBuffer, m_voxelBuffer,
									static_cast<std::uint32_t>(regions.size()), regions.data());

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

bool VoxelResources::writeChunkTable(
		const std::vector<std::uint32_t>& slotPerCell) {
	if (slotPerCell.size() != m_tableElements || m_mappedTable == nullptr) {
		return false;
	}
	std::memcpy(m_mappedTable, slotPerCell.data(),
							slotPerCell.size() * sizeof(std::uint32_t));
	return true;
}

void VoxelResources::cleanup(VkDevice device) {
	if (m_mappedTable != nullptr && m_chunkTableMemory != VK_NULL_HANDLE) {
		vkUnmapMemory(device, m_chunkTableMemory);
		m_mappedTable = nullptr;
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
	m_tableElements = 0;
}

}  // namespace vv::vulkan
