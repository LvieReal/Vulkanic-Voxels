#pragma once

#include <cstdint>

namespace vv::voxel {

struct Extent3u final {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t z = 0;
};

} // namespace vv::voxel
