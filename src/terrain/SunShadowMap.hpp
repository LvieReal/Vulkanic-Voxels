#pragma once

#include <cstdint>
#include <vector>

namespace vv::terrain {

// Sun shadow heightmap (pass 24). For a FIXED sun direction, whether a
// point (x, y, z) is sun-lit depends only on the column (x, z) and y:
// blocked iff y < H(x, z), where H is the "shadow height" - the highest
// terrain surface (height bound) along the sun direction, attenuated by
// the ray's descent. One precomputed field makes shading a couple of
// buffer fetches per pixel instead of a march per ray (this replaces
// the 9-ray cone trace, which was both slow and scattered).
//
// The field is built by RAY SPLATTING: parallel sun rays launched from
// the up-sun grid edges at sub-cell offsets (0.25/0.75 - never cell
// corners, where DDA ties degenerate), marched with the exact 2D DDA;
// each ray carries a running height (max of tops seen, decayed by sunY
// per unit of ray parameter) and writes it into every column it
// crosses. Parallel-line spacing is ~0.35 columns, so every column's
// field value is within sub-voxel error of the true
// sup-over-column-area. A two-axis dynamic program was tried and
// REJECTED: it smears shadows along both axes independently (a blocker
// diagonal to a column shadows it without the ray ever passing over).
//
// The builder also produces, per column, the DISTANCE D (in ray-
// parameter units, quantized /4) from the winning blocker to the
// column. D is what soft shadows need: the sun-disk coverage of a
// blocker of height H at receiver distance D is
//   cov = clamp(0.5 + (H - y) / (2 * tan * D), 0, 1)
// (the same knife-edge formula the far tier uses), and the penumbra
// half width on the ground is tan * D. D = 0 marks the column's OWN
// top as the winner (an overhang directly above the receiver): the
// whole disk is then under it - a binary test, which also keeps top
// faces acne-free (their own bound equals the hit height).
//
// The builder runs INCREMENTALLY: tick() advances every ray a few
// crossings, so the renderer can spend a bounded slice of the frame
// budget (like the far-LOD seam patch). Results are double-buffered:
// heightField()/distField() are the last COMPLETED cycle; a new cycle
// can be requested at any time (tops may change mid-cycle - rays read
// them live, so the next cycle picks up changes, giving eventual
// consistency during streaming).
//
// Heights are u16 bounds (top + 1, 0 = no blocker), matching the chunk
// heightmap encoding. Pure logic - unit tested against a brute
// per-column DDA sweep in tests/terrain_world_tests.cpp.
class SunShadowBuilder final {
 public:
	// (Re)configures the grid size and sun direction (voxel space, y > 0,
	// any length). Allocates buffers and schedules the first cycle.
	void configure(std::uint32_t w, std::uint32_t h, double sunX,
								 double sunY, double sunZ);

	// The tops grid (w * h u16 bounds). NOT copied: rays read it live
	// every tick, so the owner updates it in place (chunk installs,
	// region shifts).
	void setTops(const std::uint16_t* tops) { m_tops = tops; }

	// Starts a fresh cycle after the current one completes (or
	// immediately if idle). Idempotent while a rebuild is pending.
	void requestRebuild() { m_rebuildRequested = true; }

	// Advances every active ray by up to `crossings` DDA steps. Returns
	// true when a cycle just completed (heightField() swapped to the new
	// field). A pending requestRebuild() starts the next cycle on the
	// following tick.
	bool tick(std::uint32_t crossings);

	// The last completed fields (w * h u16 each). Empty before the
	// first completed cycle.
	const std::vector<std::uint16_t>& heightField() const {
		return m_fieldFront;
	}
	// Blocker distance (ray-parameter units / 4; 0 = own column won).
	const std::vector<std::uint16_t>& distField() const {
		return m_distFront;
	}

	// Shifts the completed field by whole columns (region moves keep the
	// stale field aligned while the next cycle rebuilds). Cells shifted
	// in from outside are zero (no shadow).
	void shiftField(std::int32_t dxCells, std::int32_t dzCells);

	// The sun direction (voxel space) the builder was configured with.
	double sunX() const { return m_sunX; }
	double sunY() const { return m_sunY; }
	double sunZ() const { return m_sunZ; }

	std::uint32_t width() const { return m_w; }
	std::uint32_t height() const { return m_h; }

 private:
	struct Ray {
		// 2D DDA state; the ray parameter advances by the crossing
		// distance of whichever boundary is hit (like the marches).
		double tMaxX = 0.0;
		double tMaxZ = 0.0;
		double deltaX = 0.0;  // 1/|sunX| (huge when sunX ~ 0)
		double deltaZ = 0.0;
		std::int32_t colX = 0;
		std::int32_t colZ = 0;
		std::int32_t stepX = 0;
		std::int32_t stepZ = 0;
		double hRun = 0.0;  // running shadow height (decays with s)
		double s = 0.0;      // ray parameter at the current column entry
		double sSeen = 0.0;  // parameter where the current max was set
		bool done = false;
	};

	void launchRays();

	std::uint32_t m_w = 0;
	std::uint32_t m_h = 0;
	double m_sunX = 0.0, m_sunY = 0.0, m_sunZ = 0.0;
	double m_decayX = 0.0;  // sunY / |sunX| per X crossing
	double m_decayZ = 0.0;  // sunY / |sunZ| per Z crossing

	const std::uint16_t* m_tops = nullptr;
	std::vector<Ray> m_rays;
	std::vector<std::uint16_t> m_fieldBack;   // being built
	std::vector<std::uint16_t> m_fieldFront;  // last completed
	std::vector<std::uint16_t> m_distBack;    // blocker distance / 4
	std::vector<std::uint16_t> m_distFront;
	bool m_building = false;
	bool m_rebuildRequested = false;
};

// Brute reference for tests: exact per-column shadow height via the
// true 2D DDA staircase from (x, z) toward the sun (height bound per
// crossed column, descent by the true crossing parameter).
std::uint16_t sunShadowBruteColumn(const std::vector<std::uint16_t>& tops,
																		std::uint32_t w, std::uint32_t h,
																		double sunX, double sunY, double sunZ,
																		std::uint32_t x, std::uint32_t z);

}  // namespace vv::terrain
