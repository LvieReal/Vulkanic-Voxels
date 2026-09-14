#pragma once

#include <cstdint>

namespace vv::terrain {

// Deterministic 2D gradient (Perlin-style) noise with fractional Brownian
// motion, implemented in IEEE-754 single precision so the terrain hot path
// can be vectorized (4-wide SSE2 on x86-64; NEON is future work for ARM).
//
// Bit-exactness contract: fbm4() (vectorized) and fbmF() (scalar reference)
// produce bit-identical results by construction - same operation order, no
// FMA contraction (the terrain TUs compile with -ffp-contract=off on
// GCC/Clang; MSVC does not contract by default). Verified by unit test.
// All math is integer hashing + float32 ops, so the same seed produces the
// same terrain on every platform.
//
// Coordinate precision: float32 is exact for integer lattice coordinates up
// to ~2^24 (16.7M); region-local coordinates are far below that. Revisit if
// the world ever spans more than that from the origin.
class Noise2D final {
 public:
	explicit Noise2D(std::uint32_t seed) : m_seed(seed) {}

	// Single octave gradient noise, roughly in [-1, 1]. Scalar reference.
	float noiseF(float x, float z) const;

	// Sum of octaves, roughly in [-1, 1]. Scalar reference.
	float fbmF(float x, float z, std::uint32_t octaves,
						 float lacunarity = 2.0f, float gain = 0.5f) const;

	// 4-lane fBm: out[i] = fbmF(x[i], z[i], octaves, ...) bit-exactly.
	// SSE2 intrinsics on x86-64; the scalar reference elsewhere.
	void fbm4(const float* x, const float* z, std::uint32_t octaves,
						float lacunarity, float gain, float* out) const;

	std::uint32_t seed() const { return m_seed; }

 private:
	static float fade(float t) {
		return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
	}
	static float gradientDot(std::uint32_t hash, float dx, float dz);

	std::uint32_t m_seed;
};

}  // namespace vv::terrain
