#pragma once

#include <cstdint>
#include <vector>

namespace vv::terrain {

// Voxel source for the sun light grid (implemented by the renderer over
// World + far LOD, and by the CPU tests over their fixture). All queries
// are in world voxel coordinates.
class SunLightVoxels {
public:
	virtual ~SunLightVoxels() = default;

	// The worldHeight() voxel bytes of column (x, z), strided by
	// columnStride (the chunk's X + Y*chunkX + Z*chunkX*H layout), or
	// nullptr when the chunk is not loaded. Byte value = voxel type
	// (0 = air).
	virtual const std::uint8_t* columnVoxels(std::int32_t x,
	                                         std::int32_t z) const = 0;

	// Far-LOD cell height covering world column (x, z); 0 = no data
	// (nothing blocks from there). Used for cone columns beyond the near
	// window and for empty slots inside it (same fallback the shader's
	// shadow march uses via resolveColumn).
	virtual std::uint16_t farHeightAt(std::int32_t x,
	                                  std::int32_t z) const = 0;
};

// CPU flood-fill sun light grid (pass 26).
//
// Field: one u8 per region cell (cols x rows x height, x + z*cols +
// y*cols*rows) storing the geodesic BLEED DISTANCE from the nearest
// directly-sunlit air cell, quantized x8 per voxel (0 = directly lit,
// 255 = unreachable/solid). The shader turns it into light with
// light = clamp(1 - d*0.125 / budget, 0, 1) where budget (voxels) is a
// pure GPU knob (scene.misc.w) - the field is budget-independent.
//
// Build (exact by construction, validated vs the exact per-cell march on
// real generated terrain: 3.3M exhaustive + 300k random cells, 0 misses):
//  1. PREPASS: per column, solid spans + air bitmask + top (from real
//     voxel bytes - overhangs included).
//  2. SEED: per column, a table-driven cone walk. All seed rays start at
//     CELL CENTERS (fixed 0.5 offset), so the sun DDA column sequence and
//     integer height gains (kIn/kOut per crossing) are identical for every
//     cell - precomputed once per sun direction. Heightmap cone columns
//     block y <= H where H = max(min(top, maxTerrainY) - kIn) over the
//     cone; overhang columns contribute short solid-span intervals
//     [a - kOut, min(b, maxTerrainY) - kIn] (per-column bitset). Direct
//     light NEVER propagates - the lit/shadow boundary is exactly the
//     exact-march boundary (the pass-14 anisotropic-DP smearing cannot
//     recur). Columns beyond the window (or empty slots) use far heights,
//     exactly like the march.
//  3. BORDER: lit air cells adjacent (26-conn) to non-lit air become
//     Dijkstra sources (detected inverted: shadowed cells mark neighbors).
//  4. FILL: Dial bucket-queue Dijkstra through AIR only, 26-connectivity,
//     direction-weighted costs (x8 units): horizontal steps cost
//     1 + 0.5*dot(step, sunAzimuth) per unit length (cheap down-sun,
//     1.5x up-sun), climbing 2x, descending 1x.
//  5. PUBLISH: the working field is copied into the caller's storage,
//     sliced across ticks (the GPU keeps serving the previous field until
//     the copy completes - never a partial state).
//
// Incremental model: build cycles. requestRebuild() (chunk installs,
// window moves, sun/far changes) flags another cycle; cycles are sliced
// across tick() calls in center-out column order (setCenter = camera).
class SunLightGrid {
public:
	SunLightGrid() = default;
	~SunLightGrid() = default;
	SunLightGrid(const SunLightGrid&) = delete;
	SunLightGrid& operator=(const SunLightGrid&) = delete;

	// cols/rows: region columns (gridWidth*chunkSizeX etc.). height:
	// worldHeight. columnStride: chunkSizeX (the voxel layout stride
	// between consecutive y bytes of a column). maxTerrainY: the push
	// constant pc.grid.w (max solid voxel y) - the shader march's ascend
	// bound is maxTerrainY + 1. fieldStorage: cols*rows*height bytes
	// owned by the caller (tests: heap; renderer: persistently mapped
	// staging buffer). Only written at publish time.
	void configure(std::uint32_t cols, std::uint32_t rows,
	               std::uint32_t height, std::uint32_t columnStride,
	               double sunX, double sunY, double sunZ,
	               std::uint32_t maxTerrainY,
	               const SunLightVoxels* voxels,
	               std::uint8_t* fieldStorage);

