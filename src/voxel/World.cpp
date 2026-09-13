#include "voxel/World.hpp"

#include <algorithm>
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

namespace {

// (The old heightmap fill lived here; generation now goes through
// TerrainGenerator::generateChunkVoxels - the 3D density field that also
// produces mountains and overhangs.)

}  // namespace

Chunk* World::generateChunk(const ChunkCoord& coord) {
	auto chunk = std::make_unique<Chunk>(coord.x, coord.z, m_chunkSizeX,
																				m_worldHeight, m_chunkSizeZ);

	// Canonical fill via the terrain generator: 2D targets + coarse-lattice
	// 3D noise -> density -> layered voxel types (deterministic; the
	// world-determinism test exercises the same path).
	const std::int64_t baseX =
			static_cast<std::int64_t>(coord.x) * m_chunkSizeX;
	const std::int64_t baseZ =
			static_cast<std::int64_t>(coord.z) * m_chunkSizeZ;
	std::vector<std::uint8_t> types;
	m_terrain.generateChunkVoxels(static_cast<std::int32_t>(baseX),
																static_cast<std::int32_t>(baseZ),
																m_chunkSizeX, m_chunkSizeZ, m_worldHeight,
																types);
	for (std::uint32_t z = 0; z < m_chunkSizeZ; ++z) {
		for (std::uint32_t y = 0; y < m_worldHeight; ++y) {
			const std::size_t base = (static_cast<std::size_t>(z) *
																m_chunkSizeX * m_worldHeight) +
															 static_cast<std::size_t>(y) * m_chunkSizeX;
			for (std::uint32_t x = 0; x < m_chunkSizeX; ++x) {
				const auto type = static_cast<vv::voxel::VoxelType>(types[base + x]);
				if (type != vv::voxel::VoxelType::Air) {
					chunk->set(x, y, z, type);
				}
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
			outNew.push_back(ensureChunk(coord));
		}
	}

	evictOutside(centerX, centerZ, radius + 1, outEvicted);
}

const Chunk* World::ensureChunk(const ChunkCoord& coord) {
	if (const Chunk* existing = findChunk(coord)) {
		return existing;
	}
	return generateChunk(coord);
}

void World::evictOutside(std::int32_t centerX, std::int32_t centerZ,
													std::uint32_t evictRadius,
													std::vector<ChunkCoord>& outEvicted,
													std::size_t maxEvict) {
	// maxEvict > 0 amortizes eviction: freeing ~50 x 128 KB blocks in one
	// call (a full crossing row) causes visible allocator churn (mmap/munmap
	// per block); a per-call cap spreads it over a few frames. Leftovers
	// are picked up by the next call - the cache just lingers slightly
	// longer, bounded by movement.
	const std::int32_t r = static_cast<std::int32_t>(evictRadius);
	std::size_t evicted = 0;
	for (auto it = m_chunks.begin(); it != m_chunks.end();) {
		if (maxEvict != 0 && evicted >= maxEvict) {
			return;
		}
		if (std::abs(it->first.x - centerX) > r ||
				std::abs(it->first.z - centerZ) > r) {
			outEvicted.push_back(it->first);
			it = m_chunks.erase(it);
			++evicted;
		} else {
			++it;
		}
	}
}

const Chunk* World::installChunk(const ChunkCoord& coord,
																 std::vector<std::uint8_t>&& types) {
	auto existing = m_chunks.find(coord);
	if (existing != m_chunks.end()) {
		return existing->second.get();
	}
	auto chunk = std::make_unique<Chunk>(coord.x, coord.z, m_chunkSizeX,
																			 m_worldHeight, m_chunkSizeZ);
	chunk->setVoxelTypes(std::move(types));
	const Chunk* raw = chunk.get();
	m_chunks.emplace(coord, std::move(chunk));
	return raw;
}

const Chunk* World::findChunk(const ChunkCoord& coord) const {
	const auto it = m_chunks.find(coord);
	return it == m_chunks.end() ? nullptr : it->second.get();
}

}  // namespace vv::voxel
