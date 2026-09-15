// Pure-logic tests for the terrain/world modules. No Qt, no Vulkan: this
// suite also runs in restricted sandboxes where the game itself cannot.
//
// Run via ctest or directly: ./build/release/bin/voxel_tests

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <utility>
#include <vector>

#include "terrain/FarField.hpp"
#include "vulkan/StreamPriority.hpp"
#include "terrain/Noise.hpp"
#include "terrain/TerrainGenerator.hpp"
#include "voxel/Chunk.hpp"
#include "voxel/SdfBox.hpp"
#include "voxel/SdfField.hpp"
#include "voxel/SdfHandover.hpp"
#include "voxel/VoxelTextures.hpp"
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
		const float x = float(i) * 0.137f - 50.0f;
		const float z = float(i) * -0.211f + 30.0f;
		if (a.noiseF(x, z) != b.noiseF(x, z) ||
				a.fbmF(x, z, 5) != b.fbmF(x, z, 5)) {
			sameSeedEqual = false;
		}
		if (a.noiseF(x, z) != c.noiseF(x, z)) {
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
		const float x = float(i) * 0.031f - 30.0f;
		const float z = float(i) * 0.017f - 10.0f;
		const float n = noise.noiseF(x, z);
		const float f = noise.fbmF(x, z, 5);
		if (std::abs(n) > 1.0f + 1e-6f || std::abs(f) > 1.0f + 1e-6f) {
			inRange = false;
		}
		const float n2 = noise.noiseF(x + 1e-3f, z);
		if (std::abs(n - n2) > 0.01f) {
			continuous = false;
		}
	}
	check(inRange, "noise: outputs must stay within [-1, 1]");
	check(continuous, "noise: outputs must be continuous");
}

