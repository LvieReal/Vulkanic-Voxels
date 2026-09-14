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

}  // namespace vv::voxel
