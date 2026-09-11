#include "terrain/Noise.hpp"

#include <cmath>

// Vectorized path: SSE2 is part of the x86-64 baseline (guaranteed on every
// x86-64 CPU; also enabled on i386 when the compiler targets SSE2). All
// other platforms use the scalar reference, which is bit-identical.
#if defined(__SSE2__) || (defined(_M_X64) && !defined(_M_ARM64EC))
#define VVNOISE_SSE2 1
#include <immintrin.h>
#endif

namespace vv::terrain {

namespace {

// Eight evenly spaced unit gradient directions (multiples of 45 degrees).
// Float32 on purpose: this is the production noise path (see Noise.hpp).
constexpr float kGradients[8][2] = {
		{1.0f, 0.0f},
		{0.70710678118654752f, 0.70710678118654752f},
		{0.0f, 1.0f},
		{-0.70710678118654752f, 0.70710678118654752f},
		{-1.0f, 0.0f},
		{-0.70710678118654752f, -0.70710678118654752f},
		{0.0f, -1.0f},
		{0.70710678118654752f, -0.70710678118654752f},
};

// MurmurHash3-style 32-bit finalizer.
std::uint32_t fmix32(std::uint32_t x) {
	x ^= x >> 16;
	x *= 0x85ebca6bu;
	x ^= x >> 13;
	x *= 0xc2b2ae35u;
	x ^= x >> 16;
	return x;
}

// Hash of one integer lattice cell. Negative coordinates are fine: only the
// bit pattern is consumed.
std::uint32_t cellHash(std::uint32_t seed, std::int32_t xi, std::int32_t zi) {
	std::uint32_t h = seed;
	h ^= static_cast<std::uint32_t>(xi) + 0x9e3779b9u + (h << 6) + (h >> 2);
	h ^= static_cast<std::uint32_t>(zi) + 0x9e3779b9u + (h << 6) + (h >> 2);
	return fmix32(h);
}

#ifdef VVNOISE_SSE2

// SSE2 has no 32-bit multiply-low; compose it from 16-bit lanes. Modular
// arithmetic makes the 16-bit truncation of the cross terms exact.
inline __m128i mulloEpi32(__m128i a, __m128i b) {
	const __m128i mask16 = _mm_set1_epi32(0xFFFF);
	const __m128i aLo = _mm_and_si128(a, mask16);
	const __m128i bLo = _mm_and_si128(b, mask16);
	const __m128i aHi = _mm_srli_epi32(a, 16);
	const __m128i bHi = _mm_srli_epi32(b, 16);
	// Low 16 bits of the product: aLo*bLo (high 16-bit lanes are zero).
	const __m128i lo = _mm_mullo_epi16(aLo, bLo);
	// Bits 16..31: high(aLo*bLo) + low(aLo*bHi) + low(aHi*bLo), mod 2^16.
	const __m128i hi = _mm_add_epi16(
			_mm_mulhi_epu16(aLo, bLo),
			_mm_add_epi16(_mm_mullo_epi16(aLo, bHi), _mm_mullo_epi16(aHi, bLo)));
	return _mm_or_si128(lo, _mm_slli_epi32(hi, 16));
}

inline __m128i fmix32x4(__m128i x) {
	const __m128i c1 = _mm_set1_epi32(static_cast<int>(0x85ebca6bu));
	const __m128i c2 = _mm_set1_epi32(static_cast<int>(0xc2b2ae35u));
	x = _mm_xor_si128(x, _mm_srli_epi32(x, 16));
	x = mulloEpi32(x, c1);
	x = _mm_xor_si128(x, _mm_srli_epi32(x, 13));
	x = mulloEpi32(x, c2);
	x = _mm_xor_si128(x, _mm_srli_epi32(x, 16));
	return x;
}

// cellHash for 4 lanes (xi/zi are per-lane int coordinates).
inline __m128i cellHash4(__m128i seed, __m128i xi, __m128i zi) {
	const __m128i k = _mm_set1_epi32(static_cast<int>(0x9e3779b9u));
	const auto mixStep = [&k](__m128i h, __m128i v) {
		return _mm_xor_si128(
				h, _mm_add_epi32(v, _mm_add_epi32(
															 k, _mm_add_epi32(_mm_slli_epi32(h, 6),
																							 _mm_srli_epi32(h, 2)))));
	};
	__m128i h = mixStep(seed, xi);
	h = mixStep(h, zi);
	return fmix32x4(h);
}

// fade(t) = t*t*t*(t*(t*6-15)+10), same association as the scalar version.
inline __m128 fade4(__m128 t) {
	const __m128 t3 = _mm_mul_ps(_mm_mul_ps(t, t), t);
	const __m128 inner =
			_mm_sub_ps(_mm_mul_ps(t, _mm_set1_ps(6.0f)), _mm_set1_ps(15.0f));
	return _mm_mul_ps(t3, _mm_add_ps(_mm_mul_ps(t, inner), _mm_set1_ps(10.0f)));
}

// Gradient dot product for 4 lanes; the 8-entry gradient table is gathered
// per lane (a shuffle-based gather is not worth it for 4 lanes).
inline __m128 gradientDot4(__m128i hash, __m128 dx, __m128 dz) {
	alignas(16) std::int32_t h[4];
	_mm_store_si128(reinterpret_cast<__m128i*>(h), hash);
	return _mm_add_ps(
			_mm_mul_ps(_mm_set_ps(kGradients[h[3] & 7][0], kGradients[h[2] & 7][0],
														kGradients[h[1] & 7][0], kGradients[h[0] & 7][0]),
								 dx),
			_mm_mul_ps(_mm_set_ps(kGradients[h[3] & 7][1], kGradients[h[2] & 7][1],
														kGradients[h[1] & 7][1], kGradients[h[0] & 7][1]),
								 dz));
}

// Single-octave noise for 4 coordinate lanes: the vectorized twin of
// noiseF() below - identical operations in identical order.
inline __m128 noise4(__m128 x, __m128 z, std::uint32_t seed) {
	const __m128 one = _mm_set1_ps(1.0f);
	const __m128i oneI = _mm_set1_epi32(1);

	// floor: truncate towards zero, then subtract 1 where x < trunc(x).
	const __m128i xiT = _mm_cvttps_epi32(x);
	const __m128 tX = _mm_cvtepi32_ps(xiT);
	const __m128 mX = _mm_cmplt_ps(x, tX);
	const __m128 floorX = _mm_sub_ps(tX, _mm_and_ps(mX, one));
	const __m128i xi = _mm_add_epi32(xiT, _mm_castps_si128(mX));  // -1 lanes: 0xFFFFFFFF == -1

	const __m128i ziT = _mm_cvttps_epi32(z);
	const __m128 tZ = _mm_cvtepi32_ps(ziT);
	const __m128 mZ = _mm_cmplt_ps(z, tZ);
	const __m128 floorZ = _mm_sub_ps(tZ, _mm_and_ps(mZ, one));
	const __m128i zi = _mm_add_epi32(ziT, _mm_castps_si128(mZ));

	const __m128 xf = _mm_sub_ps(x, floorX);
	const __m128 zf = _mm_sub_ps(z, floorZ);

	const __m128i seedV = _mm_set1_epi32(static_cast<int>(seed));
	const __m128i h00 = cellHash4(seedV, xi, zi);
	const __m128i h10 = cellHash4(seedV, _mm_add_epi32(xi, oneI), zi);
	const __m128i h01 = cellHash4(seedV, xi, _mm_add_epi32(zi, oneI));
	const __m128i h11 = cellHash4(seedV, _mm_add_epi32(xi, oneI),
																_mm_add_epi32(zi, oneI));

	const __m128 n00 = gradientDot4(h00, xf, zf);
	const __m128 n10 = gradientDot4(h10, _mm_sub_ps(xf, one), zf);
	const __m128 n01 = gradientDot4(h01, xf, _mm_sub_ps(zf, one));
	const __m128 n11 = gradientDot4(h11, _mm_sub_ps(xf, one), _mm_sub_ps(zf, one));

	const __m128 u = fade4(xf);
	const __m128 v = fade4(zf);
	const __m128 a = _mm_add_ps(n00, _mm_mul_ps(u, _mm_sub_ps(n10, n00)));
	const __m128 b = _mm_add_ps(n01, _mm_mul_ps(u, _mm_sub_ps(n11, n01)));
	const __m128 value = _mm_add_ps(a, _mm_mul_ps(v, _mm_sub_ps(b, a)));

	// Unit gradients give a theoretical max of ~sqrt(2)/2 in 2D; rescale to
	// roughly [-1, 1].
	return _mm_mul_ps(value, _mm_set1_ps(1.4142135623730951f));
}

#endif  // VVNOISE_SSE2

}  // namespace

float Noise2D::gradientDot(std::uint32_t hash, float dx, float dz) {
	const float* g = kGradients[hash & 7u];
	return g[0] * dx + g[1] * dz;
}

float Noise2D::noiseF(float x, float z) const {
	const float floorX = std::floor(x);
	const float floorZ = std::floor(z);
	const std::int32_t xi = static_cast<std::int32_t>(floorX);
	const std::int32_t zi = static_cast<std::int32_t>(floorZ);
	const float xf = x - floorX;
	const float zf = z - floorZ;

	const std::uint32_t h00 = cellHash(m_seed, xi, zi);
	const std::uint32_t h10 = cellHash(m_seed, xi + 1, zi);
	const std::uint32_t h01 = cellHash(m_seed, xi, zi + 1);
	const std::uint32_t h11 = cellHash(m_seed, xi + 1, zi + 1);

	const float n00 = gradientDot(h00, xf, zf);
	const float n10 = gradientDot(h10, xf - 1.0f, zf);
	const float n01 = gradientDot(h01, xf, zf - 1.0f);
	const float n11 = gradientDot(h11, xf - 1.0f, zf - 1.0f);

	const float u = fade(xf);
	const float v = fade(zf);
	const float a = n00 + u * (n10 - n00);
	const float b = n01 + u * (n11 - n01);
	const float value = a + v * (b - a);

	return value * 1.4142135623730951f;
}

float Noise2D::fbmF(float x, float z, std::uint32_t octaves, float lacunarity,
									 float gain) const {
	float sum = 0.0f;
	float amplitude = 1.0f;
	float frequency = 1.0f;
	float norm = 0.0f;

	for (std::uint32_t octave = 0; octave < octaves; ++octave) {
		sum += amplitude * noiseF(x * frequency, z * frequency);
		norm += amplitude;
		amplitude *= gain;
		frequency *= lacunarity;
	}

	return norm > 0.0f ? sum / norm : 0.0f;
}

void Noise2D::fbm4(const float* x, const float* z, std::uint32_t octaves,
									 float lacunarity, float gain, float* out) const {
	if (octaves == 0) {
		out[0] = out[1] = out[2] = out[3] = 0.0f;
		return;
	}
#ifdef VVNOISE_SSE2
	const __m128 xv = _mm_loadu_ps(x);
	const __m128 zv = _mm_loadu_ps(z);
	__m128 sum = _mm_setzero_ps();
	__m128 norm = _mm_setzero_ps();
	float amplitude = 1.0f;
	float frequency = 1.0f;

	for (std::uint32_t octave = 0; octave < octaves; ++octave) {
		const __m128 fx = _mm_mul_ps(xv, _mm_set1_ps(frequency));
		const __m128 fz = _mm_mul_ps(zv, _mm_set1_ps(frequency));
		sum = _mm_add_ps(sum, _mm_mul_ps(_mm_set1_ps(amplitude),
																		 noise4(fx, fz, m_seed)));
		norm = _mm_add_ps(norm, _mm_set1_ps(amplitude));
		amplitude *= gain;
		frequency *= lacunarity;
	}
	_mm_storeu_ps(out, _mm_div_ps(sum, norm));
#else
	for (int i = 0; i < 4; ++i) {
		out[i] = fbmF(x[i], z[i], octaves, lacunarity, gain);
	}
#endif
}

}  // namespace vv::terrain
