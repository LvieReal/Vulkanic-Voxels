#include "voxel/Chunk.h"

#include <cassert>

namespace vv::voxel {

Chunk::Chunk(int32_t chunkX, int32_t chunkY, int32_t chunkZ, Extent3u size)
    : m_chunkX(chunkX)
    , m_chunkY(chunkY)
    , m_chunkZ(chunkZ)
    , m_size(size)
    , m_voxelsU32(static_cast<size_t>(voxelCount()), 0u)
{
}

uint64_t Chunk::index(uint32_t x, uint32_t y, uint32_t z) const
{
    return static_cast<uint64_t>(x) + static_cast<uint64_t>(y) * m_size.x
        + static_cast<uint64_t>(z) * static_cast<uint64_t>(m_size.x) * static_cast<uint64_t>(m_size.y);
}

Voxel Chunk::get(uint32_t x, uint32_t y, uint32_t z) const
{
    assert(x < m_size.x && y < m_size.y && z < m_size.z);
    return Voxel{m_voxelsU32[static_cast<size_t>(index(x, y, z))]};
}

void Chunk::set(uint32_t x, uint32_t y, uint32_t z, Voxel v)
{
    assert(x < m_size.x && y < m_size.y && z < m_size.z);
    m_voxelsU32[static_cast<size_t>(index(x, y, z))] = v.rgba8;
}

} // namespace vv::voxel
