#pragma once

#include <cstdint>
#include <vector>

namespace vv::voxel {

// Bindless voxel texture definitions (pass 21). Textures are FILES in
// resources/textures/voxels (the user's machine); types without files
// fall back to plain palette colors (kVoxelTypeInfo). The GPU side is a
// sampled-image array (binding 8, one image per FILE) plus a per-type
// table (binding 10) mapping type x face -> image index.
//
// Face id convention (used everywhere: files, table, shader):
//   0 = +Y (top)   1 = -Y (bottom)
//   2 = +X         3 = -X
//   4 = +Z         5 = -Z

// How many textures a type uses and which file serves which face.
enum class VoxelTextureMode : std::uint8_t {
	Uniform = 0,    // one texture for all 6 faces: <name>.png
	SideUniform = 1,  // <name>_top.png, <name>_bottom.png, <name>_side.png
	Custom = 2,     // <name>_top/_bottom/_px/_nx/_pz/_nz.png (6 files)
};

// Canonical lowercase file base name per VoxelType (see
// resources/textures/voxels/README.md). Index = VoxelType enum order.
constexpr const char* kVoxelTypeNames[] = {"air",    "grass", "dirt",
																					 "stone",   "sand",  "snow",
																					 "bedrock"};

// Number of texture files a mode uses.
constexpr std::uint32_t voxelTextureFileCount(VoxelTextureMode mode) {
	return mode == VoxelTextureMode::Uniform
					 ? 1u
					 : (mode == VoxelTextureMode::SideUniform ? 3u : 6u);
}

// File name suffix for file index i (0..count-1) of a mode:
//   Uniform:     ""
//   SideUniform: "_top", "_bottom", "_side"
//   Custom:      "_top", "_bottom", "_px", "_nx", "_pz", "_nz"
// Pure function on constexpr data - unit tested in
// tests/terrain_world_tests.cpp.
const char* voxelTextureSuffix(VoxelTextureMode mode, std::uint32_t file);

// Which file index (0..count-1) of a mode serves the given face id
// (0..5, convention above). SideUniform maps top->0, bottom->1, every
// side face->2; Custom is the identity. Pure - unit tested.
constexpr std::uint32_t faceTextureFile(VoxelTextureMode mode,
																				std::uint32_t faceId) {
	return mode == VoxelTextureMode::Uniform
					 ? 0u
					 : (mode == VoxelTextureMode::SideUniform
									? (faceId == 0u   ? 0u
											: faceId == 1u ? 1u
																		: 2u)
									: faceId);
}

// One RGBA8 image (any size; squareness not required). Produced by the
// file loader (vv::render::loadVoxelTextureFiles) and consumed by
// VoxelResources::createVoxelTextures.
struct VoxelTextureImage {
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::vector<std::uint8_t> rgba;  // width * height * 4, row-major
};

// Per-type texture assignment. faceIndex[f] indexes the loaded image
// list (one image per FILE); kNoFaceTexture marks the plain-color
// fallback. nominalSize = max texture dimension of the type (the
// shader's explicit-LOD estimate).
constexpr std::uint32_t kNoFaceTexture = 0xFFFFFFFFu;

// Capacity of the bindless image array (binding 8): worst case is 7
// types x 6 custom faces + the white dummy, rounded up. The descriptor
// layout and the shader array both use this; unused slots are bound to
// the dummy view.
constexpr std::uint32_t kMaxVoxelTextures = 48;

struct VoxelTextureSet {
	bool textured = false;
	std::uint32_t faceIndex[6] = {kNoFaceTexture, kNoFaceTexture,
																kNoFaceTexture, kNoFaceTexture,
																kNoFaceTexture, kNoFaceTexture};
	std::uint32_t nominalSize = 32;
};

}  // namespace vv::voxel
