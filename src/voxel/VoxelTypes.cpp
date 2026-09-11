#include "voxel/VoxelTypes.hpp"

namespace vv::voxel {

std::vector<float> buildVoxelPalette() {
	constexpr std::uint32_t kFaces = 3;  // top, side, bottom
	std::vector<float> palette(
			static_cast<std::size_t>(kFaces) * kPaletteCapacity * 4u, 0.0f);

	for (std::uint32_t type = 0;
			 type < kVoxelTypeCount && type < kPaletteCapacity; ++type) {
		const VoxelTypeInfo& info = kVoxelTypeInfo[type];
		const float* faces[kFaces] = {info.top, info.side, info.bottom};

		for (std::uint32_t face = 0; face < kFaces; ++face) {
			const std::size_t dst =
					(static_cast<std::size_t>(face) * kPaletteCapacity + type) * 4u;
			for (std::uint32_t c = 0; c < 3; ++c) {
				palette[dst + c] = faces[face][c];
			}
			palette[dst + 3] = 1.0f;
		}
	}

	return palette;
}

}  // namespace vv::voxel
