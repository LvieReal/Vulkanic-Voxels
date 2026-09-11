#pragma once

#include <cstdint>

namespace vv::voxel {

// Voxel material types. The GPU storage is exactly this enum (one byte per
// voxel, packed 4-per-uint32 in the chunk atlas); the compute shader maps
// types to colors through its palette tables (kTopColor/kSideColor/
// kBottomColor in resources/shaders/pixels_rgba.comp), which mirror
// kVoxelTypeInfo below — keep them in sync when adding types.
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

// Base albedo per type and face (linear RGB). Reference for CPU-side uses
// (minimap, previews, tests); the shader carries its own copy.
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

}  // namespace vv::voxel
