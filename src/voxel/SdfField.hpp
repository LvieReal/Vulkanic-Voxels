// 3D voxel signed distance field + sphere-traced soft shadow.
//
// This is the experimental "proper" 3D voxel SDF shadow (pass 37+), a
// replacement for the 2.5D top-plane penumbra that cannot soften the shadow
// edges running along steep / vertical casters. Technique (not reinvented -
// the standard real-time voxel-SDF soft-shadow recipe, cf. the jump-flooding /
// EDT SDF builders and iquilezles.org/articles/rmshadows/):
//
//   1. Build a 3D SDF: the distance from every point to the nearest SOLID
//      CUBE (the union of the solid voxels), from the solid/air voxel grid.
//      A two-pass chamfer distance transform gives, per cell, the nearest
//      solid cell (the seed); the SDF at a point is the exact L2 distance to
//      the nearest solid CUBE (0 inside the solid, >0 in air).
//   2. Sphere-trace the sun ray through that SDF, folding k*h/t into the min
//      at each sample (the iquilezles soft-shadow estimate).
//
// Because h is now the true 3D distance to the solid surface - vertical faces
// and overhangs included, not just a column's top plane - EVERY shadow edge
// (top, side, vertical) gets the same continuous penumbra.
//
// This file is the CPU reference implementation (no windowing, no Vulkan): the GPU
// builds the same field and traces the same march (pass 38) and the CPU
// sphere-traced shadow must agree with it (pinned by the parity test in
// tests/terrain_world_tests.cpp).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vv::voxel {

// The per-cell argmin seed encoding the shader reads: 0xFFFFFFFF = no solid
// in view. Pass 49: the field stores the seeds in EXACTLY this encoding, so
// the array the worker builds IS the array that gets uploaded (binding 12) -
// the old int32 build + separate u32 pack pass cost ~10 ms of worker time and
// a full 19 MB read/write traversal per bake for nothing.
inline constexpr std::uint32_t kSdfEmptySeed = 0xFFFFFFFFu;

// The three spans of a box must fit the packed seed together, and the packing
// must stay below 2^31 so an encoded cell can never collide with the empty
// sentinel (see below).
inline constexpr int kMaxSeedBits = 31;

class SdfField {
public:
    // Pass 50: the seed is THREE BIT FIELDS - x | (y << bits.x) |
    // (z << (bits.x + bits.y)) - so a fetch decodes with two shifts and two
    // masks instead of three integer divisions. Nsight put those divisions at
    // the top of the frame's cost (up to 13% of the frame in one pass), and
    // sampleSdf3d runs the decode up to 27 times per sphere-trace step, ~22
    // steps per traced ray. The CELL the seed names is unchanged, so the
    // field, the march and the picture are identical.
    struct SeedBits final {
        int x = 0;
        int y = 0;
        int z = 0;
        bool fits() const { return x + y + z <= kMaxSeedBits; }
    };

    // The bits needed to hold the coordinates 0..n-1 (0 for a 1-cell span).
    static int bitsForDim(int n) {
        int b = 0;
        while ((1 << b) < n) {
            ++b;
        }
        return b;
    }

    static SeedBits seedBitsFor(int nx, int ny, int nz) {
        return SeedBits{bitsForDim(nx), bitsForDim(ny), bitsForDim(nz)};
    }

