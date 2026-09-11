#pragma once

#include <glm/glm.hpp>
#include <cstdint>

namespace vv::voxel {

struct VoxelConfig final {
  glm::uvec3 chunkSizeVoxels = glm::uvec3(64u, 64u, 64u);
  glm::vec3 voxelSize = glm::vec3(1.0f, 1.0f, 1.0f);

  bool isValid() const {
    return chunkSizeVoxels.x > 0 && chunkSizeVoxels.y > 0 &&
           chunkSizeVoxels.z > 0 && voxelSize.x > 1e-6f &&
           voxelSize.y > 1e-6f && voxelSize.z > 1e-6f;
  }
};

} // namespace vv::voxel
