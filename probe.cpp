// Renderer-shaped probe for the pass-26 sun light grid: real terrain,
// real FarField, the renderer's exact adapter, renderer-scale window,
// nonzero origin, sliced ticks, two cycles. Validates seed parity vs the
// exact per-cell march (the lightprobe2 oracle) and prints lit fraction.

#include "terrain/FarField.hpp"
#include "terrain/SunLightGrid.hpp"
#include "terrain/TerrainGenerator.hpp"
#include "voxel/Chunk.hpp"
#include "voxel/World.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {

double nowMs() {
	return std::chrono::duration<double, std::milli>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

constexpr int CS = 32;    // VoxelConfig default chunkSizeX/Z
constexpr int WH = 128;   // worldHeight
constexpr int RAD = 12;   // renderRadiusChunks
constexpr int NEARW = (2 * RAD + 1) * CS;  // 800: near region width
// Near region centered at chunk (0,0): voxels [-384, 416)
constexpr int NEARMIN = -RAD * CS;
constexpr int NEARMAX = NEARMIN + NEARW;

// The renderer's SunGridVoxels adapter, verbatim semantics.
struct Adapter final : public vv::terrain::SunLightVoxels {
	vv::voxel::World* world = nullptr;
	const vv::terrain::FarField* far = nullptr;
	bool farActive = false;

	const std::uint8_t* columnVoxels(std::int32_t x,
	                                 std::int32_t z) const override {
		const std::int32_t cs = CS;
		const std::int32_t cx = static_cast<std::int32_t>(
		    std::floor(static_cast<double>(x) / cs));
		const std::int32_t cz = static_cast<std::int32_t>(
		    std::floor(static_cast<double>(z) / cs));
		const vv::voxel::Chunk* chunk = world->findChunk({cx, cz});
		if (!chunk || chunk->voxelTypes().empty()) {
			return nullptr;
		}
		const std::size_t height = WH;
		const std::size_t lx = static_cast<std::size_t>(x - cx * cs);
		const std::size_t lz = static_cast<std::size_t>(z - cz * cs);
		return chunk->voxelTypes().data() + (lx + lz * cs * height);
	}

	std::uint16_t farHeightAt(std::int32_t x, std::int32_t z) const override {
		if (!farActive || far->dim == 0) {
			return 0;
		}
		const double fx = std::floor(
		    (static_cast<double>(x) + 0.5 - far->originVoxX) /
		    static_cast<double>(far->cellVoxels));
		const double fz = std::floor(
		    (static_cast<double>(z) + 0.5 - far->originVoxZ) /
		    static_cast<double>(far->cellVoxels));
		if (fx < 0.0 || fz < 0.0 || fx >= static_cast<double>(far->dim) ||
		    fz >= static_cast<double>(far->dim)) {
			return 0;
		}
		return static_cast<std::uint16_t>(
		    far->cells[static_cast<std::size_t>(fx) +
		               static_cast<std::size_t>(fz) * far->dim] & 0xFFFFu);
	}
};

}  // namespace

