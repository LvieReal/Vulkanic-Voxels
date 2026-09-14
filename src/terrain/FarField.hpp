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
	// Actual (snapped) field center in voxels - see build; the renderer
	// uses it for the recenter-hysteresis check.
	std::int32_t centerVoxX = 0;
	std::int32_t centerVoxZ = 0;

	std::vector<std::uint32_t> cells;

	static std::uint32_t packColumn(std::uint16_t height, std::uint8_t type) {
		return static_cast<std::uint32_t>(height) |
					 (static_cast<std::uint32_t>(type) << 16u);
	}

	// One loaded chunk's per-column heights for patchRegion (see below).
	struct RegionChunkHeights {
		std::int32_t minVoxX = 0;   // chunk min corner in voxel space
		std::int32_t minVoxZ = 0;
		std::uint32_t sizeX = 0;    // chunk footprint (voxels)
		std::uint32_t sizeZ = 0;
		const std::uint16_t* heights = nullptr;  // top+1 per column (0 = air),
																							// row-major x + z*sizeX
	};

	// Rewrites the cells covered by the given chunks with the REAL column
	// tops: fully covered cells take the exact per-cell max (so the far
	// surface continues the near terrain 1:1 across the seam), partially
	// covered edge cells keep the existing estimate as a floor. Surface
	// types come from the generator's layering rule. Pure; returns the
	// number of changed cells; if outChangedIndices is non-null it receives
	// the changed cell indices (row-major, ascending) for delta uploads.
	// This is what kills near/far seam holes in folded mountains (the
	// estimate alone can under-shoot by 20+ voxels). Pass only the NEWLY
	// covered chunks for incremental patches: untouched cells keep their
	// (already exact) values.
	static std::size_t patchRegion(std::vector<std::uint32_t>& cells,
																 std::uint32_t dim, std::uint32_t cellVoxels,
																 std::int32_t originVoxX,
																 std::int32_t originVoxZ,
																 const std::vector<RegionChunkHeights>& chunks,
																 const TerrainGenerator& gen,
																 std::vector<std::uint32_t>* outChangedIndices = nullptr);

	// Builds the field centered on the given chunk, snapped to the
	// world-aligned grid (see the .cpp). Pure and thread-safe: only reads
	// the (const) terrain generator.
	// When `previous` is given (same dim + cell footprint, world-aligned),
	// cells that were already computed in the previous window are COPIED
	// instead of recomputed - a recenter then only evaluates the newly
	// exposed strips (~13% of the grid) instead of the whole field. This
	// keeps recenter builds short enough to never disturb the frame loop
	// (full builds took ~1 s of background CPU).
	static FarField build(const TerrainGenerator& gen,
												std::int32_t centerChunkX,
												std::int32_t centerChunkZ,
												std::uint32_t radiusChunks,
												std::uint32_t cellVoxels,
												std::uint32_t chunkSize,
												const FarField* previous = nullptr);
};

}  // namespace vv::terrain
