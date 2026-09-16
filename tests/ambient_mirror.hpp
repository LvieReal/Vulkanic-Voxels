#pragma once

// CPU mirror of the pass-62 ambient sky visibility (resources/shaders/voxels.comp).
//
// The shader is the shipping implementation; this is the reference the tests
// march against, and the probe harness includes it. It mirrors the shader's
// arithmetic literally - same constants, same order, same clamps - so a change
// on either side that is not mirrored here shows up as a test failure. The
// constants are also pinned against the shader's text in
// tests/terrain_world_tests.cpp (testAmbientVisibility), which is what catches
// "the shader moved and nobody noticed" in the other direction.
//
// Pure functions, no windows, no Vulkan: a `ColumnTop` callback provides the
// height atlas (highest solid voxel + 1, 0 = all air), and the SDF ray takes
// the shipping `vv::voxel::SdfField`, so both the synthetic cases and the real
// terrain can drive it.

#include <cmath>
#include <cstdint>

#include "voxel/SdfField.hpp"

namespace vv::ambient {

// --- constants (mirror of voxels.comp) -------------------------------------
inline constexpr int kAmbientAzimuths = 6;
inline constexpr int kAmbientDistances = 6;
inline constexpr float kAmbientDistancesVox[kAmbientDistances] = {
    1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f};
inline constexpr float kAmbientHorizonBias = 1.0f;
inline constexpr float kAmbientSdfRange = 32.0f;
inline constexpr float kAmbientSdfLift = 1.0f;
inline constexpr float kAmbientSdfHit = 0.5f;
inline constexpr float kAmbientSdfAboveRatio = 1.3f;
inline constexpr float kAmbientSdfAboveTan = 0.08f;
inline constexpr float kAmbientSdfMaxSin = 0.966f;
inline constexpr float kAmbientSdfMaxTan = 3.732f;
inline constexpr float kAmbientFloorDefault = 0.12f;
inline constexpr float kAmbientGroundTint[3] = {0.45f, 0.40f, 0.32f};

// Highest solid voxel + 1 at a world column, 0 = all air. Returning 0 for an
// unloaded column is the shader's resolveColumn returning false.
using ColumnTop = std::uint32_t (*)(int x, int z, void* user);

inline float floorVox(float v) { return std::floor(v); }

// Mirror of ambientHorizonVisibility: the SUM over the six azimuths of
// (1 - sin(horizon elevation)) - the caller adds the two SDF rays and divides
// by 8 - plus the worst azimuth and its sine.
inline float horizonVisibility(float px, float py, float pz, ColumnTop top,
                               void* user, float* outWorstAzimuth,
                               float* outWorstSin) {
  float total = 0.0f;
  float worstSin = 2.0f;
  float worstAzimuth = 0.0f;
  for (int a = 0; a < kAmbientAzimuths; ++a) {
    const float azimuth =
        static_cast<float>(a) * (6.2831853f / static_cast<float>(kAmbientAzimuths));
    const float dirX = std::cos(azimuth);
    const float dirZ = std::sin(azimuth);
    float tMax = 0.0f;
    for (int i = 0; i < kAmbientDistances; ++i) {
      const float d = kAmbientDistancesVox[i];
      const int cellX = static_cast<int>(floorVox(px + dirX * d));
      const int cellZ = static_cast<int>(floorVox(pz + dirZ * d));
      const std::uint32_t topValue = top(cellX, cellZ, user);
      if (topValue == 0u) {
        continue;  // unloaded column: no obstruction (the shader: no slot)
      }
      const float dy = std::max(static_cast<float>(topValue) - py -
                                    kAmbientHorizonBias,
                                0.0f);
      tMax = std::max(tMax, dy / d);
    }
    const float sinTheta = tMax / std::sqrt(1.0f + tMax * tMax);
    total += 1.0f - sinTheta;
    if (sinTheta < worstSin) {
      worstSin = sinTheta;
      worstAzimuth = azimuth;
    }
  }
  if (outWorstAzimuth != nullptr) {
    *outWorstAzimuth = worstAzimuth;
  }
  if (outWorstSin != nullptr) {
    *outWorstSin = worstSin;
  }
  return total;  // sum over the azimuths: mean of eight is the caller's job
}

// Mirror of ambientSdfRay: 1.0 = the ray left kAmbientSdfRange without meeting
// anything, t / range otherwise. `field` empty (no live field) = no occlusion
// information, exactly like the shader's scene.misc.w <= 0.5 / sdfBox.box.w < 0
// guard returning 1.0.
inline float sdfRayVisibility(const vv::voxel::SdfField* field, const float* p,
                              const float* dir) {
  if (field == nullptr || field->nx() <= 0 || field->ny() <= 0 ||
      field->nz() <= 0) {
    return 1.0f;  // no live field: no occlusion information
  }
  float t = kAmbientSdfLift;
  for (int i = 0; i < 8; ++i) {
    const float sample[3] = {p[0] + dir[0] * t, p[1] + dir[1] * t,
                             p[2] + dir[2] * t};
    const float h = field->sampleCorners(sample[0], sample[1], sample[2]);
    if (h < kAmbientSdfHit) {
      return std::min(std::max(t / kAmbientSdfRange, 0.0f), 1.0f);
    }
    t += std::max(h * 1.5f, 4.0f);
    if (t >= kAmbientSdfRange) {
      break;
    }
  }
  return 1.0f;
}

// Mirror of ambientSkyVisibility: the 6-azimuth scan plus the two SDF rays
// (along the normal, and into the scan's worst azimuth at the elevation it
// found), all in one mean of eight.
inline float skyVisibility(const vv::voxel::SdfField* field, float px, float py,
                           float pz, const float* n, ColumnTop top,
                           void* user) {
  float worstAzimuth = 0.0f;
  float worstSin = 0.0f;
  float visibility =
      horizonVisibility(px, py, pz, top, user, &worstAzimuth, &worstSin);

  const float sinClamped = std::min(worstSin, kAmbientSdfMaxSin);
  const float tanTheta =
      sinClamped / std::sqrt(std::max(1e-6f, 1.0f - sinClamped * sinClamped));
  const float tanAbove = std::min(
      tanTheta * kAmbientSdfAboveRatio + kAmbientSdfAboveTan,
      kAmbientSdfMaxTan);
  const float silhouette[3] = {std::cos(worstAzimuth), tanAbove,
                               std::sin(worstAzimuth)};
  const float silhouetteLen =
      std::sqrt(silhouette[0] * silhouette[0] + silhouette[1] * silhouette[1] +
                silhouette[2] * silhouette[2]);
  const float normalLen =
      std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  const float normal[3] = {n[0] / normalLen, n[1] / normalLen, n[2] / normalLen};
  const float silhouetteDir[3] = {silhouette[0] / silhouetteLen,
                                  silhouette[1] / silhouetteLen,
                                  silhouette[2] / silhouetteLen};
  const float p[3] = {px, py, pz};
  visibility += sdfRayVisibility(field, p, normal);
  visibility += sdfRayVisibility(field, p, silhouetteDir);
  return std::min(std::max(visibility / 8.0f, 0.0f), 1.0f);
}

// Mirror of ambientFloor() (scene.ambient.y < 0 = the shader's default).
inline float ambientFloor(float uniformValue) {
  return (uniformValue >= 0.0f) ? uniformValue : kAmbientFloorDefault;
}

} // namespace vv::ambient
