#include "terrain/Noise3D.hpp"

#include <cmath>

namespace vv::terrain {

namespace {

// The 12 edge midpoints of a cube (Perlin's improved-noise gradient set),
// normalized to unit length; the interpolated single-octave maximum is then
// ~sqrt(3)/2, which the rescale below maps to roughly [-1, 1].
constexpr float kInvSqrt2 = 0.70710678118654752f;

constexpr float kGradients[12][3] = {
		{kInvSqrt2, kInvSqrt2, 0.0f},
		{-kInvSqrt2, kInvSqrt2, 0.0f},
		{kInvSqrt2, -kInvSqrt2, 0.0f},
		{-kInvSqrt2, -kInvSqrt2, 0.0f},
		{kInvSqrt2, 0.0f, kInvSqrt2},
		{-kInvSqrt2, 0.0f, kInvSqrt2},
		{kInvSqrt2, 0.0f, -kInvSqrt2},
		{-kInvSqrt2, 0.0f, -kInvSqrt2},
		{0.0f, kInvSqrt2, kInvSqrt2},
		{0.0f, -kInvSqrt2, kInvSqrt2},
		{0.0f, kInvSqrt2, -kInvSqrt2},
		{0.0f, -kInvSqrt2, -kInvSqrt2},
};

// MurmurHash3-style 32-bit finalizer (same as Noise2D).
std::uint32_t fmix32(std::uint32_t x) {
	x ^= x >> 16;
	x *= 0x85ebca6bu;
	x ^= x >> 13;
	x *= 0xc2b2ae35u;
	x ^= x >> 16;
	return x;
}

// Hash of one 3D integer lattice cell. Negative coordinates are fine: only
// the bit pattern is consumed (same mixing scheme as Noise2D::cellHash).
std::uint32_t cellHash3(std::uint32_t seed, std::int32_t xi, std::int32_t yi,
												std::int32_t zi) {
	std::uint32_t h = seed;
	h ^= static_cast<std::uint32_t>(xi) + 0x9e3779b9u + (h << 6) + (h >> 2);
	h ^= static_cast<std::uint32_t>(yi) + 0x9e3779b9u + (h << 6) + (h >> 2);
	h ^= static_cast<std::uint32_t>(zi) + 0x9e3779b9u + (h << 6) + (h >> 2);
	return fmix32(h);
}

}  // namespace

float Noise3D::gradientDot(std::uint32_t hash, float dx, float dy, float dz) {
	const float* g = kGradients[hash % 12u];
	return g[0] * dx + g[1] * dy + g[2] * dz;
}

float Noise3D::noiseF(float x, float y, float z) const {
	const float floorX = std::floor(x);
	const float floorY = std::floor(y);
	const float floorZ = std::floor(z);
	const std::int32_t xi = static_cast<std::int32_t>(floorX);
	const std::int32_t yi = static_cast<std::int32_t>(floorY);
	const std::int32_t zi = static_cast<std::int32_t>(floorZ);
	const float xf = x - floorX;
	const float yf = y - floorY;
	const float zf = z - floorZ;

	const std::uint32_t h000 = cellHash3(m_seed, xi, yi, zi);
	const std::uint32_t h100 = cellHash3(m_seed, xi + 1, yi, zi);
	const std::uint32_t h010 = cellHash3(m_seed, xi, yi + 1, zi);
	const std::uint32_t h110 = cellHash3(m_seed, xi + 1, yi + 1, zi);
	const std::uint32_t h001 = cellHash3(m_seed, xi, yi, zi + 1);
	const std::uint32_t h101 = cellHash3(m_seed, xi + 1, yi, zi + 1);
	const std::uint32_t h011 = cellHash3(m_seed, xi, yi + 1, zi + 1);
	const std::uint32_t h111 = cellHash3(m_seed, xi + 1, yi + 1, zi + 1);

	const float n000 = gradientDot(h000, xf, yf, zf);
	const float n100 = gradientDot(h100, xf - 1.0f, yf, zf);
	const float n010 = gradientDot(h010, xf, yf - 1.0f, zf);
	const float n110 = gradientDot(h110, xf - 1.0f, yf - 1.0f, zf);
	const float n001 = gradientDot(h001, xf, yf, zf - 1.0f);
	const float n101 = gradientDot(h101, xf - 1.0f, yf, zf - 1.0f);
	const float n011 = gradientDot(h011, xf, yf - 1.0f, zf - 1.0f);
	const float n111 = gradientDot(h111, xf - 1.0f, yf - 1.0f, zf - 1.0f);

	const float u = fade(xf);
	const float v = fade(yf);
	const float w = fade(zf);

	const float a = n000 + u * (n100 - n000);
	const float b = n010 + u * (n110 - n010);
	const float c = n001 + u * (n101 - n001);
	const float d = n011 + u * (n111 - n011);
	const float e = a + v * (b - a);
	const float f = c + v * (d - c);

	// Unit gradients interpolate to at most ~sqrt(3)/2; rescale so the
	// single-octave output lands roughly in [-1, 1] (same spirit as the 2D
	// noise's sqrt(2) rescale).
	return (e + w * (f - e)) * 1.1547005383792515f;
}

float Noise3D::fbmF(float x, float y, float z, std::uint32_t octaves,
									 float lacunarity, float gain) const {
	float sum = 0.0f;
	float amplitude = 1.0f;
	float frequency = 1.0f;
	float norm = 0.0f;

	for (std::uint32_t octave = 0; octave < octaves; ++octave) {
		sum += amplitude * noiseF(x * frequency, y * frequency,
															z * frequency);
		norm += amplitude;
		amplitude *= gain;
		frequency *= lacunarity;
	}

	return norm > 0.0f ? sum / norm : 0.0f;
}

}  // namespace vv::terrain
