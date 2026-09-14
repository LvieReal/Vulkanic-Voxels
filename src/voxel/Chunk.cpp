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
	m_heightMapDirty = true;
}

void Chunk::recomputeHeightMap() const {
	// Ground truth scan (not derived from the generator): the highest
	// non-air cell per column. Conservative upper bound on solid content, so
	// the shader's air-skip stays correct even for future overhangs/edits.
	m_heightMap.assign(static_cast<std::size_t>(m_sizeX) * m_sizeZ, 0);
	for (std::uint32_t z = 0; z < m_sizeZ; ++z) {
		for (std::uint32_t x = 0; x < m_sizeX; ++x) {
			for (std::uint32_t y = m_sizeY; y-- > 0;) {
				if (m_voxelTypes[static_cast<std::size_t>(index(x, y, z))] !=
						static_cast<std::uint8_t>(VoxelType::Air)) {
					m_heightMap[static_cast<std::size_t>(x) +
											static_cast<std::size_t>(z) * m_sizeX] =
							static_cast<std::uint16_t>(y + 1u);
					break;
				}
			}
		}
	}
	// Pack u16 pairs into u32 words (column i even -> low half). Explicit
	// packing, independent of host endianness.
	const std::size_t words =
			static_cast<std::size_t>((m_sizeX * m_sizeZ + 1u) / 2u);
	m_heightMapWords.assign(words, 0);
	for (std::size_t i = 0; i < m_heightMap.size(); ++i) {
		if ((i & 1u) == 0u) {
			m_heightMapWords[i >> 1u] |=
					static_cast<std::uint32_t>(m_heightMap[i]);
		} else {
			m_heightMapWords[i >> 1u] |=
					static_cast<std::uint32_t>(m_heightMap[i]) << 16u;
		}
	}

	// Block maxima for the hierarchical DDA (pass 30): max column bound
	// per kHeightBlockVoxels^2 block (0 = whole block air). Edge blocks
	// of non-divisible chunk sizes simply cover fewer columns.
	const std::uint32_t b = kHeightBlockVoxels;
	const std::uint32_t blocksX = (m_sizeX + b - 1u) / b;
	const std::uint32_t blocksZ = (m_sizeZ + b - 1u) / b;
	m_blockHeightMap.assign(std::size_t(blocksX) * blocksZ, 0);
	for (std::uint32_t bz = 0; bz < blocksZ; ++bz) {
		for (std::uint32_t bx = 0; bx < blocksX; ++bx) {
			std::uint16_t m = 0;
			for (std::uint32_t z = bz * b;
			     z < std::min((bz + 1u) * b, m_sizeZ); ++z) {
				for (std::uint32_t x = bx * b;
				     x < std::min((bx + 1u) * b, m_sizeX); ++x) {
					m = std::max(
							m, m_heightMap[std::size_t(x) +
							               std::size_t(z) * m_sizeX]);
				}
			}
			m_blockHeightMap[std::size_t(bx) +
			                 std::size_t(bz) * blocksX] = m;
		}
	}
	const std::size_t blockWords =
			(std::size_t(blocksX) * blocksZ + 1u) / 2u;
	m_blockHeightMapWords.assign(blockWords, 0);
	for (std::size_t i = 0; i < m_blockHeightMap.size(); ++i) {
		if ((i & 1u) == 0u) {
			m_blockHeightMapWords[i >> 1u] |=
					static_cast<std::uint32_t>(m_blockHeightMap[i]);
		} else {
			m_blockHeightMapWords[i >> 1u] |=
					static_cast<std::uint32_t>(m_blockHeightMap[i]) << 16u;
		}
	}
	m_heightMapDirty = false;
}

const std::vector<std::uint16_t>& Chunk::blockHeightMap() const {
	if (m_heightMapDirty) {
		recomputeHeightMap();
	}
	return m_blockHeightMap;
}

const std::vector<std::uint32_t>& Chunk::blockHeightMapWords() const {
	if (m_heightMapDirty) {
		recomputeHeightMap();
	}
	return m_blockHeightMapWords;
}

void Chunk::setVoxelTypes(std::vector<std::uint8_t>&& types) {
	m_voxelTypes = std::move(types);
	// The uploader copies paddedByteSize() bytes; keep the vector at that
	// size (no-op for the default 32x128x32 chunk - 131072 is divisible
	// by 4 - but stay safe for any future geometry).
	m_voxelTypes.resize(paddedByteSize());
	m_heightMapDirty = true;
}

const std::vector<std::uint16_t>& Chunk::heightMap() const {
	if (m_heightMapDirty) {
		recomputeHeightMap();
	}
	return m_heightMap;
}

const std::vector<std::uint32_t>& Chunk::heightMapWords() const {
	if (m_heightMapDirty) {
		recomputeHeightMap();
	}
	return m_heightMapWords;
}

}  // namespace vv::voxel
