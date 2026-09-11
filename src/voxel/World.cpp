#include "voxel/World.hpp"

#include <cmath>

namespace vv::voxel {

std::size_t ChunkCoordHash::operator()(const ChunkCoord& coord) const noexcept {
	// 64-bit mix of the two coordinates (splitmix-style).
	std::uint64_t h = (static_cast<std::uint64_t>(
											 static_cast<std::uint32_t>(coord.x))
										 << 32) ^
										static_cast<std::uint32_t>(coord.z);
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdull;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53ull;
	h ^= h >> 33;
	return static_cast<std::size_t>(h);
}

World::World(const vv::terrain::TerrainConfig& terrainConfig,
						 std::uint32_t chunkSizeX, std::uint32_t worldHeight,
						 std::uint32_t chunkSizeZ)
		: m_terrain(terrainConfig),
			m_chunkSizeX(chunkSizeX),
			m_worldHeight(worldHeight),
			m_chunkSizeZ(chunkSizeZ) {}

Chunk* World::generateChunk(const ChunkCoord& coord) {
	auto chunk = std::make_unique<Chunk>(coord.x, coord.z, m_chunkSizeX,
																			 m_worldHeight, m_chunkSizeZ);

	// Column-major fill: one noise evaluation per column, then layered types
	// down the column.
	for (std::uint32_t z = 0; z < m_chunkSizeZ; ++z) {
		for (std::uint32_t x = 0; x < m_chunkSizeX; ++x) {
			const double worldX =
					static_cast<double>(static_cast<std::int64_t>(coord.x) *
																static_cast<std::int64_t>(m_chunkSizeX) +
															x);
			const double worldZ =
					static_cast<double>(static_cast<std::int64_t>(coord.z) *
																static_cast<std::int64_t>(m_chunkSizeZ) +
															z);
			const double height = m_terrain.heightAt(worldX, worldZ);
			for (std::uint32_t y = 0; y < m_worldHeight; ++y) {
				chunk->set(x, y, z,
									 m_terrain.typeForColumn(static_cast<std::int32_t>(y),
																					 height));
			}
		}
	}

	Chunk* raw = chunk.get();
	m_chunks.emplace(coord, std::move(chunk));
	return raw;
}

void World::ensureRegion(std::int32_t centerX, std::int32_t centerZ,
												 std::uint32_t radius,
												 std::vector<const Chunk*>& outNew,
												 std::vector<ChunkCoord>& outEvicted) {
	outNew.clear();
	outEvicted.clear();

	const std::int32_t r = static_cast<std::int32_t>(radius);
	for (std::int32_t dz = -r; dz <= r; ++dz) {
		for (std::int32_t dx = -r; dx <= r; ++dx) {
			const ChunkCoord coord{centerX + dx, centerZ + dz};
			if (m_chunks.find(coord) != m_chunks.end()) {
				continue;
			}
			outNew.push_back(generateChunk(coord));
		}
	}

	const std::int32_t evictRadius = r + 1;
	for (auto it = m_chunks.begin(); it != m_chunks.end();) {
		if (std::abs(it->first.x - centerX) > evictRadius ||
				std::abs(it->first.z - centerZ) > evictRadius) {
			outEvicted.push_back(it->first);
			it = m_chunks.erase(it);
		} else {
			++it;
		}
	}
}

const Chunk* World::findChunk(const ChunkCoord& coord) const {
	const auto it = m_chunks.find(coord);
	return it == m_chunks.end() ? nullptr : it->second.get();
}

}  // namespace vv::voxel
