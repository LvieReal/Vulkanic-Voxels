#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "terrain/Noise.hpp"
#include "terrain/Noise3D.hpp"
#include "voxel/VoxelTypes.hpp"

namespace vv::terrain {

struct TerrainConfig final {
	std::uint32_t seed = 1337;

	// Height shape (all lengths in voxels). baseHeight/amplitude/
	// featureScale/hillinessScale shape the ROLLING base surface
	// (heightAtF, unchanged from the pre-3D era and still bounded by
	// base +- 1.2 * amplitude in practice).
	double baseHeight = 44.0;    // sea-level-ish average surface height
	double amplitude = 26.0;     // hill height before the hilliness mask
	double featureScale = 96.0;  // wavelength of the main terrain features
	double hillinessScale = 260.0;  // wavelength of the broad hill/valley mask
	std::uint32_t octaves = 5;
	double lacunarity = 2.0;
	double gain = 0.5;

	// Mountain ranges + overhangs (3D density field). The world is solid
	// where density(x, y, z) > 0, with
	//   density = clamp(warpGradient * (target(x,z) - y), -3, 3)
	//             + fbm3(x, y, z) * warpAmplitude(mask)
	// The saturating height term bounds how far the 3D noise can displace
	// the surface: no solid can exist above target + warpAmp/warpGradient
	// and none can be MISSING below target - (3 + warpAmp)/warpGradient,
	// so there are no floating islands and every column has ground. Within
	// roughly +-warpAmp/warpGradient of the target the isosurface folds -
	// those folds are the overhangs.
	double mountainScale = 340.0;  // wavelength of the mountain-range mask
	double mountainLift = 42.0;    // extra height at mountain-range cores
	// Ridged-noise threshold window for the mountain mask: 0 below Low, 1
	// above High (smoothstep between). The ridged value 1-|fbm| clusters
	// near 1 (median ~0.87, p85 ~0.97), so the window must sit very high:
	// (0.58, 0.74) put mask>0.5 over 93% of the world. (0.92, 0.995)
	// gives ~19% foothills, ~9% ranges, ~3% full-lift mountains.
	double mountainMaskLow = 0.92;
	double mountainMaskHigh = 0.995;
	double surfaceCeiling = 96.0;  // target clamp (summits; keeps target +
	                               // the warp band under the world height)
	double warpWavelength = 48.0;  // 3D noise wavelength (voxels, XZ)
	std::uint32_t warpOctaves = 3;
	// Vertical squash of the 3D noise: the y coordinate is sampled at
	// y * squash / wavelength, steepening vertical variation. Folds need
	// the warp's vertical slope to exceed 1, and gradient noise alone is
	// too smooth for that at terrain wavelengths - squash 2 with the
	// amplitude/gradient below folds ~15% of mountain columns (measured)
	// while plains stay a heightfield.
	double warpVerticalSquash = 2.0;
	double warpGradient = 0.10;      // density units per voxel of height
	double warpAmpPlains = 1.2;      // 3D noise weight in density units
	double warpAmpMountains = 5.5;   // 3D noise weight inside ranges

	// Surface layering.
	double dirtDepth = 4.0;  // dirt (or sand) thickness below the surface
	double snowLine = 82.0;  // surfaces at/above become snow
	double sandLine = 26.0;  // surfaces at/below become sand
};

// Procedural terrain: a hilliness-modulated fBm heightmap lifted by broad
// mountain ranges, thickened into a 3D density field (fBm3 warp) that
// produces cliffs, ledges and overhangs. Layered voxel types
// (grass/dirt/stone, sand in valleys, snow on peaks, bedrock at y=0).
// Fully deterministic for a given TerrainConfig. The 2D path (heightAtF /
// heightAt4) is bit-identical between its scalar and vectorized forms by
// construction; the 3D path is scalar float32 with a fixed operation order.
class TerrainGenerator final {
 public:
	explicit TerrainGenerator(const TerrainConfig& config);

	// The ROLLING base surface height (voxel-space y) at column (x, z):
	// hills only, no mountains, no overhang warp. Kept as the stable 2D
	// core (bounds tests, the far-LOD silhouette estimate and the density
	// field's target all build on it).
	double heightAt(double x, double z) const;

