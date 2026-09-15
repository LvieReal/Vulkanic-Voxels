// Probe: is an SDF REBUILD visible? The field is recentered on the camera's
// chunk every time a region crossing completes, so on the crossing the
// shadows switch from the field built for the OLD center to the one built for
// the NEW center - with the camera already at the new position. This measures
// exactly that swap: for every ground column's top face, the visibility with
// the field centered one chunk back vs one chunk forward, in the SAME world.
//
// Anything with |delta| above the "visible" threshold is ground whose shading
// changes (blinks) the moment a rebuild lands - the flicker, if the rebuild is
// what flickers.
//
//   g++ -std=c++20 -O2 -ffp-contract=off -I. -Isrc probes/probe_rebuild_swap.cpp \
//       src/terrain/{Noise,Noise3D,TerrainGenerator,FarField}.cpp \
//       src/voxel/{Chunk,VoxelTypes,VoxelTextures,World}.cpp -pthread -o /tmp/probe_swap
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "terrain/TerrainGenerator.hpp"
#include "voxel/SdfBox.hpp"
#include "voxel/SdfField.hpp"
#include "voxel/World.hpp"

namespace {

constexpr std::uint32_t kChunkX = 32, kWorldH = 128, kChunkZ = 32;
constexpr std::uint32_t kHalf = 3;  // kSdfHalfChunks
constexpr int kRegionRadius = 10;   // chunks resident in this probe

const double kSun[3] = {0.4082482905, 0.8164965809, 0.4082482905};

struct View {
  const vv::voxel::World* world;
  std::int32_t originChunk;

  // Column top (highest solid + 1) of the world column at (x, z), or -1.
  int topAt(int x, int z) const {
    const std::int32_t ccx = x / int(kChunkX), ccz = z / int(kChunkZ);
    const vv::voxel::Chunk* c =
        world->findChunk(vv::voxel::ChunkCoord{ccx, ccz});
    if (c == nullptr) {
      return -1;
    }
    const auto& types = c->voxelTypes();
    const std::uint32_t lx = std::uint32_t(x) % kChunkX;
    const std::uint32_t lz = std::uint32_t(z) % kChunkZ;
    for (int y = int(kWorldH) - 1; y >= 0; --y) {
      if (types[lx + std::uint32_t(y) * kChunkX + lz * kChunkX * kWorldH] !=
          0) {
        return y + 1;
      }
    }
    return 0;
  }

