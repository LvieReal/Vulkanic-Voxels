#pragma once

#include <glm/glm.hpp>
#include <cstdint>

namespace vv::voxel {

// World layout configuration. The world is infinite along X and Z and is
// tiled by square chunks; each chunk spans the full world height (Y), so
// chunking exists on the X/Z axes only.
struct VoxelConfig final {
	std::uint32_t chunkSizeX = 32;
	std::uint32_t worldHeight = 128;
	std::uint32_t chunkSizeZ = 32;
	glm::vec3 voxelSize = glm::vec3(1.0f, 1.0f, 1.0f);

	// Radius in chunks around the camera that is generated and resident on
	// the GPU (region is (2r+1) x (2r+1) chunks).
	std::uint32_t renderRadiusChunks = 6;

	std::uint32_t terrainSeed = 1337;

	// Hard cap on DDA steps per pixel in the compute shader. Keeps worst-case
	// dispatch cost bounded (long air traversal is the expensive case); the
	// distance fog is nearly opaque well before this many steps, so the cap
	// is visually lossless.
	std::uint32_t maxTraceSteps = 512;

	std::uint32_t gridWidth() const { return 2 * renderRadiusChunks + 1; }
	std::uint32_t gridHeight() const { return 2 * renderRadiusChunks + 1; }

	// Total number of chunk slots in the GPU atlas (= region grid cells).
	std::uint64_t slotCount() const {
		return static_cast<std::uint64_t>(gridWidth()) * gridHeight();
	}

	bool isValid() const {
		return chunkSizeX > 0 && chunkSizeZ > 0 && worldHeight > 0 &&
					 voxelSize.x > 1e-6f && voxelSize.y > 1e-6f &&
					 voxelSize.z > 1e-6f && renderRadiusChunks >= 1 &&
					 renderRadiusChunks <= 16 && maxTraceSteps >= 16;
	}
};

}  // namespace vv::voxel
