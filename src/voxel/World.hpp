#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "terrain/TerrainGenerator.hpp"
#include "voxel/Chunk.hpp"

namespace vv::voxel {

// Chunk grid coordinate (X/Z; chunks span the full world height).
struct ChunkCoord final {
	std::int32_t x = 0;
	std::int32_t z = 0;
	bool operator==(const ChunkCoord& other) const = default;
};

struct ChunkCoordHash final {
	std::size_t operator()(const ChunkCoord& coord) const noexcept;
};

// Infinite (X/Z) world of terrain-generated chunks with a bounded CPU cache.
// The renderer keeps a square region resident and asks ensureRegion() whenever
// the camera crosses a chunk boundary.
class World final {
 public:
	World(const vv::terrain::TerrainConfig& terrainConfig,
				std::uint32_t chunkSizeX, std::uint32_t worldHeight,
				std::uint32_t chunkSizeZ);

	// Ensures every chunk within the square [center-radius, center+radius]
	// exists and evicts cached chunks outside radius+1 (hysteresis, so a
	// camera jittering around a border does not thrash). Newly generated
	// chunks are appended to outNew (pointers owned by the world, valid until
	// the next ensureRegion call); evicted coordinates are appended to
	// outEvicted.
	void ensureRegion(std::int32_t centerX, std::int32_t centerZ,
										std::uint32_t radius,
										std::vector<const Chunk*>& outNew,
										std::vector<ChunkCoord>& outEvicted);

	const Chunk* findChunk(const ChunkCoord& coord) const;

	const vv::terrain::TerrainGenerator& terrain() const { return m_terrain; }

	std::uint32_t chunkSizeX() const { return m_chunkSizeX; }
	std::uint32_t worldHeight() const { return m_worldHeight; }
	std::uint32_t chunkSizeZ() const { return m_chunkSizeZ; }

	std::size_t cachedChunkCount() const { return m_chunks.size(); }

 private:
	Chunk* generateChunk(const ChunkCoord& coord);

	vv::terrain::TerrainGenerator m_terrain;
	std::uint32_t m_chunkSizeX;
	std::uint32_t m_worldHeight;
	std::uint32_t m_chunkSizeZ;
	std::unordered_map<ChunkCoord, std::unique_ptr<Chunk>, ChunkCoordHash>
			m_chunks;
};

}  // namespace vv::voxel
