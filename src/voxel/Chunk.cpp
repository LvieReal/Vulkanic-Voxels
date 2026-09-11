#include "voxel/Chunk.hpp"

#include <cassert>
#include <cstddef>

namespace vv::voxel {

Chunk::Chunk(std::int32_t chunkX, std::int32_t chunkZ, std::uint32_t sizeX,
						 std::uint32_t sizeY, std::uint32_t sizeZ)
		: m_chunkX(chunkX),
			m_chunkZ(chunkZ),
			m_sizeX(sizeX),
			m_sizeY(sizeY),
			m_sizeZ(sizeZ),
			m_voxelTypes(static_cast<std::size_t>(voxelCount()),
									 static_cast<std::uint8_t>(VoxelType::Air)) {}

std::uint64_t Chunk::index(std::uint32_t x, std::uint32_t y,
													 std::uint32_t z) const {
	return static_cast<std::uint64_t>(x) +
				 static_cast<std::uint64_t>(y) * m_sizeX +
				 static_cast<std::uint64_t>(z) * m_sizeX * m_sizeY;
}

vv::voxel::VoxelType Chunk::get(std::uint32_t x, std::uint32_t y,
																std::uint32_t z) const {
	assert(x < m_sizeX && y < m_sizeY && z < m_sizeZ);
	return static_cast<VoxelType>(
			m_voxelTypes[static_cast<std::size_t>(index(x, y, z))]);
}

void Chunk::set(std::uint32_t x, std::uint32_t y, std::uint32_t z,
								vv::voxel::VoxelType type) {
	assert(x < m_sizeX && y < m_sizeY && z < m_sizeZ);
	m_voxelTypes[static_cast<std::size_t>(index(x, y, z))] =
			static_cast<std::uint8_t>(type);
}

}  // namespace vv::voxel
