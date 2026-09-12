// Pure-logic tests for the terrain/world modules. No Qt, no Vulkan: this
// suite also runs in restricted sandboxes where the game itself cannot.
//
// Run via ctest or directly: ./build/bin/voxel_tests

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <vector>

#include "terrain/FarField.hpp"
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
	// Box centered on chunk (0,0)'s center voxel (16,16).
	check(field.originVoxX == 16 - 64 && field.originVoxZ == 16 - 64,
				"far: origin = center - dim*cell/2");

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
			const float h = gen.heightAtF(wx, wz);
			const std::int32_t surface = std::int32_t(std::floor(h));
			if (int(packed & 0xFFFFu) != surface + 1) {
				heightsOk = false;
			}
			if ((packed >> 16u) != std::uint32_t(gen.typeForColumn(surface, h))) {
				typesOk = false;
			}
		}
	}
	check(heightsOk, "far: height = floor(heightAtF(center)) + 1 per cell");
	check(typesOk, "far: type = surface type at cell center");

	const auto again = vv::terrain::FarField::build(gen, 0, 0, radius, cell,
																										chunk);
	check(again.cells == field.cells && again.originVoxX == field.originVoxX,
				"far: deterministic rebuild");
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

// Mirror of the shader's sunShadow (same offsets, clamps, tie-breaks, cap).
bool sunLitMirror(const ShadowWorld& w, const double origin[3],
									const double n[3], const double sun[3]) {
	if (sun[1] <= 0.05) {
		return true;
	}
	const double EPS = 1e-6;
	double o[3];
	for (int a = 0; a < 3; ++a) {
		o[a] = origin[a] + n[a] * 1e-3 + sun[a] * 1e-2;
	}
	int stepX = (sun[0] > 0.0) ? 1 : -1;
	int stepZ = (sun[2] > 0.0) ? 1 : -1;
	double tMaxX = 1e30, tMaxZ = 1e30, dX = 1e30, dZ = 1e30;
	int colX = int(std::floor(o[0]));
	int colZ = int(std::floor(o[2]));
	if (std::abs(sun[0]) > EPS) {
		tMaxX = (double(colX + ((stepX > 0) ? 1 : 0)) - o[0]) / sun[0];
		dX = std::abs(1.0 / sun[0]);
	} else {
		stepX = 0;
	}
	if (std::abs(sun[2]) > EPS) {
		tMaxZ = (double(colZ + ((stepZ > 0) ? 1 : 0)) - o[2]) / sun[2];
		dZ = std::abs(1.0 / sun[2]);
	} else {
		stepZ = 0;
	}

	double s = 0.0;
	for (int i = 0; i < 256; ++i) {
		const double sExit = std::min(tMaxX, tMaxZ);
		const double y0 = o[1] + sun[1] * s;
		if (y0 >= w.maxTerr) {
			return true;
		}
		const bool inNear = colX >= 0 && colX < w.near.wx && colZ >= 0 &&
												colZ < w.near.wz;
		if (inNear) {
			const unsigned bound = w.near.boundAt(colX, colZ);
			if (bound != 0xFFFFu && double(bound) > y0) {
				const double y1 = o[1] + sun[1] * sExit;
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

void testSunShadowMarch() {
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

	// Far field: rolling hills + a ridge taller than the near terrain (so
	// far terrain can shadow near terrain).
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

	// Sun: normalize(0.5, 1.0, 0.5) - same shape as the game default.
	const double len = std::sqrt(0.25 + 1.0 + 0.25);
	const double sun[3] = {0.5 / len, 1.0 / len, 0.5 / len};

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
		bool m = sunLitMirror(w, origin, n, sun);
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

int main() {
	testVertexAO();
	testNoiseDeterministic();
	testNoiseRangeAndContinuity();
	testNoiseSimdParity();
	testTerrainHeightBounds();
	testTerrainLayering();
	testWorldRegion();
	testWorldWalk();
	testWorldDeterminism();
	testWorldEnsureChunk();
	testChunkMatchesGenerator();
	testChunkHeightMap();
	testTraversalParity();
	testFarField();
	testFarMarch();
	testSunShadowMarch();

	if (g_failures == 0) {
		std::printf("all tests passed\n");
		return 0;
	}
	std::printf("%d test(s) failed\n", g_failures);
	return 1;
}