  // Exact binary march of the near world (no far cells in this probe).
  bool solidAt(int x, int y, int z) const {
    if (y < 0 || y >= int(kWorldH)) {
      return false;
    }
    const std::int32_t ccx = x / int(kChunkX), ccz = z / int(kChunkZ);
    if (x < 0 || z < 0) {
      return false;
    }
    const vv::voxel::Chunk* c =
        world->findChunk(vv::voxel::ChunkCoord{ccx, ccz});
    if (c == nullptr) {
      return false;
    }
    const auto& types = c->voxelTypes();
    const std::uint32_t lx = std::uint32_t(x) % kChunkX;
    const std::uint32_t lz = std::uint32_t(z) % kChunkZ;
    return types[lx + std::uint32_t(y) * kChunkX + lz * kChunkX * kWorldH] != 0;
  }
};

float penumbra(float h, float t) {
  const float k = 8.0f;
  h = std::max(h, 0.0f);
  return std::min(std::clamp(k * h / std::max(t, 1e-4f), 0.0f, 1.0f), 1.0f);
}

// 2.5D penumbra march over the near world (the shader's sunRayEscapesSdf).
float march2d(const View& v, const float o[3], float startT, float startVis) {
  float visibility = startVis;
  float t = 0.0f;
  int colX = int(std::floor(o[0])), colZ = int(std::floor(o[2]));
  const int stepX = kSun[0] > 0 ? 1 : -1, stepZ = kSun[2] > 0 ? 1 : -1;
  float tMaxX = (float(colX + (stepX > 0 ? 1 : 0)) - o[0]) / float(kSun[0]);
  float tMaxZ = (float(colZ + (stepZ > 0 ? 1 : 0)) - o[2]) / float(kSun[2]);
  const float dX = float(1.0 / std::abs(kSun[0])), dZ = float(1.0 / std::abs(kSun[2]));
  for (int i = 0; i < 256; ++i) {
    const float sExit = std::min(tMaxX, tMaxZ);
    const float y0 = o[1] + float(kSun[1]) * t;
    if (y0 >= float(kWorldH)) {
      return visibility;
    }
    const int top = v.topAt(colX, colZ);
    if (top > 0) {
      if (y0 >= float(top)) {
        visibility = std::min(visibility, penumbra(y0 - float(top), startT + t));
      } else {
        const float y1 = o[1] + float(kSun[1]) * sExit;
        const int yTop = std::min(int(std::floor(std::min(y1, float(top - 1)))),
                                  int(kWorldH) - 1);
        for (int y = std::max(int(std::floor(y0)), 0); y <= yTop; ++y) {
          if (v.solidAt(colX, y, colZ)) {
            return 0.0f;
          }
        }
      }
    }
    t = sExit;
    const bool takeX = tMaxX < tMaxZ;
    tMaxX += takeX ? dX : 0.0f;
    tMaxZ += takeX ? 0.0f : dZ;
    colX += takeX ? stepX : 0;
    colZ += takeX ? 0 : stepZ;
  }
  return visibility;
}

// Sphere trace that hands off where the ray leaves the box INSET by `margin`
// voxels on every side (margin 0 = the pass-40 hand-off at the box edge).
bool traceFieldInset(const vv::voxel::SdfField& sdf, const float o[3],
                     const float d[3], float margin, float outExit[3],
                     float* outT, float* outVisibility) {
  // The truncation is lateral: the box's Y faces are the world floor and sky.
  const float lo[3] = {margin, 0.0f, margin};
  const float hi[3] = {float(sdf.nx()) - margin, float(sdf.ny()),
                       float(sdf.nz()) - margin};
  float tExit = 0.0f;
  bool inside = true;
  for (int a = 0; a < 3; ++a) {
    if (o[a] < lo[a] || o[a] >= hi[a]) {
      inside = false;
    }
  }
  if (inside) {
    tExit = 1e30f;
    for (int a = 0; a < 3; ++a) {
      if (std::abs(d[a]) > 1e-6f) {
        const float face = d[a] > 0.0f ? hi[a] : lo[a];
        tExit = std::min(tExit, (face - o[a]) / d[a]);
      }
    }
  }
  float visibility = 1.0f, t = 0.0f;
  for (int i = 0; i < 160; ++i) {
    if (t >= tExit) {
      for (int a = 0; a < 3; ++a) {
        outExit[a] = o[a] + d[a] * tExit;
      }
      *outT = tExit;
      *outVisibility = visibility;
      return true;
    }
    const float px = o[0] + d[0] * t, py = o[1] + d[1] * t,
                pz = o[2] + d[2] * t;
    const float h = sdf.sample(px, py, pz);
    if (h < 1e-3f) {
      *outVisibility = 0.0f;
      return false;
    }
    visibility = std::min(visibility, penumbra(h, t));
    t += std::max(h * 0.7f, 0.05f);
  }
  for (int a = 0; a < 3; ++a) {
    outExit[a] = o[a] + d[a] * t;
  }
  *outT = t;
  *outVisibility = visibility;
  return true;
}

// Fixed-length field trace (candidate pass 42): the SDF term is the sphere
// trace over AT MOST `len` voxels of ray from the shaded point. Points whose
// segment leaves the lattice sooner fall back to the pure 2.5D march, which
// is placement-free. The remaining ray (from the fixed point p + len*sun) is
// handed to the 2.5D march - also placement-free, because the hand-off point
// is a function of the shaded point alone, not of the box.
float sdfPathFixedLen(const vv::voxel::SdfField& sdf,
                      const std::int32_t boxOrigin[3], const View& v,
                      const double p[3], const double n[3], float len,
                      float halo) {
  const float o[3] = {float(p[0] - double(boxOrigin[0]) + halo),
                      float(p[1] - double(boxOrigin[1])),
                      float(p[2] - double(boxOrigin[2]) + halo)};
  (void)n;
  const float d[3] = {float(kSun[0]), float(kSun[1]), float(kSun[2])};
  // Where does the ray leave the lattice? (Y faces are the world floor/sky.)
  const float lo[3] = {0.0f, 0.0f, 0.0f};
  const float hi[3] = {float(sdf.nx()), float(sdf.ny()), float(sdf.nz())};
  float tExit = 1e30f;
  for (int a = 0; a < 3; ++a) {
    if (o[a] < lo[a] || o[a] >= hi[a]) {
      tExit = 0.0f;
      continue;
    }
    if (std::abs(d[a]) > 1e-6f) {
      const float face = d[a] > 0.0f ? hi[a] : lo[a];
      tExit = std::min(tExit, (face - o[a]) / d[a]);
    }
  }
  const float tEnd = std::min(len, tExit);
  float visibility = 1.0f, t = 0.0f;
  bool blocked = false;
  for (int i = 0; i < 160; ++i) {
    if (t >= tEnd) {
      break;
    }
    const float h = sdf.sample(o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t);
    if (h < 1e-3f) {
      visibility = 0.0f;
      blocked = true;
      break;
    }
    visibility = std::min(visibility, penumbra(h, t));
    t += std::max(h * 0.7f, 0.05f);
  }
  if (blocked) {
    return 0.0f;
  }
  if (tEnd < len) {
    // The segment left the lattice: the field has nothing more to say, so
    // answer with the placement-free 2.5D march from the shaded point.
    const float start[3] = {float(p[0]), float(p[1]), float(p[2])};
    return march2d(v, start, 0.0f, 1.0f);
  }
  const float tHand = tEnd;
  const float world[3] = {float(p[0]) + d[0] * tHand, float(p[1]) + d[1] * tHand,
                          float(p[2]) + d[2] * tHand};
  return march2d(v, world, tHand, visibility);
}

// The renderer's SDF path for one point: sphere trace in the field, hand the
// ray to the 2.5D march when it leaves the box (pass 40).
float sdfPath(const vv::voxel::SdfField& sdf, const std::int32_t boxOrigin[3],
              const View& v, const double p[3], const double n[3],
              float margin = 0.0f, float halo = 0.0f) {
  const float o[3] = {float(p[0] - double(boxOrigin[0]) + halo + kSun[0] * 1e-2),
                      float(p[1] - double(boxOrigin[1]) + kSun[1] * 1e-2),
                      float(p[2] - double(boxOrigin[2]) + halo + kSun[2] * 1e-2)};
  (void)n;
  const float d[3] = {float(kSun[0]), float(kSun[1]), float(kSun[2])};
  float exit[3] = {0, 0, 0}, exitT = 0.0f, vis = 1.0f;
  if (!traceFieldInset(sdf, o, d, margin, exit, &exitT, &vis)) {
    return vis;
  }
  const float world[3] = {exit[0] - halo + float(boxOrigin[0]),
                          exit[1] + float(boxOrigin[1]),
                          exit[2] - halo + float(boxOrigin[2])};
  return march2d(v, world, exitT, vis);
}

}  // namespace

