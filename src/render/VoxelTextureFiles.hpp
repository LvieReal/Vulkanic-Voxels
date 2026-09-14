#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "voxel/VoxelTextures.hpp"

namespace vv::render {

// Loads the per-type voxel textures from a texture directory (see
// resources/textures/voxels/README.md for the file naming and the three
// definition modes). For every VoxelType the most specific complete
// mode wins (custom 6 files > side-uniform 3 > uniform 1); a type whose
// files are missing entirely falls back to plain palette colors
// (set.textured = false) - which is exactly what happens in checkouts
// without texture files, e.g. CI/sandboxes.
//
// outImages receives one RGBA8 image per loaded FILE (deduplicated per
// file path); outSets is resized to kVoxelTypeCount with faceIndex
// pointing into outImages. outLog collects one status line per type.
// Returns false only on I/O errors that make the whole directory
// unreadable (still fills plain sets in that case).
bool loadVoxelTextureFiles(const std::filesystem::path& dir,
													 std::vector<vv::voxel::VoxelTextureImage>& outImages,
													 std::vector<vv::voxel::VoxelTextureSet>& outSets,
													 std::string& outLog);

}  // namespace vv::render