    SeedBits seedBits() const { return bits_; }
    // Build the SDF over the box [0,nx) x [0,ny) x [0,nz) from a solid/air
    // predicate. solid(x,y,z) is true for a solid voxel. sample() is the
    // distance to the nearest solid cube (0 inside the solid).
    void build(int nx, int ny, int nz, const auto& solid) {
        nx_ = nx;
        ny_ = ny;
        nz_ = nz;
        const std::size_t n =
            static_cast<std::size_t>(nx) * ny * nz;
        dist_.assign(n, kInf);
        seed_.assign(n, kSdfEmptySeed);
        bits_ = seedBitsFor(nx, ny, nz);
        // A box whose spans do not fit the packing cannot be encoded. The
        // field then holds no seeds at all, which reads as "open space
        // everywhere" (no SDF shadows) rather than as a WRONG field; the
        // renderer refuses such a build up front, so this is belt and braces.
        const bool encodable = bits_.fits();
        for (int z = 0; z < nz; ++z)
            for (int y = 0; y < ny; ++y)
                for (int x = 0; x < nx; ++x) {
                    if (solid(x, y, z)) {
                        dist_[I(x, y, z)] = 0.0f;
                        if (encodable) {
                            seed_[I(x, y, z)] = encodeSeed(x, y, z);
                        }
                    }
                }
        // Two-pass chamfer relaxation tracking the nearest seed: W1 = 1
        // (face), W2 = sqrt(2) (edge), W3 = sqrt(3) (corner). Each pass
        // relaxes from one diagonal corner; two passes cover all 26-neighbor
        // directions.
        const float w1 = 1.0f;
        const float w2 = 1.41421356f;
        const float w3 = 1.73205081f;
        const auto relax = [this](int x, int y, int z, int ox, int oy, int oz,
                                  float w) {
            const int jx = x + ox, jy = y + oy, jz = z + oz;
            if (jx < 0 || jx >= nx_ || jy < 0 || jy >= ny_ || jz < 0 ||
                jz >= nz_) {
                return;
            }
            const std::size_t i = I(x, y, z);
            const std::size_t j = I(jx, jy, jz);
            const float nd = dist_[j] + w;
            if (nd < dist_[i]) {
                dist_[i] = nd;
                seed_[i] = seed_[j];
            }
        };
        // Forward pass (ascending).
        for (int z = 0; z < nz; ++z)
            for (int y = 0; y < ny; ++y)
                for (int x = 0; x < nx; ++x) {
                    relax(x, y, z, -1, 0, 0, w1);
                    relax(x, y, z, 0, -1, 0, w1);
                    relax(x, y, z, 0, 0, -1, w1);
                    relax(x, y, z, -1, -1, 0, w2);
                    relax(x, y, z, -1, 0, -1, w2);
                    relax(x, y, z, 0, -1, -1, w2);
                    relax(x, y, z, -1, -1, -1, w3);
                }
        // Backward pass (descending).
        for (int z = nz - 1; z >= 0; --z)
            for (int y = ny - 1; y >= 0; --y)
                for (int x = nx - 1; x >= 0; --x) {
                    relax(x, y, z, 1, 0, 0, w1);
                    relax(x, y, z, 0, 1, 0, w1);
                    relax(x, y, z, 0, 0, 1, w1);
                    relax(x, y, z, 1, 1, 0, w2);
                    relax(x, y, z, 1, 0, 1, w2);
                    relax(x, y, z, 0, 1, 1, w2);
                    relax(x, y, z, 1, 1, 1, w3);
                }
    }

    int nx() const { return nx_; }
    int ny() const { return ny_; }
    int nz() const { return nz_; }

    // The per-cell argmin seed (nearest solid cell index, kSdfEmptySeed = no
    // solid in view) in box layout x + y*nx + z*nx*ny - the SAME encoding the
    // shader reads (pass 49), converted per sample to its cell to compute the
    // exact L2 distance to that solid CUBE, so the CPU and GPU build the
    // identical field (parity).
    const std::vector<std::uint32_t>& seeds() const { return seed_; }

    // Hand the packed seed array over to the caller (the renderer's upload
    // staging copy): no second array, no copy - the build wrote it in place.
    // The field is left EMPTY (dims reset), so a released field can never be
    // sampled: sample()/cellDistance() then answer "outside the field".
    void releaseSeeds(std::vector<std::uint32_t>& out) {
        out = std::move(seed_);
        dist_.clear();
        nx_ = ny_ = nz_ = 0;
    }

    // Distance from the cell (x, y, z) to the nearest solid surface, in
    // voxel units (the chamfer field minus the half-voxel). 0 inside solid,
    // ~0.5 face-adjacent, growing with distance. Large outside the field.
    float cellDistance(int x, int y, int z) const {
        if (x < 0 || x >= nx_ || y < 0 || y >= ny_ || z < 0 || z >= nz_) {
            return kInf;
        }
        return std::max(0.0f, dist_[I(x, y, z)] - 0.5f);
    }

