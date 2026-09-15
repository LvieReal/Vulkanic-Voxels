// Camera-centered 3D voxel SDF box (pass 38/39, VV_SDF_SHADOWS=1).
//
// The box is a square of whole chunks on X/Z (2 * halfChunks of them) x the
// full world height on Y. It is built on a BACKGROUND thread from per-chunk
// voxel-type snapshots (taken on the render thread, the only thread that
// mutates the world chunk map) and uploaded as one argmin seed per cell
// (binding 12) plus its geometry (binding 13); the compute shader sphere-
// traces it for the soft sun shadow.
//
// This header owns the box's voxel -> chunk mapping because that is where the
// GPU path can silently go wrong: the snapshot walk must use the SAME voxel
// layout as Chunk::index() and the shader's fetchVoxel() -
//
//     x + y*sizeX + z*sizeX*worldHeight
//
// (see AGENT_NOTES "Voxel atlas" contract). Pass 39 fixes exactly that: the
// box was indexed with a Z stride of chunkSizeX * chunkSizeZ instead of
// chunkSizeX * worldHeight, so with the default config (chunkSizeZ 32,
// worldHeight 128) it read a 1024-word stride where the chunks lay out 4096 -
// i.e. the SDF was built from a scrambled projection of the terrain, and the
// whole region around the camera came out fully shadowed. The CPU test
// (testSdfBoxBuild) now pins this walk against real Chunk data.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "voxel/SdfField.hpp"
#include "voxel/VoxelTypes.hpp"

namespace vv::voxel {

// Geometry of the SDF box in world voxels: `origin*` is the min corner,
// `nx/ny/nz` the size in cells.
struct SdfBoxGeometry final {
	std::int32_t originX = 0;
	std::int32_t originY = 0;
	std::int32_t originZ = 0;
	std::uint32_t nx = 0;
	std::uint32_t ny = 0;
	std::uint32_t nz = 0;
	// Box layout: chunksPerSide^2 whole chunks on X/Z, full world height on Y.
	std::uint32_t chunksPerSide = 0;
	std::uint32_t chunkSizeX = 0;
	std::uint32_t chunkSizeZ = 0;
	std::uint32_t worldHeight = 0;

	std::uint64_t cells() const {
		return static_cast<std::uint64_t>(nx) * ny * nz;
	}

	bool valid() const {
		return chunksPerSide > 0 && chunkSizeX > 0 && chunkSizeZ > 0 &&
					 worldHeight > 0 && nx == chunksPerSide * chunkSizeX &&
					 ny == worldHeight && nz == chunksPerSide * chunkSizeZ;
	}

	// The box centered on a chunk: chunks [center - half, center + half).
	static SdfBoxGeometry centeredOn(std::int32_t centerChunkX,
																	 std::int32_t centerChunkZ,
																	 std::uint32_t halfChunks,
																	 std::uint32_t chunkSizeX,
																	 std::uint32_t chunkSizeZ,
																	 std::uint32_t worldHeight) {
		SdfBoxGeometry box;
		box.chunksPerSide = 2u * halfChunks;
		box.chunkSizeX = chunkSizeX;
		box.chunkSizeZ = chunkSizeZ;
		box.worldHeight = worldHeight;
		box.originX =
				(centerChunkX - static_cast<std::int32_t>(halfChunks)) *
				static_cast<std::int32_t>(chunkSizeX);
		box.originY = 0;  // the box always spans the whole world height
		box.originZ =
				(centerChunkZ - static_cast<std::int32_t>(halfChunks)) *
				static_cast<std::int32_t>(chunkSizeZ);
		box.nx = box.chunksPerSide * chunkSizeX;
		box.ny = worldHeight;
		box.nz = box.chunksPerSide * chunkSizeZ;
		return box;
	}
};

// Solid test for a BOX-LOCAL cell (x, y, z), from the per-chunk snapshots in
// box chunk order [cz * chunksPerSide + cx]. A missing/foreign-sized chunk
// reads as air (the box build runs when the region is complete, but the
// fallback must never index out of bounds).
inline bool sdfBoxCellSolid(
		const SdfBoxGeometry& box,
		const std::vector<std::vector<std::uint8_t>>& snapshots,
		std::uint32_t x, std::uint32_t y, std::uint32_t z) {
	if (y >= box.worldHeight) {
		return false;
	}
	const std::uint32_t chunkX = x / box.chunkSizeX;
	const std::uint32_t chunkZ = z / box.chunkSizeZ;
	if (chunkX >= box.chunksPerSide || chunkZ >= box.chunksPerSide) {
		return false;
	}
	const std::size_t chunk =
			static_cast<std::size_t>(chunkZ) * box.chunksPerSide + chunkX;
	if (chunk >= snapshots.size()) {
		return false;
	}
	const std::vector<std::uint8_t>& types = snapshots[chunk];
	const std::size_t expected = static_cast<std::size_t>(box.chunkSizeX) *
															 box.worldHeight * box.chunkSizeZ;
	if (types.size() != expected) {
		return false;  // not installed yet (or a foreign chunk size): air
	}
	// Chunk layout: X + Y*sizeX + Z*sizeX*worldHeight (sync contract with
	// Chunk::index and the shader's fetchVoxel).
	const std::size_t u = x - chunkX * box.chunkSizeX;
	const std::size_t v = z - chunkZ * box.chunkSizeZ;
	const std::size_t i = u + static_cast<std::size_t>(y) * box.chunkSizeX +
												v * box.chunkSizeX * box.worldHeight;
	return types[i] != static_cast<std::uint8_t>(VoxelType::Air);
}

// Build the box's SDF (two-pass chamfer EDT + argmin seeds) from the
// snapshots. `out` gets the same field the CPU test pins, so the GPU sphere
// trace stays a parity of vv::voxel::SdfField.
inline void buildSdfBoxField(
		const SdfBoxGeometry& box,
		const std::vector<std::vector<std::uint8_t>>& snapshots, SdfField& out) {
	out.build(static_cast<int>(box.nx), static_cast<int>(box.ny),
						static_cast<int>(box.nz),
						[&box, &snapshots](int x, int y, int z) {
							return sdfBoxCellSolid(box, snapshots,
																		 static_cast<std::uint32_t>(x),
																		 static_cast<std::uint32_t>(y),
																		 static_cast<std::uint32_t>(z));
						});
}

}  // namespace vv::voxel
