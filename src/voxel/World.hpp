#pragma once

#include "voxel/Chunk.h"

namespace vv::voxel {

class World final {
public:
  explicit World(Extent3u chunkSize);

  const Chunk &chunk0() const { return m_chunk0; }

private:
  Chunk m_chunk0;
};

} // namespace vv::voxel
