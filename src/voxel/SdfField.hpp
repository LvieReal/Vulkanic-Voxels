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
// This file is the CPU reference implementation (no Qt, no Vulkan): the GPU
// builds the same field and traces the same march (pass 38) and the CPU
// sphere-traced shadow must agree with it (pinned by the parity test in
// tests/terrain_world_tests.cpp).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace vv::voxel {

class SdfField {
public:
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
        seed_.assign(n, -1);
        for (int z = 0; z < nz; ++z)
            for (int y = 0; y < ny; ++y)
                for (int x = 0; x < nx; ++x) {
                    if (solid(x, y, z)) {
                        dist_[I(x, y, z)] = 0.0f;
                        seed_[I(x, y, z)] = static_cast<int>(I(x, y, z));
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
                    const int s = seed_[I(cx, cy, cz)];
                    if (s < 0) {
                        continue;
                    }
                    const int vx = s % nx_;
                    const int vy = (s / nx_) % ny_;
                    const int vz = s / (nx_ * ny_);
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
    std::vector<float> dist_;
    std::vector<int> seed_;
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
// o/d are in voxel units; d must be a unit vector with d.y > ~0 (the sun is
// up). Returns visibility in [0, 1].
inline float sphereTracedShadow(const SdfField& sdf, const float o[3],
                                const float d[3], float sharpness = 8.0f,
                                int steps = 160) {
    if (d[1] <= 0.05f) {
        return 1.0f;  // low/sunset sun: no cheap ascend bound, skip
    }
    float visibility = 1.0f;
    float t = 0.0f;
    for (int i = 0; i < steps; ++i) {
        const float px = o[0] + d[0] * t;
        const float py = o[1] + d[1] * t;
        const float pz = o[2] + d[2] * t;
        // Left the field: open space, nothing left to block the sun.
        if (px < 0.0f || px >= float(sdf.nx()) || py < 0.0f ||
            py >= float(sdf.ny()) || pz < 0.0f || pz >= float(sdf.nz())) {
            break;
        }
        const float h = sdf.sample(px, py, pz);
        if (h < 1e-3f) {
            return 0.0f;  // hit the surface: fully shadowed from here
        }
        visibility = std::min(
            visibility,
            std::clamp(sharpness * h / std::max(t, 1e-4f), 0.0f, 1.0f));
        // Conservative step: 0.7x the SDF keeps the march from overshooting a
        // surface the chamfer field slightly over-estimates.
        t += std::max(h * 0.7f, 0.05f);
    }
    return visibility;
}

}  // namespace vv::voxel