// The vectorized 4-lane fBm must be BIT-identical to the scalar reference
// (this is what lets chunks generated via heightAt4 match typeAt, which
// goes through heightAtF). Includes coordinates on and next to lattice
// lines, where floor() edge cases live.
void testNoiseSimdParity() {
	const vv::terrain::Noise2D noise(1337);
	bool bitExact = true;
	int checked = 0;
	for (int i = 0; i < 2048; ++i) {
		float x[4], z[4];
		for (int lane = 0; lane < 4; ++lane) {
			const int k = i * 4 + lane;
			if (k % 7 == 0) {
				x[lane] = float(k / 3);          // exactly on lattice lines
				z[lane] = float(-(k / 5));
			} else if (k % 7 == 1) {
				x[lane] = float(k / 3) + 1e-4f;  // just next to them
				z[lane] = float(-(k / 5)) - 1e-4f;
			} else {
				x[lane] = k * 0.117f - 240.0f;
				z[lane] = k * -0.209f + 130.0f;
			}
		}
		float out[4];
		noise.fbm4(x, z, 5, 2.0f, 0.5f, out);
		for (int lane = 0; lane < 4; ++lane) {
			const float ref = noise.fbmF(x[lane], z[lane], 5, 2.0f, 0.5f);
			++checked;
			if (out[lane] != ref) {
				bitExact = false;
				if (checked < 20) {
					std::printf("FAIL simd parity: fbm4(%f,%f)=%a vs fbmF=%a\n",
											x[lane], z[lane], double(out[lane]), double(ref));
				}
			}
		}
	}
	check(bitExact, "noise: fbm4 (SSE2) is bit-identical to fbmF");
	check(checked > 8000, "noise: simd parity actually exercised");

	// heightAt4 must also be bit-identical to the scalar heightAtF path.
	const vv::terrain::TerrainGenerator gen(vv::terrain::TerrainConfig{});
	bool heightsExact = true;
	for (int i = 0; i < 512; ++i) {
		float x[4], z[4];
		for (int lane = 0; lane < 4; ++lane) {
			x[lane] = float(i * 4 + lane) * 1.03f - 1000.0f;
			z[lane] = float(i * 7 + lane) * -0.51f + 640.0f;
		}
		float out[4];
		gen.heightAt4(x, z, out);
		for (int lane = 0; lane < 4; ++lane) {
			if (out[lane] != gen.heightAtF(x[lane], z[lane])) {
				heightsExact = false;
			}
		}
	}
	check(heightsExact, "terrain: heightAt4 is bit-identical to heightAtF");
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
	// Density-model invariants (the exact 2D heightmap semantics are gone:
	// the 3D warp can push solids above heightAt's surface and carve air
	// below it; what MUST hold are the structural guarantees the renderer
	// and the far LOD rely on).
	const vv::terrain::TerrainGenerator gen(testTerrainConfig());
	const std::int32_t warp = gen.maxWarpVoxels();
	bool ok = true;
	for (int i = 0; i < 500; ++i) {
		const std::int32_t x = i * 3 - 700;
		const std::int32_t z = -i * 2 + 400;
		const float target =
				gen.surfaceTargetF(float(x), float(z));
		const std::int32_t above =
				static_cast<std::int32_t>(std::ceil(target)) + warp;
		const std::int32_t deep =
				static_cast<std::int32_t>(std::floor(target)) - 60;

		// Above the warp band: provably air (the sky-skip/ceiling contract).
		if (gen.typeAt(x, above, z) != vv::voxel::VoxelType::Air) {
			ok = false;
		}
		// Deep below the target: provably stone (or bedrock at the floor).
		const auto deepType = gen.typeAt(x, std::max(deep, 1), z);
		if (deepType != vv::voxel::VoxelType::Stone &&
			deepType != vv::voxel::VoxelType::Bedrock) {
			ok = false;
		}
		// Bottom of the world: bedrock.
		if (gen.typeAt(x, 0, z) != vv::voxel::VoxelType::Bedrock) {
			ok = false;
		}
		// The topmost solid voxel carries a top-layer type (this is what the
		// far LOD stores as its surface type) and stays under the bound.
		const std::int32_t top = gen.topSolidVoxels(x, z);
		if (top < 0 || top > gen.maxHeightVoxels()) {
			ok = false;
			continue;
		}
		const auto topType = gen.typeAt(x, top, z);
		if (topType != vv::voxel::VoxelType::Grass &&
			topType != vv::voxel::VoxelType::Sand &&
			topType != vv::voxel::VoxelType::Snow) {
			ok = false;
		}
	}
	check(ok, "terrain: density layering rules (air/stone/bedrock/top)");
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

void testVoxelPalette() {
	// The palette is what the compute shader reads for voxel colors; a layout
	// mismatch turns the world into colored noise (this test was added after
	// exactly that bug shipped).
	const std::vector<float> palette = vv::voxel::buildVoxelPalette();

	check(palette.size() == 3u * vv::voxel::kPaletteCapacity * 4u,
				"palette: total size (3 faces x capacity x vec4)");

	const auto entry = [&palette](std::uint32_t face, std::uint32_t type,
																std::uint32_t component) {
		return palette[(static_cast<std::size_t>(face) * vv::voxel::kPaletteCapacity +
										type) * 4u + component];
	};

	bool correct = true;
	for (std::uint32_t type = 0; type < vv::voxel::kVoxelTypeCount; ++type) {
		const auto& info = vv::voxel::kVoxelTypeInfo[type];
		for (std::uint32_t c = 0; c < 3; ++c) {
			if (entry(0, type, c) != info.top[c] ||
					entry(1, type, c) != info.side[c] ||
					entry(2, type, c) != info.bottom[c]) {
				correct = false;
			}
		}
		if (entry(0, type, 3) != 1.0f || entry(1, type, 3) != 1.0f ||
				entry(2, type, 3) != 1.0f) {
			correct = false;
		}
	}
	check(correct, "palette: per-type top/side/bottom entries and alpha");

	bool unusedZeroed = true;
	for (std::uint32_t type = vv::voxel::kVoxelTypeCount;
			 type < vv::voxel::kPaletteCapacity; ++type) {
		for (std::uint32_t face = 0; face < 3; ++face) {
			const std::size_t base =
					(static_cast<std::size_t>(face) * vv::voxel::kPaletteCapacity +
					 type) * 4u;
			for (std::uint32_t i = 0; i < 4; ++i) {
				if (palette[base + i] != 0.0f) {
					unusedZeroed = false;
				}
			}
		}
	}
	check(unusedZeroed, "palette: unused capacity entries are zero");
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

	// Canonical comparison: the chunk must equal a fresh
	// generateChunkVoxels fill (that path IS the generator now - exact
	// per-voxel typeAt walks cannot match the chunk by construction because
	// chunks sample the 3D noise on a coarse lattice).
	std::vector<std::uint8_t> expected;
	world.terrain().generateChunkVoxels(0, 0, 32, 32, 128, expected);
	check(expected.size() == chunk->voxelCount(),
				"chunk: generator fill covers the whole chunk");
	bool matches = true;
	for (std::uint32_t z = 0; z < 32 && matches; ++z) {
		for (std::uint32_t y = 0; y < 128 && matches; ++y) {
			for (std::uint32_t x = 0; x < 32 && matches; ++x) {
				if (chunk->get(x, y, z) !=
						static_cast<vv::voxel::VoxelType>(
								expected[std::size_t(x) + std::size_t(y) * 32 +
												 std::size_t(z) * 32 * 128])) {
					matches = false;
				}
			}
		}
	}
	check(matches, "chunk: contents match the terrain generator");

	// Structural zones must also agree with the exact typeAt (air above the
	// warp band, bedrock at the floor) - see testTerrainLayering.
	bool zonesOk = true;
	for (std::uint32_t z = 0; z < 32; z += 3) {
		for (std::uint32_t x = 0; x < 32; x += 3) {
			const std::int32_t xi = static_cast<std::int32_t>(x);
			const std::int32_t zi = static_cast<std::int32_t>(z);
			const float target =
					world.terrain().surfaceTargetF(float(xi), float(zi));
			const std::int32_t above =
					static_cast<std::int32_t>(std::ceil(target)) +
					world.terrain().maxWarpVoxels();
			if (above >= 0 && above < 128 &&
				chunk->get(x, static_cast<std::uint32_t>(above), z) !=
						vv::voxel::VoxelType::Air) {
				zonesOk = false;
			}
			if (chunk->get(x, 0, z) != vv::voxel::VoxelType::Bedrock) {
				zonesOk = false;
			}
		}
	}
	check(zonesOk, "chunk: structural zones match the generator guarantees");
}

void testChunkHeightMap() {
	// Ground-truth scan vs the chunk's lazy heightmap, including edge cases:
	// all-air column, bedrock-only column, edits marking the map dirty.
	vv::voxel::Chunk chunk(0, 0, 8, 16, 8);
	bool matches = true;
	for (std::uint32_t z = 0; z < 8; ++z) {
		for (std::uint32_t x = 0; x < 8; ++x) {
			// Independent scan from the top.
			std::uint16_t expected = 0;
			for (std::uint32_t y = 16; y-- > 0;) {
				if (chunk.get(x, y, z) != vv::voxel::VoxelType::Air) {
					expected = static_cast<std::uint16_t>(y + 1);
					break;
				}
			}
			if (chunk.heightMap()[x + z * 8] != expected) {
				matches = false;
			}
		}
	}
	check(matches, "heightmap: all-air chunk has zero heights");

	// Fill some columns and re-check (set() must invalidate the cache).
	chunk.set(2, 0, 2, vv::voxel::VoxelType::Bedrock);
	chunk.set(2, 5, 2, vv::voxel::VoxelType::Stone);
	chunk.set(3, 15, 3, vv::voxel::VoxelType::Snow);
	check(chunk.heightMap()[2 + 2 * 8] == 6,
				"heightmap: max solid y + 1 (bedrock + stone at 5)");
	check(chunk.heightMap()[3 + 3 * 8] == 16,
				"heightmap: top-of-world solid counts");
	check(chunk.heightMap()[0 + 0 * 8] == 0, "heightmap: air column stays 0");

	// u16 pair packing into u32 words (shader sync contract).
	const auto& words = chunk.heightMapWords();
	check(words.size() == chunk.heightMapWordStride(),
				"heightmap: word stride is (sizeX*sizeZ+1)/2");
	bool packed = true;
	for (std::uint32_t z = 0; z < 8; ++z) {
		for (std::uint32_t x = 0; x < 8; ++x) {
			const std::size_t i = x + z * 8;
			const std::uint32_t w = words[i >> 1];
			const std::uint32_t h = ((i & 1u) == 0u) ? (w & 0xFFFFu) : (w >> 16u);
			if (h != chunk.heightMap()[i]) {
				packed = false;
			}
		}
	}
	check(packed, "heightmap: u16 pairs packed two-per-u32");

	// Block max-height atlas (pass 30): the 8x8x16 test chunk is exactly
	// ONE block, so its bound = the max column bound; set() must
	// invalidate the cache.
	check(chunk.blockHeightWordStride() == 1,
			"blockmap: 8x8 chunk has a single 1-word block slot");
	check(chunk.blockHeightMap().size() == 1,
			"blockmap: 8x8 chunk has exactly one block");
	check(chunk.blockHeightMap()[0] == 16,
			"blockmap: single block bound is the max column bound");
	{
		const std::uint32_t w0 = chunk.blockHeightMapWords()[0];
		check((w0 & 0xFFFFu) == 16u && (w0 >> 16u) == 0u,
				"blockmap: block 0 packed in the low half, high half 0");
	}
	chunk.set(4, 15, 4, vv::voxel::VoxelType::Stone);
	check(chunk.heightMap()[4 + 4 * 8] == 16 &&
			chunk.blockHeightMap()[0] == 16,
			"blockmap: set() re-dirties the block map (still 16)");
	chunk.set(4, 7, 4, vv::voxel::VoxelType::Air);
	check(chunk.blockHeightMap()[0] == 16,
			"blockmap: clearing the top solid keeps the block bound");

	// A chunk whose size is NOT a multiple of the block size rounds UP
	// the block grid (9 columns -> 2 blocks per axis).
	{
		vv::voxel::Chunk odd(0, 0, 9, 16, 9);
		odd.set(8, 4, 8, vv::voxel::VoxelType::Stone);
		check(odd.blockHeightWordStride() == 2,
				"blockmap: 9x9 chunk rounds up to 2x2 blocks (2 words)");
		check(odd.blockHeightMap().size() == 4,
				"blockmap: 9x9 chunk has 4 block entries");
		check(odd.blockHeightMap()[1 + 1 * 2] == 5,
				"blockmap: corner block holds the max of its columns");
		check(odd.blockHeightMap()[0] == 0,
				"blockmap: empty block stays 0");
	}

	// Generated chunks: heightmap equals an independent scan of their data.
	vv::voxel::World world(vv::terrain::TerrainConfig{}, 32, 128, 32);
	std::vector<const vv::voxel::Chunk*> created;
	std::vector<vv::voxel::ChunkCoord> evicted;
	world.ensureRegion(0, 0, 1, created, evicted);
	bool genMatches = true;
	for (const vv::voxel::Chunk* c : created) {
		for (std::uint32_t z = 0; z < 32 && genMatches; ++z) {
			for (std::uint32_t x = 0; x < 32 && genMatches; ++x) {
				std::uint16_t expected = 0;
				for (std::uint32_t y = 128; y-- > 0;) {
					if (c->get(x, y, z) != vv::voxel::VoxelType::Air) {
						expected = static_cast<std::uint16_t>(y + 1);
						break;
					}
				}
				if (c->heightMap()[x + z * 32] != expected) {
					genMatches = false;
				}
			}
		}
	}
	check(genMatches, "heightmap: generated chunks match ground-truth scan");

	// Block atlas on generated 32x32 chunks: 4x4 blocks, 8 words, each
	// entry = max over its 64 columns, packed two per u32 (even block
	// index -> low half).
	bool blockOk = true;
	for (const vv::voxel::Chunk* c : created) {
		check(c->blockHeightWordStride() == 8,
				"blockmap: 32x32 chunk slot is 8 words (16 blocks)");
		for (std::uint32_t bz = 0; bz < 4 && blockOk; ++bz) {
			for (std::uint32_t bx = 0; bx < 4 && blockOk; ++bx) {
				std::uint16_t expected = 0;
				for (std::uint32_t z = bz * 8; z < bz * 8 + 8; ++z) {
					for (std::uint32_t x = bx * 8; x < bx * 8 + 8; ++x) {
						expected = std::max(
							expected,
							c->heightMap()[std::size_t(x) +
							               std::size_t(z) * 32]);
					}
				}
				const std::uint32_t blk = bx + bz * 4;
				const std::uint32_t w =
					c->blockHeightMapWords()[std::size_t(blk >> 1)];
				const std::uint32_t got =
					((blk & 1u) == 0u) ? (w & 0xFFFFu) : (w >> 16u);
				if (got != expected ||
					c->blockHeightMap()[std::size_t(blk)] != expected) {
					blockOk = false;
				}
			}
		}
	}
	check(blockOk,
			"blockmap: generated chunk blocks are column maxima, packed");
}

// ---------------------------------------------------------------------------
// Traversal parity: CPU mirrors of the old (pass 2.5) 3D DDA and the new
// heightmap-guided column traversal, run over the same synthetic worlds.
// The new algorithm must find exactly the same hit cell, hit t, entry face
// and voxel type for every ray - it is a pure performance rewrite.
// ---------------------------------------------------------------------------


struct RayHit {
	bool hit = false;
	double t = 0.0;
	int axis = -1;      // entry-face axis: 0=x, 1=y, 2=z, -1=segment start
	int sign = 0;       // face normal sign on that axis
	int cell[3] = {0, 0, 0};
	std::uint8_t type = 0;
};

struct TestWorld {
	int wx = 64, wz = 64, wh = 48;
	std::vector<std::uint8_t> cells;    // x + y*wx + z*wx*wh
	std::vector<std::uint16_t> heights; // max solid + 1, 0xFFFF = no data
	// Per 8x8 block of columns: the MAX column bound in the block (pass
	// 30 hierarchical DDA mirror; same semantics as the shader's
	// BlockHeights atlas). 0xFFFF = no data.
	std::vector<std::uint16_t> blockBounds;

	std::uint8_t at(int x, int y, int z) const {
		if (x < 0 || x >= wx || y < 0 || y >= wh || z < 0 || z >= wz) {
			return 0;
		}
		return cells[static_cast<std::size_t>(x) +
								 static_cast<std::size_t>(y) * wx +
								 static_cast<std::size_t>(z) * wx * wh];
	}
	std::uint16_t boundAt(int x, int z) const {
		if (x < 0 || x >= wx || z < 0 || z >= wz) {
			return 0xFFFFu;
		}
		return heights[static_cast<std::size_t>(x) +
									 static_cast<std::size_t>(z) * wx];
	}
	void recomputeHeights() {
		heights.assign(static_cast<std::size_t>(wx) * wz, 0);
		for (int z = 0; z < wz; ++z) {
			for (int x = 0; x < wx; ++x) {
				for (int y = wh; y-- > 0;) {
					if (at(x, y, z) != 0) {
						heights[x + z * wx] = static_cast<std::uint16_t>(y + 1);
						break;
					}
				}
			}
		}
		// Block maxima over the 8x8 grid (the test world models one big
		// chunk, so the grid aligns with the world origin like the
		// shader's per-chunk local grid).
		const int bx = (wx + 7) / 8;
		const int bz = (wz + 7) / 8;
		blockBounds.assign(static_cast<std::size_t>(bx) * bz, 0);
		for (int bzI = 0; bzI < bz; ++bzI) {
			for (int bxI = 0; bxI < bx; ++bxI) {
				std::uint16_t m = 0;
				for (int z = bzI * 8; z < std::min((bzI + 1) * 8, wz); ++z) {
					for (int x = bxI * 8; x < std::min((bxI + 1) * 8, wx); ++x) {
						m = std::max(m, heights[x + z * wx]);
					}
				}
				blockBounds[bxI + bzI * bx] = m;
			}
		}
	}
	// Max bound over the 8x8 block containing (x, z); 0xFFFF outside.
	std::uint16_t blockBoundAt(int x, int z) const {
		const int bx = (wx + 7) / 8;
		const int bz = (wz + 7) / 8;
		const int bxI = x / 8;
		const int bzI = z / 8;
		if (bxI < 0 || bxI >= bx || bzI < 0 || bzI >= bz) {
			return 0xFFFFu;
		}
		return blockBounds[static_cast<std::size_t>(bxI) +
		                   static_cast<std::size_t>(bzI) * bx];
	}
};

// Old algorithm: plain 3D DDA, one cell per step (mirror of pass 2.5 shader).
RayHit traceOld(const TestWorld& w, const double ro[3], const double rd[3],
								double tEnd, int budget) {
	RayHit out;
	double start[3] = {ro[0], ro[1], ro[2]};
	for (int a = 0; a < 3; ++a) {
		start[a] += rd[a] * 1e-4;  // same nudge as the shader
	}
	int cell[3] = {int(std::floor(start[0])), int(std::floor(start[1])),
								 int(std::floor(start[2]))};
	int step[3];
	double tMax[3], tDelta[3];
	for (int a = 0; a < 3; ++a) {
		step[a] = (rd[a] > 0.0) ? 1 : -1;
		tMax[a] = 1e30;
		tDelta[a] = 1e30;
		if (std::abs(rd[a]) > 1e-6) {
			const double next = double(cell[a] + ((step[a] > 0) ? 1 : 0));
			tMax[a] = (next - start[a]) / rd[a];
			tDelta[a] = std::abs(1.0 / rd[a]);
		} else {
			step[a] = 0;
		}
	}
	int lastAxis = -1;
	double t = 0.0;
	for (int i = 0; i < budget; ++i) {
		const std::uint8_t type = w.at(cell[0], cell[1], cell[2]);
		if (type != 0) {
			out.hit = true;
			out.t = t;
			out.axis = lastAxis;
			out.sign = (lastAxis < 0) ? 0 : -step[lastAxis];
			out.cell[0] = cell[0];
			out.cell[1] = cell[1];
			out.cell[2] = cell[2];
			out.type = type;
			return out;
		}
		double nextT = 0.0;
		int axis = 2;
		if (tMax[0] < tMax[1] && tMax[0] < tMax[2]) {
			nextT = tMax[0];
			tMax[0] += tDelta[0];
			cell[0] += step[0];
			axis = 0;
		} else if (tMax[1] < tMax[2]) {
			nextT = tMax[1];
			tMax[1] += tDelta[1];
			cell[1] += step[1];
			axis = 1;
		} else {
			nextT = tMax[2];
			tMax[2] += tDelta[2];
			cell[2] += step[2];
			axis = 2;
		}
		lastAxis = axis;
		if (nextT > tEnd) {
			break;
		}
		t = nextT;
	}
	return out;
}

// New algorithm: 2D column DDA + height bound skip + bounded cell walk
// (mirror of the current shader, including tie-breaking and clamps).
RayHit traceNew(const TestWorld& w, const double ro[3], const double rd[3],
								double tEnd, int budget) {
	RayHit out;
	double start[3] = {ro[0], ro[1], ro[2]};
	for (int a = 0; a < 3; ++a) {
		start[a] += rd[a] * 1e-4;
	}
	int cell[3] = {int(std::floor(start[0])), int(std::floor(start[1])),
								 int(std::floor(start[2]))};
	int step[2] = {(rd[0] > 0.0) ? 1 : -1, (rd[2] > 0.0) ? 1 : -1};
	double tMax[2], tDelta[2];
	for (int a = 0; a < 2; ++a) {
		const int axis = (a == 0) ? 0 : 2;
		tMax[a] = 1e30;
		tDelta[a] = 1e30;
		if (std::abs(rd[axis]) > 1e-6) {
			const double next = double(cell[axis] + ((step[a] > 0) ? 1 : 0));
			tMax[a] = (next - start[axis]) / rd[axis];
			tDelta[a] = std::abs(1.0 / rd[axis]);
		} else {
			step[a] = 0;
		}
	}
	const int worldTop = w.wh - 1;
	int lastAxis = -1;
	double t = 0.0;
	for (int i = 0; i < budget; ++i) {
		if (t >= tEnd) {
			break;
		}
		const double tColExit = std::min(std::min(tMax[0], tMax[1]), tEnd);
		const double y0 = start[1] + rd[1] * t;
		const double y1 = start[1] + rd[1] * tColExit;
		const double yMin = std::min(y0, y1);
		const std::uint16_t bound = w.boundAt(cell[0], cell[2]);
		const bool skip = (bound != 0xFFFFu) && (yMin >= double(bound));
		if (!skip) {
			int yFirst = std::min(int(std::floor(y0)), worldTop);
			if (bound != 0xFFFFu) {
				yFirst = std::min(yFirst, int(bound) - 1);
			}
			const int yLast = int(std::floor(y1));
			int yHit = -1;
			bool entryCell = false;
			if (rd[1] < 0.0) {
				for (int y = yFirst; y >= std::max(yLast, 0); --y) {
					if (w.at(cell[0], y, cell[2]) != 0) {
						yHit = y;
						entryCell = (y == int(std::floor(y0)));
						break;
					}
				}
			} else {
				const int yTop = std::min(yLast, worldTop);
				for (int y = std::max(yFirst, 0); y <= yTop; ++y) {
					if (bound != 0xFFFFu && y >= int(bound)) {
						break;
					}
					if (w.at(cell[0], y, cell[2]) != 0) {
						yHit = y;
						entryCell = (y == int(std::floor(y0)));
						break;
					}
				}
			}
			if (yHit >= 0) {
				out.hit = true;
				out.cell[0] = cell[0];
				out.cell[1] = yHit;
				out.cell[2] = cell[2];
				out.type = w.at(cell[0], yHit, cell[2]);
				if (entryCell) {
					out.t = t;
					out.axis = lastAxis;
					out.sign = (lastAxis < 0) ? 0
								: (lastAxis == 0) ? -step[0] : -step[1];
				} else if (rd[1] < 0.0) {
					out.t = (double(yHit + 1) - start[1]) / rd[1];
					out.axis = 1;
					out.sign = 1;   // top face
				} else {
					out.t = (double(yHit) - start[1]) / rd[1];
					out.axis = 1;
					out.sign = -1;  // bottom face
				}
				return out;
			}
		}
		if (tMax[0] < tMax[1]) {
			t = tMax[0];
			tMax[0] += tDelta[0];
			cell[0] += step[0];
			lastAxis = 0;
		} else {
			t = tMax[1];
			tMax[1] += tDelta[1];
			cell[2] += step[1];
			lastAxis = 2;
		}
	}
	return out;
}

static inline int bFloorDiv(int a, int b) {
	return (a >= 0) ? (a / b) : -((-a + b - 1) / b);
}

// Pass 32: the shader's floorDiv takes a power-of-two fast path
// (a >> log2(b) instead of '/' plus '%'), relying on arithmetic right
// shift being floor division for two's-complement integers (SPIR-V
// OpShiftRightArithmetic is sign-extending by spec). Pin that identity
// on the CPU mirror for every divisor the shader uses.
static void testFloorDivShiftIdentity() {
	bool ok = true;
	for (int shift = 1; shift <= 5; ++shift) {  // divisors 2, 4, 8, 16, 32
		const int b = 1 << shift;
		for (int a = -70000; a <= 70000; ++a) {
			if ((a >> shift) != bFloorDiv(a, b)) {
				ok = false;
				break;
			}
		}
	}
	check(ok, "floordiv: pow2 arithmetic shift == floor division");
}

struct TraceStats {
	int columns = 0;         // per-column iterations actually run
	int blockSkips = 0;      // whole-block skips taken
	int skippedColumns = 0;  // DDA crossings inside skips (old cost)
};

// Hierarchical algorithm (pass 30): traceNew + the whole-block air-skip
// over the 8x8 block max heights (mirror of the shader's BlockHeights
// path, including the fast-forward, block-exit resume and the ulp-safety
// re-entry guard). Must produce identical hits to traceOld/traceNew.
RayHit traceHier(const TestWorld& w, const double ro[3], const double rd[3],
							double tEnd, int budget, TraceStats* stats = nullptr) {
	RayHit out;
	double start[3] = {ro[0], ro[1], ro[2]};
	for (int a = 0; a < 3; ++a) {
		start[a] += rd[a] * 1e-4;
	}
	int cell[3] = {int(std::floor(start[0])), int(std::floor(start[1])),
								 int(std::floor(start[2]))};
	int step[2] = {(rd[0] > 0.0) ? 1 : -1, (rd[2] > 0.0) ? 1 : -1};
	double tMax[2], tDelta[2];
	for (int a = 0; a < 2; ++a) {
		const int axis = (a == 0) ? 0 : 2;
		tMax[a] = 1e30;
		tDelta[a] = 1e30;
		if (std::abs(rd[axis]) > 1e-6) {
			const double next = double(cell[axis] + ((step[a] > 0) ? 1 : 0));
			tMax[a] = (next - start[axis]) / rd[axis];
			tDelta[a] = std::abs(1.0 / rd[axis]);
		} else {
			step[a] = 0;
		}
	}
	const int worldTop = w.wh - 1;
	int lastAxis = -1;
	double t = 0.0;
	int curBX = -0x40000000;  // current block (never a real block id)
	int curBZ = -0x40000000;
	bool curBlockSkippable = false;
	double tBlockExitCache = 0.0;
	for (int i = 0; i < budget; ++i) {
		if (t >= tEnd) {
			break;
		}
		const double tColExit = std::min(std::min(tMax[0], tMax[1]), tEnd);
		const double y0 = start[1] + rd[1] * t;
		const double y1 = start[1] + rd[1] * tColExit;
		const double yMin = std::min(y0, y1);
		// --- Hierarchical block skip (mirror of the shader) ---
		const int bx = bFloorDiv(cell[0], 8);
		const int bz = bFloorDiv(cell[2], 8);
		if (bx != curBX || bz != curBZ) {
			curBX = bx;
			curBZ = bz;
			double tBlockExit = tEnd;
			if (std::abs(rd[0]) > 1e-6) {
				const double edgeX =
					double((bx + ((step[0] > 0) ? 1 : 0)) * 8);
				tBlockExit = std::min(tBlockExit, (edgeX - start[0]) / rd[0]);
			}
			if (std::abs(rd[2]) > 1e-6) {
				const double edgeZ =
					double((bz + ((step[1] > 0) ? 1 : 0)) * 8);
				tBlockExit = std::min(tBlockExit, (edgeZ - start[2]) / rd[2]);
			}
			const std::uint16_t blockMax = w.blockBoundAt(cell[0], cell[2]);
			const double yBExit = start[1] + rd[1] * tBlockExit;
			curBlockSkippable = (blockMax != 0xFFFFu) &&
				(std::min(y0, yBExit) >= double(blockMax));
			tBlockExitCache = tBlockExit;
		}
		if (curBlockSkippable) {
			if (stats) {
				++stats->blockSkips;
			}
			for (int g = 0; g < 2 * 8 + 2; ++g) {
				const double tNext = std::min(tMax[0], tMax[1]);
				if (tNext > tBlockExitCache) {
					break;
				}
				t = tNext;
				const bool ffx = tMax[0] < tMax[1];
				tMax[0] += ffx ? tDelta[0] : 0.0;
				tMax[1] += ffx ? 0.0 : tDelta[1];
				cell[0] += ffx ? step[0] : 0;
				cell[2] += ffx ? 0 : step[1];
				lastAxis = ffx ? 0 : 2;
				if (stats) {
					++stats->skippedColumns;
				}
			}
			if (t < tBlockExitCache) {
				t = tBlockExitCache;
			}
			if (bFloorDiv(cell[0], 8) == curBX &&
				bFloorDiv(cell[2], 8) == curBZ) {
				curBlockSkippable = false;
			}
			continue;
		}
		if (stats) {
			++stats->columns;
		}
		const std::uint16_t bound = w.boundAt(cell[0], cell[2]);
		const bool skip = (bound != 0xFFFFu) && (yMin >= double(bound));
		if (!skip) {
			int yFirst = std::min(int(std::floor(y0)), worldTop);
			if (bound != 0xFFFFu) {
				yFirst = std::min(yFirst, int(bound) - 1);
			}
			const int yLast = int(std::floor(y1));
			int yHit = -1;
			bool entryCell = false;
			if (rd[1] < 0.0) {
				for (int y = yFirst; y >= std::max(yLast, 0); --y) {
					if (w.at(cell[0], y, cell[2]) != 0) {
						yHit = y;
						entryCell = (y == int(std::floor(y0)));
						break;
					}
				}
			} else {
				const int yTop = std::min(yLast, worldTop);
				for (int y = std::max(yFirst, 0); y <= yTop; ++y) {
					if (bound != 0xFFFFu && y >= int(bound)) {
						break;
					}
					if (w.at(cell[0], y, cell[2]) != 0) {
						yHit = y;
						entryCell = (y == int(std::floor(y0)));
						break;
					}
				}
			}
			if (yHit >= 0) {
				out.hit = true;
				out.cell[0] = cell[0];
				out.cell[1] = yHit;
				out.cell[2] = cell[2];
				out.type = w.at(cell[0], yHit, cell[2]);
				if (entryCell) {
					out.t = t;
					out.axis = lastAxis;
					out.sign = (lastAxis < 0) ? 0
								: (lastAxis == 0) ? -step[0] : -step[1];
				} else if (rd[1] < 0.0) {
					out.t = (double(yHit + 1) - start[1]) / rd[1];
					out.axis = 1;
					out.sign = 1;   // top face
				} else {
					out.t = (double(yHit) - start[1]) / rd[1];
					out.axis = 1;
					out.sign = -1;  // bottom face
				}
				return out;
			}
		}
		if (tMax[0] < tMax[1]) {
			t = tMax[0];
			tMax[0] += tDelta[0];
			cell[0] += step[0];
			lastAxis = 0;
		} else {
			t = tMax[1];
			tMax[1] += tDelta[1];
			cell[2] += step[1];
			lastAxis = 2;
		}
	}
	return out;
}

void testTraversalParity() {
	// World 1: pure heightfield (rolling hills) - the common case.
	// World 2: heightfield + floating slabs + a carved hole + a wall -
	// overhang-ish content the conservative bound must still handle.
	for (int worldKind = 0; worldKind < 2; ++worldKind) {
		TestWorld w;
		w.cells.assign(std::size_t(w.wx) * w.wh * w.wz, 0);
		for (int z = 0; z < w.wz; ++z) {
			for (int x = 0; x < w.wx; ++x) {
				const double h = 20.0 + 8.0 * std::sin(x * 0.31) +
												 6.0 * std::cos(z * 0.23) +
												 3.0 * std::sin((x + z) * 0.11);
				const int top = int(std::floor(h));
				for (int y = 0; y <= top; ++y) {
					w.cells[std::size_t(x) + std::size_t(y) * w.wx +
									std::size_t(z) * w.wx * w.wh] = 1 + (y % 4);
				}
			}
		}
		if (worldKind == 1) {
			// Floating slab (overhang), a wall, and an all-air shaft.
			for (int z = 20; z < 28; ++z) {
				for (int x = 20; x < 40; ++x) {
					w.cells[std::size_t(x) + std::size_t(38) * w.wx +
									std::size_t(z) * w.wx * w.wh] = 5;
				}
			}
			for (int y = 0; y < 44; ++y) {
				for (int z = 0; z < w.wz; ++z) {
					w.cells[std::size_t(50) + std::size_t(y) * w.wx +
									std::size_t(z) * w.wx * w.wh] = 2;
				}
			}
			for (int y = 0; y < w.wh; ++y) {
				for (int z = 8; z < 12; ++z) {
					for (int x = 8; x < 12; ++x) {
						w.cells[std::size_t(x) + std::size_t(y) * w.wx +
										std::size_t(z) * w.wx * w.wh] = 0;
					}
				}
			}
		}
		w.recomputeHeights();

		// Rays: deterministic LCG origins/directions + targeted edge cases.
		std::uint64_t rng = 0x9e3779b97f4a7c15ull * (worldKind + 1);
		auto next01 = [&rng]() {
			rng ^= rng >> 12;
			rng ^= rng << 25;
			rng ^= rng >> 27;
			return double(rng >> 11) / double(1ull << 53);
		};
		int checked = 0;
		int mismatches = 0;
		long statColumns = 0;
		long statBlockSkips = 0;
		long statSkippedColumns = 0;
		for (int ray = 0; ray < 6000; ++ray) {
			double ro[3], rd[3];
			if (ray % 6 == 0) {
				// Targeted: look straight down / up / horizontal from
				// interesting heights.
				const double ys[] = {0.5, 21.0, 26.5, 39.0, 44.0, 47.5};
				ro[0] = next01() * w.wx;
				ro[1] = ys[ray / 6 % 6];
				ro[2] = next01() * w.wz;
				rd[0] = 0.0;
				rd[1] = (ray % 12 == 0) ? -1.0 : ((ray % 18 == 0) ? 1.0 : 0.0);
				rd[2] = 0.0;
				if (rd[1] == 0.0) {
					rd[0] = 1.0;
				}
			} else if (ray % 6 == 1) {
				// Diagonals from high above.
				ro[0] = next01() * w.wx;
				ro[1] = 47.5;
				ro[2] = next01() * w.wz;
				rd[0] = 1.0;
				rd[1] = -1.0;
				rd[2] = 1.0;
			} else {
				ro[0] = next01() * w.wx;
				ro[1] = next01() * w.wh;
				ro[2] = next01() * w.wz;
				rd[0] = next01() * 2.0 - 1.0;
				rd[1] = next01() * 2.0 - 1.0;
				rd[2] = next01() * 2.0 - 1.0;
			}
			double len = std::sqrt(rd[0] * rd[0] + rd[1] * rd[1] +
														 rd[2] * rd[2]);
			if (len < 1e-6) {
				continue;
			}
			for (int a = 0; a < 3; ++a) {
				rd[a] /= len;
			}
			// tEnd: exit of the world box (both algorithms only trace
			// within it, like the shader within its region).
			double tEnd = 1e30;
			for (int a = 0; a < 3; ++a) {
				if (std::abs(rd[a]) < 1e-9) {
					continue;
				}
				for (const double boundPlane : {0.0, double((a == 1) ? w.wh : (a == 0 ? w.wx : w.wz))}) {
					const double tp = (boundPlane - ro[a]) / rd[a];
					if (tp > 0.0) {
						tEnd = std::min(tEnd, tp);
					}
				}
			}
			if (tEnd <= 0.0 || tEnd > 300.0) {
				tEnd = 300.0;
			}
			const RayHit a = traceOld(w, ro, rd, tEnd, 4096);
			const RayHit b = traceNew(w, ro, rd, tEnd, 4096);
			TraceStats st;
			const RayHit c = traceHier(w, ro, rd, tEnd, 4096, &st);
			++checked;
			statColumns += st.columns;
			statBlockSkips += st.blockSkips;
			statSkippedColumns += st.skippedColumns;
			const bool equal = (a.hit == b.hit) && (a.hit == c.hit) &&
				(!a.hit ||
				 (a.cell[0] == b.cell[0] && a.cell[1] == b.cell[1] &&
				  a.cell[2] == b.cell[2] && a.type == b.type &&
				  a.axis == b.axis && a.sign == b.sign &&
				  std::abs(a.t - b.t) < 1e-9)) &&
				(!a.hit ||
				 (a.cell[0] == c.cell[0] && a.cell[1] == c.cell[1] &&
				  a.cell[2] == c.cell[2] && a.type == c.type &&
				  a.axis == c.axis && a.sign == c.sign &&
				  std::abs(a.t - c.t) < 1e-9));
			if (!equal) {
				if (++mismatches <= 3) {
					std::printf("FAIL parity w%d ray %d: old(h=%d c=%d,%d,%d "
											"t=%.4f ax=%d s%d) new(h=%d c=%d,%d,%d "
											"t=%.4f ax=%d s%d)\n",
											worldKind, ray, int(a.hit), a.cell[0],
											a.cell[1], a.cell[2], a.t, a.axis,
											a.sign, int(b.hit), b.cell[0],
											b.cell[1], b.cell[2], b.t, b.axis,
											b.sign);
				}
			}
		}
		check(mismatches == 0,
					"traversal: column DDA parity with 3D DDA (heightfield + "
					"overhang content)");
		check(checked > 5000, "traversal: parity actually exercised");
		std::printf(
			"parity world %d: %d rays checked, %ld column iterations, "
			"%ld block skips covering %ld columns\n",
			worldKind, checked, statColumns, statBlockSkips,
			statSkippedColumns);
		// The hierarchical skip must actually fire on these scenes
		// (otherwise the mirror would be vacuously identical).
		check(statBlockSkips > 1000,
				"traversal: hierarchical block skip actually exercised");
	}
}

// CPU mirror of the shader's vertexAO (ported from the user's WGSL):
// verifies the truth table the AO look depends on.
void testVertexAO() {
	auto vertexAO = [](bool side1, bool side2, bool corner) {
		if (side1 && side2) {
			return 0.0f;
		}
		return 1.0f - ((side1 ? 1.0f : 0.0f) + (side2 ? 1.0f : 0.0f) +
									 (corner ? 1.0f : 0.0f)) / 3.0f;
	};
	check(vertexAO(false, false, false) == 1.0f, "ao: open corner is 1");
	check(std::abs(vertexAO(true, false, false) - 2.0f / 3.0f) < 1e-6f,
				"ao: one side is 2/3");
	check(std::abs(vertexAO(false, false, true) - 2.0f / 3.0f) < 1e-6f,
				"ao: corner only is 2/3");
	check(std::abs(vertexAO(true, false, true) - 1.0f / 3.0f) < 1e-6f,
				"ao: side+corner is 1/3");
	check(vertexAO(true, true, false) == 0.0f, "ao: two sides fully occluded");
	check(vertexAO(true, true, true) == 0.0f,
				"ao: two sides ignore the corner");
	// Bilinear blend at the face center = mean of the four corners (the
	// calculateAO weights at localPos = (0.5, 0.5)).
	const float ao00 = vertexAO(false, false, false);
	const float ao10 = vertexAO(true, false, false);
	const float ao01 = vertexAO(false, true, false);
	const float ao11 = vertexAO(true, true, false);
	const float center = ao00 * 0.25f + ao10 * 0.25f + ao01 * 0.25f +
											 ao11 * 0.25f;
	check(std::abs(center - (1.0f + 2.0f / 3.0f + 2.0f / 3.0f + 0.0f) / 4.0f) <
					1e-6f,
				"ao: bilinear center is the corner mean");
}

// Far-LOD field: builder conventions + determinism.
void testFarField() {
	const vv::terrain::TerrainGenerator gen(vv::terrain::TerrainConfig{});
	const std::uint32_t chunk = 32;
	const std::uint32_t cell = 4;
	const std::uint32_t radius = 2;  // dim = 2*2*32/4 = 32
	const auto field = vv::terrain::FarField::build(gen, 0, 0, radius, cell,
																									chunk);
	check(field.dim == 32, "far: dim = 2*radius*chunk/cell");
	check(field.cellVoxels == cell, "far: cell size stored");
	check(field.cells.size() == 32u * 32u, "far: cell count = dim^2");
	// Box centered on the world-aligned snap of chunk (0,0)'s center
	// voxel (16,16 -> 0,0; cells keep their world alignment across
	// recenters - see FarField::build).
	check(field.originVoxX == 0 - 64 && field.originVoxZ == 0 - 64,
				"far: origin = snapped center - dim*cell/2");

	// Incremental recenter: a build seeded with the previous window must
	// produce EXACTLY the same cells as a fresh build (window reuse copies,
	// exposed strips recompute - values must not diverge).
	{
		// Center chunk (17, 0): its center voxel (560, 16) snaps to a
		// DIFFERENT 512-cell than (16, 16) -> 0, so the seeded build
		// exercises a REAL window shift (128 cells). (The pass-14 version
		// used center (3,2) - same snap cell, shift 0 - which let a sign
		// error in the copy index pass vacuously: every recenter showed
		// the old terrain shifted twice = "two different worlds".)
		auto fresh = vv::terrain::FarField::build(gen, 17, 0, 2, cell, chunk);
		auto seeded = vv::terrain::FarField::build(gen, 17, 0, 2, cell, chunk,
																							 &field);
		check(fresh.originVoxX != field.originVoxX,
					"far: recenter test actually shifts the window");
		check(fresh.dim == seeded.dim && fresh.originVoxX == seeded.originVoxX &&
						fresh.originVoxZ == seeded.originVoxZ,
					"far: incremental recenter geometry matches");
		bool same = fresh.cells.size() == seeded.cells.size();
		for (std::size_t k = 0; same && k < fresh.cells.size(); ++k) {
				same = fresh.cells[k] == seeded.cells[k];
		}
		check(same, "far: incremental recenter cells identical to fresh build");
	}

	bool heightsOk = true;
	bool typesOk = true;
	for (std::uint32_t j = 0; j < field.dim; ++j) {
		for (std::uint32_t i = 0; i < field.dim; ++i) {
			const std::uint32_t packed =
					field.cells[i + j * field.dim];
			const float wx =
					float(field.originVoxX + int(i * cell) + int(cell / 2));
			const float wz =
					float(field.originVoxZ + int(j * cell) + int(cell / 2));
			const float target = gen.surfaceTargetF(wx, wz);
			const float mask = gen.mountainMaskF(wx, wz);
			const std::int32_t top =
					gen.estimatedTopSolid(wx, wz, target, mask);
			if (int(packed & 0xFFFFu) != top + 1) {
				heightsOk = false;
			}
			if ((packed >> 16u) !=
				std::uint32_t(gen.typeForDepth(top, top))) {
				typesOk = false;
			}
			// The estimate must stay within the warp band of the target and
			// under the solid bound (seam consistency with the near region).
			if (top > gen.maxHeightVoxels() ||
				float(top) > target + float(gen.maxWarpVoxels()) || top < 0) {
				heightsOk = false;
			}
		}
	}
	check(heightsOk, "far: height = estimated top solid + 1 per cell");
	check(typesOk, "far: type = surface type at cell center");

	const auto again = vv::terrain::FarField::build(gen, 0, 0, radius, cell,
																										chunk);
	check(again.cells == field.cells && again.originVoxX == field.originVoxX,
				"far: deterministic rebuild");
}


// ---------------------------------------------------------------------------
// Density terrain: mountains and overhangs. The 3D warp must actually fold
// the surface (air gaps under solids) inside mountain ranges, every column
// must keep ground (no floating islands - the saturating height term), and
// the solid bound must hold everywhere sampled.
// ---------------------------------------------------------------------------

void testTerrainOverhangs() {
	const vv::terrain::TerrainGenerator gen(testTerrainConfig());

	// Find a mountain core deterministically (seed 1337): scan a broad area
	// for a strong mask, then examine a chunk around it.
	float bestMask = 0.0f;
	std::int32_t bx = 0, bz = 0;
	for (std::int32_t z = -1024; z <= 1024; z += 16) {
		for (std::int32_t x = -1024; x <= 1024; x += 16) {
			const float m = gen.mountainMaskF(float(x), float(z));
			if (m > bestMask) {
				bestMask = m;
				bx = x;
				bz = z;
			}
		}
	}
	check(bestMask > 0.7f, "terrain: a mountain core exists near the origin");
	check(gen.surfaceTargetF(float(bx), float(bz)) >=
					gen.heightAtF(float(bx), float(bz)) + 0.8f *
							float(gen.config().mountainLift),
				"terrain: mountain core target is lifted");

	// Generate the chunk at the core (snapped to chunk-local coordinates:
	// pass absolute voxel coordinates; the generator is coordinate-absolute).
	const std::int32_t baseX = bx & ~31;
	const std::int32_t baseZ = bz & ~31;
	// "Tall" = clearly lifted above the rolling base at the core (the
	// absolute threshold from the first pass assumed the old lift/ceiling).
	const int tallThreshold = static_cast<int>(
			gen.heightAtF(float(bx), float(bz)) +
			0.5f * float(gen.config().mountainLift));
	std::vector<std::uint8_t> types;
	gen.generateChunkVoxels(baseX, baseZ, 32, 32, 128, types);
	const auto solidAt = [&](std::uint32_t x, std::uint32_t y,
													 std::uint32_t z) {
		return types[std::size_t(x) + std::size_t(y) * 32 +
							 std::size_t(z) * 32 * 128] !=
					 static_cast<std::uint8_t>(vv::voxel::VoxelType::Air);
	};

	std::size_t overhangColumns = 0;
	std::size_t tallColumns = 0;
	bool groundEverywhere = true;
	bool boundOk = true;
	for (std::uint32_t z = 0; z < 32; ++z) {
		for (std::uint32_t x = 0; x < 32; ++x) {
			// Topmost solid in the chunk column.
			std::int32_t top = -1;
			for (std::int32_t y = 127; y >= 0; --y) {
				if (solidAt(x, static_cast<std::uint32_t>(y), z)) {
					top = y;
					break;
				}
			}
			if (top > tallThreshold) {
				++tallColumns;
			}
			if (top > gen.maxHeightVoxels()) {
				boundOk = false;
			}
			// Ground continuity: solid deep below the target.
			const float target = gen.surfaceTargetF(
					float(baseX + static_cast<std::int32_t>(x)),
					float(baseZ + static_cast<std::int32_t>(z)));
			const std::int32_t ground =
					std::max<std::int32_t>(
							static_cast<std::int32_t>(std::floor(target)) - 60, 1);
			if (!solidAt(x, static_cast<std::uint32_t>(ground), z)) {
				groundEverywhere = false;
			}
			// Overhang: a gap strictly between two solids in the column.
			if (top >= 2) {
				bool inGap = false;
				for (std::int32_t y = top - 1; y >= 1; --y) {
					if (!solidAt(x, static_cast<std::uint32_t>(y), z)) {
					inGap = true;
				} else if (inGap) {
					++overhangColumns;
					break;
				}
			}
			}
		}
	}
	check(tallColumns > 0, "terrain: mountain chunk has tall columns");
	check(groundEverywhere, "terrain: every column keeps ground (no floats)");
	check(boundOk, "terrain: no solid above maxHeightVoxels()");
	check(overhangColumns > 0,
				"terrain: overhangs exist in the mountain chunk");
}


// ---------------------------------------------------------------------------
// Far seam patch: the far-LOD cells covered by loaded chunks must take the
// REAL per-column tops (exact max on full coverage, estimate as a floor on
// the partial edge), so the near/far seam continues the exact terrain.
// ---------------------------------------------------------------------------

void testFarPatchRegion() {
	const vv::terrain::TerrainGenerator gen(vv::terrain::TerrainConfig{});
	const std::uint32_t chunk = 32;
	const std::uint32_t cell = 4;
	// radius 2 -> dim 32; center (16,16) snaps to the world-aligned grid
	// (0,0), so origin = (-64, -64) (negative: exercises the
	// floor-division in the cell mapping).
	auto field = vv::terrain::FarField::build(gen, 0, 0, 2, cell, chunk);
	check(field.dim == 32 && field.originVoxX == -64,
				"far patch: field geometry");
	const std::vector<std::uint32_t> before = field.cells;

	// Fabricate 3x3 chunks of synthetic column tops covering voxels
	// [-32, 64) on both axes (a 96x96 block inside the field).
	std::vector<std::vector<std::uint16_t>> heights(9);
	std::vector<vv::terrain::FarField::RegionChunkHeights> chunks;
	for (int cz = -1; cz <= 1; ++cz) {
		for (int cx = -1; cx <= 1; ++cx) {
			auto& h = heights[static_cast<std::size_t>((cz + 1) * 3 + cx + 1)];
			h.resize(chunk * chunk);
			for (std::uint32_t z = 0; z < chunk; ++z) {
				for (std::uint32_t x = 0; x < chunk; ++x) {
					const std::int32_t vx = cx * 32 + static_cast<std::int32_t>(x);
					const std::int32_t vz = cz * 32 + static_cast<std::int32_t>(z);
					// Deterministic pattern with air columns and tall spikes.
					std::uint16_t top1;
					if ((vx * 31 + vz * 17) % 11 == 0) {
						top1 = 0;  // air column
					} else if ((vx % 7) == 3 && (vz % 5) == 2) {
						top1 = 90;  // spike
					} else {
						top1 = static_cast<std::uint16_t>(30 + (vx * vz) % 23);
					}
					h[x + z * chunk] = top1;
				}
			}
			chunks.push_back({cx * 32, cz * 32, chunk, chunk, h.data()});
		}
	}

	const std::size_t changed = vv::terrain::FarField::patchRegion(
				field.cells, field.dim, field.cellVoxels, field.originVoxX,
				field.originVoxZ, chunks, gen);
	check(changed > 0, "far patch: something changed");

	// Incremental semantics: re-patching with NO new chunks must change
	// nothing (the renderer only passes newly loaded chunks on region
	// swaps; interior cells must keep their exact values).
	{
		std::vector<vv::terrain::FarField::RegionChunkHeights> none;
		const std::size_t again = vv::terrain::FarField::patchRegion(
				field.cells, field.dim, field.cellVoxels, field.originVoxX,
				field.originVoxZ, none, gen);
		check(again == 0, "far patch: incremental no-op changes nothing");
	}
	// And the changed-index out-param reports exactly the cells written.
	{
		std::vector<std::uint32_t> indices;
		std::vector<vv::terrain::FarField::RegionChunkHeights> none;
		vv::terrain::FarField::patchRegion(
				field.cells, field.dim, field.cellVoxels, field.originVoxX,
				field.originVoxZ, none, gen, &indices);
		check(indices.empty(), "far patch: no-op reports no indices");
	}

	// Independent recomputation: per-cell (max covered column top, covered
	// column count) over the fabricated chunk block.
	const int dim = int(field.dim);
	const int blockMin = -32, blockMax = 64;  // covered voxel range (X and Z)
	auto cellStats = [&](int ci, int cj) {
		std::uint32_t m = 0;
		int inside = 0;
		for (int z = 0; z < int(cell); ++z) {
			for (int x = 0; x < int(cell); ++x) {
				const int vx = field.originVoxX + ci * int(cell) + x;
				const int vz = field.originVoxZ + cj * int(cell) + z;
				if (vx < blockMin || vx >= blockMax || vz < blockMin ||
						vz >= blockMax) {
					continue;
				}
				++inside;
				const int lcx = vx < 0 ? (vx - 31) / 32 : vx / 32;
				const int lcz = vz < 0 ? (vz - 31) / 32 : vz / 32;
				const auto& h =
						heights[static_cast<std::size_t>((lcz + 1) * 3 + lcx + 1)];
				m = std::max(m, std::uint32_t(h[(vx - lcx * 32) +
																	 (vz - lcz * 32) * 32]));
			}
		}
		return std::pair(m, inside);
	};

	bool fullOk = true;   // fully covered: exact max (or untouched if air)
	bool partialOk = true;  // partial edge: max(exact, estimate)
	bool floorOk = true;
	bool typeOk = true;
	bool outsideOk = true;
	for (int cj = 0; cj < dim; ++cj) {
		for (int ci = 0; ci < dim; ++ci) {
			const std::size_t idx = std::size_t(ci) + std::size_t(cj) * dim;
			const auto [expect, inside] = cellStats(ci, cj);
			const std::uint32_t got = field.cells[idx] & 0xFFFFu;
			const std::uint32_t est = before[idx] & 0xFFFFu;
			if (inside == 0) {
				if (field.cells[idx] != before[idx]) {
					outsideOk = false;
				}
				continue;
			}
			if (expect > 0 && got < expect) {
				floorOk = false;  // never below the covered columns' max
			}
			if (inside == int(cell) * int(cell)) {
				// Fully covered: exact value (all-air cells stay untouched).
				if (expect > 0 && got != expect) {
					fullOk = false;
				}
				if (expect == 0 && field.cells[idx] != before[idx]) {
					fullOk = false;
				}
			} else {
				if (got != std::max(expect, est)) {
					partialOk = false;
				}
			}
			if (expect > 0) {
				const auto type = gen.typeForDepth(int(got) - 1, int(got) - 1);
				if ((field.cells[idx] >> 16u) != std::uint32_t(type)) {
					typeOk = false;
				}
			}
		}
	}
	check(fullOk, "far patch: fully covered cells take the exact max");
	check(partialOk, "far patch: partial edge cells = max(exact, estimate)");
	check(floorOk, "far patch: never below the real column tops");
	check(typeOk, "far patch: surface type from the layering rule");
	check(outsideOk, "far patch: cells outside the region untouched");
}

// ---------------------------------------------------------------------------
// Far-LOD march: CPU mirror of the shader's far DDA vs a dense-sampling
// brute force over the same far grid. The march must find a hit exactly
// when the ray ever passes below a cell's surface top, at the right t.
// ---------------------------------------------------------------------------


struct FarGrid {
	int originX = 0, originZ = 0;
	unsigned dim = 0;
	double cell = 4.0;
	std::vector<std::uint32_t> cells;

	unsigned packedAt(int cx, int cz) const {
		if (cx < 0 || cz < 0 || cx >= int(dim) || cz >= int(dim)) {
			return 0u;
		}
		return cells[std::size_t(cx) + std::size_t(cz) * dim];
	}
};

// Mirror of the shader far march (same clamps, tie-breaks and budgets).
bool traceFarDDA(const FarGrid& g, const double ro[3], const double rd[3],
							 double tStart, double tEnd, double& outT) {
	const double EPS = 1e-6;
	const double maxTerrainY = 200.0;  // generous for synthetic heights
	int stepX = (rd[0] > 0.0) ? 1 : -1;
	int stepZ = (rd[2] > 0.0) ? 1 : -1;
	double tMaxX = 1e30, tMaxZ = 1e30, dX = 1e30, dZ = 1e30;

	const double px = ro[0] + rd[0] * tStart;
	const double pz = ro[2] + rd[2] * tStart;
	int cx = int(std::floor((px - g.originX) / g.cell));
	int cz = int(std::floor((pz - g.originZ) / g.cell));
	if (std::abs(rd[0]) > EPS) {
		const double next = g.originX +
				double(cx + ((stepX > 0) ? 1 : 0)) * g.cell;
		tMaxX = (next - ro[0]) / rd[0];
		dX = g.cell / std::abs(rd[0]);
	} else {
		stepX = 0;
	}
	if (std::abs(rd[2]) > EPS) {
		const double next = g.originZ +
				double(cz + ((stepZ > 0) ? 1 : 0)) * g.cell;
		tMaxZ = (next - ro[2]) / rd[2];
		dZ = g.cell / std::abs(rd[2]);
	} else {
		stepZ = 0;
	}

	double t = tStart;
	const int budget = int(2 * g.dim);
	for (int i = 0; i < budget; ++i) {
		if (t >= tEnd) {
			return false;
		}
		const double tExit = std::min(std::min(tMaxX, tMaxZ), tEnd);
		const double y0 = ro[1] + rd[1] * t;
		const double y1 = ro[1] + rd[1] * tExit;
		if (rd[1] > 0.0 && y0 >= maxTerrainY + 1.0) {
			return false;
		}
		const unsigned packed = g.packedAt(cx, cz);
		const double h = double(packed & 0xFFFFu);
		if (h > 0.0 && y0 < h) {
			outT = t;
			return true;
		}
		if (h > 0.0 && rd[1] < 0.0 && y1 < h) {
			outT = std::min(std::max((h - ro[1]) / rd[1], t), tExit);
			return true;
		}
		if (tMaxX < tMaxZ) {
			t = tMaxX;
			tMaxX += dX;
			cx += stepX;
		} else {
			t = tMaxZ;
			tMaxZ += dZ;
			cz += stepZ;
		}
	}
	return false;
}

// Brute force: dense sampling of the same discrete far field. Outside the
// grid there is NO data (no terrain beyond the far field), unlike inside
// where height 0 means an all-air column.
bool traceFarBrute(const FarGrid& g, const double ro[3], const double rd[3],
									 double tStart, double tEnd, double& outT) {
	const double dt = 0.02;
	for (double t = tStart; t < tEnd; t += dt) {
		const double x = ro[0] + rd[0] * t;
		const double z = ro[2] + rd[2] * t;
		const double y = ro[1] + rd[1] * t;
		const int cx = int(std::floor((x - g.originX) / g.cell));
		const int cz = int(std::floor((z - g.originZ) / g.cell));
		if (cx < 0 || cz < 0 || cx >= int(g.dim) || cz >= int(g.dim)) {
			continue;  // outside the far field: nothing to hit
		}
		const double h = double(g.packedAt(cx, cz) & 0xFFFFu);
		if (h > 0.0 && y < h) {
			outT = t;
			return true;
		}
	}
	return false;
}

void testFarMarch() {
	// Synthetic far grid: rolling heights 10..70 + a few towers/holes.
	FarGrid g;
	g.dim = 48;
	g.cells.assign(std::size_t(g.dim) * g.dim, 0u);
	for (unsigned j = 0; j < g.dim; ++j) {
		for (unsigned i = 0; i < g.dim; ++i) {
			double h = 30.0 + 15.0 * std::sin(i * 0.31) + 12.0 * std::cos(j * 0.23);
			unsigned type = 1u + ((i + j) % 5u);
			g.cells[std::size_t(i) + std::size_t(j) * g.dim] =
					(std::uint32_t(std::max(1.0, h)) & 0xFFFFu) | (type << 16u);
		}
	}
	for (unsigned i = 10; i < 16; ++i) {  // tower
		g.cells[std::size_t(i) + 20u * g.dim] = (120u) | (2u << 16u);
	}
	for (unsigned j = 30; j < 36; ++j) {  // hole (height 0 = air column)
		for (unsigned i = 30; i < 36; ++i) {
			g.cells[std::size_t(i) + std::size_t(j) * g.dim] = (2u << 16u);
		}
	}

	std::uint64_t rng = 0x243f6a8885a308d3ull;
	auto next01 = [&rng]() {
		rng ^= rng >> 12;
		rng ^= rng << 25;
		rng ^= rng >> 27;
		return double(rng >> 11) / double(1ull << 53);
	};

	int bothHit = 0, bothMiss = 0, mismatches = 0;
	for (int ray = 0; ray < 4000; ++ray) {
		double ro[3], rd[3];
		// Origins: inside the grid at varied heights, plus outside edges.
		ro[0] = (ray % 5 == 0) ? -20.0 : next01() * 192.0;
		ro[2] = (ray % 7 == 0) ? 220.0 : next01() * 192.0;
		ro[1] = next01() * 140.0;
		rd[0] = next01() * 2.0 - 1.0;
		rd[1] = (ray % 3 == 0) ? -1.0 : next01() * 2.0 - 1.0;
		rd[2] = next01() * 2.0 - 1.0;
		double len = std::sqrt(rd[0] * rd[0] + rd[1] * rd[1] +
													 rd[2] * rd[2]);
		if (len < 1e-6) {
			continue;
		}
		for (int a = 0; a < 3; ++a) {
			rd[a] /= len;
		}
		double tD = 0.0, tB = 0.0;
		const bool hitD = traceFarDDA(g, ro, rd, 0.0, 500.0, tD);
		const bool hitB = traceFarBrute(g, ro, rd, 0.0, 500.0, tB);
		if (hitD && hitB) {
			++bothHit;
			if (std::abs(tD - tB) > 0.5) {
				if (++mismatches <= 3) {
					std::printf("FAIL farmarch ray %d: dda t=%.4f brute t=%.4f\n",
											ray, tD, tB);
				}
			}
		} else if (!hitD && !hitB) {
			++bothMiss;
		} else {
			// Existence disagreement: only acceptable within a hair of the
			// sampling resolution (grazing corner cases).
			const double tOther = hitD ? tD : tB;
			if (tOther < 498.0 || std::abs(tD - tB) > 0.5) {
				if (++mismatches <= 3) {
					std::printf("FAIL farmarch ray %d: dda=%d(%.4f) brute=%d(%.4f)\n",
											ray, int(hitD), tD, int(hitB), tB);
				}
			}
		}
	}
	check(mismatches == 0, "far march: DDA matches dense brute force");
	check(bothHit > 1000 && bothMiss > 200,
				"far march: both outcomes well exercised");
	std::printf("far march: %d hit / %d miss rays agree\n", bothHit, bothMiss);
}


// Incremental-loading primitives: ensureChunk (idempotent generation) and
// evictOutside (Chebyshev radius eviction).
void testWorldEnsureChunk() {
	vv::voxel::World world(vv::terrain::TerrainConfig{}, 32, 128, 32);
	const vv::voxel::ChunkCoord coord{5, -3};
	const vv::voxel::Chunk* a = world.ensureChunk(coord);
	check(a != nullptr, "ensureChunk: generates and returns a chunk");
	check(world.cachedChunkCount() == 1, "ensureChunk: one cached chunk");
	const vv::voxel::Chunk* b = world.ensureChunk(coord);
	check(a == b, "ensureChunk: idempotent (same pointer, no regeneration)");
	check(world.cachedChunkCount() == 1,
				"ensureChunk: no duplicate cache entries");

	std::vector<vv::voxel::ChunkCoord> evicted;
	world.evictOutside(0, 0, 4, evicted);  // |5| > 4
	check(world.cachedChunkCount() == 0, "evictOutside: drops far chunks");
	check(evicted.size() == 1 && evicted[0] == coord,
				"evictOutside: reports the evicted coord");

	// Hysteresis parity with ensureRegion (whose steady state is checked in
	// testWorldRegion): evictOutside with r+1 keeps the +1 ring.
	std::vector<const vv::voxel::Chunk*> created;
	world.ensureRegion(0, 0, 2, created, evicted);
	check(world.cachedChunkCount() == 25, "region: 5x5 resident at r=2");
	world.evictOutside(0, 0, 3, evicted);  // r+1 hysteresis
	check(world.cachedChunkCount() == 25,
				"evictOutside: keeps everything within r+1");
	const vv::voxel::ChunkCoord inside{2, -2};
	world.ensureChunk(inside);
	world.ensureChunk(vv::voxel::ChunkCoord{6, 0});  // outside r+1
	world.evictOutside(0, 0, 3, evicted);
	check(world.findChunk(inside) != nullptr,
				"evictOutside: chunk at the ring edge survives");
	check(world.findChunk(vv::voxel::ChunkCoord{6, 0}) == nullptr,
				"evictOutside: chunk beyond the ring is evicted");
}


// ---------------------------------------------------------------------------
// Sun-shadow march: CPU mirror of the shader's sunShadow (near-column walk
// via the height bound + coarse far cells) vs dense sampling along the sun
// ray over the same two-tier world. Must agree on lit/shadowed exactly.
// ---------------------------------------------------------------------------

namespace {

struct ShadowWorld {
	// Near: full voxels in [0,64)^2 x [0,48). Far: coarse heights over
	// [-64,128)^2 in 4-unit cells.
	TestWorld near;
	std::vector<std::uint32_t> farCells;  // dim^2, u16 height | type<<16
	int farOrigin = -64;
	unsigned farDim = 48;
	float farCell = 4.0f;
	float maxTerr = 150.0f;  // shared ascend bound (mirror + brute)

	unsigned farAt(int cx, int cz) const {
		if (cx < 0 || cz < 0 || cx >= int(farDim) || cz >= int(farDim)) {
			return 0u;
		}
		return farCells[std::size_t(cx) + std::size_t(cz) * farDim];
	}
};

// Mirror of the shader's sunRayEscapes: one exact binary shadow ray
// (ascending column DDA + height-bound voxel walk + coarse far cells).
bool sunRayEscapesMirror(const ShadowWorld& w, const double o[3],
												 const double dir[3]) {
	if (dir[1] <= 0.05) {
		return true;
	}
	const double EPS = 1e-6;
	int stepX = (dir[0] > 0.0) ? 1 : -1;
	int stepZ = (dir[2] > 0.0) ? 1 : -1;
	double tMaxX = 1e30, tMaxZ = 1e30, dX = 1e30, dZ = 1e30;
	int colX = int(std::floor(o[0]));
	int colZ = int(std::floor(o[2]));
	if (std::abs(dir[0]) > EPS) {
		tMaxX = (double(colX + ((stepX > 0) ? 1 : 0)) - o[0]) / dir[0];
		dX = std::abs(1.0 / dir[0]);
	} else {
		stepX = 0;
	}
	if (std::abs(dir[2]) > EPS) {
		tMaxZ = (double(colZ + ((stepZ > 0) ? 1 : 0)) - o[2]) / dir[2];
		dZ = std::abs(1.0 / dir[2]);
	} else {
		stepZ = 0;
	}

	double s = 0.0;
	for (int i = 0; i < 256; ++i) {
		const double sExit = std::min(tMaxX, tMaxZ);
		const double y0 = o[1] + dir[1] * s;
		if (y0 >= w.maxTerr) {
			return true;
		}
		const bool inNear = colX >= 0 && colX < w.near.wx && colZ >= 0 &&
												colZ < w.near.wz;
		if (inNear) {
			const unsigned bound = w.near.boundAt(colX, colZ);
			if (bound != 0xFFFFu && double(bound) > y0) {
				const double y1 = o[1] + dir[1] * sExit;
				const int yTop = std::min(
						int(std::floor(std::min(y1, double(bound) - 1.0))),
						w.near.wh - 1);
				for (int y = std::max(int(std::floor(y0)), 0); y <= yTop; ++y) {
					if (w.near.at(colX, y, colZ) != 0) {
						return false;
					}
				}
			}
		} else {
			const int fcX = int(std::floor((double(colX) + 0.5 - w.farOrigin) /
																		 w.farCell));
			const int fcZ = int(std::floor((double(colZ) + 0.5 - w.farOrigin) /
																		 w.farCell));
			const unsigned packed = w.farAt(fcX, fcZ);
			const double h = double(packed & 0xFFFFu);
			if (h > 0.0 && y0 < h) {
				return false;
			}
		}
		s = std::min(tMaxX, tMaxZ);
		const bool takeX = tMaxX < tMaxZ;
		tMaxX += takeX ? dX : 0.0;
		tMaxZ += takeX ? 0.0 : dZ;
		colX += takeX ? stepX : 0;
		colZ += takeX ? 0 : stepZ;
	}
	return true;
}

// Brute force: dense sampling along the sun ray with identical semantics
// (near voxels; far column tops; shared ascend bound).
bool sunLitBrute(const ShadowWorld& w, const double origin[3],
								 const double n[3], const double sun[3],
								 double dt = 0.05) {
	if (sun[1] <= 0.05) {
		return true;
	}
	double o[3];
	for (int a = 0; a < 3; ++a) {
		o[a] = origin[a] + n[a] * 1e-3 + sun[a] * 1e-2;
	}
	for (double s = 0.0; s < 2000.0; s += dt) {
		const double x = o[0] + sun[0] * s;
		const double y = o[1] + sun[1] * s;
		const double z = o[2] + sun[2] * s;
		if (y >= w.maxTerr) {
			return true;
		}
		const int cx = int(std::floor(x));
		const int cz = int(std::floor(z));
		if (cx >= 0 && cx < w.near.wx && cz >= 0 && cz < w.near.wz &&
				y < double(w.near.wh)) {
			if (w.near.at(cx, int(std::floor(y)), cz) != 0) {
				return false;
			}
		} else {
			const int fcX =
					int(std::floor((double(cx) + 0.5 - w.farOrigin) / w.farCell));
			const int fcZ =
					int(std::floor((double(cz) + 0.5 - w.farOrigin) / w.farCell));
			const unsigned packed = w.farAt(fcX, fcZ);
			const double h = double(packed & 0xFFFFu);
			if (h > 0.0 && y < h) {
				return false;
			}
		}
	}
	return true;
}

// Shared world for both shadow tests: rolling near terrain + a tall
// wall + a tower, over a far field with a ridge taller than the near
// terrain (so far terrain can shadow near terrain).
ShadowWorld makeShadowWorld() {
	ShadowWorld w;
	w.near.cells.assign(std::size_t(w.near.wx) * w.near.wh * w.near.wz, 0);
	for (int z = 0; z < w.near.wz; ++z) {
		for (int x = 0; x < w.near.wx; ++x) {
			const double h = 18.0 + 7.0 * std::sin(x * 0.29) +
											 5.0 * std::cos(z * 0.21);
			const int top = int(std::floor(h));
			for (int y = 0; y <= top; ++y) {
				w.near.cells[std::size_t(x) + std::size_t(y) * w.near.wx +
										 std::size_t(z) * w.near.wx * w.near.wh] = 1;
			}
		}
	}
	// A tall wall and a tower to cast clear shadows.
	for (int y = 0; y < 40; ++y) {
		for (int z = 0; z < w.near.wz; ++z) {
			w.near.cells[std::size_t(20) + std::size_t(y) * w.near.wx +
									 std::size_t(z) * w.near.wx * w.near.wh] = 2;
		}
	}
	for (int z = 30; z < 36; ++z) {
		for (int x = 40; x < 46; ++x) {
			for (int y = 0; y < 44; ++y) {
				w.near.cells[std::size_t(x) + std::size_t(y) * w.near.wx +
										 std::size_t(z) * w.near.wx * w.near.wh] = 3;
			}
		}
	}
	w.near.recomputeHeights();

	// Far field: rolling hills + the ridge.
	w.farCells.assign(std::size_t(w.farDim) * w.farDim, 0u);
	for (unsigned j = 0; j < w.farDim; ++j) {
		for (unsigned i = 0; i < w.farDim; ++i) {
			double h = 30.0 + 10.0 * std::sin(i * 0.19) + 8.0 * std::cos(j * 0.27);
			if (i >= 34 && i <= 38) {
				h = 120.0 + 10.0 * std::sin(j * 0.3);  // ridge east of the near box
			}
			w.farCells[std::size_t(i) + std::size_t(j) * w.farDim] =
					std::uint32_t(std::max(1.0, h)) | (1u << 16u);
		}
	}
	return w;
}

// Sun: normalize(0.5, 1.0, 0.5) - same shape as the game default.
void shadowSun(double out[3]) {
	const double len = std::sqrt(0.25 + 1.0 + 0.25);
	out[0] = 0.5 / len;
	out[1] = 1.0 / len;
	out[2] = 0.5 / len;
}

void testSunShadowMarch() {
	ShadowWorld w = makeShadowWorld();
	double sun[3] = {0, 0, 0};
	shadowSun(sun);

	std::uint64_t rng = 0x9e3779b97f4a7c15ull;
	auto next01 = [&rng]() {
		rng ^= rng >> 12;
		rng ^= rng << 25;
		rng ^= rng >> 27;
		return double(rng >> 11) / double(1ull << 53);
	};

	int lit = 0, shadowed = 0, mismatches = 0;
	for (int i = 0; i < 3000; ++i) {
		double origin[3], n[3] = {0.0, 1.0, 0.0};
		if (i % 3 == 0) {
			// Surface point on near terrain (top face).
			origin[0] = next01() * 64.0;
			origin[2] = next01() * 64.0;
			const unsigned bound =
					w.near.boundAt(int(std::floor(origin[0])),
												 int(std::floor(origin[2])));
			origin[1] = bound == 0xFFFFu ? 10.0 : double(bound);
		} else if (i % 3 == 1) {
			// Side face at the wall (normal -x).
			origin[0] = 20.0;
			origin[1] = next01() * 40.0;
			origin[2] = next01() * 64.0;
			n[0] = -1.0;
			n[1] = 0.0;
		} else {
			// Point on far terrain (far hits shadow too).
			origin[0] = -60.0 + next01() * 180.0;
			origin[2] = -60.0 + next01() * 180.0;
			const int fcX = int(std::floor(origin[0] + 64.0) / 4.0);
			const int fcZ = int(std::floor(origin[2] + 64.0) / 4.0);
			const unsigned packed =
					w.farAt(fcX < 0 ? -1 : fcX, fcZ < 0 ? -1 : fcZ);
			origin[1] = double(packed & 0xFFFFu);
			if (origin[1] == 0.0) {
				continue;
			}
		}
		double o[3];
		for (int a = 0; a < 3; ++a) {
			o[a] = origin[a] + n[a] * 1e-3 + sun[a] * 1e-2;
		}
		bool m = sunRayEscapesMirror(w, o, sun);
		bool b = sunLitBrute(w, origin, n, sun);
		if (m != b) {
			// The brute sampler can miss grazes shallower than sun.y * dt
			// (the mirror is exact); refine once before calling it a bug.
			b = sunLitBrute(w, origin, n, sun, 0.0005);
		}
		m ? ++lit : ++shadowed;
		if (m != b) {
			if (++mismatches <= 3) {
				std::printf("FAIL shadow ray %d at (%.2f,%.2f,%.2f): mirror "
										"lit=%d brute lit=%d\n",
										i, origin[0], origin[1], origin[2], int(m),
										int(b));
			}
		}
	}
	check(mismatches == 0, "shadow march: column DDA matches dense sampling");
	check(lit > 400 && shadowed > 400,
			"shadow march: both outcomes well exercised");
	std::printf("shadow march: %d lit / %d shadowed agree\n", lit, shadowed);
}


// ---------------------------------------------------------------------------
// SDF soft shadow: CPU mirror of the shader's shadowPenumbra +
// sunRayEscapesSdf (the VV_SDF_SHADOWS=1 path). Same traversal as the exact
// march mirror, with the Quilez penumbra estimate sampled at every cleared
// column (iquilezles.org/articles/rmshadows/).
// ---------------------------------------------------------------------------

// Mirror of the shader's shadowPenumbra: the plain Quilez estimate k*h/t.
// h is the distance from the sample to the closest relevant surface (a
// column's top plane), t the distance marched so far. Returns the
// visibility factor to fold into the min (1.0 = no darkening). The
// Aaltonen two-sphere refinement was removed from the shader (ill-
// conditioned where the distance grows; it projected a hard "clamped edge"
// stripe sampled per DDA column) - see the shader comment.
double shadowPenumbraMirror(double h, double t) {
	const double k = 8.0;  // = the shader's kShadowSharpness
	return std::clamp(k * h / std::max(t, 1e-4), 0.0, 1.0);
}

// Mirror of the shader's sunRayEscapesSdf: the exact march's ascending
// column DDA + height-bound walk + coarse far cells (same occlusion
// events), fractional visibility from per-column penumbra samples.
double sunRayEscapesSdfMirror(const ShadowWorld& w, const double o[3],
		const double dir[3], double startT = 0.0, double startVisibility = 1.0) {
	if (dir[1] <= 0.05) {
		return startVisibility;
	}
	const double EPS = 1e-6;
	int stepX = (dir[0] > 0.0) ? 1 : -1;
	int stepZ = (dir[2] > 0.0) ? 1 : -1;
	double tMaxX = 1e30, tMaxZ = 1e30, dX = 1e30, dZ = 1e30;
	int colX = int(std::floor(o[0]));
	int colZ = int(std::floor(o[2]));
	if (std::abs(dir[0]) > EPS) {
		tMaxX = (double(colX + ((stepX > 0) ? 1 : 0)) - o[0]) / dir[0];
		dX = std::abs(1.0 / dir[0]);
	} else {
		stepX = 0;
	}
	if (std::abs(dir[2]) > EPS) {
		tMaxZ = (double(colZ + ((stepZ > 0) ? 1 : 0)) - o[2]) / dir[2];
		dZ = std::abs(1.0 / dir[2]);
	} else {
		stepZ = 0;
	}

	// `startT` is how far the ray has already come from the shaded surface
	// (the 3D SDF march hands over mid-flight, pass 40) and startVisibility
	// what it has already accumulated; positions run off tLocal.
	double visibility = startVisibility;
	double tLocal = 0.0;
	for (int i = 0; i < 256; ++i) {
		const double sExit = std::min(tMaxX, tMaxZ);
		const double y0 = o[1] + dir[1] * tLocal;
		if (y0 >= w.maxTerr) {
			return visibility;
		}
		const bool inNear = colX >= 0 && colX < w.near.wx && colZ >= 0 &&
			colZ < w.near.wz;
		if (inNear) {
			const unsigned bound = w.near.boundAt(colX, colZ);
			if (bound != 0xFFFFu) {
				if (y0 >= double(bound)) {
					const double h = y0 - double(bound);
					visibility = std::min(
							visibility,
							shadowPenumbraMirror(h, startT + tLocal));
				} else {
					const double y1 = o[1] + dir[1] * sExit;
					const int yTop = std::min(
							int(std::floor(std::min(y1, double(bound) - 1.0))),
							w.near.wh - 1);
					bool solid = false;
					for (int y = std::max(int(std::floor(y0)), 0);
							y <= yTop; ++y) {
						if (w.near.at(colX, y, colZ) != 0) {
							solid = true;
							break;
						}
					}
					if (solid) {
						return 0.0;  // opaque world (all test types opaque)
					}
					// Air all the way (overhang shaft): no relevant surface
					// here, no estimate for this column.
				}
			}
		} else {
			const int fcX = int(std::floor((double(colX) + 0.5 - w.farOrigin) /
				w.farCell));
			const int fcZ = int(std::floor((double(colZ) + 0.5 - w.farOrigin) /
				w.farCell));
			const unsigned packed = w.farAt(fcX, fcZ);
			const double h = double(packed & 0xFFFFu);
			if (h > 0.0) {
				if (y0 < h) {
					return 0.0;
				}
				visibility = std::min(
						visibility, shadowPenumbraMirror(y0 - h, startT + tLocal));
			}
		}
		tLocal = sExit;
		const bool takeX = tMaxX < tMaxZ;
		tMaxX += takeX ? dX : 0.0;
		tMaxZ += takeX ? 0.0 : dZ;
		colX += takeX ? stepX : 0;
		colZ += takeX ? 0 : stepZ;
	}
	return visibility;
}

// Mirror of the shader's sunRayEscapesSdf3d + its pass-40 hand-off: sphere
// trace the ray through the (box-local) SDF, and when the march leaves the
// field - or spends its step budget - continue in world space with the
// whole-region 2.5D march, seeded with the distance travelled and the
// visibility the field accumulated. `boxOrigin` is where the field's (0,0,0)
// sits in world coordinates.
double sdf3dHandoffMirror(const vv::voxel::SdfField& sdf,
		const double boxOrigin[3], const ShadowWorld& w, const double o[3],
		const double dir[3]) {
	const float of[3] = {float(o[0] - boxOrigin[0]),
			float(o[1] - boxOrigin[1]), float(o[2] - boxOrigin[2])};
	const float df[3] = {float(dir[0]), float(dir[1]), float(dir[2])};
	float exit[3] = {0.0f, 0.0f, 0.0f};
	float exitT = 0.0f;
	float vis = 1.0f;
	if (!vv::voxel::sphereTracedShadowExits(sdf, of, df, exit, &exitT, &vis)) {
		return double(vis);
	}
	const double exitWorld[3] = {boxOrigin[0] + double(exit[0]),
			boxOrigin[1] + double(exit[1]), boxOrigin[2] + double(exit[2])};
	return sunRayEscapesSdfMirror(w, exitWorld, dir, double(exitT), double(vis));
}

// SDF soft shadow: occlusion parity with the exact march (fully dark
// wherever the exact march is blocked - same traversal, opaque world),
// visibility range, the penumbra being a real minority (the old
// clamp-to-zero pinned every grazing pixel to black), and the penumbra
// shape against the known wall (hard shadow under the top, partial light
// grazing it, lit well clear, monotonic toward the wall).
void testSunShadowSdfMarch() {
	ShadowWorld w = makeShadowWorld();
	double sun[3] = {0, 0, 0};
	shadowSun(sun);

	std::uint64_t rng = 0x9e3779b97f4a7c15ull;
	auto next01 = [&rng]() {
		rng ^= rng >> 12;
		rng ^= rng << 25;
		rng ^= rng >> 27;
		return double(rng >> 11) / double(1ull << 53);
	};

	int outOfRange = 0, leaked = 0, grazing = 0, shadowed = 0, total = 0;
	for (int i = 0; i < 3000; ++i) {
		double origin[3], n[3] = {0.0, 1.0, 0.0};
		if (i % 3 == 0) {
			// Surface point on near terrain (top face).
			origin[0] = next01() * 64.0;
			origin[2] = next01() * 64.0;
			const unsigned bound =
					w.near.boundAt(int(std::floor(origin[0])),
							int(std::floor(origin[2])));
			origin[1] = bound == 0xFFFFu ? 10.0 : double(bound);
		} else if (i % 3 == 1) {
			// Side face at the wall (normal -x).
			origin[0] = 20.0;
			origin[1] = next01() * 40.0;
			origin[2] = next01() * 64.0;
			n[0] = -1.0;
			n[1] = 0.0;
		} else {
			// Point on far terrain (far hits shadow too).
			origin[0] = -60.0 + next01() * 180.0;
			origin[2] = -60.0 + next01() * 180.0;
			const int fcX = int(std::floor(origin[0] + 64.0) / 4.0);
			const int fcZ = int(std::floor(origin[2] + 64.0) / 4.0);
			const unsigned packed =
					w.farAt(fcX < 0 ? -1 : fcX, fcZ < 0 ? -1 : fcZ);
			origin[1] = double(packed & 0xFFFFu);
			if (origin[1] == 0.0) {
				continue;
			}
		}
		double o[3];
		for (int a = 0; a < 3; ++a) {
			o[a] = origin[a] + n[a] * 1e-3 + sun[a] * 1e-2;
		}
		++total;
		const bool exactLit = sunRayEscapesMirror(w, o, sun);
		const double soft = sunRayEscapesSdfMirror(w, o, sun);
		if (soft < -1e-9 || soft > 1.0 + 1e-9) {
			++outOfRange;
		}
		// Occlusion parity: wherever the exact march is blocked, the SDF
		// march must be fully dark (same traversal, opaque test world).
		if (!exactLit && soft > 1e-6) {
			if (leaked <= 3) {
				std::printf("FAIL sdf shadow %d at (%.2f,%.2f,%.2f): "
						"exact shadowed, soft %.3f\n",
						i, origin[0], origin[1], origin[2], soft);
			}
			++leaked;
		}
		if (!exactLit) {
			++shadowed;
		}
		// Exact-lit but dark pixels are the penumbra (a grazing near-miss):
		// they must exist (the feature) yet stay a clear minority (the old
		// degenerate-triangle clamp turned nearly all of them into black).
		if (exactLit && soft < 0.1) {
			++grazing;
		}
	}
	check(outOfRange == 0, "sdf shadow: visibility stays in [0, 1]");
	check(leaked == 0,
			"sdf shadow: fully dark wherever the exact march is occluded");
	check(shadowed > 300, "sdf shadow: shadowed cases well exercised");
	check(grazing > 5 && grazing < 1500,
			"sdf shadow: penumbra pixels exist but are a minority");
	std::printf("sdf shadow: %d shadowed, %d penumbral, %d lit of %d\n",
			shadowed, grazing, total - shadowed - grazing, total);

	// Penumbra shape against the known wall (column x=20, top at y=40, sun
	// (0.5,1,0.5) rises 2 voxels per column of x): the ray from (x, 25, 10)
	// reaches the wall at y ~= 25 + 2*(20-x), so x <= 12 grazes/clears the
	// top (partial to full light) and x >= 13 hits the wall (hard shadow).
	const double wallX[5] = {8.0, 10.0, 12.0, 13.0, 14.0};
	double wallSoft[5] = {0.0, 0.0, 0.0, 0.0, 0.0};
	for (int i = 0; i < 5; ++i) {
		double o[3] = {wallX[i] + sun[0] * 1e-2,
				25.0 + 1e-3 + sun[1] * 1e-2, 10.0 + sun[2] * 1e-2};
		wallSoft[i] = sunRayEscapesSdfMirror(w, o, sun);
	}
	check(wallSoft[4] == 0.0 && wallSoft[3] == 0.0,
			"sdf penumbra: hard shadow under the wall top");
	check(wallSoft[0] > 0.9 && wallSoft[1] > 0.9,
			"sdf penumbra: lit well clear of the wall top");
	check(wallSoft[2] > 0.05 && wallSoft[2] < 0.9,
			"sdf penumbra: partial light grazing the wall top");
	for (int i = 1; i < 5; ++i) {
		check(wallSoft[i] <= wallSoft[i - 1] + 1e-6,
				"sdf penumbra: no lighter farther into the shadow");
	}
}

// ---------------------------------------------------------------------------
// 3D voxel SDF soft shadow (pass 37): a REAL 3D distance field (distance to
// the nearest solid surface, via a two-pass chamfer EDT) sphere-traced with
// k*h/t, cross-checked against the exact binary march. The point: h is now
// the true 3D distance to the surface - vertical faces and overhangs
// included - so the shadow edges running along steep casters get the same
// continuous penumbra as the top edge. That is exactly what the 2.5D
// top-plane path (sunRayEscapesSdfMirror) cannot do.
// ---------------------------------------------------------------------------

// A world with a MESA (a finite block: vertical faces on every side, top
// y=40) and an OVERHANG (a floating slab with air beneath) on rolling ground.
// No far field, so the SDF box and the exact march see identical geometry.
ShadowWorld makeSdfTestWorld() {
	ShadowWorld w;
	w.near.wx = 64;
	w.near.wz = 64;
	w.near.wh = 48;
	w.near.cells.assign(std::size_t(w.near.wx) * w.near.wh * w.near.wz, 0);
	for (int z = 0; z < w.near.wz; ++z) {
		for (int x = 0; x < w.near.wx; ++x) {
			const double h = 16.0 + 5.0 * std::sin(x * 0.29) +
				4.0 * std::cos(z * 0.21);
			const int top = int(std::floor(h));
			for (int y = 0; y <= top; ++y) {
				w.near.cells[std::size_t(x) + std::size_t(y) * w.near.wx +
						std::size_t(z) * w.near.wx * w.near.wh] = 1;
			}
		}
	}
	// MESA: x in [20,24), z in [12,44), solid from the ground to y=40.
	for (int z = 12; z < 44; ++z)
		for (int x = 20; x < 24; ++x)
			for (int y = 0; y < 40; ++y)
				w.near.cells[std::size_t(x) + std::size_t(y) * w.near.wx +
						std::size_t(z) * w.near.wx * w.near.wh] = 2;
	// OVERHANG: x in [44,52), z in [20,30), a slab at y in [30,34) with air
	// beneath (the ground there is only ~16).
	for (int z = 20; z < 30; ++z)
		for (int x = 44; x < 52; ++x)
			for (int y = 30; y < 34; ++y)
				w.near.cells[std::size_t(x) + std::size_t(y) * w.near.wx +
						std::size_t(z) * w.near.wx * w.near.wh] = 3;
	w.near.recomputeHeights();
	// No far field: the SDF box and the exact march see the same geometry.
	w.farDim = 0;
	w.farCells.clear();
	return w;
}

// Does the ray actually cross a solid cell of the near voxel world? The
// exact mirror's blocked verdict also fires where a ray merely skims a
// column top in its height-field model, which the cell-accurate field march
// (correctly) does not - such a column is not a usable leak oracle.
bool nearSolidOnRay(const ShadowWorld& w, const double o[3],
		const double dir[3]) {
	for (double t = 0.0; t <= 200.0; t += 0.05) {
		const int cx = int(std::floor(o[0] + dir[0] * t));
		const int cy = int(std::floor(o[1] + dir[1] * t));
		const int cz = int(std::floor(o[2] + dir[2] * t));
		if (cx < 0 || cx >= w.near.wx || cz < 0 || cz >= w.near.wz || cy < 0 ||
				cy >= w.near.wh) {
			return false;  // left the near world: no cell can block further
		}
		if (w.near.at(cx, cy, cz) != 0) {
			return true;
		}
	}
	return false;
}

// Pass 40: the 6x6-chunk SDF box is not the world. Its whole point is to
// soften the near shadows; casters OUTSIDE it still shadow the ray, so
// sunRayEscapesSdf3d hands the ray to the 2.5D march at the boundary instead
// of treating it as open space. Pins (1) the scenario - a caster outside the
// box, where the field alone leaks full light, (2) no leak once the hand-off
// continues the ray, and (3) that outside the field the composite is exactly
// the 2.5D march.
void testSdfBoxHandoff() {
	ShadowWorld w = makeSdfTestWorld();
	double sun[3] = {0, 0, 0};
	shadowSun(sun);

	// A field over x in [0,16) only: the mesa (x in [20,24), the caster of
	// every shadow over this ground) lies outside it, like the GPU's field
	// (6x6 chunks around the camera) versus the whole near region.
	const int bx = 16;
	vv::voxel::SdfField sdf;
	sdf.build(bx, w.near.wh, w.near.wz,
			[&](int x, int y, int z) { return w.near.at(x, y, z) != 0; });
	const double boxOrigin[3] = {0.0, 0.0, 0.0};

	int shadowed = 0, lit = 0, leaksFieldOnly = 0, leaksHandoff = 0;
	int darkened = 0;  // the continuation found occlusion the field missed
	int judged = 0;    // shadowed columns a real cell blocks (usable oracle)
	int unjudged = 0;  // shadowed only in the height-field model: skipped
	for (int x = 6; x < 16; ++x) {
		for (int z = 12; z < 44; z += 2) {
			const unsigned bound = w.near.boundAt(x, z);
			if (bound == 0xFFFFu) {
				continue;
			}
			double p[3] = {double(x) + 0.5, double(bound), double(z) + 0.5};
			double n[3] = {0.0, 1.0, 0.0};
			double o[3];
			for (int a = 0; a < 3; ++a) {
				o[a] = p[a] + n[a] * 1e-3 + sun[a] * 1e-2;
			}
			const float of[3] = {float(o[0]), float(o[1]), float(o[2])};
			const float df[3] = {float(sun[0]), float(sun[1]), float(sun[2])};
			const double softFieldOnly =
					double(vv::voxel::sphereTracedShadow(sdf, of, df));
			const double softHandoff =
					sdf3dHandoffMirror(sdf, boxOrigin, w, o, sun);
			if (softHandoff < softFieldOnly - 1e-6) {
				++darkened;
			}
			if (sunRayEscapesMirror(w, o, sun)) {
				++lit;
				continue;
			}
			++shadowed;
			if (softFieldOnly > 0.5 + 1e-3) {
				++leaksFieldOnly;
			}
			// Judge the leak only where voxels really block the ray; a
			// height-field skim is a model disagreement, not a leak.
			if (!nearSolidOnRay(w, o, sun)) {
				++unjudged;
				continue;
			}
			++judged;
			if (softHandoff > 0.5 + 1e-3) {
				++leaksHandoff;
			}
		}
	}
	check(shadowed > 20,
			"sdf box hand-off: the caster-outside-the-box setup is exercised");
	check(leaksFieldOnly > 0,
			"sdf box hand-off: the field alone really leaks here (scenario valid)");
	check(leaksHandoff == 0,
			"sdf box hand-off: no light leak with the whole-region continuation");
	check(judged > 10,
			"sdf box hand-off: real cell blockers are exercised");
	check(darkened > 0,
			"sdf box hand-off: the continuation actually finds the casters");
	// Outside the field the composite must reduce to the plain 2.5D march.
	const double outside[3] = {40.5, 20.0, 20.5};
	check(std::abs(sdf3dHandoffMirror(sdf, boxOrigin, w, outside, sun) -
			sunRayEscapesSdfMirror(w, outside, sun)) < 1e-9,
			"sdf box hand-off: outside the field it is exactly the 2.5D march");
	std::printf("sdf box hand-off: %d shadowed / %d lit columns (%d judged, "
			"%d height-field-only); leaks field only %d, with the hand-off %d; "
			"%d darkened by it\n",
			shadowed, lit, judged, unjudged, leaksFieldOnly, leaksHandoff,
			darkened);
}

// Pass 42: the handover rules that keep the box uniform and the seed buffer
// consistent - a box published next to another build's seeds shades a frame
// out of a field describing different terrain, which is the owner's one-frame
// "chunks go dark just before the SDF shadows come back" - and that make the
// field follow the camera instead of waiting for the next region move.
void testSdfHandoverPolicy() {
	using vv::voxel::SdfHandover;

	SdfHandover h;
	check(h.step() == SdfHandover::Step::Idle,
			"sdf handover: no build before a region move has been seen");
	h.wantValid = true;  // the first region move completed
	check(h.step() == SdfHandover::Step::Relaunch,
			"sdf handover: the first complete region starts a build");

	h.buildRunning = true;  // the build is on the worker
	h.wantCenterX = 1;      // the camera crosses a chunk while it builds
	check(h.step() == SdfHandover::Step::Idle,
			"sdf handover: a running build is waited for, never duplicated");
	h.buildReady = true;
	check(h.step() == SdfHandover::Step::JoinAndUpload,
			"sdf handover: a finished build is picked up");

	// The renderer joins and SUBMITS the copy (no wait); the box has to stay
	// unpublished until that copy's fence signals.
	h.buildRunning = false;
	h.buildReady = false;
	h.uploadInFlight = true;
	h.copyComplete = false;
	check(h.step() == SdfHandover::Step::Idle,
			"sdf handover: the box is NOT published while the copy is in flight");
	h.copyComplete = true;
	check(h.step() == SdfHandover::Step::Publish,
			"sdf handover: the box is published once the copy has landed");

	// The copy must target the SPARE half: the live one has to keep holding
	// exactly the seeds its published box describes for as long as a frame can
	// still read them. After a publish they swap.
	check(h.uploadHalf() == 1u,
			"sdf handover: the copy targets the half no live box points at");
	h.liveHalf = h.uploadHalf();
	check(h.uploadHalf() == 0u,
			"sdf handover: the halves alternate, so the live half is never the "
			"one being filled");

	// The renderer publishes (center 0) while the camera is already on chunk
	// 1: the launch the running build swallowed must be retried, or the soft
	// shadows stay a whole crossing behind the camera.
	h.uploadInFlight = false;
	h.copyComplete = false;
	h.haveField = true;
	h.activeCenterX = 0;
	check(h.step() == SdfHandover::Step::Relaunch,
			"sdf handover: a swallowed launch is retried once the pipeline is idle");

	// That build lands too: the field now covers the camera, nothing to do.
	h.buildRunning = false;
	h.buildReady = false;
	h.activeCenterX = 1;
	h.wantCenterX = 1;
	check(h.step() == SdfHandover::Step::Idle,
			"sdf handover: a field that covers the camera is not rebuilt");
	std::printf("sdf handover: box/seed pairing pinned either side of the copy "
			"(spare half, payload before active); the field follows the "
			"camera's chunk instead of waiting for the next region move\n");
}

void testSdfSoftShadow3d() {
	ShadowWorld w = makeSdfTestWorld();
	double sun[3] = {0, 0, 0};
	shadowSun(sun);

	// Build the 3D SDF over the near box (distance to the nearest solid).
	vv::voxel::SdfField sdf;
	sdf.build(w.near.wx, w.near.wh, w.near.wz,
		[&](int x, int y, int z) { return w.near.at(x, y, z) != 0; });

	std::uint64_t rng = 0x51ed270b85e8ab9full;
	auto next01 = [&rng]() {
		rng ^= rng >> 12;
		rng ^= rng << 25;
		rng ^= rng >> 27;
		return double(rng >> 11) / double(1ull << 53);
	};
	auto originOf = [&](const double p[3], const double n[3], double o[3]) {
		for (int a = 0; a < 3; ++a) {
			o[a] = p[a] + n[a] * 1e-3 + sun[a] * 1e-2;
		}
	};
	auto soft3d = [&](const double o[3]) {
		const float of[3] = {float(o[0]), float(o[1]), float(o[2])};
		const float sf[3] = {float(sun[0]), float(sun[1]), float(sun[2])};
		return vv::voxel::sphereTracedShadow(sdf, of, sf);
	};

	// (1) Occlusion parity + range over a mix of surface kinds. The critical
	// invariant: wherever the exact (center-ray) march is blocked, the soft
	// shadow is at most half-lit (the dark-side penumbra) - never fully lit
	// (no light leak through a vertical face or an overhang).
	int outOfRange = 0, leaked = 0, shadowed = 0, lit = 0, total = 0;
	for (int i = 0; i < 2500; ++i) {
		double p[3], n[3] = {0.0, 1.0, 0.0};
		const int kind = i % 4;
		if (kind == 0) {  // rolling ground top
			p[0] = next01() * 64.0;
			p[2] = next01() * 64.0;
			const unsigned b = w.near.boundAt(int(std::floor(p[0])),
				int(std::floor(p[2])));
			p[1] = b == 0xFFFFu ? 10.0 : double(b);
		} else if (kind == 1) {  // mesa -x vertical face (x=20)
			p[0] = 20.0;
			p[1] = 1.0 + next01() * 38.0;
			p[2] = 12.0 + next01() * 32.0;
			n[0] = -1.0; n[1] = 0.0; n[2] = 0.0;
		} else if (kind == 2) {  // mesa top
			p[0] = 20.0 + next01() * 4.0;
			p[1] = 40.0;
			p[2] = 12.0 + next01() * 32.0;
		} else {  // under the overhang slab
			p[0] = 44.0 + next01() * 8.0;
			p[2] = 20.0 + next01() * 10.0;
			const unsigned b = w.near.boundAt(int(std::floor(p[0])),
				int(std::floor(p[2])));
			p[1] = b == 0xFFFFu ? 10.0 : double(b);
		}
		double o[3];
		originOf(p, n, o);
		++total;
		const bool exactLit = sunRayEscapesMirror(w, o, sun);
		const float soft = soft3d(o);
		if (soft < -1e-9f || soft > 1.0f + 1e-9f) {
			++outOfRange;
		}
		if (!exactLit && soft > 0.5 + 1e-3f) {
			if (leaked <= 3) {
				std::printf("FAIL sdf3d leak at (%.2f,%.2f,%.2f): exact "
					"shadowed, soft %.3f\n", p[0], p[1], p[2], double(soft));
			}
			++leaked;
		}
		exactLit ? ++lit : ++shadowed;
	}
	check(outOfRange == 0, "sdf3d shadow: visibility stays in [0, 1]");
	check(leaked == 0,
		"sdf3d shadow: no light leak (at most half-lit where exact is dark)");
	check(shadowed > 200 && lit > 200,
		"sdf3d shadow: both outcomes well exercised");
	std::printf("sdf3d shadow: %d shadowed, %d lit of %d; %d leaks, %d "
		"out-of-range\n",
		shadowed, lit, total, leaked, outOfRange);

	// (2) The mesa's SIDE edge (the shadow boundary running along its west
	// face, x=20) is a smooth ramp under the 3D SDF, not a hard 0->1 jump.
	// Scan the GROUND SURFACE across that edge (x from 5 to 14 at z=28, on
	// the actual ground top): the 3D SDF must transition lit<->shadowed over
	// a few voxels (the side penumbra), and must not make a >0.7 jump between
	// adjacent columns (a hard edge). The 2.5D top-plane path has no data for
	// the vertical face and makes a hard jump here.
	const double scanZ = 28.0;
	int hardJumps = 0;
	double prevSoft = 1.0;
	bool seenShadow = false, seenLit = false;
	for (int x = 5; x <= 14; ++x) {
		const unsigned b = w.near.boundAt(x, 28);
		double p[3] = {double(x) + 0.5, b == 0xFFFFu ? 10.0 : double(b),
			(scanZ + 0.5)};
		double n[3] = {0.0, 1.0, 0.0};
		double o[3];
		originOf(p, n, o);
		const float soft = soft3d(o);
		if (soft < 0.25) {
			seenShadow = true;
		}
		if (soft > 0.75) {
			seenLit = true;
		}
		if (std::abs(soft - prevSoft) > 0.7) {
			++hardJumps;
		}
		prevSoft = soft;
	}
	check(seenShadow && seenLit,
		"sdf3d side edge: scan crosses both shadow and light");
	check(hardJumps == 0,
		"sdf3d side edge: no hard 0->1 jump along the vertical-caster edge");

	// (3) The overhang's UNDERSIDE: a point on the ground directly beneath
	// the slab is in the slab's shadow (the exact march blocks it), and the
	// 3D SDF keeps it at most half-lit (the 2.5D top-plane path sees only the
	// column's top and can light it).
	{
		double p[3] = {48.0, 16.0, 25.0};  // under the slab (ground ~16)
		double n[3] = {0.0, 1.0, 0.0};
		double o[3];
		originOf(p, n, o);
		const bool exactLit = sunRayEscapesMirror(w, o, sun);
		const float soft = soft3d(o);
		check(!exactLit, "sdf3d overhang: point under the slab is shadowed");
		check(soft <= 0.5 + 1e-3f,
			"sdf3d overhang: underside stays at most half-lit");
	}
}

// ---------------------------------------------------------------------------

// Pass 39: the SDF BOX BUILD (VV_SDF_SHADOWS=1). The renderer builds the
// field from per-chunk voxel-type snapshots, so that walk MUST use the chunk
// voxel layout (X + Y*sizeX + Z*sizeX*worldHeight - Chunk::index and the
// shader's fetchVoxel). The bug pinned here: pass 38 indexed the snapshot with
// a Z stride of chunkSizeX * chunkSizeZ (1024 with the default config, where
// the chunks lay out 4096), i.e. the field was built from a scrambled
// projection of the terrain - which rendered the whole region around the
// camera fully shadowed. Both checks below fail against that indexing.
void testSdfBoxBuild() {
	using vv::voxel::Chunk;
	using vv::voxel::ChunkCoord;
	using vv::voxel::SdfBoxGeometry;
	using vv::voxel::SdfField;

	const int cx = 32, cz = 32, wh = 128;
	vv::terrain::TerrainConfig tcfg = testTerrainConfig();
	vv::voxel::World world(tcfg, cx, wh, cz);
	std::vector<const Chunk*> added;
	std::vector<ChunkCoord> evicted;
	world.ensureRegion(0, 0, 4, added, evicted);  // 9x9 chunks

	// Dense region copy (the reference the box must agree with), built from
	// the chunks' own voxel data - the REAL layout, not the box's walk.
	const int regionBase = -4, regionChunks = 9;
	const int rnx = regionChunks * cx, rnz = regionChunks * cz;
	std::vector<std::uint8_t> region(std::size_t(rnx) * wh * rnz, 0);
	for (int rcz = 0; rcz < regionChunks; ++rcz) {
		for (int rcx = 0; rcx < regionChunks; ++rcx) {
			const Chunk* c = world.findChunk(
					ChunkCoord{regionBase + rcx, regionBase + rcz});
			if (c == nullptr) {
				continue;
			}
			// Copy row by row (the chunk's X rows are contiguous, the region's
			// are rnx apart).
			for (int lz = 0; lz < cz; ++lz) {
				for (int y = 0; y < wh; ++y) {
					const std::size_t src =
							std::size_t(lz) * cx * wh + std::size_t(y) * cx;
					const std::size_t dst = std::size_t(rcx * cx) +
																	std::size_t(y) * rnx +
																	std::size_t(rcz * cz + lz) * rnx * wh;
					std::copy_n(c->voxelTypes().begin() + static_cast<std::ptrdiff_t>(src),
											std::size_t(cx),
											region.begin() + static_cast<std::ptrdiff_t>(dst));
				}
			}
		}
	}
	auto regionSolid = [&](int x, int y, int z) {
		if (y < 0 || y >= wh) {
			return false;
		}
		const int rx = x - regionBase * cx;
		const int rz = z - regionBase * cz;
		if (rx < 0 || rx >= rnx || rz < 0 || rz >= rnz) {
			return false;  // outside the generated region: air
		}
		return region[std::size_t(rx) + std::size_t(y) * rnx +
									std::size_t(rz) * rnx * wh] != 0;
	};

	// The renderer's box: 6x6 whole chunks centered on chunk (0,0), full
	// world height.
	const SdfBoxGeometry box = SdfBoxGeometry::centeredOn(0, 0, 3, cx, cz, wh);
	check(box.valid() && box.nx == 192 && box.ny == 128 && box.nz == 192 &&
					box.originX == -96 && box.originZ == -96 && box.originY == 0,
				"sdfBox: 6x6-chunk geometry, full world height");

	// Snapshot the box's chunks exactly like launchSdfBuild does.
	const std::size_t side = box.chunksPerSide;
	std::vector<std::vector<std::uint8_t>> snapshots(side * side);
	for (std::size_t i = 0; i < side * side; ++i) {
		const int32_t ccx =
				box.originX / cx + static_cast<int32_t>(i % side);
		const int32_t ccz =
				box.originZ / cz + static_cast<int32_t>(i / side);
		if (const Chunk* c = world.findChunk(ChunkCoord{ccx, ccz})) {
			snapshots[i] = c->voxelTypes();
		}
	}

	SdfField sdf;
	vv::voxel::buildSdfBoxField(box, snapshots, sdf);
	check(sdf.nx() == int(box.nx) && sdf.ny() == int(box.ny) &&
					sdf.nz() == int(box.nz),
				"sdfBox: field dims match the box");

	// (1) The field must describe EXACTLY the box's chunk voxels. The chamfer
	// transform's zero-distance cells are the solid cells, so cellDistance()
	// is 0 iff that voxel is solid in the chunk data - any stride mismatch
	// (pass 38: 1024 instead of 4096) scrambles this on most of the box.
	std::size_t mismatches = 0, solidCells = 0;
	for (std::uint32_t z = 0; z < box.nz; ++z) {
		for (std::uint32_t y = 0; y < box.ny; ++y) {
			for (std::uint32_t x = 0; x < box.nx; ++x) {
				const bool solid = regionSolid(box.originX + int(x),
																			 box.originY + int(y),
																			 box.originZ + int(z));
				if (solid) {
					++solidCells;
				}
				if ((sdf.cellDistance(int(x), int(y), int(z)) == 0.0f) != solid) {
					++mismatches;
				}
			}
		}
	}
	check(mismatches == 0,
				"sdfBox: the field covers exactly the chunks' solid voxels");
	check(solidCells > 1000000,
				"sdfBox: the box really contains the terrain (sanity)");

	// A chunk that is not installed (or has a foreign size) reads as air, so
	// the build can never index out of bounds.
	const std::vector<std::vector<std::uint8_t>> none;
	check(!vv::voxel::sdfBoxCellSolid(box, none, 5, 5, 5),
				"sdfBox: a missing chunk snapshot reads as air");
	std::vector<std::vector<std::uint8_t>> shortSet(1,
																								 std::vector<std::uint8_t>(4, 1));
	check(!vv::voxel::sdfBoxCellSolid(box, shortSet, 5, 5, 5),
				"sdfBox: a foreign-sized chunk snapshot reads as air");

	// (2) The user-visible symptom: with a garbled field the ground around
	// the camera rendered fully shadowed (every lit pixel black). Sample
	// surface columns inside the box and compare the soft shadow against an
	// exact binary march over the region.
	const double len = std::sqrt(0.25 + 1.0 + 0.25);
	const float sun[3] = {float(0.5 / len), float(1.0 / len),
												float(0.5 / len)};
	auto exactLit = [&](const float o[3]) {
		for (float t = 0.0f; t < 400.0f; t += 0.25f) {
			if (regionSolid(int(std::floor(o[0] + sun[0] * t)),
											int(std::floor(o[1] + sun[1] * t)),
											int(std::floor(o[2] + sun[2] * t)))) {
				return false;
			}
		}
		return true;
	};
	std::uint64_t rng = 0x51ed270b85e8ab9full;
	auto next01 = [&rng]() {
		rng ^= rng >> 12;
		rng ^= rng << 25;
		rng ^= rng >> 27;
		return double(rng >> 11) / double(1ull << 53);
	};

	int total = 0, exactLitCount = 0, litKept = 0, leaks = 0, dark = 0;
	double softSum = 0.0;
	for (int i = 0; i < 400; ++i) {
		const int wx = box.originX + 4 + int(next01() * double(box.nx - 8));
		const int wz = box.originZ + 4 + int(next01() * double(box.nz - 8));
		int top = -1;
		for (int y = wh - 1; y >= 0; --y) {
			if (regionSolid(wx, y, wz)) {
				top = y;
				break;
			}
		}
		if (top < 1) {
			continue;
		}
		float o[3] = {float(wx) + 0.5f + sun[0] * 1e-2f,
									float(top + 1) + 1e-3f + sun[1] * 1e-2f,
									float(wz) + 0.5f + sun[2] * 1e-2f};
		const float ol[3] = {o[0] - float(box.originX),
											 o[1] - float(box.originY),
											 o[2] - float(box.originZ)};
		const bool ex = exactLit(o);
		const float soft = vv::voxel::sphereTracedShadow(sdf, ol, sun);
		++total;
		softSum += soft;
		if (soft < 0.05f) {
			++dark;
		}
		if (ex) {
			++exactLitCount;
			if (soft > 0.5f) {
				++litKept;
			}
		} else if (soft > 0.5f + 1e-3f) {
			++leaks;
		}
	}
	check(total > 300, "sdfBox shadow: enough surface samples");
	check(leaks == 0, "sdfBox shadow: no light leak on the real terrain");
	// The regression: an unscrambled field keeps most lit ground lit. The
	// pass-38 indexing left the whole box dark (0 of 400 here).
	check(exactLitCount > 0 && litKept * 2 > exactLitCount,
				"sdfBox shadow: lit ground stays lit (no whole-region shadowing)");
	std::printf(
			"sdfBox: %zu solid cells, %zu mismatches; %d samples, %d exactly "
			"lit (%d kept lit), %d fully dark, mean soft %.3f\n",
			solidCells, mismatches, total, exactLitCount, litKept, dark,
			softSum / double(total > 0 ? total : 1));
}

}  // namespace
}  // namespace