    // Distance to the nearest solid CUBE at an arbitrary point p in voxel
    // units (cell (x,y,z) spans [x,x+1) x [y,y+1) x [z,z+1)). Exactly 0
    // inside the solid, and the L2 distance to the nearest solid cube
    // outside. Returns a large value when no solid is found (open space).
    float sample(float px, float py, float pz) const {
        if (nx_ <= 0 || ny_ <= 0 || nz_ <= 0) {
            return kInf;
        }
        const int x0 = static_cast<int>(std::floor(px));
        const int y0 = static_cast<int>(std::floor(py));
        const int z0 = static_cast<int>(std::floor(pz));
        // The seed's three bit fields (the shader's arithmetic, pass 50).
        const std::uint32_t mx = maskX();
        const std::uint32_t my = maskY();
        const int shiftY = bits_.x;
        const int shiftZ = bits_.x + bits_.y;
        float best = kInf;
        // The nearest solid cube to p is the seed of one of the cells around
        // p; take the min over the 3x3x3 neighborhood of cells (the chamfer
        // argmin is per cell-center, so the true nearest can sit in a
        // neighbor at a cell boundary).
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int cx = x0 + dx, cy = y0 + dy, cz = z0 + dz;
                    if (cx < 0 || cx >= nx_ || cy < 0 || cy >= ny_ ||
                        cz < 0 || cz >= nz_) {
                        continue;
                    }
                    const std::uint32_t s = seed_[I(cx, cy, cz)];
                    if (s == kSdfEmptySeed) {
                        continue;
                    }
                    const int vx = static_cast<int>(s & mx);
                    const int vy = static_cast<int>((s >> shiftY) & my);
                    const int vz = static_cast<int>(s >> shiftZ);
                    // L2 distance from p to the solid cube [v, v+1)^3.
                    const float qx = std::clamp(px, float(vx), float(vx + 1));
                    const float qy = std::clamp(py, float(vy), float(vy + 1));
                    const float qz = std::clamp(pz, float(vz), float(vz + 1));
                    const float ex = px - qx;
                    const float ey = py - qy;
                    const float ez = pz - qz;
                    best = std::min(best, std::sqrt(ex * ex + ey * ey +
                                                    ez * ez));
                }
        return best;
    }

private:
    static constexpr float kInf = 1e30f;
    int nx_ = 0;
    int ny_ = 0;
    int nz_ = 0;
    SeedBits bits_{};
    std::vector<float> dist_;
    std::vector<std::uint32_t> seed_;  // packed cell or kSdfEmptySeed
    // A field of bits_.x bits holds coordinates 0..2^bits_.x - 1, i.e. 0 when
    // the span is one cell - so the masks below are always well defined
    // (bits are capped at kMaxSeedBits = 31 by fits()).
    std::uint32_t maskX() const {
        return bits_.x == 0 ? 0u : (1u << bits_.x) - 1u;
    }
    std::uint32_t maskY() const {
        return bits_.y == 0 ? 0u : (1u << bits_.y) - 1u;
    }
    std::uint32_t encodeSeed(int x, int y, int z) const {
        return static_cast<std::uint32_t>(x) |
               (static_cast<std::uint32_t>(y) << bits_.x) |
               (static_cast<std::uint32_t>(z) << (bits_.x + bits_.y));
    }
    std::size_t I(int x, int y, int z) const {
        return (static_cast<std::size_t>(z) * ny_ +
                static_cast<std::size_t>(y)) *
                   nx_ +
               static_cast<std::size_t>(x);
    }
};

