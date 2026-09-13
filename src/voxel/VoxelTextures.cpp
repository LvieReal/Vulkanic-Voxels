#include "VoxelTextures.hpp"

#include "VoxelTypes.hpp"

#include <cmath>

namespace vv::voxel {
namespace {

// Integer hash -> [0, 1). Deterministic across platforms (pure integer
// arithmetic, no RNG state).
float hash2(std::uint32_t x, std::uint32_t y, std::uint32_t seed) {
	std::uint32_t h = x * 374761393u + y * 668265263u + seed * 1274126177u;
	h = (h ^ (h >> 13u)) * 1274126177u;
	h ^= h >> 16u;
	return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

// Tileable value noise over a `period` x `period` lattice spread across
// the kVoxelTextureSize tile. Lattice coordinates wrap, so the noise is
// seamless at tile edges (per-voxel tiling would otherwise show a visible
// seam grid). `x`, `y` in texel units.
float tileNoise(float x, float y, std::uint32_t period, std::uint32_t seed) {
	const float fx = x * static_cast<float>(period) /
									 static_cast<float>(kVoxelTextureSize);
	const float fy = y * static_cast<float>(period) /
									 static_cast<float>(kVoxelTextureSize);
	const auto x0 = static_cast<std::uint32_t>(fx) % period;
	const auto y0 = static_cast<std::uint32_t>(fy) % period;
	const std::uint32_t x1 = (x0 + 1u) % period;
	const std::uint32_t y1 = (y0 + 1u) % period;
	const float tx = fx - std::floor(fx);
	const float ty = fy - std::floor(fy);
	const float sx = tx * tx * (3.0f - 2.0f * tx);
	const float sy = ty * ty * (3.0f - 2.0f * ty);
	const float n00 = hash2(x0, y0, seed);
	const float n10 = hash2(x1, y0, seed);
	const float n01 = hash2(x0, y1, seed);
	const float n11 = hash2(x1, y1, seed);
	const float a = n00 + (n10 - n00) * sx;
	const float b = n01 + (n11 - n01) * sx;
	return a + (b - a) * sy;
}

// Two tileable octaves (period 4 then 8): enough character per material
// at 32x32 without turning every type into the same mush.
float fbm2(float x, float y, std::uint32_t seed) {
	return tileNoise(x, y, 4u, seed) * (2.0f / 3.0f) +
				 tileNoise(x, y, 8u, seed + 101u) * (1.0f / 3.0f);
}

float clampf(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

// Per-type detail recipes. All values are clamped to [0.55, 1.0] (the
// byte texture cannot encode > 1; the shader multiplies it straight into
// the palette albedo from kVoxelTypeInfo, which carries the color
// identity). Means sit near 0.85-0.9 so the tuned pass-19 look is only
// textured, not re-graded.
float detailAt(std::uint32_t type, std::uint32_t x, std::uint32_t y) {
	const float fx = static_cast<float>(x);
	const float fy = static_cast<float>(y);
	switch (type) {
		case 1: {  // Grass: fine vertical blade streaks + bright speckles.
			const float n = fbm2(fx, fy, 11u);
			const float blade =
					0.5f + 0.5f * std::sin((fy + 4.0f * n) * 3.14159265f / 4.0f);
			const float sparkle = tileNoise(fx, fy, 16u, 23u);
			return 0.82f + 0.13f * (n - 0.5f) + 0.08f * blade * sparkle;
		}
		case 2: {  // Dirt: clumpy noise + dark pebbles.
			const float n = fbm2(fx, fy, 31u);
			const float pebble = hash2(x, y, 43u) > 0.97f ? -0.15f : 0.0f;
			return 0.80f + 0.22f * (n - 0.5f) + pebble;
		}
		case 3: {  // Stone: broad blotches + crack lines + bright specks.
			const float n = fbm2(fx, fy, 57u);
			const float crackField = tileNoise(fx, fy, 8u, 67u);
			const float crack = std::fabs(crackField - 0.5f) < 0.025f ? -0.18f : 0.0f;
			const float speck = hash2(x, y, 71u) > 0.985f ? 0.10f : 0.0f;
			return 0.82f + 0.16f * (n - 0.5f) + crack + speck;
		}
		case 4: {  // Sand: wind ripples, warped by noise.
			const float warp = fbm2(fx, fy, 83u);
			const float ripple = std::sin((fx + 5.0f * warp * fy * 0.25f) *
																		3.14159265f / 4.0f);
			return 0.82f + 0.07f * ripple + 0.05f * (warp - 0.5f);
		}
		case 5: {  // Snow: smooth undulations, near-flat.
			const float n = tileNoise(fx, fy, 4u, 97u);
			const float fine = tileNoise(fx, fy, 16u, 103u);
			return 0.88f + 0.08f * (n - 0.5f) + 0.03f * (fine - 0.5f);
		}
		case 6: {  // Bedrock: high-contrast angular blobs + dark pits.
			const float n = fbm2(fx, fy, 127u);
			const float pit = hash2(x, y, 131u) > 0.94f ? -0.20f : 0.0f;
			return 0.75f + 0.45f * (n - 0.5f) + pit;
		}
		default:  // Air (never sampled; kept valid for the array).
			return 1.0f;
	}
}

}  // namespace

std::vector<std::uint8_t> generateVoxelTextureRGBA(std::uint32_t type) {
	const std::size_t texels =
			static_cast<std::size_t>(kVoxelTextureSize) * kVoxelTextureSize;
	std::vector<std::uint8_t> rgba(texels * 4, 255);
	for (std::uint32_t y = 0; y < kVoxelTextureSize; ++y) {
		for (std::uint32_t x = 0; x < kVoxelTextureSize; ++x) {
			const float d = clampf(detailAt(type, x, y), 0.55f, 1.0f);
			const auto v = static_cast<std::uint8_t>(
					static_cast<int>(d * 255.0f + 0.5f) > 255
							? 255
							: static_cast<int>(d * 255.0f + 0.5f));
			const std::size_t i =
					(static_cast<std::size_t>(y) * kVoxelTextureSize + x) * 4;
			rgba[i + 0] = v;
			rgba[i + 1] = v;
			rgba[i + 2] = v;
			rgba[i + 3] = 255;
		}
	}
	return rgba;
}

}  // namespace vv::voxel
