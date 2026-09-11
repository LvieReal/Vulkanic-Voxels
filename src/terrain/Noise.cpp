#include "terrain/Noise.hpp"

#include <cmath>

namespace vv::terrain {

namespace {

// Eight evenly spaced unit gradient directions (multiples of 45 degrees).
constexpr double kGradients[8][2] = {
		{1.0, 0.0},
		{0.70710678118654752, 0.70710678118654752},
		{0.0, 1.0},
		{-0.70710678118654752, 0.70710678118654752},
		{-1.0, 0.0},
		{-0.70710678118654752, -0.70710678118654752},
		{0.0, -1.0},
		{0.70710678118654752, -0.70710678118654752},
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

}  // namespace

double Noise2D::gradientDot(std::uint32_t hash, double dx, double dz) {
	const double* g = kGradients[hash & 7u];
	return g[0] * dx + g[1] * dz;
}

double Noise2D::noise(double x, double z) const {
	const double floorX = std::floor(x);
	const double floorZ = std::floor(z);
	const std::int32_t xi = static_cast<std::int32_t>(floorX);
	const std::int32_t zi = static_cast<std::int32_t>(floorZ);
	const double xf = x - floorX;
	const double zf = z - floorZ;

	const std::uint32_t h00 = cellHash(m_seed, xi, zi);
	const std::uint32_t h10 = cellHash(m_seed, xi + 1, zi);
	const std::uint32_t h01 = cellHash(m_seed, xi, zi + 1);
	const std::uint32_t h11 = cellHash(m_seed, xi + 1, zi + 1);

	const double n00 = gradientDot(h00, xf, zf);
	const double n10 = gradientDot(h10, xf - 1.0, zf);
	const double n01 = gradientDot(h01, xf, zf - 1.0);
	const double n11 = gradientDot(h11, xf - 1.0, zf - 1.0);

	const double u = fade(xf);
	const double v = fade(zf);
	const double a = n00 + u * (n10 - n00);
	const double b = n01 + u * (n11 - n01);
	const double value = a + v * (b - a);

	// Unit gradients give a theoretical max of ~sqrt(2)/2 in 2D; rescale to
	// roughly [-1, 1].
	return value * 1.4142135623730951;
}

double Noise2D::fbm(double x, double z, std::uint32_t octaves, double lacunarity,
									 double gain) const {
	double sum = 0.0;
	double amplitude = 1.0;
	double frequency = 1.0;
	double norm = 0.0;

	for (std::uint32_t octave = 0; octave < octaves; ++octave) {
		sum += amplitude * noise(x * frequency, z * frequency);
		norm += amplitude;
		amplitude *= gain;
		frequency *= lacunarity;
	}

	return norm > 0.0 ? sum / norm : 0.0;
}

}  // namespace vv::terrain