	// Near-window min corner in world voxels (the active chunk-table
	// region: pc.region.xz * chunkSize). Columns outside [origin,
	// origin+cols) fall back to far heights in the cone walk.
	void setOrigin(std::int32_t originX, std::int32_t originZ);

	// Priority center for the column order (camera position, world
	// voxels). Applied at the start of each cycle.
	void setCenter(double worldVoxX, double worldVoxZ) {
		m_centerX = worldVoxX;
		m_centerZ = worldVoxZ;
	}

	// Schedule another full cycle after the current one completes.
	void requestRebuild() { m_rebuildRequested = true; }

	bool idle() const { return m_phase == Phase::Idle; }

	// Advance the current cycle by up to budgetMs milliseconds.
	// Returns true exactly once, when a cycle fully completes (the field
	// storage now holds a coherent publishable snapshot).
	bool tick(double budgetMs);

	// Stats.
	std::uint64_t litCells() const { return m_litCells; }
	const char* phaseName() const;

	// Test hook: the x8-quantized fill cost of one step (0 if invalid).
	int stepCost8ForTest(int dx, int dy, int dz) const;

private:
	struct Crossing {
		std::int16_t dx, dz;     // relative column offset
		std::int16_t kIn, kOut;  // entry/exit floor height gains
	};
	struct Step {
		std::int8_t dx, dy, dz;
		std::int16_t cost8;
	};

	enum class Phase {
		Idle,
		Prepass,
		Seeding,
		Border,
		Filling,
		Publishing,
	};

	void buildCone();
	void buildSteps();
	void beginCycle();
	void refreshColumn(std::uint32_t gx, std::uint32_t gz);
	void seedColumn(std::uint32_t gx, std::uint32_t gz);

	// Configuration.
	std::uint32_t m_cols = 0, m_rows = 0, m_height = 0;
	std::uint32_t m_columnStride = 1;
	std::uint32_t m_maxTerrainY = 0;
	double m_sun[3] = {0.0, 1.0, 0.0};
	double m_sunXZ[2] = {1.0, 0.0};
	const SunLightVoxels* m_voxels = nullptr;
	std::uint8_t* m_field = nullptr;   // published (caller storage)
	std::int32_t m_originX = 0, m_originZ = 0;
	double m_centerX = 0.0, m_centerZ = 0.0;

	// Cone table + fill steps (per sun direction).
	std::vector<Crossing> m_cone;
	std::vector<Step> m_steps;

	// Working field + column metadata (region-sized; top 0xFFFF = no
	// data). m_air/m_pushed: 1 bit per cell.
	std::vector<std::uint8_t> m_work;
	std::vector<std::uint8_t> m_hasData;  // 1 = chunk present (all-air ok)
	std::vector<std::uint16_t> m_tops;
	std::vector<std::uint8_t> m_spanCount;
	std::vector<std::uint32_t> m_spanIdx;
	std::vector<std::pair<std::uint16_t, std::uint16_t>> m_spans;
	std::vector<std::uint64_t> m_air;
	std::vector<std::uint64_t> m_pushed;
	std::vector<std::uint64_t> m_extra;  // per-column span-interval bits
	std::vector<std::uint32_t> m_order;  // center-out column order

	// Cycle cursors.
	std::uint32_t m_orderPos = 0;
	std::uint64_t m_borderCursor = 0;
	std::uint32_t m_publishSlice = 0;

	// Fill state.
	std::vector<std::uint32_t> m_buckets[256];
	std::uint32_t m_bucketMin = 256;
	std::size_t m_bucketPos = 0;

	Phase m_phase = Phase::Idle;
	bool m_rebuildRequested = false;
	std::uint64_t m_litCells = 0;
};

}  // namespace vv::terrain