// Sphere-traced soft shadow (iquilezles.org/articles/rmshadows/ with a proper
// 3D SDF). March the ray toward the sun, step by the SDF distance
// (conservatively scaled, so an approximate SDF can never skip a surface),
// and fold k*h/t into the running min. h is the 3D distance to the nearest
// solid surface, so every shadow edge shares the same continuous penumbra.
// o/d are in voxel units (field-local); d must be a unit vector with
// d.y > ~0 (the sun is up).
//
// This variant reports where the march STOPPED when it leaves the field (or
// spends `steps`) without hitting anything: outExit/outExitT/outVisibility
// (all optional) receive the exit point, the distance travelled and the
// accumulated visibility, and the return value is true - the caller MUST
// continue the ray with a wider traversal. outExit is the point where the
// ray crosses the field boundary (not the first sample past it), so the
// continuation starts exactly where the field stops. On the GPU the field
// covers only the 6x6 chunks around the camera, so leaving it is NOT open
// space: the shader hands the ray to its whole-region 2.5D march (see
// sunRayEscapesSdf3d, pass 40). Returns false when the field resolved the
// ray itself (a hit, a low sun, or the march never left it): outVisibility
// is then the final answer.
inline bool sphereTracedShadowExits(const SdfField& sdf, const float o[3],
                                    const float d[3], float outExit[3],
                                    float* outExitT, float* outVisibility,
                                    float sharpness = 8.0f, int steps = 160) {
    if (outExitT != nullptr) {
        *outExitT = 0.0f;
    }
    if (d[1] <= 0.05f) {
        // Low/sunset sun: no cheap ascend bound, skip (the shader's hand-off
        // traversal applies the same rule to startVisibility).
        if (outVisibility != nullptr) {
            *outVisibility = 1.0f;
        }
        return false;
    }
    // Pass 40 (shader mirror): hand the ray over at the box CROSSING, not at
    // the first sample past it, so a step (up to 0.7 * h) cannot skip a
    // caster sitting in the strip just outside the box. 0 when the origin
    // already starts outside - those rays belong to the wider march alone.
    const float hi[3] = {float(sdf.nx()), float(sdf.ny()), float(sdf.nz())};
    float tExit = 0.0f;
    if (o[0] >= 0.0f && o[0] < hi[0] && o[1] >= 0.0f && o[1] < hi[1] &&
        o[2] >= 0.0f && o[2] < hi[2]) {
        tExit = 1e30f;
        for (int a = 0; a < 3; ++a) {
            if (std::abs(d[a]) > 1e-6f) {
                const float face = (d[a] > 0.0f) ? hi[a] : 0.0f;
                tExit = std::min(tExit, (face - o[a]) / d[a]);
            }
        }
    }
    float visibility = 1.0f;
    float t = 0.0f;
    for (int i = 0; i < steps; ++i) {
        // Left the field: report where, so the caller can keep marching.
        if (t >= tExit) {
            if (outExit != nullptr) {
                outExit[0] = o[0] + d[0] * tExit;
                outExit[1] = o[1] + d[1] * tExit;
                outExit[2] = o[2] + d[2] * tExit;
            }
            if (outExitT != nullptr) {
                *outExitT = tExit;
            }
            if (outVisibility != nullptr) {
                *outVisibility = visibility;
            }
            return true;
        }
        const float px = o[0] + d[0] * t;
        const float py = o[1] + d[1] * t;
        const float pz = o[2] + d[2] * t;
        const float h = sdf.sample(px, py, pz);
        if (h < 1e-3f) {
            if (outVisibility != nullptr) {
                *outVisibility = 0.0f;  // hit the surface: fully shadowed
            }
            return false;
        }
        visibility = std::min(
            visibility,
            std::clamp(sharpness * h / std::max(t, 1e-4f), 0.0f, 1.0f));
        // Conservative step: 0.7x the SDF keeps the march from overshooting a
        // surface the chamfer field slightly over-estimates.
        t += std::max(h * 0.7f, 0.05f);
    }
    // Budget spent inside the field: the shader hands the rest over as well.
    if (outExit != nullptr) {
        outExit[0] = o[0] + d[0] * t;
        outExit[1] = o[1] + d[1] * t;
        outExit[2] = o[2] + d[2] * t;
    }
    if (outExitT != nullptr) {
        *outExitT = t;
    }
    if (outVisibility != nullptr) {
        *outVisibility = visibility;
    }
    return true;
}

// The march alone, treating "left the field" as open space - the pre-pass-40
// shader behavior, kept for the field-only tests (the field must agree with
// the voxels it was built from) and probes. Returns visibility in [0, 1].
inline float sphereTracedShadow(const SdfField& sdf, const float o[3],
                                const float d[3], float sharpness = 8.0f,
                                int steps = 160) {
    if (d[1] <= 0.05f) {
        return 1.0f;  // low/sunset sun: no cheap ascend bound, skip
    }
    float visibility = 1.0f;
    sphereTracedShadowExits(sdf, o, d, nullptr, nullptr, &visibility,
                            sharpness, steps);
    return visibility;
}

}  // namespace vv::voxel
