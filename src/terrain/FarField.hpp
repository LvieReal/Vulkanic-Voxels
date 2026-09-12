#pragma once

#include <cstdint>
#include <vector>

#include "terrain/TerrainGenerator.hpp"

namespace vv::terrain {

// Far-LOD height field: the world beyond the full-detail chunk region,
// compressed to one coarse column per cell. Each cell stores the highest
// solid voxel height + 1 (u16, same convention as the chunk heightmaps)
// and the surface voxel type (u8), packed as height | type << 16 into one
// u32, row-major over X + Z * dim.
//
// Sampling: one column evaluation at the cell CENTER - the terrain
// generator's estimated topmost solid voxel (see TerrainGenerator::
// estimatedTopSolid), matching the near-region silhouette to within the
// existing far quantization. This quantizes distant silhouettes by roughly
// +-cellVoxels/2 of terrain height - the accepted far-LOD trade-off (see
// AGENT_NOTES pass 4); a conservative max-of-samples seam band is a future
// refinement if the seam is ever objectionable.
//
// GPU sync contract (shader binding 6 + push constants farOriginDim /
// farParams): cells[] is row-major X + Z * dim; height in the low 16 bits,
// type in bits 16..23; the grid covers voxel space
// [originVox, originVox + dim * cellVoxels) on X and Z.
struct FarField final {
	std::int32_t originVoxX = 0;  // voxel-space min corner of cell (0, 0)
	std::int32_t originVoxZ = 0;
	std::uint32_t dim = 0;         // cells per side (square grid)
	std::uint32_t cellVoxels = 0;  // cell footprint in voxels (X and Z)

	std::vector<std::uint32_t> cells;

	static std::uint32_t packColumn(std::uint16_t height, std::uint8_t type) {
		return static_cast<std::uint32_t>(height) |
					 (static_cast<std::uint32_t>(type) << 16u);
	}

	// Builds the field centered on the given chunk (the box is centered on
	// that chunk's center voxel so it always contains the full near region
	// plus recenter hysteresis). Pure and thread-safe: only reads the
	// (const) terrain generator.
	static FarField build(const TerrainGenerator& gen,
												std::int32_t centerChunkX,
												std::int32_t centerChunkZ,
												std::uint32_t radiusChunks,
												std::uint32_t cellVoxels,
												std::uint32_t chunkSize);
};

}  // namespace vv::terrain
