#pragma once

#include <glm/glm.hpp>
#include <cstdint>

namespace vv::render {

// Push constants sent to the compute shader per dispatch.
// Layout must match the Push block in resources/shaders/pixels_rgba.comp.
struct PushConstants final {
	glm::uvec4 screen{};    // x=width, y=height, z=bgra, w=frame
	glm::vec4 camera{};     // x=tanHalfFov, y=fogDensity
	glm::uvec4 chunkSize{}; // x=chunkX, y=worldHeight, z=chunkZ, w=maxTraceSteps
	glm::vec4 voxelSize{};  // xyz=voxel size in world units
	glm::ivec4 region{};    // x,z = region origin (min corner) in chunk coords
	glm::uvec4 grid{};      // x=gridWidth, y=gridHeight, z=slot stride (words), w=max terrain voxel y (sky-skip)
};

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
	glm::vec4 misc{}; // x = timeSeconds
};

}  // namespace vv::render
