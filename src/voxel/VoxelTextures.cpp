#include "VoxelTextures.hpp"

#include <array>

namespace vv::voxel {
namespace {

// Suffix tables per mode; index = file index within the mode.
constexpr std::array<const char*, 1> kUniformSuffixes = {""};
constexpr std::array<const char*, 3> kSideSuffixes = {"_top", "_bottom",
																											"_side"};
constexpr std::array<const char*, 6> kCustomSuffixes = {
		"_top", "_bottom", "_back", "_front", "_right", "_left"};

}  // namespace

const char* voxelTextureSuffix(VoxelTextureMode mode, std::uint32_t file) {
	switch (mode) {
		case VoxelTextureMode::Uniform:
			return file < kUniformSuffixes.size() ? kUniformSuffixes[file] : "";
		case VoxelTextureMode::SideUniform:
			return file < kSideSuffixes.size() ? kSideSuffixes[file] : "";
		case VoxelTextureMode::Custom:
		default:
			return file < kCustomSuffixes.size() ? kCustomSuffixes[file] : "";
	}
}

bool voxelFaceIdFromName(const std::string& name, std::uint32_t& faceId) {
	if (name == "top") {
		faceId = 0;
	} else if (name == "bottom") {
		faceId = 1;
	} else if (name == "back") {
		faceId = 2;
	} else if (name == "front") {
		faceId = 3;
	} else if (name == "right") {
		faceId = 4;
	} else if (name == "left") {
		faceId = 5;
	} else {
		return false;
	}
	return true;
}

const char* voxelFaceNameOfId(std::uint32_t faceId) {
	static const char* kNames[6] = {"top", "bottom", "back",
													 "front", "right", "left"};
	return faceId < 6 ? kNames[faceId] : "top";
}

std::uint32_t resolveFaceTextureIndex(const VoxelTextureSet& set,
                                      const std::string& sourceFaceName) {
	if (!set.textured) {
		return kNoFaceTexture;
	}
	if (sourceFaceName == "side" || sourceFaceName == "sides") {
		return set.faceIndex[2];
	}
	std::uint32_t faceId = 0;
	if (!voxelFaceIdFromName(sourceFaceName, faceId)) {
		return kNoFaceTexture;
	}
	return set.faceIndex[faceId];
}

}  // namespace vv::voxel
