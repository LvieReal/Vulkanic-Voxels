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
#include "terrain/SunLightGrid.hpp"
#include "terrain/Noise.hpp"
#include "terrain/TerrainGenerator.hpp"
#include "voxel/Chunk.hpp"
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
			++checked;
			const bool equal = (a.hit == b.hit) && (!a.hit ||
					(a.cell[0] == b.cell[0] && a.cell[1] == b.cell[1] &&
					 a.cell[2] == b.cell[2] && a.type == b.type &&
					 a.axis == b.axis && a.sign == b.sign &&
					 std::abs(a.t - b.t) < 1e-9));
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
		std::printf("parity world %d: %d rays checked\n", worldKind, checked);
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
}


// ---------------------------------------------------------------------------
// Flood-fill sun light grid (pass 26): SunLightGrid over the ShadowWorld
// fixture + a deterministic cost-model fixture. Pins:
//  - seed parity: field d==0  <=>  exact march lit, EVERY air cell;
//  - boundary: lit ground light exactly 1.0, shadowed < 1.0 (no lit gaps);
//  - smoothness: flat-adjacent ground light steps <= 0.25;
//  - deep umbra: far-from-lit shadowed cells are dark;
//  - directional costs: sealed tunnels through solid rock force single
//    paths, so d must equal k * round(8 * cost(direction));
//  - incremental ticks == one-shot; refresh after geometry change;
//  - low sun => everything lit.
// ---------------------------------------------------------------------------

namespace {

class FixtureLightVoxels : public vv::terrain::SunLightVoxels {
public:
	const ShadowWorld* w = nullptr;

	const std::uint8_t* columnVoxels(std::int32_t x,
	                                 std::int32_t z) const override {
		if (x < 0 || x >= w->near.wx || z < 0 || z >= w->near.wz) {
			return nullptr;
		}
		return &w->near.cells[std::size_t(x) +
		                      std::size_t(z) * w->near.wx * w->near.wh];
	}

	std::uint16_t farHeightAt(std::int32_t x,
	                          std::int32_t z) const override {
		const int fx = int(std::floor(
			(double(x) + 0.5 - double(w->farOrigin)) / w->farCell));
		const int fz = int(std::floor(
			(double(z) + 0.5 - double(w->farOrigin)) / w->farCell));
		if (fx < 0 || fz < 0 || fx >= int(w->farDim) ||
		    fz >= int(w->farDim)) {
			return 0;
		}
		return std::uint16_t(
			w->farCells[std::size_t(fx) + std::size_t(fz) * w->farDim] &
			0xFFFFu);
	}
};

double lightFromDist(std::uint8_t d, double budget) {
	return std::max(0.0, 1.0 - (double(d) * 0.125) / budget);
}

void drainGrid(vv::terrain::SunLightGrid& g, double slice) {
	for (;;) {
		if (g.tick(slice)) {
			return;
		}
	}
}

}  // namespace

