#include "voxel/World.h"

#include <random>

namespace vv::voxel {

World::World(Extent3u chunkSize)
    : m_chunk0(0, 0, 0, chunkSize)
{
    std::random_device rd;
    std::mt19937 rng(rd());
    std::bernoulli_distribution solidDist(0.5);
    std::uniform_int_distribution<int> colorDist(32, 255);

    for (uint32_t z = 0; z < chunkSize.z; ++z) {
        for (uint32_t y = 0; y < chunkSize.y; ++y) {
            for (uint32_t x = 0; x < chunkSize.x; ++x) {
                Voxel v{};
                if (solidDist(rng)) {
                    const uint32_t r = static_cast<uint32_t>(colorDist(rng));
                    const uint32_t g = static_cast<uint32_t>(colorDist(rng));
                    const uint32_t b = static_cast<uint32_t>(colorDist(rng));
                    v.rgba8 = (255u << 24) | (r << 16) | (g << 8) | (b << 0);
                } else {
                    v.rgba8 = 0u;
                }
                m_chunk0.set(x, y, z, v);
            }
        }
    }
}

} // namespace vv::voxel
