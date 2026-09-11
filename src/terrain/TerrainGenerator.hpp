#pragma once

#include <cmath>
#include <cstdint>

#include "terrain/Noise.hpp"
#include "voxel/VoxelTypes.hpp"

namespace vv::terrain {

struct TerrainConfig final {
	std::uint32_t seed = 1337;

	// Height shape (all lengths in voxels).
	double baseHeight = 44.0;    // sea-level-ish average surface height
	double amplitude = 26.0;     // hill height before the hilliness mask
	double featureScale = 96.0;  // wavelength of the main terrain features
	double hillinessScale = 260.0;  // wavelength of the broad hill/valley mask
	std::uint32_t octaves = 5;
	double lacunarity = 2.0;
	double gain = 0.5;

	// Surface layering.
	double dirtDepth = 4.0;  // dirt (or sand) thickness below the surface
	double snowLine = 68.0;  // surfaces at/above become snow
	double sandLine = 26.0;  // surfaces at/below become sand
};

// Procedural terrain: a hilliness-modulated fBm heightmap with layered voxel
// types (grass/dirt/stone, sand in valleys, snow on peaks, bedrock at y=0).
// Fully deterministic for a given TerrainConfig.
class TerrainGenerator final {
 public:
	explicit TerrainGenerator(const TerrainConfig& config);

	// Terrain surface height (voxel-space y) at column (x, z).
	double heightAt(double x, double z) const;

	// Conservative upper bound (voxel-space y) on every height this
	// generator can produce: base + 1.35 * amplitude. Guaranteed >= heightAt
	// everywhere (the heightmap itself is bounded by ~1.2 * amplitude; the
	// extra margin absorbs noise overshoot). The renderer uses it as an
	// early-out: rays above this line can never hit terrain.
	std::int32_t maxHeightVoxels() const {
		return static_cast<std::int32_t>(
				std::ceil(m_config.baseHeight + 1.35 * m_config.amplitude));
	}

	// Voxel type at an absolute voxel coordinate.
	vv::voxel::VoxelType typeAt(std::int32_t x, std::int32_t y,
															std::int32_t z) const;

	// Voxel type within a column whose terrain height is already known
	// (cheaper: no second noise evaluation). y=0 is the bottom of the world.
	vv::voxel::VoxelType typeForColumn(std::int32_t y, double height) const;

	const TerrainConfig& config() const { return m_config; }

 private:
	TerrainConfig m_config;
	Noise2D m_noise;
	Noise2D m_hillNoise;
};

}  // namespace vv::terrain