	// Float32 height core (scalar reference).
	float heightAtF(float x, float z) const;

	// heightAtF for 4 columns at once (vectorized noise; bit-identical to
	// heightAtF per lane). x/z/out must hold 4 elements.
	void heightAt4(const float* x, const float* z, float* out) const;

	// Mountain-range mask in [0, 1] at column (x, z): smoothstep of a
	// ridged fBm, so ranges form connected lines with rolling approaches.
	float mountainMaskF(float x, float z) const;

	// Full 2D target surface = heightAtF + mountain lift, clamped to
	// surfaceCeiling (flat summit plateaus where it binds). This is the
	// height the 3D density field warps around.
	float surfaceTargetF(float x, float z) const;

	// 3D density at (x, y, z): > 0 means solid. target/mask are the
	// column's 2D values (pass them in to avoid recomputation).
	float densityAtF(float x, float y, float z, float target,
									 float mask) const;

	// Density with the 2D part recomputed internally (convenience).
	float densityF(float x, float y, float z) const;

	// Vertical reach of the 3D warp above the target (voxels): no solid
	// can exist above target + maxWarpVoxels(). Measured |fbm3| stays
	// below ~0.5, so the physical reach is amp * 0.5 / g; the +3 margin
	// absorbs interpolation overshoot.
	std::int32_t maxWarpVoxels() const {
		return static_cast<std::int32_t>(std::ceil(
							0.5 * m_config.warpAmpMountains / m_config.warpGradient)) +
					 3;
	}

	// Conservative upper bound (voxel-space y) on every SOLID voxel this
	// generator can produce: the clamped target ceiling plus the warp
	// band. The renderer uses it as the sky-skip ceiling (grid.w), so it
	// must be a true upper bound on solid terrain.
	std::int32_t maxHeightVoxels() const;

	// Topmost solid voxel y of column (x, z) by exact scan (-1 = none;
	// cannot happen with the default config, which forces solid below the
	// saturation band). Used for spawn placement; chunks use the faster
	// generateChunkVoxels path.
	std::int32_t topSolidVoxels(std::int32_t x, std::int32_t z) const;

	// Voxel type at an absolute voxel coordinate (exact density
	// evaluation; agrees with generateChunkVoxels everywhere the
	// guarantees are structural: air above the warp band, stone/bedrock
	// below the saturation band, and the column-top layering).
	vv::voxel::VoxelType typeAt(std::int32_t x, std::int32_t y,
															std::int32_t z) const;

	// Layering rule by depth below the column's topmost solid voxel.
	vv::voxel::VoxelType typeForDepth(std::int32_t y,
																		std::int32_t topSolid) const;

	// Canonical chunk fill: generates sizeX * worldHeight * sizeZ voxel
	// types for the chunk with min corner (baseX, baseZ), layout matching
	// vv::voxel::Chunk (X + Y*sizeX + Z*sizeX*worldHeight). The 3D noise
	// is evaluated on a coarse 4-voxel lattice and trilinearly
	// interpolated (~30x fewer fBm3 evaluations than per-voxel sampling;
	// the interpolation error is below one noise wavelength's detail and
	// is what the far-LOD silhouette estimate is tuned to match).
	void generateChunkVoxels(std::int32_t baseX, std::int32_t baseZ,
														std::uint32_t sizeX, std::uint32_t sizeZ,
														std::uint32_t worldHeight,
														std::vector<std::uint8_t>& outTypes) const;

	// Estimated topmost solid voxel of a column for the far LOD: one
	// fixed-point iteration of the isosurface (3D noise sampled AT the
	// target height). Within a couple of voxels of the true surface -
	// far below the far cell footprint (4 voxels).
	std::int32_t estimatedTopSolid(float x, float z, float target,
																 float mask) const;

	const TerrainConfig& config() const { return m_config; }

 private:
	// 3D warp noise value at (x, y, z) with the config's squash applied.
	float n3Seed(float x, float z, float y) const;

	TerrainConfig m_config;
	Noise2D m_noise;
	Noise2D m_hillNoise;
	Noise2D m_ridgeNoise;
	Noise3D m_warpNoise;
};

}  // namespace vv::terrain