static void testVoxelTextures() {
	using vv::voxel::VoxelTextureMode;
	namespace vt = vv::voxel;

	// Type file names: 7 entries, non-empty, distinct, air first.
	bool namesOk = vt::kVoxelTypeNames[0] == std::string("air");
	std::vector<std::string> seen;
	for (std::uint32_t t = 0; t < vt::kVoxelTypeCount; ++t) {
		const std::string n = vt::kVoxelTypeNames[t];
		if (n.empty()) {
			namesOk = false;
		}
		for (const std::string& o : seen) {
			if (o == n) {
				namesOk = false;
			}
		}
		seen.push_back(n);
	}
	check(namesOk, "textures: per-type file names unique, air first");

	// Mode file counts: uniform 1, side-uniform 3, custom 6.
	check(vt::voxelTextureFileCount(VoxelTextureMode::Uniform) == 1u,
				"textures: uniform mode uses 1 file");
	check(vt::voxelTextureFileCount(VoxelTextureMode::SideUniform) == 3u,
				"textures: side-uniform mode uses 3 files");
	check(vt::voxelTextureFileCount(VoxelTextureMode::Custom) == 6u,
				"textures: custom mode uses 6 files");

	// Suffix tables.
	check(std::string(vt::voxelTextureSuffix(VoxelTextureMode::Uniform, 0)) ==
					"",
				"textures: uniform suffix empty");
	bool sideSuffixes =
			std::string(vt::voxelTextureSuffix(VoxelTextureMode::SideUniform, 0)) ==
					"_top" &&
			std::string(vt::voxelTextureSuffix(VoxelTextureMode::SideUniform, 1)) ==
					"_bottom" &&
			std::string(vt::voxelTextureSuffix(VoxelTextureMode::SideUniform, 2)) ==
					"_side";
	check(sideSuffixes, "textures: side-uniform suffixes top/bottom/side");
	const char* customWant[6] = {"_top", "_bottom", "_back", "_front",
															"_right", "_left"};
	bool customSuffixes = true;
	for (std::uint32_t f = 0; f < 6; ++f) {
		if (std::string(vt::voxelTextureSuffix(VoxelTextureMode::Custom, f)) !=
				customWant[f]) {
			customSuffixes = false;
		}
	}
	check(customSuffixes, "textures: custom suffixes per face");

	// Face -> file mapping for all three modes over all 6 faces.
	bool faceMapOk = true;
	for (std::uint32_t face = 0; face < 6; ++face) {
		if (vt::faceTextureFile(VoxelTextureMode::Uniform, face) != 0u) {
			faceMapOk = false;
		}
		const std::uint32_t sideWant =
				face == 0 ? 0u : (face == 1 ? 1u : 2u);
		if (vt::faceTextureFile(VoxelTextureMode::SideUniform, face) !=
				sideWant) {
			faceMapOk = false;
		}
		if (vt::faceTextureFile(VoxelTextureMode::Custom, face) != face) {
			faceMapOk = false;
		}
	}
	check(faceMapOk, "textures: face->file mapping per mode");

	// Default set = plain colors (the sandbox/CI fallback).
	const vv::voxel::VoxelTextureSet plain{};
	bool plainOk = !plain.textured && plain.nominalSize == 32u;
	for (std::uint32_t f = 0; f < 6; ++f) {
		if (plain.faceIndex[f] != vv::voxel::kNoFaceTexture) {
			plainOk = false;
		}
	}
	check(plainOk, "textures: default set is plain colors");

	// --- per-face resolution chains (pass 28) ---
	{
		using vv::voxel::kVoxelFaceSuffixChain;
		const char* want[6][3] = {
		    {"_top", "", nullptr},
		    {"_bottom", "", nullptr},
		    {"_back", "_side", ""},
		    {"_front", "_side", ""},
		    {"_right", "_side", ""},
		    {"_left", "_side", ""},
		};
		bool chainOk = true;
		for (int face = 0; face < 6; ++face) {
			for (int c = 0; c < 3; ++c) {
				const char* got = kVoxelFaceSuffixChain[face][c];
				const char* exp = want[face][c];
				if ((got == nullptr) != (exp == nullptr) ||
				    (got != nullptr && std::string(got) != exp)) {
					chainOk = false;
				}
			}
		}
		check(chainOk, "textures: per-face candidate chains");

		// Mirror of the loader's per-face walk: given the set of
		// existing suffixes, each face takes its first existing
		// candidate; nullptr = no file (plain color for that face).
		const auto resolve = [&](std::uint32_t face,
		                         const std::vector<std::string>& existing) {
			for (const char* s : kVoxelFaceSuffixChain[face]) {
				if (s == nullptr) {
					break;
				}
				if (std::find(existing.begin(), existing.end(), s) !=
				    existing.end()) {
					return std::string(s);
				}
			}
			return std::string();
		};

		// THE reported scenario: grass_top.png + grass_side.png, no
		// bottom file. Old rule: no complete side-uniform set -> the
		// WHOLE type plain (the alias then textured only the bottom).
		// New rule: top + all 4 sides textured, bottom waits for an
		// alias (or stays plain).
		{
			const std::vector<std::string> files = {"_top", "_side"};
			std::string perFace[6];
			for (std::uint32_t f = 0; f < 6; ++f) {
				perFace[f] = resolve(f, files);
			}
			check(perFace[0] == "_top" &&
			          perFace[2] == "_side" && perFace[3] == "_side" &&
			          perFace[4] == "_side" && perFace[5] == "_side" &&
			          perFace[1].empty(),
			      "textures: partial set works per face "
			      "(grass top+side, bottom missing)");
		}
		// Complete custom set: every face its own file.
		{
			const std::vector<std::string> files = {
			    "_top", "_bottom", "_back", "_front", "_right",
			    "_left", "_side", ""};
			bool ok = true;
			const char* wantFace[6] = {"_top", "_bottom", "_back",
			                           "_front", "_right", "_left"};
			for (std::uint32_t f = 0; f < 6; ++f) {
				ok = ok && resolve(f, files) == wantFace[f];
			}
			check(ok, "textures: complete custom set uses per-face files");
		}
		// Side file only: sides textured, top/bottom plain.
		{
			const std::vector<std::string> files = {"_side"};
			check(resolve(0, files).empty() && resolve(1, files).empty() &&
			          resolve(3, files) == "_side",
			      "textures: side-only set textures just the sides");
		}
		// Custom front + uniform file: front specific, rest uniform.
		{
			const std::vector<std::string> files = {"_front", ""};
			check(resolve(3, files) == "_front" &&
			          resolve(0, files) == "" &&
			          resolve(2, files) == "",
			      "textures: custom face beats the uniform fallback");
		}
	}

	// --- alias source resolution (per-face; pass 28) ---
	{
		using vv::voxel::VoxelTextureSet;
		using vv::voxel::kNoFaceTexture;
		// A uniform source: all faces share image 7.
		VoxelTextureSet uni;
		uni.textured = true;
		for (std::uint32_t f = 0; f < 6; ++f) {
			uni.faceIndex[f] = 7;
		}
		// A partial side-uniform source: top=3, sides=9, no bottom.
		VoxelTextureSet partial;
		partial.textured = true;
		partial.faceIndex[0] = 3;
		partial.faceIndex[2] = partial.faceIndex[3] = 9;
		partial.faceIndex[4] = partial.faceIndex[5] = 9;

		check(vv::voxel::resolveFaceTextureIndex(uni, "bottom") == 7u,
		      "textures: uniform source serves any face");
		check(vv::voxel::resolveFaceTextureIndex(partial, "top") == 3u &&
		          vv::voxel::resolveFaceTextureIndex(partial, "bottom") ==
		              kNoFaceTexture &&
		          vv::voxel::resolveFaceTextureIndex(partial, "front") ==
		              9u,
		      "textures: partial source resolves per face");
		check(vv::voxel::resolveFaceTextureIndex(partial, "side") == 9u,
		      "textures: 'side' resolves to the side texture");
		check(vv::voxel::resolveFaceTextureIndex(partial, "wat") ==
		          kNoFaceTexture,
		      "textures: unknown face name -> no texture");
		const VoxelTextureSet untextured{};
		check(vv::voxel::resolveFaceTextureIndex(untextured, "top") ==
		          kNoFaceTexture,
		      "textures: untextured source -> no texture");

		// Face name mapping round trip.
		bool namesOk = true;
		const char* names[6] = {"top", "bottom", "back",
		                        "front", "right", "left"};
		for (std::uint32_t f = 0; f < 6; ++f) {
			std::uint32_t id = 99;
			namesOk = namesOk &&
			          vv::voxel::voxelFaceIdFromName(names[f], id) &&
			          id == f &&
			          std::string(vv::voxel::voxelFaceNameOfId(f)) ==
			              names[f];
		}
		std::uint32_t dummy = 0;
		namesOk = namesOk &&
		          !vv::voxel::voxelFaceIdFromName("sides", dummy);
		check(namesOk, "textures: face name <-> id mapping");
	}
}