int main() {
	using vv::voxel::World;
	using vv::voxel::Chunk;

	double t0 = nowMs();
	World world(vv::terrain::TerrainConfig{}, CS, WH, CS);
	std::vector<const Chunk*> created;
	std::vector<vv::voxel::ChunkCoord> evicted;
	world.ensureRegion(0, 0, RAD, created, evicted);
	std::printf("chunks: %zu in %.0f ms\n", created.size(), nowMs() - t0);

	t0 = nowMs();
	const vv::terrain::TerrainGenerator gen(vv::terrain::TerrainConfig{});
	const auto far = vv::terrain::FarField::build(gen, 0, 0, 64, 4, CS);
	std::printf("far: dim %u cell %u origin (%d,%d) in %.0f ms\n", far.dim,
	            far.cellVoxels, far.originVoxX, far.originVoxZ, nowMs() - t0);

	const int maxTerr = gen.maxHeightVoxels() + 1;  // march ascend bound
	const double len = std::sqrt(0.25 + 1.0 + 0.25);
	const double sun[3] = {0.5 / len, 1.0 / len, 0.5 / len};
	std::printf("maxTerr %d, sun (%.4f, %.4f, %.4f)\n", maxTerr, sun[0],
	            sun[1], sun[2]);

	Adapter adapter;
	adapter.world = &world;
	adapter.far = &far;
	adapter.farActive = true;

	// Exact per-cell march oracle (lightprobe2, region = near region).
	auto litMarch = [&](double ox, double oy, double oz) {
		if (sun[1] <= 0.05) return true;
		int cx = (int)std::floor(ox), cz = (int)std::floor(oz);
		const int sx = sun[0] > 0 ? 1 : -1, sz = sun[2] > 0 ? 1 : -1;
		double tMaxX = 1e30, tMaxZ = 1e30, dX = 1e30, dZ = 1e30;
		if (std::abs(sun[0]) > 1e-6) {
			const double n = cx + (sx > 0 ? 1 : 0);
			tMaxX = (n - ox) / sun[0];
			dX = std::abs(1.0 / sun[0]);
		}
		if (std::abs(sun[2]) > 1e-6) {
			const double n = cz + (sz > 0 ? 1 : 0);
			tMaxZ = (n - oz) / sun[2];
			dZ = std::abs(1.0 / sun[2]);
		}
		double s = 0.0;
		for (int i = 0; i < 256; ++i) {
			const double y0 = oy + sun[1] * s;
			if (y0 >= maxTerr) return true;
			const Chunk* ch =
			    (cx >= NEARMIN && cx < NEARMAX && cz >= NEARMIN && cz < NEARMAX)
			        ? world.findChunk(
			              {static_cast<std::int32_t>(std::floor(
			                   static_cast<double>(cx) / CS)),
			               static_cast<std::int32_t>(std::floor(
			                   static_cast<double>(cz) / CS))})
			        : nullptr;
			if (ch) {
				const unsigned bound = ch->heightMap()[(cx & 31) + (cz & 31) * CS];
				if (bound != 0xFFFFu && double(bound) > y0) {
					const double y1 = oy + sun[1] * std::min(tMaxX, tMaxZ);
					const int yTop = std::min(
					    int(std::floor(std::min(y1, double(bound) - 1.0))),
					    WH - 1);
					const std::uint8_t* types = ch->voxelTypes().data();
					for (int y = std::max(int(std::floor(y0)), 0); y <= yTop;
					     ++y) {
						if (types[(cx & 31) + y * CS + (cz & 31) * CS * WH] != 0) {
							return false;
						}
					}
				}
			} else {
				const int fx = int(std::floor(
				    (double(cx) + 0.5 - far.originVoxX) /
				    double(far.cellVoxels)));
				const int fz = int(std::floor(
				    (double(cz) + 0.5 - far.originVoxZ) /
				    double(far.cellVoxels)));
				if (fx >= 0 && fx < int(far.dim) && fz >= 0 &&
				    fz < int(far.dim)) {
					const double h =
					    double(far.cells[fx + fz * int(far.dim)] & 0xFFFFu);
					if (h > 0.0 && y0 < h) return false;
				}
			}
			const double t = std::min(tMaxX, tMaxZ);
			const bool takeX = tMaxX < tMaxZ;
			s = t;
			tMaxX += takeX ? dX : 0.0;
			tMaxZ += takeX ? 0.0 : dZ;
			cx += takeX ? sx : 0;
			cz += takeX ? 0 : sz;
		}
		return true;
	};

	// Voxel helper on world space.
	auto solidAt = [&](int x, int y, int z) -> int {
		if (y < 0 || y >= WH) return 0;
		if (x < NEARMIN || x >= NEARMAX || z < NEARMIN || z >= NEARMAX) {
			return 0;  // outside near: not sampled for air-cell parity
		}
		const Chunk* ch = world.findChunk(
		    {static_cast<std::int32_t>(std::floor(double(x) / CS)),
		     static_cast<std::int32_t>(std::floor(double(z) / CS))});
		if (!ch) return 0;
		return ch->voxelTypes()[(x & 31) + y * CS + (z & 31) * CS * WH] != 0;
	};

	const int W = 800, H = WH;  // the renderer's window size
	std::vector<std::uint8_t> field(std::size_t(W) * W * H, 77);
	vv::terrain::SunLightGrid grid;

	// The renderer's origin math, verbatim (camera voxel pos -> origin).
	const auto originFor = [&](double camX, double camZ) {
		const double half = 0.5 * W;
		const double snap = 256.0;
		const std::int32_t ox =
		    std::int32_t(std::floor((camX - half) / snap)) * 256;
		const std::int32_t oz =
		    std::int32_t(std::floor((camZ - half) / snap)) * 256;
		return std::make_pair(ox, oz);
	};

	const auto runCycle = [&](double camX, double camZ, int cycle) {
		std::printf("--- cycle %d: cam (%.1f, %.1f)\n", cycle, camX, camZ);
		auto [ox, oz] = originFor(camX, camZ);
		std::printf("origin (%d, %d), window [%d,%d) x [%d,%d)\n", ox, oz, ox,
		            ox + W, oz, oz + W);
		grid.setOrigin(ox, oz);
		grid.setCenter(camX, camZ);
		grid.requestRebuild();
		t0 = nowMs();
		int ticks = 0;
		for (;;) {
			++ticks;
			if (grid.tick(3.0)) break;
		}
		std::printf("built in %.0f ms (%d ticks), %llu lit cells\n",
		            nowMs() - t0, ticks,
		            (unsigned long long)grid.litCells());

		// Seed parity: random air cells inside near ∩ window.
		std::mt19937 rng(1234 + cycle);
		long long checked = 0, lit = 0, wrong = 0;
		auto checkCell = [&](int x, int y, int z) {
			if (solidAt(x, y, z)) return;
			if (x < ox || x >= ox + W || z < oz || z >= oz + W) return;
			++checked;
			const bool m = litMarch(x + 0.5, y + 0.5, z + 0.5);
			const std::size_t idx =
			    std::size_t(x - ox) + std::size_t(z - oz) * W +
			    std::size_t(y) * W * W;
			const bool g = field[idx] == 0;
			if (m) ++lit;
			if (m != g && wrong < 8) {
				std::printf("  MISMATCH (%d,%d,%d): march %d grid %d "
				            "(d=%u)\n",
				            x, y, z, int(m), int(g), unsigned(field[idx]));
			}
			if (m != g) ++wrong;
		};
		for (long long i = 0; i < 400000; ++i) {
			const int x = NEARMIN + int(rng() % unsigned(NEARW));
			const int z = NEARMIN + int(rng() % unsigned(NEARW));
			const int y = int(rng() % unsigned(WH));
			checkCell(x, y, z);
		}
		// Ground band (first air above solid): the visible surface.
		long long groundChecked = 0, groundLit = 0;
		for (int z = NEARMIN + 100; z < NEARMAX - 100; z += 3) {
			for (int x = NEARMIN + 100; x < NEARMAX - 100; x += 3) {
				for (int y = 0; y < WH; ++y) {
					if (solidAt(x, y, z)) continue;
					++groundChecked;
					const bool m = litMarch(x + 0.5, y + 0.5, z + 0.5);
					const std::size_t idx =
					    std::size_t(x - ox) + std::size_t(z - oz) * W +
					    std::size_t(y) * W * W;
					if (m) ++groundLit;
					if (x >= ox && x < ox + W && z >= oz && z < oz + W) {
						++checked;
						if (m) ++lit;
						if (m != (field[idx] == 0)) {
							++wrong;
							if (wrong < 8) {
								std::printf("  GROUND MISMATCH (%d,%d,%d): "
								            "march %d grid %d (d=%u)\n",
								            x, y, z, int(m),
								            int(field[idx] == 0),
								            unsigned(field[idx]));
							}
						}
					}
					break;  // first air only
				}
			}
		}
		std::printf("parity: %lld wrong / %lld checked (%lld lit)\n", wrong,
		            checked, lit);
		std::printf("ground lit fraction: %.3f (%lld / %lld)\n",
		            groundChecked ? double(groundLit) / groundChecked : 0.0,
		            groundLit, groundChecked);
		if (wrong > 0) {
			std::printf("RESULT: PARITY FAILED\n");
		} else {
			std::printf("RESULT: parity ok\n");
		}
	};

	grid.configure(W, W, H, CS, sun[0], sun[1], sun[2],
	               std::uint32_t(maxTerr - 1), &adapter, field.data());

	// Camera inside the near region, asymmetric (nonzero origins, window
	// deliberately offset from the near region on both axes).
	runCycle(137.5, -251.5, 1);
	// Second cycle: moved camera (stale-state across origin change).
	runCycle(455.5, 120.5, 2);

	std::printf("done\n");
	return 0;
}
