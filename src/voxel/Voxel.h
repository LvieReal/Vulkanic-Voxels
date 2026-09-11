#pragma once

#include <cstdint>

namespace vv::voxel {

struct Voxel final {
    // Packed as 0xAARRGGBB. Value 0 means "empty".
    uint32_t rgba8 = 0;
};

} // namespace vv::voxel
