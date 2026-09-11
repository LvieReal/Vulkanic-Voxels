// Pure-logic tests for the terrain/world modules. No Qt, no Vulkan: this
// suite also runs in restricted sandboxes where the game itself cannot.
//
// Run via ctest or directly: ./build/bin/voxel_tests

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "terrain/Noise.hpp"
#include "terrain/TerrainGenerator.hpp"
#include "voxel/Chunk.hpp"
#include "voxel/VoxelTypes.hpp"
#include "voxel/World.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
	if (!ok) {
		std::printf("FAIL: %s\n", what);
		++g_failures;
	}
}

void testNoiseDeterministic() {
	const vv::terrain::Noise2D a(42);
	const vv::terrain::Noise2D b(42);
	const vv::terrain::Noise2D c(43);
	bool sameSeedEqual = true;
	bool otherSeedDiffers = false;
	for (int i = 0; i < 1000; ++i) {
		const double x = i * 0.137 - 50.0;
		const double z = i * -0.211 + 30.0;
		if (a.noise(x, z) != b.noise(x, z) ||
				a.fbm(x, z, 5) != b.fbm(x, z, 5)) {
			sameSeedEqual = false;
		}
		if (a.noise(x, z) != c.noise(x, z)) {
			otherSeedDiffers = true;
		}
	}
	check(sameSeedEqual, "noise: same seed must be identical");
	check(otherSeedDiffers, "noise: different seed must differ");
}

void testNoiseRangeAndContinuity() {
	const vv::terrain::Noise2D noise(7);
	bool inRange = true;
	bool continuous = true;
	for (int i = 0; i < 2000; ++i) {
		const double x = i * 0.031 - 30.0;
		const double z = i * 0.017 - 10.0;
		const double n = noise.noise(x, z);
		const double f = noise.fbm(x, z, 5);
		if (std::abs(n) > 1.0 + 1e-6 || std::abs(f) > 1.0 + 1e-6) {
			inRange = false;
		}
		const double n2 = noise.noise(x + 1e-3, z);
		if (std::abs(n - n2) > 0.01) {
			continuous = false;
		}
	}
	check(inRange, "noise: outputs must stay within [-1, 1]");
	check(continuous, "noise: outputs must be continuous");
}

vv::terrain::TerrainConfig testTerrainConfig() {
	vv::terrain::TerrainConfig cfg;
	cfg.seed = 1337;
	return cfg;
}

void testTerrainHeightBounds() {
	const vv::terrain::TerrainGenerator gen(testTerrainConfig());
	const auto& cfg = gen.config();
	const double bound = std::abs(cfg.amplitude) * 1.2 + 1e-9;
	const double maxHeight = double(gen.maxHeightVoxels());
	bool inBounds = true;
	bool underSkySkipBound = true;
	for (int i = 0; i < 50000; ++i) {
		const double x = i * 1.7 - 40000.0;
		const double z = i * -2.3 + 30000.0;
		const double h = gen.heightAt(x, z);
		if (h < cfg.baseHeight - bound || h > cfg.baseHeight + bound) {
			inBounds = false;
		}
		// The renderer's sky-skip early-out depends on this: no height may
		// exceed maxHeightVoxels().
		if (h > maxHeight) {
			underSkySkipBound = false;
		}
	}
	check(inBounds, "terrain: height must stay within base +- 1.2*amplitude");
	check(underSkySkipBound,
				"terrain: maxHeightVoxels() must bound every height");
}

void testTerrainLayering() {
	const vv::terrain::TerrainGenerator gen(testTerrainConfig());
	const auto& cfg = gen.config();
	const double dirtDepth = std::floor(cfg.dirtDepth);
	bool ok = true;
	for (int i = 0; i < 500; ++i) {
		// Integer columns: typeAt() re-evaluates heightAt at the exact column,
		// so the test must not mix fractional and truncated coordinates.
		const std::int32_t x = i * 3 - 700;
		const std::int32_t z = -i * 2 + 400;
		const double h = gen.heightAt(double(x), double(z));
		const std::int32_t surface = static_cast<std::int32_t>(std::floor(h));

		// Above the surface: air.
		if (gen.typeAt(x, surface + 1, z) != vv::voxel::VoxelType::Air) {
			ok = false;
		}
		// Surface voxel: a top-layer type.
		const auto top = gen.typeAt(x, surface, z);
		if (top != vv::voxel::VoxelType::Grass &&
				top != vv::voxel::VoxelType::Sand &&
				top != vv::voxel::VoxelType::Snow) {
			ok = false;
		}
		// Bottom of the world: bedrock.
		if (gen.typeAt(x, 0, z) != vv::voxel::VoxelType::Bedrock) {
			ok = false;
		}
		// Below the dirt layer (when such a layer exists at all): stone.
		if (surface > dirtDepth + 1) {
			const std::int32_t deepY =
					surface - static_cast<std::int32_t>(dirtDepth) - 1;
			if (gen.typeAt(x, deepY, z) != vv::voxel::VoxelType::Stone) {
				ok = false;
			}
		}
	}
	check(ok, "terrain: layering rules (air/top/bedrock/stone)");
}

