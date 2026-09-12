#pragma once

#include <glm/glm.hpp>
#include <cstdint>

namespace vv::render {

// Push constants sent to the compute shader per dispatch.
// Layout must match the Push block in resources/shaders/pixels_rgba.comp.
struct PushConstants final {
	glm::uvec4 screen{};    // x=width, y=height, z=bgra, w=frame
	glm::vec4 camera{};     // x=tanHalfFov, y=fogDensity (= 1 / fogCutDistance; see VulkanRenderer::fogCutDistance)
	glm::uvec4 chunkSize{}; // x=chunkX, y=worldHeight, z=chunkZ, w=maxTraceSteps
	glm::vec4 voxelSize{};  // xyz=voxel size in world units
	glm::ivec4 region{};    // x,z = region origin (min corner) in chunk coords
	glm::uvec4 grid{};      // x=gridWidth, y=gridHeight, z=slot stride (words), w=max terrain voxel y (sky-skip)
	glm::ivec4 far{};       // x,y = far-LOD field min corner (voxel X/Z), z,w = cell dims (z=0 = far LOD off)
	glm::vec4 farParams{};  // x = far cell footprint in voxels
};

// The Vulkan spec guarantees at least 128 bytes of push constants; this
// layout must also match the Push block in pixels_rgba.comp exactly.
static_assert(sizeof(PushConstants) == 128, "push constant layout grew");

// Uniform buffer updated each frame with camera and lighting.
struct SceneUBO final {
	glm::vec4 camPos{};
	glm::vec4 camForward{};
	glm::vec4 camRight{};
	glm::vec4 camUp{};
	glm::vec4 lightDir{};
	glm::vec4 lightColor{};
	glm::vec4 skyLow{};
	glm::vec4 skyHigh{};
	glm::vec4 misc{}; // x = timeSeconds, y = VV_DEBUG_TERM, z = VV_DEBUG_SSAA
	// TAA (pass 6). prevCam* is the camera that rendered the history the
	// current frame reads (2 frames old with 2 frames in flight - see the
	// renderer's 4-deep history ring). Layout must match the Scene block
	// in pixels_rgba.comp.
	glm::vec4 prevCamPos{};
	glm::vec4 prevCamForward{};
	glm::vec4 prevCamRight{};
	glm::vec4 prevCamUp{};
	glm::vec4 taa{};       // x = enabled, y = historyValid (0 = reset/first frames)
	glm::vec4 taaJitter{}; // xy = current frame jitter (pixels), zw = history frame jitter
};

// TAA inputs the renderer derives from its history ring each frame
// (defaults: TAA off). Consumed by SceneUniform::update.
struct TaaData final {
	glm::vec4 prevCamPos{};
	glm::vec4 prevCamForward{};
	glm::vec4 prevCamRight{};
	glm::vec4 prevCamUp{};
	glm::vec4 taa{};       // x = enabled, y = historyValid
	glm::vec4 taaJitter{}; // xy = current, zw = history jitter
};

}  // namespace vv::render
