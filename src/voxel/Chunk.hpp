#pragma once

#include <cstdint>
#include <vector>

#include "voxel/VoxelTypes.hpp"

namespace vv::voxel {

// One full-height column of the world: chunkSizeX x worldHeight x chunkSizeZ
// voxels laid out as X + Y*sizeX + Z*sizeX*worldHeight (matching the compute
// shader's index math). Storage is one byte per voxel (VoxelType).
class Chunk final {
 public:
	Chunk() = default;
	Chunk(std::int32_t chunkX, std::int32_t chunkZ, std::uint32_t sizeX,
				std::uint32_t sizeY, std::uint32_t sizeZ);

	std::int32_t chunkX() const { return m_chunkX; }
	std::int32_t chunkZ() const { return m_chunkZ; }

	std::uint32_t sizeX() const { return m_sizeX; }
	std::uint32_t sizeY() const { return m_sizeY; }
	std::uint32_t sizeZ() const { return m_sizeZ; }

	std::uint64_t voxelCount() const {
		return static_cast<std::uint64_t>(m_sizeX) * m_sizeY * m_sizeZ;
	}

	// Byte size padded to a multiple of 4 (voxel types are uploaded packed
	// 4-per-uint32; the pad bytes are never read back).
	std::uint64_t paddedByteSize() const { return (voxelCount() + 3u) / 4u * 4u; }

	// Per-column height bound for the GPU air-skip: (highest solid voxel Y +
	// 1) per column, 0 = all-air column. Stored packed as u16 pairs in u32
	// words, row-major over X + Z*sizeX, (sizeX*sizeZ + 1) / 2 words - this
	// layout is a sync contract with the compute shader's ColumnHeights
	// buffer. Lazily recomputed after set() calls (dirty flag), so it always
	// matches the actual voxel data.
	const std::vector<std::uint16_t>& heightMap() const;
	const std::vector<std::uint32_t>& heightMapWords() const;
	// Heightmap slot stride in u32 words (sync contract with the shader).
	std::uint64_t heightMapWordStride() const {
		return (static_cast<std::uint64_t>(m_sizeX) * m_sizeZ + 1u) / 2u;
	}

	// Per-BLOCK max height for the hierarchical DDA (pass 30): one u16
	// per kHeightBlockVoxels^2 block of columns = the MAX column bound
	// in the block (0 = whole block air). Layout: blockX + blockZ *
	// blocksX, packed two per u32 (block i even -> low half) like the
	// heightmap - a sync contract with the shader's BlockHeights buffer
	// (binding 11). Recomputed together with the heightmap (dirty flag).
	const std::vector<std::uint16_t>& blockHeightMap() const;
	const std::vector<std::uint32_t>& blockHeightMapWords() const;
	// Block-atlas slot stride in u32 words (sync contract with the
	// shader).
	std::uint64_t blockHeightWordStride() const {
		const std::uint32_t b = kHeightBlockVoxels;
		const std::uint64_t blocksX = (m_sizeX + b - 1u) / b;
		const std::uint64_t blocksZ = (m_sizeZ + b - 1u) / b;
		return (blocksX * blocksZ + 1u) / 2u;
	}

	vv::voxel::VoxelType get(std::uint32_t x, std::uint32_t y,
													 std::uint32_t z) const;
	void set(std::uint32_t x, std::uint32_t y, std::uint32_t z,
					 vv::voxel::VoxelType type);
	// Installs a full voxel-type vector generated elsewhere (async worker;
	// same X + Y*sizeX + Z*sizeX*sizeY layout as generateChunkVoxels).
	// Moves instead of 131k set() calls; marks the heightmap dirty.
	void setVoxelTypes(std::vector<std::uint8_t>&& types);

	const std::vector<std::uint8_t>& voxelTypes() const { return m_voxelTypes; }

 private:
	std::uint64_t index(std::uint32_t x, std::uint32_t y,
											std::uint32_t z) const;
	void recomputeHeightMap() const;

	std::int32_t m_chunkX = 0;
	std::int32_t m_chunkZ = 0;
	std::uint32_t m_sizeX = 0;
	std::uint32_t m_sizeY = 0;
	std::uint32_t m_sizeZ = 0;
	std::vector<std::uint8_t> m_voxelTypes;

	mutable std::vector<std::uint16_t> m_heightMap;
	mutable std::vector<std::uint32_t> m_heightMapWords;
	mutable std::vector<std::uint16_t> m_blockHeightMap;
	mutable std::vector<std::uint32_t> m_blockHeightMapWords;
	mutable bool m_heightMapDirty = true;
};

}  // namespace vv::voxel