int main() {
  vv::terrain::TerrainConfig tcfg;
  vv::voxel::World world(tcfg, kChunkX, kWorldH, kChunkZ);
  std::vector<const vv::voxel::Chunk*> added;
  std::vector<vv::voxel::ChunkCoord> evicted;
  world.ensureRegion(0, 0, kRegionRadius, added, evicted);
  std::printf("region: %zu chunks, radius %d\n", added.size(), kRegionRadius);

  // The two fields a crossing swaps between: centered on chunk 0 and chunk 1.
  const vv::voxel::SdfBoxGeometry boxA = vv::voxel::SdfBoxGeometry::centeredOn(
      0, 0, kHalf, kChunkX, kChunkZ, kWorldH);
  const vv::voxel::SdfBoxGeometry boxB = vv::voxel::SdfBoxGeometry::centeredOn(
      1, 0, kHalf, kChunkX, kChunkZ, kWorldH);
  const auto snapshots = [&](const vv::voxel::SdfBoxGeometry& box,
                             std::int32_t centerChunkX,
                             std::int32_t centerChunkZ) {
    std::vector<std::vector<std::uint8_t>> snaps(
        std::size_t(box.chunksPerSide) * box.chunksPerSide);
    const std::int32_t first = -std::int32_t(box.chunksPerSide / 2u);
    for (std::uint32_t cz = 0; cz < box.chunksPerSide; ++cz) {
      for (std::uint32_t cx = 0; cx < box.chunksPerSide; ++cx) {
        if (const vv::voxel::Chunk* c = world.findChunk(vv::voxel::ChunkCoord{
                centerChunkX + first + std::int32_t(cx),
                centerChunkZ + first + std::int32_t(cz)})) {
          snaps[std::size_t(cz) * box.chunksPerSide + cx] = c->voxelTypes();
        }
      }
    }
    return snaps;
  };
  vv::voxel::SdfField fieldA, fieldB;
  vv::voxel::buildSdfBoxField(boxA, snapshots(boxA, 0, 0), fieldA);
  vv::voxel::buildSdfBoxField(boxB, snapshots(boxB, 1, 0), fieldB);
  const std::int32_t originA[3] = {boxA.originX, boxA.originY, boxA.originZ};
  const std::int32_t originB[3] = {boxB.originX, boxB.originY, boxB.originZ};

  View view{&world, 0};

  // Columns that are visible with the camera one chunk past the crossing: the
  // box spans [c-3, c+2] chunks, so only columns the field actually covers are
  // judged (the renderer's shadows are only field-driven there).
  const int x0 = -3 * int(kChunkX) + 4, x1 = 3 * int(kChunkX) - 4;  // trusted box
  int samples = 0, changed5 = 0, changed20 = 0, onlyA = 0, onlyB = 0;
  double sumAbs = 0.0, maxAbs = 0.0;
  int worstX = 0, worstZ = 0;
  for (int x = x0; x < x1; x += 2) {
    for (int z = -3 * int(kChunkZ) + 4; z < 3 * int(kChunkZ) - 4; z += 2) {
      const int top = view.topAt(x, z);
      if (top <= 0) {
        continue;
      }
      const double p[3] = {double(x) + 0.5, double(top), double(z) + 0.5};
      const double n[3] = {0.0, 1.0, 0.0};
      const float a = sdfPath(fieldA, originA, view, p, n);
      const float b = sdfPath(fieldB, originB, view, p, n);
      const double d = std::abs(double(a) - double(b));
      ++samples;
      sumAbs += d;
      if (d > maxAbs) {
        maxAbs = d;
        worstX = x;
        worstZ = z;
      }
      if (d > 0.05) ++changed5;
      if (d > 0.20) ++changed20;
      if (a > 0.5f && b <= 0.5f) ++onlyA;  // lit before the swap, dark after
      if (b > 0.5f && a <= 0.5f) ++onlyB;
    }
  }
  // Bucket the disagreement by how deep inside the field's box the column
  // sits: chamfer EDT truncation at the box faces is the prime suspect.
  int bucketN[5] = {0, 0, 0, 0, 0}, bucketCh[5] = {0, 0, 0, 0, 0};
  for (int x = x0; x < x1; x += 2) {
    for (int z = -3 * int(kChunkZ) + 1; z < 3 * int(kChunkZ); z += 2) {
      const int top = view.topAt(x, z);
      if (top <= 0) {
        continue;
      }
      // Depth inside box B, in chunks (box B spans chunks [-2, 3] on X/Z).
      const int cx = x / int(kChunkX), cz = z / int(kChunkZ);
      const int depth = std::min(std::min(cx - (-2), 3 - cx),
                                 std::min(cz - (-2), 3 - cz));
      const int b = std::clamp(depth, 0, 2);
      const double p[3] = {double(x) + 0.5, double(top), double(z) + 0.5};
      const double n[3] = {0.0, 1.0, 0.0};
      const float a = sdfPath(fieldA, originA, view, p, n);
      const float bf = sdfPath(fieldB, originB, view, p, n);
      const double d = std::abs(double(a) - double(bf));
      bucketN[b] += 1;
      if (d > 0.05) {
        bucketCh[b] += 1;
      }
    }
  }
  std::printf("disagreement by depth inside the box (0 = edge chunk, 2 = deep):\n");
  for (int b = 0; b < 3; ++b) {
    std::printf("  depth %d: %d/%d changed (%.1f%%)\n", b, bucketCh[b],
                bucketN[b], 100.0 * bucketCh[b] / std::max(1, bucketN[b]));
  }
  std::printf("rebuild swap (same world, field center chunk 0 -> 1):\n");
  std::printf("  %d ground columns; visibility changed > 0.05: %d (%.1f%%), "
              "> 0.20: %d (%.1f%%)\n",
              samples, changed5, 100.0 * changed5 / samples, changed20,
              100.0 * changed20 / samples);
  std::printf("  mean |delta| %.4f, max |delta| %.3f at column (%d,%d)\n",
              sumAbs / samples, maxAbs, worstX, worstZ);
  std::printf("  lit->dark across the swap: %d, dark->lit: %d\n", onlyA, onlyB);

  // How far the two fields disagree at all, as an SDF: the max |distance|
  // difference over the cells both boxes cover - the raw material of the
  // truncation error at the box edge.
  double maxDistDiff = 0.0;
  const int nxA = fieldA.nx(), nyA = fieldA.ny(), nzA = fieldA.nz();
  const int oxA = boxA.originX - boxB.originX;
  const int ozA = boxA.originZ - boxB.originZ;
  long long compared = 0;
  for (int z = 0; z < nzA; ++z) {
    for (int y = 0; y < nyA; y += 2) {
      for (int x = 0; x < nxA; ++x) {
        const int bx = x + oxA, bz = z + ozA;
        if (bx < 0 || bx >= fieldB.nx() || bz < 0 || bz >= fieldB.nz()) {
          continue;
        }
        const float da = fieldA.sample(float(x) + 0.5f, float(y) + 0.5f,
                                       float(z) + 0.5f);
        const float db = fieldB.sample(float(bx) + 0.5f, float(y) + 0.5f,
                                       float(bz) + 0.5f);
        maxDistDiff = std::max(maxDistDiff, double(std::abs(da - db)));
        ++compared;
      }
    }
  }
  std::printf("  field distance disagreement over %lld shared cells: max %.3f "
              "voxels\n",
              compared, maxDistDiff);

  // Pass 41 arm: the renderer's build - a field over the trusted box grown by
  // one chunk on each lateral side, shaded only inside the trusted box.
  {
    const auto snapshotsHalo = [&](const vv::voxel::SdfBoxGeometry& box,
                                   std::int32_t centerChunkX,
                                   std::int32_t centerChunkZ) {
      std::vector<std::vector<std::uint8_t>> snaps(
          std::size_t(box.chunksPerSide) * box.chunksPerSide);
      const std::int32_t first = -std::int32_t(box.chunksPerSide / 2u);
      for (std::uint32_t cz = 0; cz < box.chunksPerSide; ++cz) {
        for (std::uint32_t ccx = 0; ccx < box.chunksPerSide; ++ccx) {
          if (const vv::voxel::Chunk* c = world.findChunk(vv::voxel::ChunkCoord{
                  centerChunkX + first + std::int32_t(ccx),
                  centerChunkZ + first + std::int32_t(cz)})) {
            snaps[std::size_t(cz) * box.chunksPerSide + ccx] = c->voxelTypes();
          }
        }
      }
      return snaps;
    };
    const float haloVox = float(kChunkX);
    const vv::voxel::SdfBoxGeometry trustedA =
        vv::voxel::SdfBoxGeometry::centeredOn(0, 0, kHalf, kChunkX, kChunkZ,
                                              kWorldH);
    const vv::voxel::SdfBoxGeometry trustedB =
        vv::voxel::SdfBoxGeometry::centeredOn(1, 0, kHalf, kChunkX, kChunkZ,
                                              kWorldH);
    const vv::voxel::SdfBoxGeometry buildA = trustedA.haloed(1);
    const vv::voxel::SdfBoxGeometry buildB = trustedB.haloed(1);
    vv::voxel::SdfField haloA, haloB;
    vv::voxel::buildSdfBoxField(buildA, snapshotsHalo(buildA, 0, 0), haloA);
    vv::voxel::buildSdfBoxField(buildB, snapshotsHalo(buildB, 1, 0), haloB);

    int n = 0, ch5 = 0, ch20 = 0, leaks = 0, overDark = 0;
    double sum = 0.0, maxD = 0.0;
    int bn[4] = {0, 0, 0, 0}, bh[4] = {0, 0, 0, 0};   // halo build, by depth
    int pn[4] = {0, 0, 0, 0}, ph[4] = {0, 0, 0, 0};   // plain build, by depth
    for (int x = -3 * int(kChunkX) + 4; x < 3 * int(kChunkX) - 4; x += 2) {
      for (int z = -3 * int(kChunkZ) + 4; z < 3 * int(kChunkZ) - 4; z += 2) {
        const int top = view.topAt(x, z);
        if (top <= 0) {
          continue;
        }
        const double p[3] = {double(x) + 0.5, double(top), double(z) + 0.5};
        const double nn[3] = {0.0, 1.0, 0.0};
        const float a2 =
            sdfPath(haloA, originA, view, p, nn, haloVox, haloVox);
        const float b2 =
            sdfPath(haloB, originB, view, p, nn, haloVox, haloVox);
        // Depth buckets: chunk 0 = the trusted box's outermost chunk, etc.
        const int edgeT = std::min(3 * int(kChunkX) - std::abs(x),
                                  3 * int(kChunkZ) - std::abs(z));
        const int bucket = std::min(edgeT / int(kChunkX), 3);
        const float pa2 = sdfPath(fieldA, originA, view, p, nn);
        const float pb2 = sdfPath(fieldB, originB, view, p, nn);
        ++pn[bucket];
        if (std::abs(double(pa2) - double(pb2)) > 0.05) ++ph[bucket];
        ++bn[bucket];
        if (std::abs(double(a2) - double(b2)) > 0.05) ++bh[bucket];
        const float o3[3] = {float(p[0]) + float(kSun[0]) * 1e-2f,
                             float(p[1]) + float(kSun[1]) * 1e-2f,
                             float(p[2]) + float(kSun[2]) * 1e-2f};
        const bool exactlyDark = march2d(view, o3, 0.0f, 1.0f) == 0.0f;
        if (!exactlyDark && b2 < 0.5f) ++overDark;
        if (exactlyDark && b2 > 0.5f) ++leaks;
        const double d = std::abs(double(a2) - double(b2));
        ++n;
        sum += d;
        maxD = std::max(maxD, d);
        if (d > 0.05) ++ch5;
        if (d > 0.20) ++ch20;
      }
    }
    std::printf("PASS-41 halo build (trusted 6x6 + 1 chunk halo): swap >0.05 "
                "%d/%d (%.1f%%), >0.20 %d (%.1f%%), mean |d| %.4f, max %.3f | "
                "leaks %d, over-dark %d\n",
                ch5, n, 100.0 * ch5 / n, ch20, 100.0 * ch20 / n, sum / n, maxD,
                leaks, overDark);
    for (int b = 0; b < 4; ++b) {
      if (bn[b] == 0) {
        continue;
      }
      std::printf("  depth %d chunk(s) in from the trusted edge: halo %d/%d "
                  "(%.1f%%) | plain %d/%d (%.1f%%)\n",
                  b, bh[b], bn[b], 100.0 * bh[b] / bn[b], ph[b], pn[b],
                  100.0 * ph[b] / pn[b]);
    }
  }

  // Arm 5: fixed-length field trace on the haloed build.
  {
    const auto snapshotsHalo2 = [&](const vv::voxel::SdfBoxGeometry& box,
                                    std::int32_t centerChunkX,
                                    std::int32_t centerChunkZ) {
      std::vector<std::vector<std::uint8_t>> snaps(
          std::size_t(box.chunksPerSide) * box.chunksPerSide);
      const std::int32_t first = -std::int32_t(box.chunksPerSide / 2u);
      for (std::uint32_t cz = 0; cz < box.chunksPerSide; ++cz) {
        for (std::uint32_t ccx = 0; ccx < box.chunksPerSide; ++ccx) {
          if (const vv::voxel::Chunk* c = world.findChunk(vv::voxel::ChunkCoord{
                  centerChunkX + first + std::int32_t(ccx),
                  centerChunkZ + first + std::int32_t(cz)})) {
            snaps[std::size_t(cz) * box.chunksPerSide + ccx] = c->voxelTypes();
          }
        }
      }
      return snaps;
    };
    const float haloVox = float(kChunkX);
    const vv::voxel::SdfBoxGeometry buildA =
        vv::voxel::SdfBoxGeometry::centeredOn(0, 0, kHalf, kChunkX, kChunkZ,
                                              kWorldH).haloed(1);
    const vv::voxel::SdfBoxGeometry buildB =
        vv::voxel::SdfBoxGeometry::centeredOn(1, 0, kHalf, kChunkX, kChunkZ,
                                              kWorldH).haloed(1);
    vv::voxel::SdfField hfA, hfB;
    vv::voxel::buildSdfBoxField(buildA, snapshotsHalo2(buildA, 0, 0), hfA);
    vv::voxel::buildSdfBoxField(buildB, snapshotsHalo2(buildB, 1, 0), hfB);
    // The published box origin is the TRUSTED origin; the lattice offset is
    // the halo, exactly like the shader's box + trust.w.
    const std::int32_t tA[3] = {buildA.originX + int(haloVox),
                                buildA.originY,
                                buildA.originZ + int(haloVox)};
    const std::int32_t tB[3] = {buildB.originX + int(haloVox),
                                buildB.originY,
                                buildB.originZ + int(haloVox)};
    const float lens[4] = {32.0f, 48.0f, 64.0f, 96.0f};
    for (float len : lens) {
      int n = 0, ch5 = 0;
      for (int x = -3 * int(kChunkX) + 4; x < 3 * int(kChunkX) - 4; x += 2) {
        for (int z = -3 * int(kChunkZ) + 4; z < 3 * int(kChunkZ) - 4; z += 2) {
          const int top = view.topAt(x, z);
          if (top <= 0) {
            continue;
          }
          const double p[3] = {double(x) + 0.5, double(top), double(z) + 0.5};
          const double nn[3] = {0.0, 1.0, 0.0};
          const float a = sdfPathFixedLen(hfA, tA, view, p, nn, len, haloVox);
          const float b = sdfPathFixedLen(hfB, tB, view, p, nn, len, haloVox);
          ++n;
          if (std::abs(double(a) - double(b)) > 0.05) {
            ++ch5;
          }
        }
      }
      std::printf("PASS-42 fixed trace len %.0f vox: swap >0.05 %d/%d (%.1f%%)\n",
                  len, ch5, n, 100.0 * ch5 / n);
    }
  }

  // Arm 6/7: with the fixed-length trace, what is LEFT? The flip band is now
  // the ring where the box's coverage changes and the shader switches to the
  // plain 2.5D march (the two models disagree by a lot there). Arm 6 shades
  // the whole lattice (halo included), arm 7 only the trusted box.
  {
    const auto snapshotsHalo3 = [&](const vv::voxel::SdfBoxGeometry& box,
                                    std::int32_t centerChunkX,
                                    std::int32_t centerChunkZ) {
      std::vector<std::vector<std::uint8_t>> snaps(
          std::size_t(box.chunksPerSide) * box.chunksPerSide);
      const std::int32_t first = -std::int32_t(box.chunksPerSide / 2u);
      for (std::uint32_t cz = 0; cz < box.chunksPerSide; ++cz) {
        for (std::uint32_t ccx = 0; ccx < box.chunksPerSide; ++ccx) {
          if (const vv::voxel::Chunk* c = world.findChunk(vv::voxel::ChunkCoord{
                  centerChunkX + first + std::int32_t(ccx),
                  centerChunkZ + first + std::int32_t(cz)})) {
            snaps[std::size_t(cz) * box.chunksPerSide + ccx] = c->voxelTypes();
          }
        }
      }
      return snaps;
    };
    const float haloVox = float(kChunkX);
    const vv::voxel::SdfBoxGeometry trustedA =
        vv::voxel::SdfBoxGeometry::centeredOn(0, 0, kHalf, kChunkX, kChunkZ,
                                              kWorldH);
    const vv::voxel::SdfBoxGeometry trustedB =
        vv::voxel::SdfBoxGeometry::centeredOn(1, 0, kHalf, kChunkX, kChunkZ,
                                              kWorldH);
    const vv::voxel::SdfBoxGeometry latA = trustedA.haloed(1);
    const vv::voxel::SdfBoxGeometry latB = trustedB.haloed(1);
    vv::voxel::SdfField fA, fB;
    vv::voxel::buildSdfBoxField(latA, snapshotsHalo3(latA, 0, 0), fA);
    vv::voxel::buildSdfBoxField(latB, snapshotsHalo3(latB, 1, 0), fB);
    const std::int32_t oA[3] = {latA.originX, latA.originY, latA.originZ};
    const std::int32_t oB[3] = {latB.originX, latB.originY, latB.originZ};
    const auto insideTrusted = [](const vv::voxel::SdfBoxGeometry& b,
                                  int x, int z) {
      return x >= b.originX && x < b.originX + int(b.nx) && z >= b.originZ &&
             z < b.originZ + int(b.nz);
    };
    const float len = 64.0f;
    for (int arm = 6; arm <= 7; ++arm) {
      const bool shadeLattice = (arm == 6);
      int n = 0, ch5 = 0, ringN = 0, ring5 = 0;
      for (int x = -256 + 4; x < 256 - 4; x += 2) {
        for (int z = -256 + 4; z < 256 - 4; z += 2) {
          const int top = view.topAt(x, z);
          if (top <= 0) {
            continue;
          }
          const double p[3] = {double(x) + 0.5, double(top), double(z) + 0.5};
          const double nn[3] = {0.0, 1.0, 0.0};
          const bool aUses = shadeLattice || insideTrusted(trustedA, x, z);
          const bool bUses = shadeLattice || insideTrusted(trustedB, x, z);
          const float start[3] = {float(p[0]), float(p[1]), float(p[2])};
          // Outside the box/mode's reach the shader runs the plain march.
          const float a = aUses ? sdfPathFixedLen(fA, oA, view, p, nn, len, 0.0f)
                                : march2d(view, start, 0.0f, 1.0f);
          const float b = bUses ? sdfPathFixedLen(fB, oB, view, p, nn, len, 0.0f)
                                : march2d(view, start, 0.0f, 1.0f);
          const bool changed = std::abs(double(a) - double(b)) > 0.05;
          // Only the ring where the box coverage differs can flip.
          const bool inRing = (aUses != bUses);
          ++n;
          if (changed) ++ch5;
          if (inRing) {
            ++ringN;
            if (changed) ++ring5;
          }
        }
      }
      std::printf("PASS-41b arm %d (%s): swap >0.05 %d/%d (%.2f%%); coverage-"
                  "change ring %d/%d (%.1f%%)\n",
                  arm, shadeLattice ? "shade the lattice" : "shade the trusted",
                  ch5, n, 100.0 * ch5 / n, ring5, ringN,
                  ringN ? 100.0 * ring5 / ringN : 0.0);
    }
  }

  // Arm 8: TODAY'S box (6x6, no halo) + the fixed-length trace. If this
  // already kills the flicker, the halo is not needed for it.
  {
    const auto snaps6 = [&](const vv::voxel::SdfBoxGeometry& box,
                            std::int32_t centerChunkX,
                            std::int32_t centerChunkZ) {
      std::vector<std::vector<std::uint8_t>> snaps(
          std::size_t(box.chunksPerSide) * box.chunksPerSide);
      const std::int32_t first = -std::int32_t(box.chunksPerSide / 2u);
      for (std::uint32_t cz = 0; cz < box.chunksPerSide; ++cz) {
        for (std::uint32_t ccx = 0; ccx < box.chunksPerSide; ++ccx) {
          if (const vv::voxel::Chunk* c = world.findChunk(vv::voxel::ChunkCoord{
                  centerChunkX + first + std::int32_t(ccx),
                  centerChunkZ + first + std::int32_t(cz)})) {
            snaps[std::size_t(cz) * box.chunksPerSide + ccx] = c->voxelTypes();
          }
        }
      }
      return snaps;
    };
    const vv::voxel::SdfBoxGeometry bA =
        vv::voxel::SdfBoxGeometry::centeredOn(0, 0, kHalf, kChunkX, kChunkZ,
                                              kWorldH);
    const vv::voxel::SdfBoxGeometry bB =
        vv::voxel::SdfBoxGeometry::centeredOn(1, 0, kHalf, kChunkX, kChunkZ,
                                              kWorldH);
    vv::voxel::SdfField gA, gB;
    vv::voxel::buildSdfBoxField(bA, snaps6(bA, 0, 0), gA);
    vv::voxel::buildSdfBoxField(bB, snaps6(bB, 1, 0), gB);
    const std::int32_t qA[3] = {bA.originX, bA.originY, bA.originZ};
    const std::int32_t qB[3] = {bB.originX, bB.originY, bB.originZ};
    for (float len : {32.0f, 48.0f, 64.0f}) {
      int n = 0, ch5 = 0, ringN = 0, ring5 = 0;
      for (int x = -3 * int(kChunkX) + 2; x < 3 * int(kChunkX) - 2; x += 2) {
        for (int z = -3 * int(kChunkZ) + 2; z < 3 * int(kChunkZ) - 2; z += 2) {
          const int top = view.topAt(x, z);
          if (top <= 0) {
            continue;
          }
          const double p[3] = {double(x) + 0.5, double(top), double(z) + 0.5};
          const double nn[3] = {0.0, 1.0, 0.0};
          // Both placements shade their own box; a point outside one of them
          // runs the plain march there (that is the coverage flip).
          const bool inA = x >= bA.originX && x < bA.originX + int(bA.nx) &&
                           z >= bA.originZ && z < bA.originZ + int(bA.nz);
          const bool inB = x >= bB.originX && x < bB.originX + int(bB.nx) &&
                           z >= bB.originZ && z < bB.originZ + int(bB.nz);
          const float start[3] = {float(p[0]), float(p[1]), float(p[2])};
          const float a = inA ? sdfPathFixedLen(gA, qA, view, p, nn, len, 0.0f)
                              : march2d(view, start, 0.0f, 1.0f);
          const float b = inB ? sdfPathFixedLen(gB, qB, view, p, nn, len, 0.0f)
                              : march2d(view, start, 0.0f, 1.0f);
          const bool changed = std::abs(double(a) - double(b)) > 0.05;
          ++n;
          if (changed) ++ch5;
          if (inA != inB) {
            ++ringN;
            if (changed) ++ring5;
          }
        }
      }
      std::printf("PASS-41c 6x6 box + fixed trace len %.0f: swap >0.05 %d/%d "
                  "(%.2f%%); coverage ring %d/%d (%.1f%%)\n",
                  len, ch5, n, 100.0 * ch5 / n, ring5, ringN,
                  ringN ? 100.0 * ring5 / ringN : 0.0);
    }
  }

  // Sweep the inset: how small can the trusted region be while a rebuild
  // stops changing pixels, and how much does the trusted region's shading
  // differ from the plain 2.5D march (the look change the inset costs)?
  const float margins[5] = {0.0f, 16.0f, 32.0f, 48.0f, 64.0f};
  for (float m : margins) {
    int count = 0, ch5 = 0, ch20 = 0, chModel5 = 0, leaks = 0, overDark = 0;
    int insideBoth = 0, insideChanged5 = 0;
    double sum = 0.0, sumModel = 0.0;
    for (int x = x0; x < x1; x += 4) {
      for (int z = -3 * int(kChunkZ) + 1; z < 3 * int(kChunkZ); z += 4) {
        const int top = view.topAt(x, z);
        if (top <= 0) {
          continue;
        }
        const double p[3] = {double(x) + 0.5, double(top), double(z) + 0.5};
        const double n[3] = {0.0, 1.0, 0.0};
        const float a = sdfPath(fieldA, originA, view, p, n, m);
        const float b = sdfPath(fieldB, originB, view, p, n, m);
        const float o3[3] = {float(p[0]) + float(kSun[0]) * 1e-2f,
                             float(p[1]) + float(kSun[1]) * 1e-2f,
                             float(p[2]) + float(kSun[2]) * 1e-2f};
        const float m2 = march2d(view, o3, 0.0f, 1.0f);
        const double d = std::abs(double(a) - double(b));
        const double dm = std::abs(double(b) - double(m2));
        ++count;
        sum += d;
        sumModel += dm;
        if (d > 0.05) ++ch5;
        if (d > 0.20) ++ch20;
        if (dm > 0.05) ++chModel5;
        // Shadow correctness against the exact march (0 = blocked).
        const bool exactlyDark = march2d(view, o3, 0.0f, 1.0f) == 0.0f;
        if (!exactlyDark && b < 0.5f) ++overDark;
        if (exactlyDark && b > 0.5f) ++leaks;
        // Is this column field-shaded in BOTH fields (the only place a rebuild
        // can change the picture)? The boxes span [c-3, c+2] chunks on x/z.
        const auto trusted = [&](int base) {
          const float lx = float(x) - float(base - 3) * float(kChunkX);
          const float lz = float(z) - float(-3) * float(kChunkZ);
          return lx >= m && lx < float(6 * int(kChunkX)) - m && lz >= m &&
                 lz < float(6 * int(kChunkZ)) - m;
        };
        if (trusted(0) && trusted(1)) {
          ++insideBoth;
          if (d > 0.05) ++insideChanged5;
        }
      }
    }
    std::printf("margin %2.0f vox (x/z): swap >0.05 %d/%d (%.1f%%), >0.20 %d "
                "(%.1f%%), mean |d| %.4f | field vs 2.5D: mean %.4f (>0.05 %d) | "
                "leaks %d, over-dark %d | inside BOTH boxes: %d cols, changed "
                "%d (%.2f%%)\n",
                double(m), ch5, count, 100.0 * ch5 / count, ch20,
                100.0 * ch20 / count, sum / count, sumModel / count, chModel5,
                leaks, overDark, insideBoth, insideChanged5,
                insideBoth ? 100.0 * insideChanged5 / insideBoth : 0.0);
  }
  return 0;
}
