#include "vulkan/VoxelResources.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "vulkan/BufferUtils.hpp"
#include "vulkan/VulkanUtils.hpp"
#include "voxel/VoxelTextures.hpp"
#include "voxel/VoxelTypes.hpp"

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

	// Per-slot fade-in alphas (binding 7; one float per atlas slot).
	// HOST_VISIBLE + COHERENT: the renderer rewrites the whole (tiny)
	// array every frame; the draw-frame barrier makes the writes visible
	// to the compute stage. Starts fully opaque (no fade anywhere).
	{
		const VkDeviceSize fadeBytes =
				static_cast<VkDeviceSize>(m_slotCount) * sizeof(float);
		if (!utils::createBuffer(device, physicalDevice, fadeBytes,
														 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
														 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
																 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
														 m_fadeBuffer, m_fadeMemory,
														 outError)) {
			cleanup(device);
			return false;
		}
		VkResult fr = vkMapMemory(device, m_fadeMemory, 0, VK_WHOLE_SIZE, 0,
															&m_mappedFade);
		if (fr != VK_SUCCESS || m_mappedFade == nullptr) {
			outError = "Failed to map the chunk fade buffer.";
			cleanup(device);
			return false;
		}
		std::vector<float> ones(m_slotCount, 1.0f);
		std::memcpy(m_mappedFade, ones.data(),
								static_cast<std::size_t>(fadeBytes));
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

	// Sun shadow field (binding 11): TWO packed grids back to back -
	// [shadow heights H][blocker distances D] (see SunShadowMap.hpp).
	// The renderer uploads the first (zeroed) field right after
	// create(), which doubles as the buffer's initialization -
	// device-local memory cannot be memset.
	m_sunShadowCols = config.gridWidth() * config.chunkSizeX;
	m_sunShadowRows = config.gridHeight() * config.chunkSizeZ;
	m_sunShadowWordsPerRow = m_sunShadowCols / 2u;
	const VkDeviceSize shadowBytes = static_cast<VkDeviceSize>(
			m_sunShadowWordsPerRow * m_sunShadowRows * 2u *
			sizeof(std::uint32_t));
	if ((m_sunShadowCols & 1u) != 0u || shadowBytes == 0) {
		outError = "Sun shadow field needs even column count.";
		cleanup(device);
		return false;
	}
	if (!utils::createBuffer(device, physicalDevice, shadowBytes,
													 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
														 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
													 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
													 m_sunShadowBuffer, m_sunShadowMemory,
													 outError)) {
		cleanup(device);
		return false;
	}

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

bool VoxelResources::createVoxelTextures(
		VkDevice device, VkPhysicalDevice physicalDevice, VkCommandPool commandPool,
		VkQueue queue, const std::vector<vv::voxel::VoxelTextureImage>& images,
		const std::vector<vv::voxel::VoxelTextureSet>& sets, std::string& outError) {
	if (!m_voxelTextureImages.empty()) {
		return true;  // idempotent
	}

	// Slot 0 is a 1x1 opaque-white dummy: it keeps the sampled-image
	// array valid even when no texture files exist. The shader never
	// samples it - untextured types carry the kNoFaceTexture sentinel
	// and use the palette colors. Caller indices shift by +1.
	std::vector<vv::voxel::VoxelTextureImage> all;
	all.reserve(images.size() + 1);
	vv::voxel::VoxelTextureImage dummy;
	dummy.width = 1;
	dummy.height = 1;
	dummy.rgba = {255, 255, 255, 255};
	all.push_back(std::move(dummy));
	all.insert(all.end(), images.begin(), images.end());

	const std::uint32_t count = static_cast<std::uint32_t>(all.size());
	std::vector<std::uint32_t> mips(count, 1);
	for (std::uint32_t i = 0; i < count; ++i) {
		const std::uint32_t largest = std::max(all[i].width, all[i].height);
		mips[i] = static_cast<std::uint32_t>(
								 std::log2(static_cast<float>(largest))) + 1u;
	}

	std::vector<VkImage> vkImages(count, VK_NULL_HANDLE);
	std::vector<VkDeviceMemory> vkMemory(count, VK_NULL_HANDLE);
	std::vector<VkImageView> views(count, VK_NULL_HANDLE);
	VkSampler sampler = VK_NULL_HANDLE;
	VkBuffer texInfoBuffer = VK_NULL_HANDLE;
	VkDeviceMemory texInfoMemory = VK_NULL_HANDLE;
	auto destroyAll = [&]() {
		for (VkImageView v : views) {
			if (v != VK_NULL_HANDLE) {
				vkDestroyImageView(device, v, nullptr);
			}
		}
		for (VkImage i : vkImages) {
			if (i != VK_NULL_HANDLE) {
				vkDestroyImage(device, i, nullptr);
			}
		}
		for (VkDeviceMemory m : vkMemory) {
			if (m != VK_NULL_HANDLE) {
				vkFreeMemory(device, m, nullptr);
			}
		}
		if (sampler != VK_NULL_HANDLE) {
			vkDestroySampler(device, sampler, nullptr);
		}
		if (texInfoBuffer != VK_NULL_HANDLE) {
			vkDestroyBuffer(device, texInfoBuffer, nullptr);
		}
		if (texInfoMemory != VK_NULL_HANDLE) {
			vkFreeMemory(device, texInfoMemory, nullptr);
		}
	};

	// Staging: every image's level-0 data in one host-visible buffer.
	std::size_t totalTexels = 0;
	for (const auto& img : all) {
		totalTexels += img.rgba.size();
	}
	std::vector<std::uint8_t> texels;
	texels.reserve(totalTexels);
	std::vector<VkDeviceSize> imageOffsets(count, 0);
	for (std::uint32_t i = 0; i < count; ++i) {
		imageOffsets[i] = static_cast<VkDeviceSize>(texels.size());
		texels.insert(texels.end(), all[i].rgba.begin(), all[i].rgba.end());
	}

	VkBuffer stagingBuffer = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
	if (!utils::createBuffer(device, physicalDevice,
													 static_cast<VkDeviceSize>(texels.size()),
													 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
													 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
															 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
													 stagingBuffer, stagingMemory, outError)) {
		return false;
	}
	void* mapped = nullptr;
	VkResult r = vkMapMemory(device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
	if (r != VK_SUCCESS || mapped == nullptr) {
		outError = "Failed to map voxel texture staging memory.";
		vkDestroyBuffer(device, stagingBuffer, nullptr);
		vkFreeMemory(device, stagingMemory, nullptr);
		return false;
	}
	std::memcpy(mapped, texels.data(), texels.size());
	vkUnmapMemory(device, stagingMemory);

	// Images (device-local, all mips allocated up front).
	for (std::uint32_t i = 0; i < count; ++i) {
		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		imageInfo.extent = {all[i].width, all[i].height, 1};
		imageInfo.mipLevels = mips[i];
		imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
											VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
											VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		r = vkCreateImage(device, &imageInfo, nullptr, &vkImages[i]);
		if (r != VK_SUCCESS) {
			outError = "Failed to create voxel texture image (" +
								 utils::vkResultToString(r) + ").";
			destroyAll();
			vkDestroyBuffer(device, stagingBuffer, nullptr);
			vkFreeMemory(device, stagingMemory, nullptr);
			return false;
		}
		VkMemoryRequirements req{};
		vkGetImageMemoryRequirements(device, vkImages[i], &req);
		VkMemoryAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.allocationSize = req.size;
		allocInfo.memoryTypeIndex = utils::findMemoryTypeIndex(
				physicalDevice, req.memoryTypeBits,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		r = vkAllocateMemory(device, &allocInfo, nullptr, &vkMemory[i]);
		if (r != VK_SUCCESS) {
			outError = "Failed to allocate voxel texture memory.";
			destroyAll();
			vkDestroyBuffer(device, stagingBuffer, nullptr);
			vkFreeMemory(device, stagingMemory, nullptr);
			return false;
		}
		vkBindImageMemory(device, vkImages[i], vkMemory[i], 0);
	}

	// One command buffer: upload level 0, blit the mip chain, transition
	// to shader-read (one-time submit + wait, like the initial uploads).
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkCommandBufferAllocateInfo alloc{};
	alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	alloc.commandPool = commandPool;
	alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	alloc.commandBufferCount = 1;
	r = vkAllocateCommandBuffers(device, &alloc, &cmd);
	if (r != VK_SUCCESS) {
		outError = "Failed to allocate voxel texture command buffer.";
		destroyAll();
		vkDestroyBuffer(device, stagingBuffer, nullptr);
		vkFreeMemory(device, stagingMemory, nullptr);
		return false;
	}
	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
		outError = "Failed to begin voxel texture command buffer.";
		vkFreeCommandBuffers(device, commandPool, 1, &cmd);
		destroyAll();
		vkDestroyBuffer(device, stagingBuffer, nullptr);
		vkFreeMemory(device, stagingMemory, nullptr);
		return false;
	}

	auto imageBarrier = [&](std::uint32_t image, std::uint32_t baseMip,
													std::uint32_t mipCount,
													VkImageLayout oldLayout,
													VkImageLayout newLayout,
													VkAccessFlags srcAccess,
													VkAccessFlags dstAccess) {
		VkImageMemoryBarrier b{};
		b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		b.srcAccessMask = srcAccess;
		b.dstAccessMask = dstAccess;
		b.oldLayout = oldLayout;
		b.newLayout = newLayout;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = vkImages[image];
		b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		b.subresourceRange.baseMipLevel = baseMip;
		b.subresourceRange.levelCount = mipCount;
		b.subresourceRange.baseArrayLayer = 0;
		b.subresourceRange.layerCount = 1;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
												 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
												 nullptr, 1, &b);
	};

	for (std::uint32_t i = 0; i < count; ++i) {
		imageBarrier(i, 0, mips[i], VK_IMAGE_LAYOUT_UNDEFINED,
								 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
								 VK_ACCESS_TRANSFER_WRITE_BIT);

		VkBufferImageCopy region{};
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount = 1;
		region.imageExtent = {all[i].width, all[i].height, 1};
		region.bufferOffset = imageOffsets[i];
		vkCmdCopyBufferToImage(cmd, stagingBuffer, vkImages[i],
													 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

		// Blit chain: each mip is downsampled from the one above it.
		// Dimensions floor at 1 so non-power-of-two sizes stay valid.
		for (std::uint32_t m = 1; m < mips[i]; ++m) {
			imageBarrier(i, m - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
									 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
									 VK_ACCESS_TRANSFER_WRITE_BIT,
									 VK_ACCESS_TRANSFER_READ_BIT);
			const auto srcW = static_cast<std::int32_t>(
					std::max<std::uint32_t>(all[i].width >> (m - 1), 1u));
			const auto srcH = static_cast<std::int32_t>(
					std::max<std::uint32_t>(all[i].height >> (m - 1), 1u));
			const auto dstW = static_cast<std::int32_t>(
					std::max<std::uint32_t>(all[i].width >> m, 1u));
			const auto dstH = static_cast<std::int32_t>(
					std::max<std::uint32_t>(all[i].height >> m, 1u));
			VkImageBlit blit{};
			blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			blit.srcSubresource.mipLevel = m - 1;
			blit.srcSubresource.baseArrayLayer = 0;
			blit.srcSubresource.layerCount = 1;
			blit.srcOffsets[0] = {0, 0, 0};
			blit.srcOffsets[1] = {srcW, srcH, 1};
			blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			blit.dstSubresource.mipLevel = m;
			blit.dstSubresource.baseArrayLayer = 0;
			blit.dstSubresource.layerCount = 1;
			blit.dstOffsets[0] = {0, 0, 0};
			blit.dstOffsets[1] = {dstW, dstH, 1};
			vkCmdBlitImage(cmd, vkImages[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
										 vkImages[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
										 VK_FILTER_LINEAR);
		}

		// Final transition: mips 0..mips-2 come back from TRANSFER_SRC, the
		// deepest one from TRANSFER_DST (a single barrier covers mips == 1).
		if (mips[i] > 1) {
			VkImageMemoryBarrier b[2]{};
			b[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			b[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			b[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			b[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b[0].image = vkImages[i];
			b[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			b[0].subresourceRange.baseMipLevel = 0;
			b[0].subresourceRange.levelCount = mips[i] - 1;
			b[0].subresourceRange.baseArrayLayer = 0;
			b[0].subresourceRange.layerCount = 1;
			b[1] = b[0];
			b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b[1].subresourceRange.baseMipLevel = mips[i] - 1;
			b[1].subresourceRange.levelCount = 1;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
													 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
													 0, nullptr, 2, b);
		} else {
			imageBarrier(i, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
									 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
									 VK_ACCESS_TRANSFER_WRITE_BIT,
									 VK_ACCESS_SHADER_READ_BIT);
			// imageBarrier pipelines TOP_OF_PIPE -> TRANSFER; the final
			// visibility to compute is guaranteed by the queue idle below.
		}
	}

	if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
		outError = "Failed to end voxel texture command buffer.";
		vkFreeCommandBuffers(device, commandPool, 1, &cmd);
		destroyAll();
		vkDestroyBuffer(device, stagingBuffer, nullptr);
		vkFreeMemory(device, stagingMemory, nullptr);
		return false;
	}

	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	r = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
	if (r == VK_SUCCESS && vkQueueWaitIdle(queue) != VK_SUCCESS) {
		r = VK_ERROR_DEVICE_LOST;
	}
	vkFreeCommandBuffers(device, commandPool, 1, &cmd);
	vkDestroyBuffer(device, stagingBuffer, nullptr);
	vkFreeMemory(device, stagingMemory, nullptr);
	if (r != VK_SUCCESS) {
		outError = "Failed to submit voxel texture uploads.";
		destroyAll();
		return false;
	}

	// Views + one shared sampler (REPEAT tiling per voxel; NEAREST texel
	// sampling by default - the crisp voxel look; mips blend linearly).
	// The shader passes an explicit LOD - ray divergence makes
	// derivatives useless in a marcher.
	for (std::uint32_t i = 0; i < count; ++i) {
		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = vkImages[i];
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = mips[i];
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = 1;
		r = vkCreateImageView(device, &viewInfo, nullptr, &views[i]);
		if (r != VK_SUCCESS) {
			outError = "Failed to create voxel texture view.";
			destroyAll();
			return false;
		}
	}

	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_NEAREST;
	samplerInfo.minFilter = VK_FILTER_NEAREST;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.minLod = 0.0f;
	samplerInfo.maxLod = 16.0f;
	r = vkCreateSampler(device, &samplerInfo, nullptr, &sampler);
	if (r != VK_SUCCESS) {
		outError = "Failed to create voxel texture sampler.";
		destroyAll();
		return false;
	}

	// Per-type face table (binding 10): 8 u32 per type. Face image
	// indices shift by +1 (the white dummy occupies 0); kNoFaceTexture
	// stays as the plain-color sentinel. Word 6 = nominal size.
	{
		const VkDeviceSize texInfoBytes =
				static_cast<VkDeviceSize>(vv::voxel::kVoxelTypeCount) * 8u * 4u;
		if (!utils::createBuffer(device, physicalDevice, texInfoBytes,
														 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
														 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
																 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
														 texInfoBuffer, texInfoMemory, outError)) {
			destroyAll();
			return false;
		}
		void* infoMapped = nullptr;
		r = vkMapMemory(device, texInfoMemory, 0, VK_WHOLE_SIZE, 0, &infoMapped);
		if (r != VK_SUCCESS || infoMapped == nullptr) {
			outError = "Failed to map the voxel texture info buffer.";
			destroyAll();
			return false;
		}
		std::vector<std::uint32_t> words(
				static_cast<std::size_t>(vv::voxel::kVoxelTypeCount) * 8u, 0u);
		for (std::uint32_t t = 0; t < vv::voxel::kVoxelTypeCount; ++t) {
			const vv::voxel::VoxelTextureSet plain{};
			const vv::voxel::VoxelTextureSet& set =
					t < sets.size() ? sets[t] : plain;
			for (std::uint32_t f = 0; f < 6; ++f) {
				words[static_cast<std::size_t>(t) * 8u + f] =
						(set.textured &&
						 set.faceIndex[f] != vv::voxel::kNoFaceTexture)
								? set.faceIndex[f] + 1u
								: vv::voxel::kNoFaceTexture;
			}
			words[static_cast<std::size_t>(t) * 8u + 6u] =
					set.textured ? set.nominalSize : 32u;
		}
		std::memcpy(infoMapped, words.data(), words.size() * 4u);
		vkUnmapMemory(device, texInfoMemory);
	}

	m_voxelTextureImages = std::move(vkImages);
	m_voxelTextureMemory = std::move(vkMemory);
	m_voxelTextureViews = std::move(views);
	m_voxelSampler = sampler;
	m_texInfoBuffer = texInfoBuffer;
	m_texInfoMemory = texInfoMemory;
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

	// Lazily create BOTH sets of the double-buffered upload resources
	// (staging + command buffer + fence each): uploads alternate so the
	// pre-write wait targets a submit TWO uploads old - always retired.
	if (m_streamStaging[0] == VK_NULL_HANDLE) {
		const VkDeviceSize bytes = static_cast<VkDeviceSize>(m_slotByteStride) +
				static_cast<VkDeviceSize>(m_heightSlotWords) * 4u;
		for (std::uint32_t k = 0; k < 2; ++k) {
			if (!utils::createBuffer(device, physicalDevice, bytes,
														 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
														 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
																 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
														 m_streamStaging[k],
														 m_streamStagingMemory[k], outError)) {
				return false;
			}
			VkResult r = vkMapMemory(device, m_streamStagingMemory[k], 0,
														VK_WHOLE_SIZE, 0, &m_streamStagingMapped[k]);
			if (r != VK_SUCCESS || m_streamStagingMapped[k] == nullptr) {
				outError = "Failed to map streaming staging memory.";
				return false;
			}
			VkCommandBufferAllocateInfo alloc{};
			alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			alloc.commandPool = commandPool;
			alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			alloc.commandBufferCount = 1;
			r = vkAllocateCommandBuffers(device, &alloc, &m_streamCmd[k]);
			if (r != VK_SUCCESS) {
				outError = "Failed to allocate streaming command buffer.";
				return false;
			}
			VkFenceCreateInfo fence{};
			fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
			fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			r = vkCreateFence(device, &fence, nullptr, &m_streamFence[k]);
			if (r != VK_SUCCESS) {
				outError = "Failed to create streaming upload fence.";
				return false;
			}
		}
		m_streamCommandPool = commandPool;
	}

	// Use the slot the previous upload did NOT use; wait its fence only if
	// pending (that submit is two uploads old - long retired, so this
	// normally costs nothing).
	const std::uint32_t idx = m_streamParity ^ 1u;
	if (m_streamFencePending[idx]) {
		vkWaitForFences(device, 1, &m_streamFence[idx], VK_TRUE, UINT64_MAX);
		vkResetFences(device, 1, &m_streamFence[idx]);
		m_streamFencePending[idx] = false;
	}

	// Staging layout: [voxel bytes][heightmap words].
	const auto& types = upload.chunk->voxelTypes();
	std::memcpy(m_streamStagingMapped[idx], types.data(), types.size());
	const auto& heights = upload.chunk->heightMapWords();
	std::memcpy(static_cast<std::uint8_t*>(m_streamStagingMapped[idx]) +
								static_cast<std::size_t>(m_slotByteStride),
					heights.data(), heights.size() * sizeof(std::uint32_t));

	VkResult r = vkResetCommandBuffer(m_streamCmd[idx], 0);
	if (r != VK_SUCCESS) {
		outError = "Failed to reset streaming command buffer.";
		return false;
	}
	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	r = vkBeginCommandBuffer(m_streamCmd[idx], &begin);
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
	vkCmdCopyBuffer(m_streamCmd[idx], m_streamStaging[idx], m_voxelBuffer, 1,
								&voxelRegion);
	VkBufferCopy heightRegion{};
	heightRegion.srcOffset = static_cast<VkDeviceSize>(m_slotByteStride);
	heightRegion.size = static_cast<VkDeviceSize>(m_heightSlotWords) * 4u;
	heightRegion.dstOffset = static_cast<VkDeviceSize>(upload.slot) *
														static_cast<VkDeviceSize>(m_heightSlotWords) * 4u;
	vkCmdCopyBuffer(m_streamCmd[idx], m_streamStaging[idx], m_heightBuffer, 1,
								&heightRegion);
	r = vkEndCommandBuffer(m_streamCmd[idx]);
	if (r != VK_SUCCESS) {
		outError = "Failed to end streaming command buffer.";
		return false;
	}

	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &m_streamCmd[idx];
	r = vkQueueSubmit(queue, 1, &submit, m_streamFence[idx]);
	if (r != VK_SUCCESS) {
		outError = "Failed to submit streaming chunk upload.";
		return false;
	}
	m_streamFencePending[idx] = true;
	m_streamParity = idx;
	return true;
}

bool VoxelResources::uploadSunShadowField(
		VkDevice device, VkPhysicalDevice physicalDevice, VkCommandPool commandPool,
		VkQueue queue, const std::vector<std::uint32_t>& packed,
		std::string& outError) {
	const VkDeviceSize bytes = static_cast<VkDeviceSize>(
			m_sunShadowWordsPerRow * m_sunShadowRows * 2u *
			sizeof(std::uint32_t));
	if (m_sunShadowBuffer == VK_NULL_HANDLE || bytes == 0 ||
			packed.size() * sizeof(std::uint32_t) != static_cast<std::size_t>(bytes)) {
		outError = "Sun shadow field size mismatch.";
		return false;
	}
	if (m_sunShadowStaging == VK_NULL_HANDLE) {
		if (!utils::createBuffer(device, physicalDevice, bytes,
														 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
														 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
															 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
														 m_sunShadowStaging,
														 m_sunShadowStagingMemory, outError)) {
			return false;
		}
		VkResult r = vkMapMemory(device, m_sunShadowStagingMemory, 0,
														 VK_WHOLE_SIZE, 0,
														 &m_sunShadowStagingMapped);
		if (r != VK_SUCCESS || m_sunShadowStagingMapped == nullptr) {
			outError = "Failed to map sun shadow staging memory.";
			return false;
		}
		VkCommandBufferAllocateInfo alloc{};
		alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		alloc.commandPool = commandPool;
		alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		alloc.commandBufferCount = 1;
		r = vkAllocateCommandBuffers(device, &alloc, &m_sunShadowCmd);
		if (r != VK_SUCCESS) {
			outError = "Failed to allocate sun shadow command buffer.";
			return false;
		}
		VkFenceCreateInfo fence{};
		fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		r = vkCreateFence(device, &fence, nullptr, &m_sunShadowFence);
		if (r != VK_SUCCESS) {
			outError = "Failed to create sun shadow upload fence.";
			return false;
		}
	}
	if (m_sunShadowFencePending) {
		vkWaitForFences(device, 1, &m_sunShadowFence, VK_TRUE, UINT64_MAX);
		vkResetFences(device, 1, &m_sunShadowFence);
		m_sunShadowFencePending = false;
	}
	std::memcpy(m_sunShadowStagingMapped, packed.data(),
							static_cast<std::size_t>(bytes));

	VkResult r = vkResetCommandBuffer(m_sunShadowCmd, 0);
	if (r != VK_SUCCESS) {
		outError = "Failed to reset sun shadow command buffer.";
		return false;
	}
	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	r = vkBeginCommandBuffer(m_sunShadowCmd, &begin);
	if (r != VK_SUCCESS) {
		outError = "Failed to begin sun shadow command buffer.";
		return false;
	}
	VkBufferCopy region{};
	region.size = bytes;
	vkCmdCopyBuffer(m_sunShadowCmd, m_sunShadowStaging, m_sunShadowBuffer, 1,
									&region);
	r = vkEndCommandBuffer(m_sunShadowCmd);
	if (r != VK_SUCCESS) {
		outError = "Failed to end sun shadow command buffer.";
		return false;
	}
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &m_sunShadowCmd;
	r = vkQueueSubmit(queue, 1, &submit, m_sunShadowFence);
	if (r != VK_SUCCESS) {
		outError = "Failed to submit sun shadow field upload.";
		return false;
	}
	m_sunShadowFencePending = true;
	return true;
}

void VoxelResources::waitStreamingUploadIdle(VkDevice device) {
	for (std::uint32_t k = 0; k < 2; ++k) {
		if (m_streamFencePending[k]) {
			vkWaitForFences(device, 1, &m_streamFence[k], VK_TRUE, UINT64_MAX);
			vkResetFences(device, 1, &m_streamFence[k]);
			m_streamFencePending[k] = false;
		}
	}
}

bool VoxelResources::ensureFarUploadResources(
		VkDevice device, VkPhysicalDevice physicalDevice, VkCommandPool commandPool,
		std::string& outError) {
	if (m_farStaging[0] != VK_NULL_HANDLE) {
		return true;
	}
	const VkDeviceSize bytes =
			static_cast<VkDeviceSize>(m_farCellsPerHalf) * 4u;
	if (bytes == 0) {
		outError = "Far LOD is not enabled on this resource set.";
		return false;
	}
	for (std::uint32_t k = 0; k < 2; ++k) {
		if (!utils::createBuffer(device, physicalDevice, bytes,
														 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
														 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
																 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
														 m_farStaging[k], m_farStagingMemory[k],
														 outError)) {
			return false;
		}
		VkResult r = vkMapMemory(device, m_farStagingMemory[k], 0, VK_WHOLE_SIZE, 0,
														 &m_farStagingMapped[k]);
		if (r != VK_SUCCESS || m_farStagingMapped[k] == nullptr) {
			outError = "Failed to map far staging memory.";
			return false;
		}
		VkCommandBufferAllocateInfo alloc{};
		alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		alloc.commandPool = commandPool;
		alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		alloc.commandBufferCount = 1;
		r = vkAllocateCommandBuffers(device, &alloc, &m_farCmd[k]);
		if (r != VK_SUCCESS) {
			outError = "Failed to allocate far upload command buffer.";
			return false;
		}
		VkFenceCreateInfo fence{};
		fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		r = vkCreateFence(device, &fence, nullptr, &m_farFence[k]);
		if (r != VK_SUCCESS) {
			outError = "Failed to create far upload fence.";
			return false;
		}
	}
	m_farCommandPool = commandPool;
	return true;
}

void VoxelResources::waitPreviousFarUpload(VkDevice device) {
	for (std::uint32_t k = 0; k < 2; ++k) {
		if (m_farFencePending[k]) {
			vkWaitForFences(device, 1, &m_farFence[k], VK_TRUE, UINT64_MAX);
			vkResetFences(device, 1, &m_farFence[k]);
			m_farFencePending[k] = false;
		}
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

	const std::uint32_t idx = m_farParity ^ 1u;
	std::memcpy(m_farStagingMapped[idx], cells.data(),
							cells.size() * sizeof(std::uint32_t));

	VkResult r = vkResetCommandBuffer(m_farCmd[idx], 0);
	if (r != VK_SUCCESS) {
		outError = "Failed to reset far upload command buffer.";
		return false;
	}
	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	r = vkBeginCommandBuffer(m_farCmd[idx], &begin);
	if (r != VK_SUCCESS) {
		outError = "Failed to begin far upload command buffer.";
		return false;
	}
	VkBufferCopy region{};
	region.dstOffset =
			static_cast<VkDeviceSize>(half) * m_farCellsPerHalf * 4u;
	region.size = static_cast<VkDeviceSize>(m_farCellsPerHalf) * 4u;
	vkCmdCopyBuffer(m_farCmd[idx], m_farStaging[idx], m_farBuffer, 1, &region);
	r = vkEndCommandBuffer(m_farCmd[idx]);
	if (r != VK_SUCCESS) {
		outError = "Failed to end far upload command buffer.";
		return false;
	}
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &m_farCmd[idx];
	r = vkQueueSubmit(queue, 1, &submit, m_farFence[idx]);
	if (r != VK_SUCCESS) {
		outError = "Failed to submit far field upload.";
		return false;
	}
	// Wait for THIS copy so the caller can flip the half index immediately
	// (a frame reading the new half must not race the copy). This waits a
	// single 4 MB transfer, not the device.
	vkWaitForFences(device, 1, &m_farFence[idx], VK_TRUE, UINT64_MAX);
	vkResetFences(device, 1, &m_farFence[idx]);
	m_farParity = idx;
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
	// Use the slot the previous far upload did NOT use; wait only its
	// fence (that submit is two uploads old - retired). The in-flight
	// previous upload reads the OTHER staging buffer, so this write is
	// safe without waiting it.
	const std::uint32_t idx = m_farParity ^ 1u;
	if (m_farFencePending[idx]) {
		vkWaitForFences(device, 1, &m_farFence[idx], VK_TRUE, UINT64_MAX);
		vkResetFences(device, 1, &m_farFence[idx]);
		m_farFencePending[idx] = false;
	}

	std::size_t src = 0;
	auto* staging = static_cast<std::uint32_t*>(m_farStagingMapped[idx]);
	for (const auto& run : cellRuns) {
		std::memcpy(staging + src, values.data() + src,
								static_cast<std::size_t>(run.second) * sizeof(std::uint32_t));
		src += run.second;
	}

	VkResult r = vkResetCommandBuffer(m_farCmd[idx], 0);
	if (r != VK_SUCCESS) {
		outError = "Failed to reset far upload command buffer.";
		return false;
	}
	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	r = vkBeginCommandBuffer(m_farCmd[idx], &begin);
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
	vkCmdCopyBuffer(m_farCmd[idx], m_farStaging[idx], m_farBuffer,
									static_cast<std::uint32_t>(regions.size()), regions.data());
	r = vkEndCommandBuffer(m_farCmd[idx]);
	if (r != VK_SUCCESS) {
		outError = "Failed to end far upload command buffer.";
		return false;
	}
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &m_farCmd[idx];
	r = vkQueueSubmit(queue, 1, &submit, m_farFence[idx]);
	if (r != VK_SUCCESS) {
		outError = "Failed to submit far delta upload.";
		return false;
	}
	m_farFencePending[idx] = true;  // async; the next far upload waits it
	m_farParity = idx;
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

void VoxelResources::writeChunkFade(const std::vector<float>& alphas) {
	if (m_mappedFade == nullptr ||
			alphas.size() != static_cast<std::size_t>(m_slotCount)) {
		return;
	}
	std::memcpy(m_mappedFade, alphas.data(), alphas.size() * sizeof(float));
}

void VoxelResources::cleanup(VkDevice device) {
	for (std::uint32_t k = 0; k < 2; ++k) {
		if (m_streamFence[k] != VK_NULL_HANDLE) {
			vkDestroyFence(device, m_streamFence[k], nullptr);
			m_streamFence[k] = VK_NULL_HANDLE;
		}
		if (m_streamCmd[k] != VK_NULL_HANDLE &&
				m_streamCommandPool != VK_NULL_HANDLE) {
			vkFreeCommandBuffers(device, m_streamCommandPool, 1, &m_streamCmd[k]);
			m_streamCmd[k] = VK_NULL_HANDLE;
		}
		if (m_streamStagingMapped[k] != nullptr) {
			vkUnmapMemory(device, m_streamStagingMemory[k]);
			m_streamStagingMapped[k] = nullptr;
		}
		if (m_streamStaging[k] != VK_NULL_HANDLE) {
			vkDestroyBuffer(device, m_streamStaging[k], nullptr);
			m_streamStaging[k] = VK_NULL_HANDLE;
		}
		if (m_streamStagingMemory[k] != VK_NULL_HANDLE) {
			vkFreeMemory(device, m_streamStagingMemory[k], nullptr);
			m_streamStagingMemory[k] = VK_NULL_HANDLE;
		}
		m_streamFencePending[k] = false;
	}
	m_streamCommandPool = VK_NULL_HANDLE;

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
	if (m_mappedFade != nullptr) {
		vkUnmapMemory(device, m_fadeMemory);
		m_mappedFade = nullptr;
	}
	if (m_fadeBuffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, m_fadeBuffer, nullptr);
		m_fadeBuffer = VK_NULL_HANDLE;
	}
	if (m_fadeMemory != VK_NULL_HANDLE) {
		vkFreeMemory(device, m_fadeMemory, nullptr);
		m_fadeMemory = VK_NULL_HANDLE;
	}
	for (VkImageView v : m_voxelTextureViews) {
		if (v != VK_NULL_HANDLE) {
			vkDestroyImageView(device, v, nullptr);
		}
	}
	m_voxelTextureViews.clear();
	for (VkImage i : m_voxelTextureImages) {
		if (i != VK_NULL_HANDLE) {
			vkDestroyImage(device, i, nullptr);
		}
	}
	m_voxelTextureImages.clear();
	for (VkDeviceMemory m : m_voxelTextureMemory) {
		if (m != VK_NULL_HANDLE) {
			vkFreeMemory(device, m, nullptr);
		}
	}
	m_voxelTextureMemory.clear();
	if (m_voxelSampler != VK_NULL_HANDLE) {
		vkDestroySampler(device, m_voxelSampler, nullptr);
		m_voxelSampler = VK_NULL_HANDLE;
	}
	if (m_texInfoBuffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, m_texInfoBuffer, nullptr);
		m_texInfoBuffer = VK_NULL_HANDLE;
	}
	if (m_texInfoMemory != VK_NULL_HANDLE) {
		vkFreeMemory(device, m_texInfoMemory, nullptr);
		m_texInfoMemory = VK_NULL_HANDLE;
	}
	for (std::uint32_t k = 0; k < 2; ++k) {
		if (m_farFence[k] != VK_NULL_HANDLE) {
			vkDestroyFence(device, m_farFence[k], nullptr);
			m_farFence[k] = VK_NULL_HANDLE;
		}
		if (m_farCmd[k] != VK_NULL_HANDLE && m_farCommandPool != VK_NULL_HANDLE) {
			vkFreeCommandBuffers(device, m_farCommandPool, 1, &m_farCmd[k]);
			m_farCmd[k] = VK_NULL_HANDLE;
		}
		if (m_farStagingMapped[k] != nullptr) {
			vkUnmapMemory(device, m_farStagingMemory[k]);
			m_farStagingMapped[k] = nullptr;
		}
		if (m_farStaging[k] != VK_NULL_HANDLE) {
			vkDestroyBuffer(device, m_farStaging[k], nullptr);
			m_farStaging[k] = VK_NULL_HANDLE;
		}
		if (m_farStagingMemory[k] != VK_NULL_HANDLE) {
			vkFreeMemory(device, m_farStagingMemory[k], nullptr);
			m_farStagingMemory[k] = VK_NULL_HANDLE;
		}
		m_farFencePending[k] = false;
	}
	m_farCommandPool = VK_NULL_HANDLE;

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
	if (m_sunShadowFence != VK_NULL_HANDLE) {
		vkDestroyFence(device, m_sunShadowFence, nullptr);
		m_sunShadowFence = VK_NULL_HANDLE;
		m_sunShadowFencePending = false;
	}
	if (m_sunShadowStagingMapped != nullptr) {
		vkUnmapMemory(device, m_sunShadowStagingMemory);
		m_sunShadowStagingMapped = nullptr;
	}
	if (m_sunShadowStaging != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, m_sunShadowStaging, nullptr);
		m_sunShadowStaging = VK_NULL_HANDLE;
	}
	if (m_sunShadowStagingMemory != VK_NULL_HANDLE) {
		vkFreeMemory(device, m_sunShadowStagingMemory, nullptr);
		m_sunShadowStagingMemory = VK_NULL_HANDLE;
	}
	if (m_sunShadowBuffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, m_sunShadowBuffer, nullptr);
		m_sunShadowBuffer = VK_NULL_HANDLE;
	}
	if (m_sunShadowMemory != VK_NULL_HANDLE) {
		vkFreeMemory(device, m_sunShadowMemory, nullptr);
		m_sunShadowMemory = VK_NULL_HANDLE;
	}
	m_slotCount = 0;
	m_slotByteStride = 0;
	m_heightSlotWords = 0;
	m_tableElements = 0;
	m_farCellsPerHalf = 0;
}

}  // namespace vv::vulkan
