#include "terrain/TerrainGenerator.hpp"

#include <algorithm>
#include <cmath>

namespace vv::terrain {

TerrainGenerator::TerrainGenerator(const TerrainConfig& config)
		: m_config(config),
			m_noise(config.seed),
			m_hillNoise(config.seed ^ 0x51ed270bu),
			m_ridgeNoise(config.seed ^ 0x2b592cf3u),
			m_warpNoise(config.seed ^ 0x3d7e1c05u) {}

// All arithmetic below is float32 with a fixed operation order, so terrain
// is bit-reproducible per config. The terrain TUs compile with
// -ffp-contract=off (GCC/Clang) to keep that true on every target.

float TerrainGenerator::heightAtF(float x, float z) const {
	const float hillScale = static_cast<float>(m_config.hillinessScale);
	const float featureScale = static_cast<float>(m_config.featureScale);

	const float hill = 0.5f + 0.5f * m_hillNoise.fbmF(x / hillScale,
																										z / hillScale, 3);
	const float mask = 0.35f + 0.85f * hill;

	const float n = m_noise.fbmF(x / featureScale, z / featureScale,
															 m_config.octaves,
															 static_cast<float>(m_config.lacunarity),
															 static_cast<float>(m_config.gain));

	const float base = static_cast<float>(m_config.baseHeight);
	const float amp = static_cast<float>(m_config.amplitude);
	return base + mask * amp * n;
}

void TerrainGenerator::heightAt4(const float* x, const float* z,
																 float* out) const {
	const float hillScale = static_cast<float>(m_config.hillinessScale);
	const float featureScale = static_cast<float>(m_config.featureScale);

	float hx[4], hz[4];
	for (int i = 0; i < 4; ++i) {
		hx[i] = x[i] / hillScale;
		hz[i] = z[i] / hillScale;
	}
	float hill[4];
	m_hillNoise.fbm4(hx, hz, 3, 2.0f, 0.5f, hill);

	float fx[4], fz[4];
	for (int i = 0; i < 4; ++i) {
		fx[i] = x[i] / featureScale;
		fz[i] = z[i] / featureScale;
	}
	float n[4];
	m_noise.fbm4(fx, fz, m_config.octaves,
							 static_cast<float>(m_config.lacunarity),
							 static_cast<float>(m_config.gain), n);

	const float base = static_cast<float>(m_config.baseHeight);
	const float amp = static_cast<float>(m_config.amplitude);
	for (int i = 0; i < 4; ++i) {
		const float mask = 0.35f + 0.85f * (0.5f + 0.5f * hill[i]);
		out[i] = base + mask * amp * n[i];
	}
}

double TerrainGenerator::heightAt(double x, double z) const {
	return static_cast<double>(heightAtF(static_cast<float>(x),
																			 static_cast<float>(z)));
}

float TerrainGenerator::mountainMaskF(float x, float z) const {
	const float scale = static_cast<float>(m_config.mountainScale);
	const float r = m_ridgeNoise.fbmF(x / scale, z / scale, 4);
	// Ridged: crests run along the fbm's zero crossings, so ranges form
	// connected lines rather than blobs.
	const float ridge = 1.0f - std::abs(r);
	// smoothstep(0.58, 0.74, ridge): ~20-25% of the world is mountainous,
	// with rolling foothills at the range edges.
	const float t = std::min(std::max((ridge - 0.58f) / 0.16f, 0.0f), 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

float TerrainGenerator::surfaceTargetF(float x, float z) const {
	const float m = mountainMaskF(x, z);
	const float lift =
			static_cast<float>(m_config.mountainLift) * m * m;
	const float target = heightAtF(x, z) + lift;
	// Summit clamp: keeps target + warp band under the world height; where
	// it binds the summits become flat snow plateaus.
	const float ceiling = static_cast<float>(m_config.surfaceCeiling);
	return target > ceiling ? ceiling : target;
}

float TerrainGenerator::densityAtF(float x, float y, float z, float target,
																	 float mask) const {
	const float amp =
			static_cast<float>(m_config.warpAmpPlains) +
			(static_cast<float>(m_config.warpAmpMountains) -
			 static_cast<float>(m_config.warpAmpPlains)) * mask;
	const float wl = static_cast<float>(m_config.warpWavelength);
	const float sq = static_cast<float>(m_config.warpVerticalSquash);
	const float n3 = m_warpNoise.fbmF(x / wl, y * sq / wl, z / wl,
																		m_config.warpOctaves);
	const float g = static_cast<float>(m_config.warpGradient);
	const float h = g * (target - y);
	// Saturating height term: beyond +-3 density units (g * 33 voxels) the
	// height term stops growing, so the 3D noise alone (|n3 * amp| <=
	// warpAmpMountains < 3) can never flip the sign - no floating islands,
	// no missing ground, and the warp is confined to a band around the
	// target where the isosurface folds into overhangs.
	const float hc = std::min(std::max(h, -3.0f), 3.0f);
	return hc + n3 * amp;
}

float TerrainGenerator::densityF(float x, float y, float z) const {
	return densityAtF(x, y, z, surfaceTargetF(x, z), mountainMaskF(x, z));
}

std::int32_t TerrainGenerator::maxHeightVoxels() const {
	const float rolling =
			static_cast<float>(m_config.baseHeight) +
			1.35f * static_cast<float>(m_config.amplitude);
	const float lifted = rolling + static_cast<float>(m_config.mountainLift);
	const float ceiling = static_cast<float>(m_config.surfaceCeiling);
	const float targetBound = std::min(lifted, ceiling);
	return static_cast<std::int32_t>(
		std::ceil(targetBound + static_cast<float>(maxWarpVoxels())));
}

std::int32_t TerrainGenerator::topSolidVoxels(std::int32_t x,
																							std::int32_t z) const {
	const float fx = static_cast<float>(x);
	const float fz = static_cast<float>(z);
	const float target = surfaceTargetF(fx, fz);
	const float mask = mountainMaskF(fx, fz);
	// Solid is impossible above target + maxWarpVoxels (see densityAtF).
	std::int32_t y = static_cast<std::int32_t>(std::ceil(target)) +
									 maxWarpVoxels();
	for (; y >= 0; --y) {
		if (densityAtF(fx, static_cast<float>(y), fz, target, mask) > 0.0f) {
			return y;
		}
	}
	return -1;
}

vv::voxel::VoxelType TerrainGenerator::typeAt(std::int32_t x, std::int32_t y,
																							std::int32_t z) const {
	if (y < 0) {
		return vv::voxel::VoxelType::Air;
	}
	const float fx = static_cast<float>(x);
	const float fy = static_cast<float>(y);
	const float fz = static_cast<float>(z);
	const float target = surfaceTargetF(fx, fz);
	const float mask = mountainMaskF(fx, fz);
	// Find the column's topmost solid at or above y (exact density).
	std::int32_t top = static_cast<std::int32_t>(std::ceil(target)) +
										 maxWarpVoxels();
	std::int32_t topSolid = -1;
	for (; top >= y; --top) {
		if (densityAtF(fx, static_cast<float>(top), fz, target, mask) > 0.0f) {
			topSolid = top;
			break;
		}
	}
	if (topSolid < 0) {
		return vv::voxel::VoxelType::Air;  // y is above the topmost solid
	}
	return typeForDepth(y, topSolid);
}

vv::voxel::VoxelType TerrainGenerator::typeForDepth(std::int32_t y,
																										std::int32_t topSolid) const {
	if (y < 0) {
		return vv::voxel::VoxelType::Air;
	}
	if (y == 0) {
		return vv::voxel::VoxelType::Bedrock;
	}
	const std::int32_t depth = topSolid - y;
	const bool snowy = static_cast<float>(topSolid) >=
											static_cast<float>(m_config.snowLine);
	const bool sandy = static_cast<float>(topSolid) <=
											static_cast<float>(m_config.sandLine);
	if (depth <= 0) {
		return snowy ? vv::voxel::VoxelType::Snow
								 : (sandy ? vv::voxel::VoxelType::Sand
													: vv::voxel::VoxelType::Grass);
	}
	const std::int32_t dirtDepth =
			static_cast<std::int32_t>(m_config.dirtDepth);
	if (depth <= dirtDepth) {
		return sandy ? vv::voxel::VoxelType::Sand : vv::voxel::VoxelType::Dirt;
	}
	return vv::voxel::VoxelType::Stone;
}

std::int32_t TerrainGenerator::estimatedTopSolid(float x, float z,
																								 float target,
																								 float mask) const {
	const float amp =
			static_cast<float>(m_config.warpAmpPlains) +
			(static_cast<float>(m_config.warpAmpMountains) -
			 static_cast<float>(m_config.warpAmpPlains)) * mask;
	const float wl = static_cast<float>(m_config.warpWavelength);
	const float sq = static_cast<float>(m_config.warpVerticalSquash);
	// Fixed-point iteration of y = target + n3(x, y, z) * amp / g, but
	// seeded at THREE heights and reduced to the MAXIMUM: the isosurface
	// folds, so a single seed can converge to a lower lobe than the
	// column's true top solid (which is what the far LOD stores). The max
	// keeps the far silhouette from dipping below the near-region terrain
	// at the seam.
	const float g = static_cast<float>(m_config.warpGradient);
	float surface = target;
	for (int k = -1; k <= 1; ++k) {
		const float y0 = target + float(k) * 6.0f;
		const float n3 = m_warpNoise.fbmF(x / wl, y0 * sq / wl, z / wl,
																			m_config.warpOctaves);
		surface = std::max(surface, target + n3 * amp / g);
	}
	const std::int32_t bound = maxHeightVoxels();
	std::int32_t top = static_cast<std::int32_t>(std::floor(surface));
	if (top < 0) {
		top = 0;
	}
	if (top > bound) {
		top = bound;
	}
	return top;
}

void TerrainGenerator::generateChunkVoxels(std::int32_t baseX,
																					 std::int32_t baseZ,
																					 std::uint32_t sizeX,
																					 std::uint32_t sizeZ,
																					 std::uint32_t worldHeight,
																					 std::vector<std::uint8_t>& outTypes) const {
	const std::size_t count = static_cast<std::size_t>(sizeX) * sizeZ *
															worldHeight;
	outTypes.assign(count, static_cast<std::uint8_t>(
														vv::voxel::VoxelType::Air));
	if (sizeX == 0 || sizeZ == 0 || worldHeight == 0) {
		return;
	}

	const std::int32_t topY = static_cast<std::int32_t>(worldHeight) - 1;

	// --- Pass 1: per-column 2D targets (and the chunk-wide Y band). ------
	struct ColumnInfo {
		float target;
		float mask;
		std::int32_t yTop;  // above this: provably air
		std::int32_t yBot;  // at/below this: provably solid
	};
	std::vector<ColumnInfo> info(static_cast<std::size_t>(sizeX) * sizeZ);
	std::int32_t bandLo = topY;
	std::int32_t bandHi = 0;
	const std::int32_t warp = maxWarpVoxels();
	const float g = static_cast<float>(m_config.warpGradient);
	// Height term saturates at +-3 density = 3/g voxels; the noise can
	// additionally push the surface out by warp voxels.
	const std::int32_t saturateVoxels =
			static_cast<std::int32_t>(std::ceil(3.0f / g)) + 1;

	for (std::uint32_t z = 0; z < sizeZ; ++z) {
		const float fz = static_cast<float>(
				baseZ + static_cast<std::int64_t>(z));
		for (std::uint32_t x = 0; x < sizeX; ++x) {
			const float fx = static_cast<float>(
					baseX + static_cast<std::int64_t>(x));
			ColumnInfo& col = info[static_cast<std::size_t>(x) +
														 static_cast<std::size_t>(z) * sizeX];
			col.target = surfaceTargetF(fx, fz);
			col.mask = mountainMaskF(fx, fz);
			col.yTop = std::min(static_cast<std::int32_t>(std::ceil(col.target)) +
														 warp + 1,
													topY);
			col.yBot = std::max(static_cast<std::int32_t>(std::floor(col.target)) -
															saturateVoxels - warp - 1,
													0);
			bandLo = std::min(bandLo, col.yBot);
			bandHi = std::max(bandHi, col.yTop);
		}
	}
	if (bandHi < bandLo) {
		return;  // empty world-height chunk (degenerate config)
	}

	// --- Pass 2: coarse 3D-noise lattice over the band -------------------
	// Stride 4 in X/Z but only 2 in Y: overhangs are vertical folds, so
	// the interpolated noise must keep its vertical slope (the finest
	// octave's vertical wavelength is wavelength/squash/2^(octaves-1) =
	// ~6 voxels at the defaults - stride 4 aliases it away and folds
	// disappear). The SAME squash as densityAtF must be applied here or
	// chunks silently diverge from typeAt/topSolidVoxels. Still ~20x fewer
	// fBm3 evaluations than per-voxel sampling.
	constexpr std::int32_t kStrideXZ = 4;
	constexpr std::int32_t kStrideY = 2;
	const std::int32_t latX =
			static_cast<std::int32_t>(sizeX) / kStrideXZ + 1;
	const std::int32_t latZ =
			static_cast<std::int32_t>(sizeZ) / kStrideXZ + 1;
	const std::int32_t latY = (bandHi - bandLo) / kStrideY + 1;
	const float wl = static_cast<float>(m_config.warpWavelength);
	const float sq = static_cast<float>(m_config.warpVerticalSquash);
	std::vector<float> lattice(static_cast<std::size_t>(latX) * latY * latZ);
	for (std::int32_t lz = 0; lz < latZ; ++lz) {
		const float fz = static_cast<float>(
				baseZ + static_cast<std::int64_t>(lz * kStrideXZ));
		for (std::int32_t ly = 0; ly < latY; ++ly) {
			const float fy = static_cast<float>(bandLo + ly * kStrideY);
			for (std::int32_t lx = 0; lx < latX; ++lx) {
				const float fx = static_cast<float>(
						baseX + static_cast<std::int64_t>(lx * kStrideXZ));
				lattice[static_cast<std::size_t>(lx) +
								static_cast<std::size_t>(ly) * latX +
								static_cast<std::size_t>(lz) * latX * latY] =
						m_warpNoise.fbmF(fx / wl, fy * sq / wl, fz / wl,
														 m_config.warpOctaves);
			}
		}
	}
	// Trilinear n3 lookup in lattice units. x/z are CHUNK-LOCAL (the
	// lattice spans [0, sizeX] x [bandLo, bandHi] x [0, sizeZ] in local
	// coordinates - the absolute coordinates were consumed when the lattice
	// values were evaluated); y is absolute.
	const auto n3At = [&](float localX, float absY, float localZ) -> float {
		float gx = localX / float(kStrideXZ);
		float gy = (absY - float(bandLo)) / float(kStrideY);
		float gz = localZ / float(kStrideXZ);
		gx = std::min(std::max(gx, 0.0f), float(latX - 1));
		gy = std::min(std::max(gy, 0.0f), float(latY - 1));
		gz = std::min(std::max(gz, 0.0f), float(latZ - 1));
		const std::int32_t x0 = latX >= 2 ? std::min<std::int32_t>(gx, latX - 2) : 0;
		const std::int32_t y0 = latY >= 2 ? std::min<std::int32_t>(gy, latY - 2) : 0;
		const std::int32_t z0 = latZ >= 2 ? std::min<std::int32_t>(gz, latZ - 2) : 0;
		const float tx = latX >= 2 ? gx - float(x0) : 0.0f;
		const float ty = latY >= 2 ? gy - float(y0) : 0.0f;
		const float tz = latZ >= 2 ? gz - float(z0) : 0.0f;
		const auto at = [&](std::int32_t dx, std::int32_t dy,
												std::int32_t dz) -> float {
			return lattice[static_cast<std::size_t>(x0 + dx) +
										 static_cast<std::size_t>(y0 + dy) * latX +
										 static_cast<std::size_t>(z0 + dz) * latX * latY];
		};
		const float a = at(0, 0, 0) + tx * (at(1, 0, 0) - at(0, 0, 0));
		const float b = at(0, 1, 0) + tx * (at(1, 1, 0) - at(0, 1, 0));
		const float c = at(0, 0, 1) + tx * (at(1, 0, 1) - at(0, 0, 1));
		const float d = at(0, 1, 1) + tx * (at(1, 1, 1) - at(0, 1, 1));
		const float e = a + ty * (b - a);
		const float f = c + ty * (d - c);
		return e + tz * (f - e);
	};

	// --- Pass 3: per-column fill + layering ------------------------------
	const auto idx = [&](std::uint32_t x, std::int32_t y, std::uint32_t z) {
		return static_cast<std::size_t>(x) +
					 static_cast<std::size_t>(y) * sizeX +
					 static_cast<std::size_t>(z) * sizeX * worldHeight;
	};
	for (std::uint32_t z = 0; z < sizeZ; ++z) {
		for (std::uint32_t x = 0; x < sizeX; ++x) {
			const ColumnInfo& col = info[static_cast<std::size_t>(x) +
																	 static_cast<std::size_t>(z) * sizeX];
			const float amp =
					static_cast<float>(m_config.warpAmpPlains) +
					(static_cast<float>(m_config.warpAmpMountains) -
					 static_cast<float>(m_config.warpAmpPlains)) * col.mask;

			// Topmost solid: scan the band from the top (interpolated
			// density, same values the fill below uses).
			std::int32_t topSolid = -1;
			for (std::int32_t y = col.yTop; y >= col.yBot; --y) {
				const float h = g * (col.target - static_cast<float>(y));
				const float hc = std::min(std::max(h, -3.0f), 3.0f);
				if (hc + n3At(static_cast<float>(x), static_cast<float>(y),
											static_cast<float>(z)) * amp > 0.0f) {
					topSolid = y;
					break;
				}
			}
			if (topSolid < 0) {
				// Degenerate config (target so low the whole band is air):
				// keep the bedrock-floor invariant and leave it at that.
				outTypes[idx(x, 0, z)] = static_cast<std::uint8_t>(
						vv::voxel::VoxelType::Bedrock);
				continue;
			}

			// Band cells above the saturation floor: exact (interpolated)
			// density - overhangs live here (gaps under earlier solids).
			for (std::int32_t y = topSolid; y > col.yBot; --y) {
				const float h = g * (col.target - static_cast<float>(y));
				const float hc = std::min(std::max(h, -3.0f), 3.0f);
				if (hc + n3At(static_cast<float>(x), static_cast<float>(y),
											static_cast<float>(z)) * amp > 0.0f) {
					outTypes[idx(x, y, z)] = static_cast<std::uint8_t>(
							typeForDepth(y, topSolid));
				}
			}
			// At/below the saturation floor the height term is clamped to
			// +3 and |n3 * amp| <= warpAmpMountains < 3: unconditionally
			// solid, no density evaluation needed.
			for (std::int32_t y = std::min(col.yBot, topSolid); y >= 0; --y) {
				outTypes[idx(x, y, z)] = static_cast<std::uint8_t>(
						typeForDepth(y, topSolid));
			}
		}
	}
}

}  // namespace vv::terrain
