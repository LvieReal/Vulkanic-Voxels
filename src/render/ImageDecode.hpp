#pragma once

#include <string>

#include "voxel/VoxelTextures.hpp"

namespace vv::render {

// Decodes an image file (PNG) into RGBA8. Returns false when the file is
// missing or not a decodable image; an optional message can be produced for
// the caller's log. This is the one place the image decoder (stb_image,
// third_party/) is compiled; everything else sees plain RGBA8 buffers.
bool loadImageFileRGBA(const std::string& path,
											 vv::voxel::VoxelTextureImage& out,
											 std::string* outError = nullptr);

}  // namespace vv::render
