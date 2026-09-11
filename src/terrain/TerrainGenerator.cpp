#include "terrain/TerrainGenerator.hpp"

#include <cmath>

namespace vv::terrain {

TerrainGenerator::TerrainGenerator(const TerrainConfig& config)
		: m_config(config),
			m_noise(config.seed),
			m_hillNoise(config.seed ^ 0x51ed270bu) {}

double TerrainGenerator::heightAt(double x, double z) const {
	// Broad, low-frequency mask deciding how hilly an area is (0.35..1.2).
	const double hill = 0.5 + 0.5 * m_hillNoise.fbm(x / m_config.hillinessScale,
																									z / m_config.hillinessScale, 3);
	const double mask = 0.35 + 0.85 * hill;

	const double n = m_noise.fbm(x / m_config.featureScale,
															 z / m_config.featureScale, m_config.octaves,
															 m_config.lacunarity, m_config.gain);

	return m_config.baseHeight + mask * m_config.amplitude * n;
}

vv::voxel::VoxelType TerrainGenerator::typeAt(std::int32_t x, std::int32_t y,
																							std::int32_t z) const {
	return typeForColumn(y, heightAt(double(x), double(z)));
}

vv::voxel::VoxelType TerrainGenerator::typeForColumn(std::int32_t y,
																										 double height) const {
	const std::int32_t surface = static_cast<std::int32_t>(std::floor(height));
	if (y > surface) {
		return vv::voxel::VoxelType::Air;
	}
	if (y <= 0) {
		return vv::voxel::VoxelType::Bedrock;
	}

	const bool snowy = height >= m_config.snowLine;
	const bool sandy = height <= m_config.sandLine;

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