// ---------------------------------------------------------------------------
// Streaming priority (pass 27): the generation backlog must be consumed
// BEST-FIRST. Pins the "frustum prioritization is backwards" bug: the
// pump stocks m_genRequests in reverse (best first, so the front is the
// best), but the worker used to pop the BACK - the best coords were
// stuck at the front forever while progressively worse top-ups were
// generated first; the nearest in-frustum chunks came dead last.
// ---------------------------------------------------------------------------
void testStreamPriority() {
	const float chunk = 32.0f;  // chunk world size (32 voxels x 1.0)
	const float cx = 400.0f, cz = 400.0f;  // camera (world units)
	const float fx = 1.0f, fz = 0.0f;      // facing +x

	const auto prio = [&](std::int32_t chx, std::int32_t chz) {
		return vv::vulkan::streamPriority(chx, chz, cx, cz, fx, fz,
		                                  chunk);
	};

	// --- semantics: near-first with a ~96-unit forward bias ---
	{
		// Same distance: in front of the camera beats behind it.
		const int frontChunk = 13;   // ~+416 vs cam x=400 -> ahead
		const int behindChunk = 11;  // ~-352... both ~1 chunk away
		check(prio(frontChunk, 12) > prio(behindChunk, 12),
		      "stream priority: in-front beats behind at equal "
		      "distance");
		// Near beats far.
		check(prio(12, 12) > prio(20, 20),
		      "stream priority: near beats far");
		// The bias is bounded: a chunk 200 units ahead still loses to
		// a sideways chunk 50 units away (near-first dominates).
		check(prio(12, 12) /* ~sideways, 1 chunk */ >
		          prio(19, 12) /* ~7 chunks ahead */,
		      "stream priority: near-first dominates the forward bias");
	}

	// --- the queue simulation: pump top-up + worker consumption ---
	{
		// A 9x9 pending set around chunk (12,12).
		std::vector<std::pair<std::int32_t, std::int32_t>> pending;
		for (std::int32_t z = 8; z <= 16; ++z) {
			for (std::int32_t x = 8; x <= 16; ++x) {
				pending.emplace_back(x, z);
			}
		}
		// rebuildStreamPending(): sort ascending = worst first.
		std::sort(pending.begin(), pending.end(),
		          [&](const auto& a, const auto& b) {
			          return prio(a.first, a.second) <
			             prio(b.first, b.second);
		          });

		// Simulate the pump (top-up: reverse iteration, cap 6) and the
		// workers (2 chunks per frame). done[i] = generated.
		const std::size_t kBacklog = 6, kWorkers = 2;
		std::vector<std::size_t> requests;  // indices into pending
		std::vector<char> done(pending.size(), 0);
		std::vector<float> genOrder;        // priority per generation
		for (int frame = 0; frame < 500; ++frame) {
			// Top-up (pump step 3).
			for (std::size_t i = pending.size(); i-- > 0;) {
				const bool queued =
				    std::find(requests.begin(), requests.end(), i) !=
				    requests.end();
				if (!queued && !done[i] &&
				    requests.size() < kBacklog) {
					requests.push_back(i);
				}
			}
			// Workers consume (FRONT = best, the pass-27 fix).
			for (std::size_t w = 0; w < kWorkers && !requests.empty();
			     ++w) {
				const std::size_t i = requests.front();
				requests.erase(requests.begin());
				done[i] = 1;
				genOrder.push_back(
				    prio(pending[i].first, pending[i].second));
			}
		}
		const bool allDone =
		    std::all_of(done.begin(), done.end(), [](char c) { return c; });
		check(allDone, "stream priority: simulation drains the backlog");
		bool monotonic = true;
		long long inversions = 0;
		for (std::size_t i = 1; i < genOrder.size(); ++i) {
			if (genOrder[i] > genOrder[i - 1]) {
				++inversions;
				monotonic = false;
			}
		}
		check(monotonic,
		      "stream priority: generation runs best-first (no "
		      "inversions)");
		std::printf("stream priority: %zu chunks generated, %lld "
		            "priority inversions\n",
		            genOrder.size(), inversions);
	}
}

int main() {
	testVertexAO();
	testNoiseDeterministic();
	testNoiseRangeAndContinuity();
	testNoiseSimdParity();
	testTerrainHeightBounds();
	testTerrainLayering();
	testTerrainOverhangs();
	testWorldRegion();
	testWorldWalk();
	testWorldDeterminism();
	testWorldEnsureChunk();
	testChunkMatchesGenerator();
	testChunkHeightMap();
	testFloorDivShiftIdentity();
	testTraversalParity();
	testFarField();
	testFarPatchRegion();
	testFarMarch();
	testSunShadowMarch();
	testSunShadowSdfMarch();
	testSdfSoftShadow3d();
	testSdfBoxBuild();
	testSdfBoxHandoff();
	testSdfHandoverPolicy();
	testStreamPriority();
	testVoxelTextures();

	if (g_failures == 0) {
		std::printf("all tests passed\n");
		return 0;
	}
	std::printf("%d test(s) failed\n", g_failures);
	return 1;
}
