#pragma once

#include "voxel/Extent3.h"
#include "voxel/Voxel.h"

#include <cstdint>
#include <vector>

namespace vv::voxel {

class Chunk final
{
public:
    Chunk() = default;
    Chunk(int32_t chunkX, int32_t chunkY, int32_t chunkZ, Extent3u size);

    int32_t chunkX() const { return m_chunkX; }
    int32_t chunkY() const { return m_chunkY; }
    int32_t chunkZ() const { return m_chunkZ; }

    Extent3u size() const { return m_size; }
    uint64_t voxelCount() const
    {
        return static_cast<uint64_t>(m_size.x) * static_cast<uint64_t>(m_size.y) * static_cast<uint64_t>(m_size.z);
    }

    Voxel get(uint32_t x, uint32_t y, uint32_t z) const;
    void set(uint32_t x, uint32_t y, uint32_t z, Voxel v);

    const std::vector<uint32_t>& rawVoxelsU32() const { return m_voxelsU32; }

private:
    uint64_t index(uint32_t x, uint32_t y, uint32_t z) const;

    int32_t m_chunkX = 0;
    int32_t m_chunkY = 0;
    int32_t m_chunkZ = 0;
    Extent3u m_size{};
    std::vector<uint32_t> m_voxelsU32;
};

} // namespace vv::voxel
