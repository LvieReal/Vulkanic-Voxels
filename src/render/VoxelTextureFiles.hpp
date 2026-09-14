#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "voxel/VoxelTextures.hpp"

namespace vv::render {

// Loads the per-type voxel textures from a texture directory (see
// resources/textures/voxels/README.md for the file naming). Resolution
// is PER FACE (pass 28): each face uses the most specific of its
// candidate files that exists (own name > _side > uniform file); faces
// without a file keep the plain palette color unless an alias fills
// them. A type with no files at all is entirely plain
// (set.textured = false) - e.g. checkouts without texture files
// (CI/sandboxes).
//
// An optional aliases.txt in the directory reuses one type's textures
// for another type's faces, e.g. "grass bottom = dirt" or
// "grass sides = dirt side" (no file duplication). Lines:
//   <type> <face> = <source type> [<source face>]
// with face in {top, bottom, front, back, right, left, sides, all};
// the source face defaults to the target face's name and resolves
// against the source's per-face assignments (a uniform source serves
// any face; side/sides uses the source's side texture). Aliases
// override file-based assignments and can fill plain faces.
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
