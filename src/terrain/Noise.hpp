#pragma once

#include <cstdint>

namespace vv::terrain {

// Deterministic 2D gradient (Perlin-style) noise with fractional Brownian
// motion. Scalar and dependency-free on purpose; SIMD-ification is a later
// optimization pass. All math is integer hashing + IEEE-754 doubles, so the
// same seed produces the same terrain on every platform.
class Noise2D final {
 public:
	explicit Noise2D(std::uint32_t seed) : m_seed(seed) {}

	// Single octave gradient noise, roughly in [-1, 1].
	double noise(double x, double z) const;

	// Sum of octaves, roughly in [-1, 1].
	double fbm(double x, double z, std::uint32_t octaves,
						 double lacunarity = 2.0, double gain = 0.5) const;

 private:
	static double fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
	static double gradientDot(std::uint32_t hash, double dx, double dz);

	std::uint32_t m_seed;
};

}  // namespace vv::terrain
