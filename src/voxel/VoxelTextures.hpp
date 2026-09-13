#pragma once

#include <cstdint>
#include <vector>

namespace vv::voxel {

// Edge length (texels) of one voxel face's detail texture. One texture
// per VoxelType, tiled per voxel via world-space UVs (REPEAT sampler in
// the compute shader - binding 8, no atlas).
constexpr std::uint32_t kVoxelTextureSize = 32;

// Generates one per-type detail texture: RGBA8, kVoxelTextureSize^2
// texels, grayscale detail in [0.55, 1.0] (the shader multiplies it
// with the palette albedo, so colors stay data-driven - see
// kVoxelTypeInfo). Tileable in X and Y (lattice noise with periods
// dividing the size), deterministic pure function of `type` - unit
// tested in tests/terrain_world_tests.cpp.
std::vector<std::uint8_t> generateVoxelTextureRGBA(std::uint32_t type);

}  // namespace vv::voxel
