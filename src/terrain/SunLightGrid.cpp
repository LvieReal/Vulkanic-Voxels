#include "terrain/SunLightGrid.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace vv::terrain {

namespace {

double nowMs() {
	return std::chrono::duration<double, std::milli>(
	           std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

}  // namespace

void SunLightGrid::configure(std::uint32_t cols, std::uint32_t rows,
                             std::uint32_t height,
                             std::uint32_t columnStride, double sunX,
                             double sunY, double sunZ,
                             std::uint32_t maxTerrainY,
                             const SunLightVoxels* voxels,
                             std::uint8_t* fieldStorage) {
	m_cols = cols;
	m_rows = rows;
	m_height = height;
	m_columnStride = std::max<std::uint32_t>(columnStride, 1);
	m_maxTerrainY = maxTerrainY;
	m_sun[0] = sunX;
	m_sun[1] = sunY;
	m_sun[2] = sunZ;
	const double hl = std::sqrt(sunX * sunX + sunZ * sunZ);
	if (hl > 1e-9) {
		m_sunXZ[0] = sunX / hl;
		m_sunXZ[1] = sunZ / hl;
	} else {
		m_sunXZ[0] = 1.0;
		m_sunXZ[1] = 0.0;
	}
	m_voxels = voxels;
	m_field = fieldStorage;

	const std::size_t cells =
	    std::size_t(cols) * std::size_t(rows) * std::size_t(height);
	m_work.assign(cells, 255);
	m_hasData.assign(std::size_t(cols) * std::size_t(rows), 0);
	m_tops.assign(std::size_t(cols) * std::size_t(rows), 0xFFFF);
	m_spanCount.assign(std::size_t(cols) * rows, 0);
	m_spanIdx.assign(std::size_t(cols) * rows, 0);
	m_spans.clear();
	m_spans.reserve(1u << 20);
	m_air.assign((cells + 63) / 64, 0);
	m_pushed.assign((cells + 63) / 64, 0);
	m_extra.assign((height + 63) / 64, 0);
	m_order.clear();
	m_order.reserve(std::size_t(cols) * std::size_t(rows));

	buildCone();
	buildSteps();

	m_phase = Phase::Idle;
	m_rebuildRequested = true;  // first cycle after configure
	m_litCells = 0;
}

void SunLightGrid::buildCone() {
	m_cone.clear();
	// Low sun: the shader march short-circuits to fully lit; an empty
	// cone makes every air cell seed lit (H stays -1, no intervals).
	if (m_sun[1] <= 0.05) {
		return;
	}
	const double sx = m_sun[0], sz = m_sun[2];
	const int stepX = sx > 0.0 ? 1 : -1;
	const int stepZ = sz > 0.0 ? 1 : -1;
	double tMaxX = 1e30, tMaxZ = 1e30, dX = 1e30, dZ = 1e30;
	if (std::abs(sx) > 1e-6) {
		tMaxX = 0.5 / std::abs(sx);  // cell center -> first boundary
		dX = std::abs(1.0 / sx);
	}
	if (std::abs(sz) > 1e-6) {
		tMaxZ = 0.5 / std::abs(sz);
		dZ = std::abs(1.0 / sz);
	}
	const auto kOf = [&](double s) {
		return std::int16_t(std::floor(0.5 + m_sun[1] * s));
	};
	// The OWN column first: entry param 0, exit = first crossing. (The
	// own cell is air by construction, but a solid directly above it -
	// an overhang lip - blocks the ray, so the own column participates.)
	m_cone.push_back({0, 0, 0, kOf(std::min(tMaxX, tMaxZ))});
	int colX = 0, colZ = 0;
	for (int i = 0; i < 256; ++i) {
		const double t = std::min(tMaxX, tMaxZ);
		const bool takeX = tMaxX < tMaxZ;  // ties take Z (march parity)
		tMaxX += takeX ? dX : 0.0;
		tMaxZ += takeX ? 0.0 : dZ;
		colX += takeX ? stepX : 0;
		colZ += takeX ? 0 : stepZ;
		const std::int16_t k = kOf(t);  // entry gain of the new column
		m_cone.back().kOut = k;         // previous column exits here
		if (k > std::int16_t(m_maxTerrainY)) {
			break;  // this column can never block anything
		}
		m_cone.push_back({std::int16_t(colX), std::int16_t(colZ), k, k});
	}
	if (!m_cone.empty() && m_cone.back().kOut == m_cone.back().kIn) {
		// 256-crossing cap without the maxTerr break: one extra step for
		// the true exit gain of the last column.
		const double t = std::min(tMaxX, tMaxZ);
		m_cone.back().kOut = kOf(t);
	}
}

void SunLightGrid::buildSteps() {
	m_steps.clear();
	for (int dy = -1; dy <= 1; ++dy) {
		for (int dz = -1; dz <= 1; ++dz) {
			for (int dx = -1; dx <= 1; ++dx) {
				if (!dx && !dy && !dz) continue;
				const double hLen = std::sqrt(double(dx * dx + dz * dz));
				const double vCost = dy > 0 ? 2.0 : 1.0;
				double cost;
				if (hLen == 0.0) {
					cost = vCost;
				} else {
					const double dirF =
					    1.0 + 0.5 *
					              (dx * m_sunXZ[0] + dz * m_sunXZ[1]) / hLen;
					cost = std::sqrt(dirF * dirF * hLen * hLen +
					                 vCost * vCost * (dy ? 1.0 : 0.0));
				}
				m_steps.push_back({std::int8_t(dx), std::int8_t(dy),
				                   std::int8_t(dz),
				                   std::int16_t(std::max(
				                       1, int(std::lround(8.0 * cost))))});
			}
		}
	}
}

void SunLightGrid::setOrigin(std::int32_t originX, std::int32_t originZ) {
	m_originX = originX;
	m_originZ = originZ;
}

void SunLightGrid::beginCycle() {
	// Center-out column order via counting sort on the Chebyshev ring.
	const std::uint32_t n = m_cols * m_rows;
	const double cx = m_centerX - double(m_originX);
	const double cz = m_centerZ - double(m_originZ);
	const std::uint32_t maxRing = m_cols + m_rows;
	std::vector<std::uint32_t> counts(std::size_t(maxRing) + 2, 0);
	const auto ringOf = [&](std::uint32_t gx, std::uint32_t gz) {
		const double ax = std::abs(double(gx) + 0.5 - cx);
		const double az = std::abs(double(gz) + 0.5 - cz);
		return std::uint32_t(std::max(ax, az));
	};
	for (std::uint32_t gz = 0; gz < m_rows; ++gz) {
		for (std::uint32_t gx = 0; gx < m_cols; ++gx) {
			++counts[ringOf(gx, gz) + 1];
		}
	}
	for (std::size_t r = 0; r + 1 < counts.size(); ++r) {
		counts[r + 1] += counts[r];
	}
	m_order.resize(n);
	for (std::uint32_t gz = 0; gz < m_rows; ++gz) {
		for (std::uint32_t gx = 0; gx < m_cols; ++gx) {
			m_order[counts[ringOf(gx, gz)]++] = gx + gz * m_cols;
		}
	}
	m_spans.clear();  // spanIdx indices are rebuilt by the prepass
	m_orderPos = 0;
	m_borderCursor = 0;
	m_publishSlice = 0;
	for (auto& b : m_buckets) {
		b.clear();
	}
	m_bucketMin = 256;
	m_bucketPos = 0;
	std::fill(m_pushed.begin(), m_pushed.end(), 0ull);
	m_litCells = 0;
	m_rebuildRequested = false;
	m_phase = Phase::Prepass;
}

void SunLightGrid::refreshColumn(std::uint32_t gx, std::uint32_t gz) {
	const std::uint32_t base = gx + gz * m_cols;
	m_hasData[base] = 0;
	m_spanCount[base] = 0;
	m_tops[base] = 0xFFFF;
	const std::int32_t wx = m_originX + std::int32_t(gx);
	const std::int32_t wz = m_originZ + std::int32_t(gz);
	const std::uint8_t* vox =
	    m_voxels ? m_voxels->columnVoxels(wx, wz) : nullptr;
	if (!vox) {
		return;
	}
	m_hasData[base] = 1;
	const std::uint32_t slice = m_cols * m_rows;
	int run = -1;
	int count = 0;
	for (std::uint32_t y = 0; y < m_height; ++y) {
		const std::size_t i =
		    std::size_t(base) + std::size_t(y) * slice;
		if (vox[y * m_columnStride] != 0) {
			m_work[i] = 255;
			m_air[i >> 6] &= ~(1ull << (i & 63));  // solid
			if (run < 0) {
				run = int(y);
			}
		} else {
			m_air[i >> 6] |= 1ull << (i & 63);  // air
			if (run >= 0) {
				if (count == 0) {
					m_spanIdx[base] = std::uint32_t(m_spans.size());
				}
				m_spans.push_back(
				    {std::uint16_t(run), std::uint16_t(y - 1)});
				++count;
				run = -1;
			}
		}
	}
	if (run >= 0) {
		if (count == 0) {
			m_spanIdx[base] = std::uint32_t(m_spans.size());
		}
		m_spans.push_back({std::uint16_t(run),
		                   std::uint16_t(m_height - 1)});
		++count;
	}
	m_spanCount[base] = std::uint8_t(count);
	// top = highest solid voxel = last span's hi (0xFFFF when all air).
	m_tops[base] = count ? m_spans[m_spanIdx[base] + count - 1].second
	                     : 0xFFFF;
}

void SunLightGrid::seedColumn(std::uint32_t gx, std::uint32_t gz) {
	const std::uint32_t base = gx + gz * m_cols;
	if (!m_hasData[base]) {
		return;
	}
	int H = -1;
	std::fill(m_extra.begin(), m_extra.end(), 0ull);
	for (const Crossing& cr : m_cone) {
		const std::int32_t cgx = std::int32_t(gx) + cr.dx;
		const std::int32_t cgz = std::int32_t(gz) + cr.dz;
		if (cgx >= 0 && cgx < std::int32_t(m_cols) && cgz >= 0 &&
		    cgz < std::int32_t(m_rows) &&
		    m_hasData[std::uint32_t(cgx) +
		              std::uint32_t(cgz) * m_cols] != 0) {
			const std::uint32_t cb =
			    std::uint32_t(cgx) + std::uint32_t(cgz) * m_cols;
			if (m_spanCount[cb] == 1) {
				const int X =
				    std::min(int(m_tops[cb]), int(m_maxTerrainY)) -
				    int(cr.kIn);
				if (X > H) {
					H = X;
				}
			} else if (m_spanCount[cb] > 1) {
				const std::uint32_t si = m_spanIdx[cb];
				for (int k = 0; k < int(m_spanCount[cb]); ++k) {
					int lo =
					    int(m_spans[si + k].first) - int(cr.kOut);
					int hi = std::min(int(m_spans[si + k].second),
					                  int(m_maxTerrainY)) -
					         int(cr.kIn);
					if (lo < 0) {
						lo = 0;
					}
					if (hi >= lo) {
						for (int y = lo; y <= hi; ++y) {
							m_extra[std::size_t(y) >> 6] |=
							    1ull << (y & 63);
						}
					}
				}
			}
			// spanCount == 0: all-air near column - blocks nothing
			// (the march's bound is 0 there, no walk, no far test).
		} else if (m_voxels) {
			// Beyond the window (or an empty slot): far cell.
			const int h = int(m_voxels->farHeightAt(
			    m_originX + cgx, m_originZ + cgz));
			if (h > 0) {
				const int X = std::min(h - 1, int(m_maxTerrainY)) -
				              int(cr.kIn);
				if (X > H) {
					H = X;
				}
			}
		}
	}
	const std::uint32_t slice = m_cols * m_rows;
	for (std::uint32_t y = 0; y < m_height; ++y) {
		const std::size_t i =
		    std::size_t(base) + std::size_t(y) * slice;
		if (!((m_air[i >> 6] >> (i & 63)) & 1ull)) {
			continue;
		}
		const bool blocked =
		    int(y) <= H ||
		    ((m_extra[std::size_t(y) >> 6] >> (y & 63)) & 1ull) != 0;
		if (blocked) {
			m_work[i] = 255;
		} else {
			m_work[i] = 0;
			++m_litCells;
		}
	}
}

int SunLightGrid::stepCost8ForTest(int dx, int dy, int dz) const {
	for (const Step& s : m_steps) {
		if (s.dx == dx && s.dy == dy && s.dz == dz) {
			return s.cost8;
		}
	}
	return 0;
}

const char* SunLightGrid::phaseName() const {
	switch (m_phase) {
	case Phase::Idle: return "idle";
	case Phase::Prepass: return "prepass";
	case Phase::Seeding: return "seeding";
	case Phase::Border: return "border";
	case Phase::Filling: return "filling";
	case Phase::Publishing: return "publishing";
	}
	return "?";
}

bool SunLightGrid::tick(double budgetMs) {
	if (m_phase == Phase::Idle) {
		if (!m_rebuildRequested) {
			return false;
		}
		beginCycle();
	}
	const double t0 = nowMs();
	const std::uint32_t slice = m_cols * m_rows;
	const std::uint64_t totalCells =
	    std::uint64_t(slice) * m_height;
	for (;;) {
		if (nowMs() - t0 >= budgetMs) {
			return false;
		}
		switch (m_phase) {
		case Phase::Idle:
			return false;
		case Phase::Prepass: {
			std::uint32_t batch = 0;
			while (m_orderPos < m_order.size()) {
				const std::uint32_t col = m_order[m_orderPos++];
				refreshColumn(col % m_cols, col / m_cols);
				if ((++batch & 63u) == 0u &&
				    nowMs() - t0 >= budgetMs) {
					break;
				}
			}
			if (m_orderPos >= m_order.size()) {
				m_orderPos = 0;
				m_phase = Phase::Seeding;
			}
			break;
		}
		case Phase::Seeding: {
			std::uint32_t batch = 0;
			while (m_orderPos < m_order.size()) {
				const std::uint32_t col = m_order[m_orderPos++];
				seedColumn(col % m_cols, col / m_cols);
				if ((++batch & 15u) == 0u &&
				    nowMs() - t0 >= budgetMs) {
					break;
				}
			}
			if (m_orderPos >= m_order.size()) {
				m_phase = Phase::Border;
			}
			break;
		}
		case Phase::Border: {
			// Inverted border detection: every SHADOWED air cell marks
			// its lit air neighbors as Dijkstra sources.
			std::uint64_t batch = 0;
			while (m_borderCursor < totalCells) {
				const std::uint64_t i = m_borderCursor++;
				if (!((m_air[i >> 6] >> (i & 63)) & 1ull)) {
					continue;  // solid
				}
				if (m_work[i] == 0) {
					continue;  // interior seed
				}
				const std::uint32_t gx = std::uint32_t(i % m_cols);
				const std::uint32_t gz =
				    std::uint32_t((i / m_cols) % m_rows);
				const std::uint32_t y =
				    std::uint32_t(i / slice);
				for (const Step& st : m_steps) {
					const int nx = int(gx) + st.dx;
					const int nz = int(gz) + st.dz;
					const int ny = int(y) + st.dy;
					if (nx < 0 || nx >= int(m_cols) || nz < 0 ||
					    nz >= int(m_rows) || ny < 0 ||
					    ny >= int(m_height)) {
						continue;
					}
					const std::uint64_t j = std::uint64_t(nx) +
					    std::uint64_t(nz) * m_cols +
					    std::uint64_t(ny) * slice;
					if ((m_air[j >> 6] >> (j & 63)) & 1ull) {
						if (m_work[j] == 0 &&
						    !((m_pushed[j >> 6] >> (j & 63)) & 1ull)) {
							m_pushed[j >> 6] |= 1ull << (j & 63);
							m_buckets[0].push_back(
							    std::uint32_t(j));
						}
					}
				}
				if ((++batch & 4095u) == 0u &&
				    nowMs() - t0 >= budgetMs) {
					break;
				}
			}
			if (m_borderCursor >= totalCells) {
				m_bucketMin = 0;
				m_bucketPos = 0;
				m_phase = Phase::Filling;
			}
			break;
		}
		case Phase::Filling: {
			// Dial bucket queue: process pops until the budget or the
			// exhaustion of all buckets.
			std::uint32_t fillBatch = 0;
			for (;;) {
				if (m_bucketMin >= 256) {
					m_phase = Phase::Publishing;
					m_publishSlice = 0;
					break;
				}
				std::vector<std::uint32_t>& b = m_buckets[m_bucketMin];
				if (m_bucketPos >= b.size()) {
					b.clear();
					b.shrink_to_fit();
					++m_bucketMin;
					m_bucketPos = 0;
					continue;
				}
				if ((++fillBatch & 255u) == 0u &&
				    nowMs() - t0 >= budgetMs) {
					return false;  // BEFORE consuming the pop
				}
				const std::uint32_t i = b[m_bucketPos++];
				if (m_work[i] != std::uint8_t(m_bucketMin)) {
					continue;  // stale entry
				}
				const std::uint32_t gx = i % m_cols;
				const std::uint32_t gz = (i / m_cols) % m_rows;
				const std::uint32_t y = i / slice;
				for (const Step& st : m_steps) {
					const int nx = int(gx) + st.dx;
					const int nz = int(gz) + st.dz;
					const int ny = int(y) + st.dy;
					if (nx < 0 || nx >= int(m_cols) || nz < 0 ||
					    nz >= int(m_rows) || ny < 0 ||
					    ny >= int(m_height)) {
						continue;
					}
					const std::uint32_t j = std::uint32_t(nx) +
					    std::uint32_t(nz) * m_cols +
					    std::uint32_t(ny) * slice;
					if (!((m_air[j >> 6] >> (j & 63)) & 1ull)) {
						continue;
					}
					const int nd = int(m_bucketMin) + int(st.cost8);
					if (nd > 255) {
						continue;
					}
					if (nd < int(m_work[j])) {
						m_work[j] = std::uint8_t(nd);
						m_buckets[nd].push_back(j);
					}
				}
			}
			break;
		}
		case Phase::Publishing: {
			// Sliced copy of the working field into the published
			// storage; the GPU keeps serving the previous field until
			// this completes.
			const std::size_t sliceBytes = std::size_t(slice);
			while (m_publishSlice < m_height &&
			       nowMs() - t0 < budgetMs) {
				const std::size_t off =
				    sliceBytes * std::size_t(m_publishSlice);
				std::memcpy(m_field + off, m_work.data() + off,
				            sliceBytes);
				++m_publishSlice;
			}
			if (m_publishSlice >= m_height) {
				m_phase = Phase::Idle;
				return true;  // publish hint
			}
			break;
		}
		}
	}
}

}  // namespace vv::terrain