void testSunLightGrid() {
	ShadowWorld w = makeShadowWorld();
	double sun[3] = {0, 0, 0};
	shadowSun(sun);
	FixtureLightVoxels vox;
	vox.w = &w;

	const int C = 64, R = 64, H = 48;
	const int V = int(w.maxTerr) - 1;  // march ascend bound = maxTerr
	std::vector<std::uint8_t> field(std::size_t(C) * R * H, 77);
	vv::terrain::SunLightGrid grid;
	grid.configure(C, R, H, w.near.wx, sun[0], sun[1], sun[2],
	               std::uint32_t(V), &vox, field.data());
	grid.setOrigin(0, 0);
	grid.setCenter(32.5, 32.5);
	drainGrid(grid, 1e9);

	// --- seed parity (exhaustive over every air cell) ---
	{
		long long checked = 0, lit = 0, wrong = 0;
		for (int z = 0; z < R; ++z) {
			for (int x = 0; x < C; ++x) {
				for (int y = 0; y < H; ++y) {
					if (w.near.at(x, y, z) != 0) {
						continue;
					}
					const double o[3] = {x + 0.5, y + 0.5, z + 0.5};
					const bool m = sunRayEscapesMirror(w, o, sun);
					const bool c =
					    field[std::size_t(x) + std::size_t(z) * C +
					          std::size_t(y) * C * R] == 0;
					++checked;
					if (m) {
						++lit;
					}
					if (m != c && ++wrong <= 3) {
						std::printf(
							"FAIL light seed (%d,%d,%d): march %d "
							"grid %d\n",
							x, y, z, int(m), int(c));
					}
				}
			}
		}
		check(wrong == 0, "light grid: seeds match the exact march");
		check(lit > 20000 && checked - lit > 2000,
		      "light grid: both outcomes well exercised");
		std::printf("light grid: %lld air cells, %lld lit, seed parity "
		            "exact\n",
		            checked, lit);
	}

	// --- boundary + smoothness + deep umbra on ground cells ---
	{
		const double budget = 14.0;
		// Ground = first air cell above the column top.
		const auto groundY = [&](int x, int z) {
			for (int y = 0; y < H; ++y) {
				if (w.near.at(x, y, z) == 0) {
					return y;
				}
			}
			return -1;
		};
		std::vector<char> litG(C * R, 0);
		for (int z = 0; z < R; ++z) {
			for (int x = 0; x < C; ++x) {
				const int y = groundY(x, z);
				if (y < 0) {
					continue;
				}
				const double o[3] = {x + 0.5, y + 0.5, z + 0.5};
				litG[x + z * C] =
				    sunRayEscapesMirror(w, o, sun) ? 1 : 0;
			}
		}
		// Boundary: lit -> exactly 1.0; shadowed -> strictly < 1.0.
		long long litCells = 0, shadowCells = 0, gaps = 0;
		for (int z = 0; z < R; ++z) {
			for (int x = 0; x < C; ++x) {
				const int y = groundY(x, z);
				if (y < 0) {
					continue;
				}
				const double l = lightFromDist(
				    field[std::size_t(x) + std::size_t(z) * C +
				          std::size_t(y) * C * R],
				    budget);
				if (litG[x + z * C]) {
					++litCells;
					if (l < 0.999) {
						++gaps;
					}
				} else {
					++shadowCells;
					if (l >= 0.999) {
						++gaps;
					}
				}
			}
		}
		check(gaps == 0, "light grid: boundary exact (no lit gaps)");
		// Smoothness: flat-adjacent ground pairs.
		double maxStep = 0;
		long long pairs = 0;
		for (int z = 0; z < R; ++z) {
			for (int x = 0; x < C; ++x) {
				const int y0 = groundY(x, z);
				if (y0 < 0) {
					continue;
				}
				const double l0 = lightFromDist(
				    field[std::size_t(x) + std::size_t(z) * C +
				          std::size_t(y0) * C * R],
				    budget);
				const auto step = [&](int x1, int z1) {
					const int y1 = groundY(x1, z1);
					if (y1 < 0 || std::abs(y1 - y0) > 1) {
						return;
					}
					const double l1 = lightFromDist(
					    field[std::size_t(x1) + std::size_t(z1) * C +
					          std::size_t(y1) * C * R],
					    budget);
					maxStep = std::max(maxStep, std::abs(l1 - l0));
					++pairs;
				};
				if (x + 1 < C) {
					step(x + 1, z);
				}
				if (z + 1 < R) {
					step(x, z + 1);
				}
			}
		}
		check(maxStep <= 0.25, "light grid: smooth ground gradients");
		// Deep umbra: shadowed >= 30 cells (Chebyshev) from any lit cell.
		long long deep = 0, deepWrong = 0;
		for (int z = 0; z < R; ++z) {
			for (int x = 0; x < C; ++x) {
				if (litG[x + z * C]) {
					continue;
				}
				bool nearLit = false;
				for (int dz = -30; dz <= 30 && !nearLit; ++dz) {
					for (int dx = -30; dx <= 30; ++dx) {
						const int nx = x + dx, nz = z + dz;
						if (nx >= 0 && nx < C && nz >= 0 && nz < R &&
						    litG[nx + nz * C]) {
							nearLit = true;
							break;
						}
					}
				}
				if (nearLit) {
					continue;
				}
				const int y = groundY(x, z);
				if (y < 0) {
					continue;
				}
				++deep;
				if (lightFromDist(field[std::size_t(x) +
				                        std::size_t(z) * C +
				                        std::size_t(y) * C * R],
				                  budget) > 0.02) {
					++deepWrong;
				}
			}
		}
		check(deepWrong == 0, "light grid: deep umbra stays dark");
		std::printf("light grid: ground %lld lit / %lld shadowed, max "
		            "step %.3f over %lld pairs, %lld deep-umbra cells\n",
		            litCells, shadowCells, maxStep, pairs, deep);
	}

	// --- incremental ticks == one shot ---
	{
		std::vector<std::uint8_t> field2(std::size_t(C) * R * H, 77);
		vv::terrain::SunLightGrid g2;
		g2.configure(C, R, H, w.near.wx, sun[0], sun[1], sun[2],
		             std::uint32_t(V), &vox, field2.data());
		g2.setOrigin(0, 0);
		g2.setCenter(10.5, 50.5);
		drainGrid(g2, 0.02);
		bool same = field2 == field;
		if (!same) {
			int shown = 0;
			for (std::size_t i = 0; i < field.size() && shown < 5; ++i) {
				if (field[i] != field2[i]) {
					const int y = int(i / (C * R));
					const int z = int((i / C) % R);
					const int x = int(i % C);
					std::printf("  DIFF cell (%d,%d,%d): one-shot d=%d "
					            "sliced d=%d\n",
					            x, y, z, int(field[i]), int(field2[i]));
					++shown;
				}
			}
		}
		check(same, "light grid: sliced ticks match a one-shot build");
	}

	// --- refresh after geometry change (new wall + all-air column) ---
	{
		for (int y = 0; y < 44; ++y) {
			w.near.cells[std::size_t(30) + std::size_t(y) * w.near.wx +
			             std::size_t(10) * w.near.wx * w.near.wh] = 2;
		}
		for (int y = 0; y < w.near.wh; ++y) {
			w.near.cells[std::size_t(50) + std::size_t(y) * w.near.wx +
			             std::size_t(40) * w.near.wx * w.near.wh] = 0;
		}
		w.near.recomputeHeights();
		grid.requestRebuild();
		drainGrid(grid, 1e9);
		// Behind the new wall (down-sun): now shadowed, march agrees.
		const double o[3] = {28.5, double(w.near.boundAt(28, 10)) + 0.5,
		                     10.5};
		const int yG = int(w.near.boundAt(28, 10));
		const bool m = sunRayEscapesMirror(w, o, sun);
		const bool c = field[std::size_t(28) + std::size_t(10) * C +
		                     std::size_t(yG) * C * R] == 0;
		check(!m && !c, "light grid: refresh picks up new blockers");
		// The all-air column (50, z=40) must behave as a NEAR no-block
		// column (bound 0, no walk) - NOT fall back to far heights
		// (~38 there), which would shadow its cells differently than
		// the march. Every air cell of the column must agree.
		long long colWrong = 0, colChecked = 0;
		for (int y = 0; y < H; ++y) {
			const double o2[3] = {50.5, double(y) + 0.5, 40.5};
			const bool m2 = sunRayEscapesMirror(w, o2, sun);
			const bool c2 = field[std::size_t(50) + std::size_t(40) * C +
			                      std::size_t(y) * C * R] == 0;
			++colChecked;
			if (m2 != c2) {
				++colWrong;
			}
		}
		check(colWrong == 0,
		      "light grid: all-air near column matches the march");
		(void)colChecked;
	}

	// --- directional cost model (sealed tunnels through solid rock) ---
	{
		ShadowWorld cw;  // 20 x 12 x 20, all solid except shafts/tunnels
		cw.near.wx = 20;
		cw.near.wz = 20;
		cw.near.wh = 12;
		cw.maxTerr = 13.0f;
		cw.near.cells.assign(std::size_t(20) * 12 * 20, 1);
		cw.farCells.clear();
		cw.farDim = 0;  // no far data: nothing blocks from beyond
		const auto carve = [&](int x, int y0, int y1, int z) {
			for (int y = y0; y <= y1; ++y) {
				cw.near.cells[std::size_t(x) + std::size_t(y) * 20 +
				              std::size_t(z) * 20 * 12] = 0;
			}
		};
		carve(2, 9, 11, 2);    // shaft S1 (lit from the top)
		for (int x = 3; x <= 10; ++x) {
			carve(x, 9, 9, 2);  // tunnel A: +x from S1
		}
		carve(17, 9, 11, 5);   // shaft S2
		for (int x = 16; x >= 9; --x) {
			carve(x, 9, 9, 5);  // tunnel B: -x from S2
		}
		carve(10, 9, 11, 10);  // shaft S4
		carve(11, 9, 9, 10);   //   then +x
		carve(12, 9, 11, 10);  //   then UP (dead end)
		carve(5, 9, 11, 12);   // shaft S5
		carve(5, 7, 8, 12);    //   then DOWN (dead end)
		cw.near.recomputeHeights();

		FixtureLightVoxels cvox;
		cvox.w = &cw;
		std::vector<std::uint8_t> cfield(std::size_t(20) * 20 * 12, 77);
		vv::terrain::SunLightGrid cg;
		cg.configure(20, 20, 12, 20, sun[0], sun[1], sun[2], 12, &cvox,
		             cfield.data());  // V = maxTerr(13) - 1, like w
		cg.setOrigin(0, 0);
		cg.setCenter(10.5, 10.5);
		drainGrid(cg, 1e9);

		const auto dAt = [&](int x, int y, int z) {
			return cfield[std::size_t(x) + std::size_t(z) * 20 +
			             std::size_t(y) * 20 * 20];
		};
		// Sun azimuth (1,1)/sqrt2: +x step cost 1+0.5*0.7071 = 1.3536
		// -> round(8*c) = 11 units; -x -> 5; up -> 16; down -> 8. The
		// tunnels are sealed, so each cell is reachable only through
		// the tunnel line: consecutive deltas must equal the step cost
		// exactly (absolute values depend on which shaft cells are lit).
		// Sun azimuth (1,1)/sqrt2. Horizontal: +sun-axis step costs
		// 1 + 0.5*0.7071 = 1.3536 units -> round(8*c) = 11; against the
		// sun 0.6464 -> 5. Vertical: down 1 -> 8, up 2 -> 16. Diagonal
		// composites: (+1,+1,0) sqrt(1.3536^2+2^2)=2.416 -> 19;
		// (-1,-1,0) sqrt(0.6464^2+1)=1.191 -> 10.
		bool costsOk = true;
		const int expectCosts[][4] = {
		    {1, 0, 0, 11},  {-1, 0, 0, 5},  {0, 0, 1, 11},
		    {0, 0, -1, 5},  {0, 1, 0, 16},  {0, -1, 0, 8},
		    {1, 1, 0, 19},  {-1, -1, 0, 10}};
		for (const auto& e : expectCosts) {
			if (cg.stepCost8ForTest(e[0], e[1], e[2]) != e[3]) {
				costsOk = false;
			}
		}
		// The tunnels are sealed, so each cell is reachable only along
		// the line: consecutive deltas must equal the step cost exactly
		// (absolute values also depend on which shaft cells are lit -
		// only the shaft tops see the sun diagonally).
		for (int k = 4; k <= 10; ++k) {  // tunnel A: +x away from S1
			const int d0 = dAt(k - 1, 9, 2), d1 = dAt(k, 9, 2);
			if (d1 - d0 != 11 || d1 <= d0) {
				costsOk = false;
			}
		}
		for (int k = 9; k <= 15; ++k) {  // tunnel B: -x toward S2
			const int d0 = dAt(k + 1, 9, 5), d1 = dAt(k, 9, 5);
			if (d1 - d0 != 5 || d1 <= d0) {
				costsOk = false;
			}
		}
		for (int k = 7; k <= 8; ++k) {  // down shaft below S5
			const int d0 = dAt(5, k, 12), d1 = dAt(5, k + 1, 12);
			if (d0 - d1 != 8 || d0 <= d1) {
				costsOk = false;
			}
		}
		// The sealed 2-cell pocket up from the horizontal tunnel at
		// (12, 10..11, 10): its TOP sees the sun diagonally (ray escapes
		// before the solid mass), so light flows DOWN it: deltas 8.
		for (int k = 10; k <= 11; ++k) {
			const int d0 = dAt(12, k - 1, 10), d1 = dAt(12, k, 10);
			if (d0 - d1 != 8 || d0 <= d1) {
				costsOk = false;
			}
		}
		check(costsOk, "light grid: directional step costs exact");
		// Seed parity on the tunnel fixture too (overhang columns).
		{
			long long wrong = 0;
			for (int z = 0; z < 20; ++z) {
				for (int x = 0; x < 20; ++x) {
					for (int y = 0; y < 12; ++y) {
						if (cw.near.at(x, y, z) != 0) {
							continue;
						}
						const double o[3] = {x + 0.5, y + 0.5, z + 0.5};
						const bool m = sunRayEscapesMirror(cw, o, sun);
						const bool c = dAt(x, y, z) == 0;
						if (m != c) {
							++wrong;
						}
					}
				}
			}
			check(wrong == 0,
			      "light grid: tunnel fixture seed parity exact");
			std::printf("light grid: cost fixture parity ok\n");
		}
	}

	// --- low sun: everything lit ---
	{
		std::vector<std::uint8_t> field3(std::size_t(C) * R * H, 77);
		vv::terrain::SunLightGrid g3;
		g3.configure(C, R, H, w.near.wx, 0.3, 0.04, 0.3,
		             std::uint32_t(V), &vox, field3.data());
		g3.setOrigin(0, 0);
		drainGrid(g3, 1e9);
		bool allLit = true;
		for (int z = 0; z < R && allLit; ++z) {
			for (int x = 0; x < C && allLit; ++x) {
				for (int y = 0; y < H; ++y) {
					if (w.near.at(x, y, z) == 0 &&
					    field3[std::size_t(x) + std::size_t(z) * C +
					           std::size_t(y) * C * R] != 0) {
						allLit = false;
						break;
					}
				}
			}
		}
		check(allLit, "light grid: low sun lights everything");
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
	testTraversalParity();
	testFarField();
	testFarPatchRegion();
	testFarMarch();
	testSunShadowMarch();
	testSunLightGrid();
	testVoxelTextures();

	if (g_failures == 0) {
		std::printf("all tests passed\n");
		return 0;
	}
	std::printf("%d test(s) failed\n", g_failures);
	return 1;
}
