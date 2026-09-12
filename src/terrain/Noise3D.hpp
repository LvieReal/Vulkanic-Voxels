#pragma once

#include <cstdint>

namespace vv::terrain {

// Deterministic 3D gradient (Perlin-style) noise with fBm, implemented in
// IEEE-754 single precision with a fixed operation order (same contract as
// Noise2D: no FMA contraction on the terrain TUs, so the same seed produces
// the same terrain on every platform).
//
// Used by the density-field terrain (mountain ranges + overhangs): the
// chunk generator samples it on a coarse 4-voxel lattice and trilinearly
// interpolates, so only a scalar reference path is provided (a vectorized
// path can be added later following Noise2D's fbm4 pattern if generation
// ever becomes the bottleneck).
class Noise3D final {
 public:
	explicit Noise3D(std::uint32_t seed) : m_seed(seed) {}

	// Single octave gradient noise, roughly in [-1, 1].
	float noiseF(float x, float y, float z) const;

	// Sum of octaves, roughly in [-1, 1].
	float fbmF(float x, float y, float z, std::uint32_t octaves,
						 float lacunarity = 2.0f, float gain = 0.5f) const;

	std::uint32_t seed() const { return m_seed; }

 private:
	static float fade(float t) {
		return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
	}
	static float gradientDot(std::uint32_t hash, float dx, float dy,
													 float dz);

	std::uint32_t m_seed;
};

}  // namespace vv::terrain
