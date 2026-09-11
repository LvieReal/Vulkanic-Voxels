#include "terrain/TerrainGenerator.hpp"

#include <cmath>

namespace vv::terrain {

TerrainGenerator::TerrainGenerator(const TerrainConfig& config)
		: m_config(config),
			m_noise(config.seed),
			m_hillNoise(config.seed ^ 0x51ed270bu) {}

// All arithmetic below is float32 with a fixed operation order, so the
// scalar path (heightAtF) and the vectorized path (heightAt4) are
// bit-identical. The terrain TUs compile with -ffp-contract=off (GCC/Clang)
// to keep that true on every target.

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

vv::voxel::VoxelType TerrainGenerator::typeAt(std::int32_t x, std::int32_t y,
																									std::int32_t z) const {
	return typeForColumn(y, heightAtF(static_cast<float>(x),
																		static_cast<float>(z)));
}

vv::voxel::VoxelType TerrainGenerator::typeForColumn(std::int32_t y,
																										 float height) const {
	const std::int32_t surface =
			static_cast<std::int32_t>(std::floor(height));
	if (y > surface) {
		return vv::voxel::VoxelType::Air;
	}
	if (y <= 0) {
		return vv::voxel::VoxelType::Bedrock;
	}

	const bool snowy = height >= static_cast<float>(m_config.snowLine);
	const bool sandy = height <= static_cast<float>(m_config.sandLine);

	if (y == surface) {
		return snowy ? vv::voxel::VoxelType::Snow
								 : (sandy ? vv::voxel::VoxelType::Sand
													: vv::voxel::VoxelType::Grass);
	}

	const std::int32_t dirtDepth =
			static_cast<std::int32_t>(m_config.dirtDepth);
	if (y >= surface - dirtDepth) {
		return sandy ? vv::voxel::VoxelType::Sand : vv::voxel::VoxelType::Dirt;
	}
	return vv::voxel::VoxelType::Stone;
}

}  // namespace vv::terrain
