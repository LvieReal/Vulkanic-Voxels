// Cost probe for the SDF rebuild path: how long does one region-crossing
// rebuild actually take, split into the pieces the renderer pays?
//
//   g++ -std=c++20 -O2 -ffp-contract=off -I. -Isrc probes/probe_sdf_cost.cpp \
//       src/terrain/{Noise,Noise3D,TerrainGenerator,FarField}.cpp \
//       src/voxel/{Chunk,VoxelTypes,VoxelTextures,World}.cpp -pthread -o /tmp/probe_cost
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "terrain/TerrainGenerator.hpp"
#include "voxel/SdfBox.hpp"
#include "voxel/SdfField.hpp"
#include "voxel/World.hpp"

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

// The renderer's shipped config (src/voxel/VoxelConfig.hpp). Kept local so
// this probe needs no glm/Vulkan headers.
struct Cfg {
  std::uint32_t chunkSizeX = 32;
  std::uint32_t worldHeight = 128;
  std::uint32_t chunkSizeZ = 32;
  std::uint32_t renderRadiusChunks = 12;
  std::uint32_t sdfHalfChunks = 3;
};

int main() {
  Cfg cfg;
  vv::terrain::TerrainConfig tcfg;
  std::printf("config: chunk %ux%ux%u, renderRadius %u, box half %u chunks\n",
              cfg.chunkSizeX, cfg.worldHeight, cfg.chunkSizeZ,
              cfg.renderRadiusChunks, cfg.sdfHalfChunks);

  vv::voxel::World world(tcfg, cfg.chunkSizeX, cfg.worldHeight, cfg.chunkSizeZ);
  // Time just the snapshot the renderer takes on the render thread.
  const int32_t cx = 0, cz = 0;
  const vv::voxel::SdfBoxGeometry box = vv::voxel::SdfBoxGeometry::centeredOn(
      cx, cz, cfg.sdfHalfChunks, cfg.chunkSizeX, cfg.chunkSizeZ, cfg.worldHeight);
  const std::uint32_t side = box.chunksPerSide;

  auto t0 = Clock::now();
  std::vector<const vv::voxel::Chunk*> added;
  std::vector<vv::voxel::ChunkCoord> evicted;
  world.ensureRegion(cx, cz, cfg.renderRadiusChunks, added, evicted);
  auto t1 = Clock::now();
  std::printf("ensureRegion(radius %u): %.1f ms  (%zu chunks newly generated, "
              "%zu cached)\n",
              cfg.renderRadiusChunks, ms(t0, t1), added.size(),
              world.cachedChunkCount());

  // (1) snapshot copy on the render thread (what launchSdfBuild does)
  std::vector<std::vector<std::uint8_t>> snaps(side * side);
  t0 = Clock::now();
  for (std::uint32_t z = 0; z < side; ++z) {
    for (std::uint32_t x = 0; x < side; ++x) {
      const int32_t ccx = cx - static_cast<int32_t>(side / 2u) +
                          static_cast<int32_t>(x);
      const int32_t ccz = cz - static_cast<int32_t>(side / 2u) +
                          static_cast<int32_t>(z);
      if (const vv::voxel::Chunk* c =
              world.findChunk(vv::voxel::ChunkCoord{ccx, ccz})) {
        snaps[static_cast<std::size_t>(z) * side + x] = c->voxelTypes();
      }
    }
  }
  t1 = Clock::now();
  std::printf("snapshot %u chunks: %.1f ms\n", side * side, ms(t0, t1));

  // (1b) the same snapshot+build with the pass-41 box (half = 4 chunks,
  // 8x8), which is what the renderer ships now: the price of the window
  // being a whole chunk bigger on every side.
  {
    const vv::voxel::SdfBoxGeometry big = vv::voxel::SdfBoxGeometry::centeredOn(
        cx, cz, cfg.sdfHalfChunks + 1, cfg.chunkSizeX, cfg.chunkSizeZ,
        cfg.worldHeight);
    const std::uint32_t bside = big.chunksPerSide;
    std::vector<std::vector<std::uint8_t>> bsnaps(bside * bside);
    auto b0 = Clock::now();
    for (std::uint32_t z = 0; z < bside; ++z) {
      for (std::uint32_t x = 0; x < bside; ++x) {
        const int32_t ccx = cx - static_cast<int32_t>(bside / 2u) +
                            static_cast<int32_t>(x);
        const int32_t ccz = cz - static_cast<int32_t>(bside / 2u) +
                            static_cast<int32_t>(z);
        if (const vv::voxel::Chunk* c =
                world.findChunk(vv::voxel::ChunkCoord{ccx, ccz})) {
          bsnaps[static_cast<std::size_t>(z) * bside + x] = c->voxelTypes();
        }
      }
    }
    auto b1 = Clock::now();
    std::printf("PASS-41 box 8x8: snapshot %u chunks %.1f ms\n", bside * bside,
                ms(b0, b1));
    for (int rep = 0; rep < 3; ++rep) {
      vv::voxel::SdfField bsdf;
      b0 = Clock::now();
      vv::voxel::buildSdfBoxField(big, bsnaps, bsdf);
      b1 = Clock::now();
      std::printf("PASS-41 buildSdfBoxField %ux%ux%u (%zu cells, %.1f MB): "
                  "%.1f ms\n",
                  big.nx, big.ny, big.nz, bsdf.seeds().size(),
                  double(bsdf.seeds().size()) * 4.0 / (1024.0 * 1024.0),
                  ms(b0, b1));
    }
  }

  // (2) the background build (chamfer EDT over the whole box)
  for (int rep = 0; rep < 3; ++rep) {
    vv::voxel::SdfField sdf;
    t0 = Clock::now();
    vv::voxel::buildSdfBoxField(box, snaps, sdf);
    t1 = Clock::now();
    std::printf("buildSdfBoxField %ux%ux%u (%zu cells): %.1f ms\n", box.nx,
                box.ny, box.nz, sdf.seeds().size(), ms(t0, t1));
  }

  // (3) packing the seeds for upload + the host-side memcpy
  vv::voxel::SdfField sdf;
  vv::voxel::buildSdfBoxField(box, snaps, sdf);
  const std::vector<int>& raw = sdf.seeds();
  std::vector<std::uint32_t> packed(raw.size());
  t0 = Clock::now();
  for (std::size_t i = 0; i < raw.size(); ++i) {
    packed[i] = raw[i] < 0 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(raw[i]);
  }
  t1 = Clock::now();
  std::printf("seed pack (%zu u32, %.1f MB): %.1f ms\n", packed.size(),
              packed.size() * 4.0 / 1048576.0, ms(t0, t1));

  void* dst = std::malloc(packed.size() * 4);
  t0 = Clock::now();
  std::memcpy(dst, packed.data(), packed.size() * 4);
  t1 = Clock::now();
  std::printf("memcpy %.1f MB (the staging copy): %.1f ms\n",
              packed.size() * 4.0 / 1048576.0, ms(t0, t1));
  std::free(dst);

  // (4) how much of the box is actually doing work
  std::size_t solid = 0;
  for (int s : raw) {
    if (s >= 0) ++solid;
  }
  std::printf("solid cells: %zu of %zu (%.1f%%); threads available: %u\n", solid,
              raw.size(), 100.0 * double(solid) / double(raw.size()),
              std::thread::hardware_concurrency());
  return 0;
}
