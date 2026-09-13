#include "terrain/SunShadowMap.hpp"

#include <algorithm>
#include <cmath>

namespace vv::terrain {
namespace {

inline std::uint16_t clampHeight(double v) {
	if (v <= 0.0) {
		return 0;
	}
	if (v >= 65535.0) {
		return 65535;
	}
	return static_cast<std::uint16_t>(v + 0.5);
}

// Blocker distance in ray-parameter units, quantized /4 (the grid
// spans < ~2000 units; /4 keeps u16 with sub-penumbra precision).
inline std::uint16_t clampDist(double v) {
	if (v <= 0.0) {
		return 0;
	}
	const double q = v * 0.25;
	if (q >= 65535.0) {
		return 65535;
	}
	return static_cast<std::uint16_t>(q + 0.5);
}

}  // namespace

void SunShadowBuilder::configure(std::uint32_t w, std::uint32_t h,
																 double sunX, double sunY,
																 double sunZ) {
	m_w = w;
	m_h = h;
	const double len =
			std::sqrt(sunX * sunX + sunY * sunY + sunZ * sunZ);
	m_sunX = sunX / len;
	m_sunY = sunY / len;
	m_sunZ = sunZ / len;
	const double EPS = 1e-9;
	m_decayX = std::abs(m_sunX) > EPS ? m_sunY / std::abs(m_sunX) : 1e30;
	m_decayZ = std::abs(m_sunZ) > EPS ? m_sunY / std::abs(m_sunZ) : 1e30;
	m_fieldBack.assign(std::size_t(w) * h, 0);
	m_fieldFront.assign(std::size_t(w) * h, 0);
	m_distBack.assign(std::size_t(w) * h, 0);
	m_distFront.assign(std::size_t(w) * h, 0);
	m_rebuildRequested = true;
	m_building = false;
	m_rays.clear();
}

void SunShadowBuilder::launchRays() {
	// Parallel LIGHT rays: they travel WITH the light (direction -sun,
	// i.e. away from the sun, descending), entering the grid at the
	// up-sun edges. Each ray carries a running shadow height (the light
	// ray from the highest blocker so far descends at sunY per unit of
	// ray parameter) and writes it into every column it crosses - so
	// blockers shadow the columns DOWN-SUN of them. Entry points sit at
	// 0.25/0.75 sub-cell offsets along the edge (spacing 0.5 =>
	// perpendicular spacing ~0.35; the 0.25 offset avoids exact
	// cell-corner crossings, where DDA ties degenerate).
	m_rays.clear();
	m_fieldBack.assign(std::size_t(m_w) * m_h, 0);
	m_distBack.assign(std::size_t(m_w) * m_h, 0);
	const double EPS = 1e-9;
	const bool sxPos = m_sunX > EPS;
	const bool sxNeg = m_sunX < -EPS;
	const bool szPos = m_sunZ > EPS;
	const bool szNeg = m_sunZ < -EPS;
	// Light direction = -sun; entry nudged a hair inside the edge so
	// floor() lands on the first interior column.
	const double dirX = -m_sunX;
	const double dirZ = -m_sunZ;
	auto makeRay = [&](double px, double pz) {
		Ray r;
		r.colX = static_cast<std::int32_t>(std::floor(px));
		r.colZ = static_cast<std::int32_t>(std::floor(pz));
		r.stepX = dirX > EPS ? 1 : (dirX < -EPS ? -1 : 0);
		r.stepZ = dirZ > EPS ? 1 : (dirZ < -EPS ? -1 : 0);
		if (r.stepX != 0) {
			const double nextX =
					static_cast<double>(r.colX + (r.stepX > 0 ? 1 : 0));
			r.tMaxX = (nextX - px) / dirX;
			r.deltaX = std::abs(1.0 / dirX);
		} else {
			r.tMaxX = 1e30;
			r.deltaX = 1e30;
		}
		if (r.stepZ != 0) {
			const double nextZ =
					static_cast<double>(r.colZ + (r.stepZ > 0 ? 1 : 0));
			r.tMaxZ = (nextZ - pz) / dirZ;
			r.deltaZ = std::abs(1.0 / dirZ);
		} else {
			r.tMaxZ = 1e30;
			r.deltaZ = 1e30;
		}
		r.hRun = 0.0;
		r.s = 0.0;
		r.sSeen = 0.0;
		r.done = false;
		m_rays.push_back(r);
	};
	const double inX = 1e-4;
	// Light enters from the HIGH-X edge when the sun is toward +X.
	if (sxPos) {
		for (std::uint32_t z = 0; z < m_h; ++z) {
			makeRay(static_cast<double>(m_w) - inX,
							static_cast<double>(z) + 0.25);
			makeRay(static_cast<double>(m_w) - inX,
							static_cast<double>(z) + 0.75);
		}
	}
	if (sxNeg) {
		for (std::uint32_t z = 0; z < m_h; ++z) {
			makeRay(inX, static_cast<double>(z) + 0.25);
			makeRay(inX, static_cast<double>(z) + 0.75);
		}
	}
	if (szPos) {
		for (std::uint32_t x = 0; x < m_w; ++x) {
			makeRay(static_cast<double>(x) + 0.25,
							static_cast<double>(m_h) - inX);
			makeRay(static_cast<double>(x) + 0.75,
							static_cast<double>(m_h) - inX);
		}
	}
	if (szNeg) {
		for (std::uint32_t x = 0; x < m_w; ++x) {
			makeRay(static_cast<double>(x) + 0.25, inX);
			makeRay(static_cast<double>(x) + 0.75, inX);
		}
	}
}

bool SunShadowBuilder::tick(std::uint32_t crossings) {
	if (!m_building) {
		if (!m_rebuildRequested) {
			return false;
		}
		m_rebuildRequested = false;
		m_building = true;
		launchRays();
	}

	bool allDone = true;
	for (Ray& r : m_rays) {
		if (r.done) {
			continue;
		}
		for (std::uint32_t k = 0; k < crossings; ++k) {
			// Visit the current column: the running height includes this
			// column's top at zero distance.
			if (r.colX >= 0 && r.colX < static_cast<std::int32_t>(m_w) &&
					r.colZ >= 0 && r.colZ < static_cast<std::int32_t>(m_h)) {
				if (m_tops != nullptr) {
					const double top =
							m_tops[static_cast<std::size_t>(r.colX) +
										 static_cast<std::size_t>(r.colZ) * m_w];
					if (static_cast<double>(top) > r.hRun) {
						r.hRun = static_cast<double>(top);
						r.sSeen = r.s;  // own column: zero distance
					}
				}
				const std::size_t idx = static_cast<std::size_t>(r.colX) +
																static_cast<std::size_t>(r.colZ) * m_w;
				// Keep (H, D) paired: the distance belongs to the
				// WINNING height (ties prefer the closer blocker).
				const std::uint16_t hNew = clampHeight(r.hRun);
				const std::uint16_t dNew =
						clampDist(std::max(r.s - r.sSeen, 0.0));
				if (hNew > m_fieldBack[idx] ||
						(hNew == m_fieldBack[idx] && dNew < m_distBack[idx])) {
					m_fieldBack[idx] = hNew;
					m_distBack[idx] = dNew;
				}
			} else if (r.colX < -1 || r.colX > static_cast<std::int32_t>(m_w) ||
								 r.colZ < -1 ||
								 r.colZ > static_cast<std::int32_t>(m_h)) {
				r.done = true;  // left the grid for good
				break;
			}
			// Step to the next column. The running height decays by the
			// TRUE ray-parameter advance between consecutive crossings
			// (the tMax delta) - NOT the per-axis crossing cost: X and Z
			// crossings interleave and share s-intervals, so summing the
			// per-axis costs would decay shadows twice as fast as
			// reality (a tied corner crossing advances s by zero).
			const bool takeX = r.tMaxX < r.tMaxZ;
			const double sCur = takeX ? r.tMaxX : r.tMaxZ;
			r.tMaxX += takeX ? r.deltaX : 0.0;
			r.tMaxZ += takeX ? 0.0 : r.deltaZ;
			r.colX += takeX ? r.stepX : 0;
			r.colZ += takeX ? 0 : r.stepZ;
			r.s = sCur;  // entry parameter of the new column
			const double sNext = std::min(r.tMaxX, r.tMaxZ);
			r.hRun -= m_sunY * std::max(sNext - sCur, 0.0);
			if (r.hRun < 0.0) {
				r.hRun = 0.0;
			}
		}
		if (!r.done) {
			allDone = false;
		}
	}

	if (allDone) {
		m_fieldFront.swap(m_fieldBack);
		m_distFront.swap(m_distBack);
		m_building = false;
		return true;
	}
	return false;
}

void SunShadowBuilder::shiftField(std::int32_t dxCells,
																	std::int32_t dzCells) {
	if (m_fieldFront.empty()) {
		return;
	}
	std::vector<std::uint16_t> shifted(m_fieldFront.size(), 0);
	std::vector<std::uint16_t> shiftedD(m_distFront.size(), 0);
	for (std::uint32_t z = 0; z < m_h; ++z) {
		const std::int64_t srcZ = static_cast<std::int64_t>(z) + dzCells;
		if (srcZ < 0 || srcZ >= static_cast<std::int64_t>(m_h)) {
			continue;
		}
		for (std::uint32_t x = 0; x < m_w; ++x) {
			const std::int64_t srcX = static_cast<std::int64_t>(x) + dxCells;
			if (srcX < 0 || srcX >= static_cast<std::int64_t>(m_w)) {
				continue;
			}
			shifted[static_cast<std::size_t>(x) +
							static_cast<std::size_t>(z) * m_w] =
					m_fieldFront[static_cast<std::size_t>(srcX) +
											 static_cast<std::size_t>(srcZ) * m_w];
			if (!m_distFront.empty()) {
				shiftedD[static_cast<std::size_t>(x) +
								 static_cast<std::size_t>(z) * m_w] =
						m_distFront[static_cast<std::size_t>(srcX) +
												static_cast<std::size_t>(srcZ) * m_w];
			}
		}
	}
	m_fieldFront.swap(shifted);
	m_distFront.swap(shiftedD);
}

std::uint16_t sunShadowBruteColumn(const std::vector<std::uint16_t>& tops,
																	 std::uint32_t w, std::uint32_t h,
																	 double sunX, double sunY, double sunZ,
																	 std::uint32_t x, std::uint32_t z) {
	// Walk the true 2D DDA from (x + 0.5, z + 0.5) toward the sun,
	// tracking the ray parameter s; a column's contribution is
	// bound - sunY * s_at_entry (a point y is blocked iff y < that).
	const double px = x + 0.5, pz = z + 0.5;
	const int stepX = sunX > 0.0 ? 1 : (sunX < 0.0 ? -1 : 0);
	const int stepZ = sunZ > 0.0 ? 1 : (sunZ < 0.0 ? -1 : 0);
	int colX = static_cast<int>(x);
	int colZ = static_cast<int>(z);
	double tMaxX = 1e30, tMaxZ = 1e30, dX = 1e30, dZ = 1e30;
	if (stepX != 0) {
		tMaxX = (double(colX + (stepX > 0 ? 1 : 0)) - px) / sunX;
		dX = std::abs(1.0 / sunX);
	}
	if (stepZ != 0) {
		tMaxZ = (double(colZ + (stepZ > 0 ? 1 : 0)) - pz) / sunZ;
		dZ = std::abs(1.0 / sunZ);
	}
	double best = 0.0;
	double sEntry = 0.0;
	for (int i = 0; i < 4096; ++i) {
		if (colX >= 0 && colX < (int)w && colZ >= 0 && colZ < (int)h) {
			const double bound =
					tops[static_cast<std::size_t>(colX) +
							 static_cast<std::size_t>(colZ) * w];
			best = std::max(best, (double)bound - sunY * sEntry);
		}
		const double sExit = std::min(tMaxX, tMaxZ);
		const bool takeX = tMaxX < tMaxZ;
		sEntry = sExit;
		tMaxX += takeX ? dX : 0.0;
		tMaxZ += takeX ? 0.0 : dZ;
		colX += takeX ? stepX : 0;
		colZ += takeX ? 0 : stepZ;
		if (colX < -2 || colX > (int)w + 2 || colZ < -2 || colZ > (int)h + 2) {
			break;
		}
	}
	if (best <= 0.0) {
		return 0;
	}
	if (best >= 65535.0) {
		return 65535;
	}
	return static_cast<std::uint16_t>(best + 0.5);
}

}  // namespace vv::terrain
