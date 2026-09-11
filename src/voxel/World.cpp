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

// Writes the solid range [0, min(surface, topBound)] of one column; cells
// above the surface stay Air (Chunk initializes to Air).
void fillColumn(const vv::terrain::TerrainGenerator& terrain,
												vv::voxel::Chunk& chunk, std::uint32_t x, std::uint32_t z,
											float height, std::int32_t topBound) {
	const std::int32_t surface = static_cast<std::int32_t>(std::floor(height));
	const std::int32_t top = std::min(std::max(surface, 0), topBound);
	for (std::int32_t y = 0; y <= top; ++y) {
		chunk.set(x, static_cast<std::uint32_t>(y), z,
						 terrain.typeForColumn(y, height));
	}
}

}  // namespace

Chunk* World::generateChunk(const ChunkCoord& coord) {
	auto chunk = std::make_unique<Chunk>(coord.x, coord.z, m_chunkSizeX,
																			 m_worldHeight, m_chunkSizeZ);

	// Column-major fill, vectorized 4 columns at a time (heightAt4 uses the
	// SSE2 fBm; bit-identical to heightAt, so chunks match the generator
	// exactly - see the parity tests). Only the solid range of each column
	// is written; cells above the surface stay Air from the constructor.
	const std::int64_t baseX =
			static_cast<std::int64_t>(coord.x) * m_chunkSizeX;
	const std::int64_t baseZ =
			static_cast<std::int64_t>(coord.z) * m_chunkSizeZ;
	const std::int32_t topBound =
			static_cast<std::int32_t>(m_worldHeight) - 1;

	float heights[4];
	for (std::uint32_t z = 0; z < m_chunkSizeZ; ++z) {
		const float wz =
				static_cast<float>(baseZ + static_cast<std::int64_t>(z));
		const float zs[4] = {wz, wz, wz, wz};
		for (std::uint32_t x = 0; x < m_chunkSizeX; x += 4) {
			const std::uint32_t count =
					std::min<std::uint32_t>(4, m_chunkSizeX - x);
			if (count == 4) {
				const float xs[4] = {
						static_cast<float>(baseX + static_cast<std::int64_t>(x)),
						static_cast<float>(baseX + static_cast<std::int64_t>(x) + 1),
						static_cast<float>(baseX + static_cast<std::int64_t>(x) + 2),
						static_cast<float>(baseX + static_cast<std::int64_t>(x) + 3)};
				m_terrain.heightAt4(xs, zs, heights);
				for (std::uint32_t k = 0; k < 4; ++k) {
					fillColumn(m_terrain, *chunk, x + k, z, heights[k], topBound);
				}
			} else {
				// Tail (< 4 columns left; not hit by the default 32-wide chunks).
				for (std::uint32_t k = 0; k < count; ++k) {
					const float height = m_terrain.heightAtF(
							static_cast<float>(baseX + static_cast<std::int64_t>(x + k)),
						wz);
					fillColumn(m_terrain, *chunk, x + k, z, height, topBound);
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