void testWorldRegion() {
	vv::terrain::TerrainConfig terrainCfg = testTerrainConfig();
	vv::voxel::World world(terrainCfg, 32, 128, 32);

	std::vector<const vv::voxel::Chunk*> newChunks;
	std::vector<vv::voxel::ChunkCoord> evicted;

	world.ensureRegion(0, 0, 3, newChunks, evicted);
	check(newChunks.size() == 49, "world: initial region has 49 chunks");
	check(evicted.empty(), "world: no evictions on first region");
	check(world.cachedChunkCount() == 49, "world: cache holds 49 chunks");

	// Step +1 in x: one new column, still nothing evicted (hysteresis). The
	// cache grows to (2R+2)*(2R+1) = 56 chunks: chunks load at radius R but
	// are only evicted beyond radius R+1.
	world.ensureRegion(1, 0, 3, newChunks, evicted);
	check(newChunks.size() == 7, "world: +1 chunk step adds one column");
	check(evicted.empty(), "world: hysteresis keeps far column cached");
	check(world.cachedChunkCount() == 56, "world: cache grows to 56 chunks");

	// Step +1 again: one new column, one column evicted; steady state.
	world.ensureRegion(2, 0, 3, newChunks, evicted);
	check(newChunks.size() == 7, "world: second step adds one column");
	check(evicted.size() == 7, "world: second step evicts one column");
	check(world.cachedChunkCount() == 56, "world: cache size stays constant");
	check(world.findChunk(vv::voxel::ChunkCoord{2, 0}) != nullptr,
				"world: center chunk findable");
	check(world.findChunk(vv::voxel::ChunkCoord{-3, 0}) == nullptr,
				"world: evicted chunk gone");
}

void testWorldWalk() {
	// Simulate the game's access pattern: a camera flying around, region
	// rebuilt on every chunk-border crossing. The cache must stay bounded.
	vv::voxel::World world(testTerrainConfig(), 32, 128, 32);
	std::vector<const vv::voxel::Chunk*> newChunks;
	std::vector<vv::voxel::ChunkCoord> evicted;

	constexpr std::uint32_t kRadius = 6;
	// After each ensureRegion, everything cached is within Chebyshev radius
	// R+1 of the center: at most (2R+3)^2 chunks.
	const std::size_t bound = (2 * kRadius + 3) * (2 * kRadius + 3);

	bool bounded = true;
	std::int32_t cx = 0;
	std::int32_t cz = 0;
	for (int step = 0; step < 300; ++step) {
		if (step % 3 == 0) ++cx;
		if (step % 5 == 0) --cz;
		if (step % 17 == 0) cx -= 3;
		world.ensureRegion(cx, cz, kRadius, newChunks, evicted);
		if (world.cachedChunkCount() > bound) {
			bounded = false;
		}
	}
	check(bounded, "world: cache stays bounded while walking");
}

void testWorldDeterminism() {
	vv::terrain::TerrainConfig cfg = testTerrainConfig();
	vv::voxel::World a(cfg, 32, 128, 32);
	vv::voxel::World b(cfg, 32, 128, 32);

	std::vector<const vv::voxel::Chunk*> newA, newB;
	std::vector<vv::voxel::ChunkCoord> evicted;
	a.ensureRegion(2, 3, 1, newA, evicted);
	b.ensureRegion(2, 3, 1, newB, evicted);

	bool identical = newA.size() == newB.size();
	for (std::size_t i = 0; identical && i < newA.size(); ++i) {
		identical = newA[i]->voxelTypes() == newB[i]->voxelTypes();
	}
	check(identical, "world: same seed produces identical chunks");
}

void testChunkMatchesGenerator() {
	vv::terrain::TerrainConfig cfg = testTerrainConfig();
	vv::voxel::World world(cfg, 32, 128, 32);

	std::vector<const vv::voxel::Chunk*> newChunks;
	std::vector<vv::voxel::ChunkCoord> evicted;
	world.ensureRegion(0, 0, 1, newChunks, evicted);

	const vv::voxel::Chunk* chunk = world.findChunk(vv::voxel::ChunkCoord{0, 0});
	check(chunk != nullptr, "chunk: (0,0) exists");
	if (chunk == nullptr) {
		return;
	}
	check(chunk->paddedByteSize() % 4 == 0 &&
					chunk->paddedByteSize() >= chunk->voxelCount(),
				"chunk: padded byte size is 4-aligned and >= voxel count");

	bool matches = true;
	for (std::uint32_t z = 0; z < 32; z += 7) {
		for (std::uint32_t y = 0; y < 128; y += 13) {
			for (std::uint32_t x = 0; x < 32; x += 5) {
				const auto expected = world.terrain().typeAt(
						static_cast<std::int32_t>(x),
						static_cast<std::int32_t>(y),
						static_cast<std::int32_t>(z));
				if (chunk->get(x, y, z) != expected) {
					matches = false;
				}
			}
		}
	}
	check(matches, "chunk: contents match the terrain generator");
}

}  // namespace

int main() {
	testNoiseDeterministic();
	testNoiseRangeAndContinuity();
	testTerrainHeightBounds();
	testTerrainLayering();
	testWorldRegion();
	testWorldWalk();
	testWorldDeterminism();
	testChunkMatchesGenerator();

	if (g_failures == 0) {
		std::printf("all tests passed\n");
		return 0;
	}
	std::printf("%d test(s) failed\n", g_failures);
	return 1;
}
