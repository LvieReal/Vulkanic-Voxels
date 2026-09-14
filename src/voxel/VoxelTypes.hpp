#pragma once

#include <cstdint>
#include <vector>

namespace vv::voxel {

// Voxel material types. The GPU storage is exactly this enum (one byte per
// voxel, packed 4-per-uint32 in the chunk atlas). The compute shader maps
// types to colors through the voxel palette buffer (binding 4), which the
// renderer fills from kVoxelTypeInfo below, so colors stay data-driven.
// Detail textures live in a separate bindless image array (binding 8,
// VoxelTextures.hpp): grayscale detail multiplied into these albedos.
enum class VoxelType : std::uint8_t {
	Air = 0,
	Grass = 1,
	Dirt = 2,
	Stone = 3,
	Sand = 4,
	Snow = 5,
	Bedrock = 6,
};

constexpr std::uint32_t kVoxelTypeCount = 7;

// Hierarchical-DDA block size (pass 30): the near march skips whole
// kHeightBlockVoxels^2 column blocks whose max height bound stays below
// the ray. Must divide chunkSizeX/Z (32/8 = 4 blocks per chunk side) and
// matches kBlockVoxels in resources/shaders/pixels_rgba.comp - a sync
// contract (see Chunk::blockHeightMapWords and the shader's
// BlockHeights buffer, binding 11).
constexpr std::uint32_t kHeightBlockVoxels = 8;

// Capacity of the GPU voxel palette, in entries per face. Must match the
// array sizes in the shader's VoxelPalette block (top/side/bottom[8]).
constexpr std::uint32_t kPaletteCapacity = 8;

// Base albedo per type and face (linear RGB), uploaded to the shader's voxel
// palette buffer at renderer init.
struct VoxelTypeInfo {
	float top[3];
	float side[3];
	float bottom[3];
};

constexpr VoxelTypeInfo kVoxelTypeInfo[kVoxelTypeCount] = {
		{{0.00f, 0.00f, 0.00f}, {0.00f, 0.00f, 0.00f}, {0.00f, 0.00f, 0.00f}},  // Air
		{{0.26f, 0.55f, 0.14f}, {0.40f, 0.31f, 0.15f}, {0.43f, 0.31f, 0.18f}},  // Grass
		{{0.43f, 0.31f, 0.18f}, {0.40f, 0.29f, 0.17f}, {0.36f, 0.25f, 0.15f}},  // Dirt
		{{0.56f, 0.56f, 0.58f}, {0.52f, 0.52f, 0.55f}, {0.47f, 0.47f, 0.50f}},  // Stone
		{{0.87f, 0.81f, 0.60f}, {0.83f, 0.77f, 0.57f}, {0.79f, 0.73f, 0.54f}},  // Sand
		{{0.93f, 0.95f, 0.98f}, {0.88f, 0.90f, 0.95f}, {0.84f, 0.86f, 0.92f}},  // Snow
		{{0.18f, 0.17f, 0.19f}, {0.18f, 0.17f, 0.19f}, {0.18f, 0.17f, 0.19f}},  // Bedrock
};

// Builds the flat voxel palette consumed by the compute shader's VoxelPalette
// SSBO: three consecutive vec4 arrays (top, side, bottom), kPaletteCapacity
// entries each, entry order = VoxelType enum order; RGB = base albedo from
// kVoxelTypeInfo, A = 1. Unused entries are zeroed.
// Returns 3 * kPaletteCapacity * 4 floats (384 bytes at the default capacity).
// Pure function on constexpr data - unit tested in tests/terrain_world_tests.cpp.
std::vector<float> buildVoxelPalette();

}  // namespace vv::voxel
