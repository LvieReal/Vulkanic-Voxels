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
	// the GPU (region is (2r+1) x (2r+1) chunks). Doubled in pass 3.5 for
	// 2x view distance: the fog cut, step budget and atlas slot count all
	// scale automatically (fog cut = [r, r+1] chunk extents from the camera
	// to the nearest region face). Startup generates (2r+1)^2 chunks
	// (~0.7 s at r=12); each border crossing streams 2(2r+1)-1 new chunks
	// (~50 ms hitch at r=12) - the LOD pass will make streaming incremental.
	std::uint32_t renderRadiusChunks = 12;

	// Far-LOD field: coarse height columns (one u32 per 4x4-voxel cell:
	// u16 max-solid height + u8 surface type) covering a square of
	// (2 * farLodRadiusChunks) chunks per side, centered near the camera
	// and rebuilt on a background thread when the camera strays too far.
	// The shader traces it after a ray leaves the full-detail region, so
	// the visible world extends to the far field boundary (~2k units at
	// radius 64) while memory stays ~4 MB instead of the ~1.25 GB that
	// full-detail chunks would need. 0 disables far LOD (fog then ends at
	// the near-region boundary as before).
	std::uint32_t farLodRadiusChunks = 64;
	// Far cell footprint in voxels (must divide 2*farLodRadiusChunks*
	// chunkSizeX; 4 = ~1-3 voxel silhouette quantization at distance).
	std::uint32_t farLodCellVoxels = 4;

	std::uint32_t terrainSeed = 1337;

	// Safety net on DDA iterations per pixel. The primary ray terminator is
	// the fog distance cut (see VulkanRenderer::fogCutDistance and the
	// shader's fogCut): the renderer raises this to at least ~1.75x the
	// region width so the budget never cuts a ray before the fog does.
	// Since the heightmap-guided traversal this counts COLUMN steps (the
	// worst ray crosses ~sqrt(2) columns per unit of distance); the cell
	// walks inside columns are bounded by the world height and the fog cut.
	// A step budget alone would crop the world in a noisy shell.
	std::uint32_t maxTraceSteps = 1024;

	std::uint32_t gridWidth() const { return 2 * renderRadiusChunks + 1; }
	std::uint32_t gridHeight() const { return 2 * renderRadiusChunks + 1; }

	// Total number of chunk slots in the GPU atlas (= region grid cells).
	std::uint64_t slotCount() const {
		return static_cast<std::uint64_t>(gridWidth()) * gridHeight();
	}

	// Total far-LOD grid cells per side (0 when far LOD is disabled).
	std::uint32_t farLodDim() const {
		return farLodRadiusChunks == 0
				? 0
				: (2 * farLodRadiusChunks * chunkSizeX) / farLodCellVoxels;
	}

	bool isValid() const {
		const bool nearValid =
				chunkSizeX > 0 && chunkSizeZ > 0 && worldHeight > 0 &&
				voxelSize.x > 1e-6f && voxelSize.y > 1e-6f &&
				voxelSize.z > 1e-6f && renderRadiusChunks >= 1 &&
				renderRadiusChunks <= 16 && maxTraceSteps >= 16;
		if (!nearValid) {
			return false;
		}
		if (farLodRadiusChunks == 0) {
			return true;  // far LOD disabled
		}
		if (farLodCellVoxels >= 1 && farLodCellVoxels <= 32 &&
				farLodRadiusChunks >= renderRadiusChunks + 2 &&
				farLodRadiusChunks <= 256 &&
				(2 * farLodRadiusChunks * chunkSizeX) % farLodCellVoxels ==
						0 &&
				farLodDim() <= 4096) {  // 4096^2 cells = 64 MB cap
			// Fog-cut ordering: with the far field active the cut is the
			// distance to the far box face, which must ALWAYS exceed the
			// near-region exit along any ray (else a gap opens between the
			// near trace and the far march). Sufficient: far half-extent >=
			// recenter hysteresis + near-region half-diagonal. With the
			// hysteresis at far/4 chunks this needs roughly
			// farRadius >= 2 * renderRadius + 4.
			return farLodRadiusChunks >= 2 * renderRadiusChunks + 4;
		}
		return false;
	}
};

}  // namespace vv::voxel
