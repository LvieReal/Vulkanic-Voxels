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

	vv::voxel::VoxelType get(std::uint32_t x, std::uint32_t y,
													 std::uint32_t z) const;
	void set(std::uint32_t x, std::uint32_t y, std::uint32_t z,
					 vv::voxel::VoxelType type);

	const std::vector<std::uint8_t>& voxelTypes() const { return m_voxelTypes; }

 private:
	std::uint64_t index(std::uint32_t x, std::uint32_t y,
											std::uint32_t z) const;

	std::int32_t m_chunkX = 0;
	std::int32_t m_chunkZ = 0;
	std::uint32_t m_sizeX = 0;
	std::uint32_t m_sizeY = 0;
	std::uint32_t m_sizeZ = 0;
	std::vector<std::uint8_t> m_voxelTypes;
};

}  // namespace vv::voxel
