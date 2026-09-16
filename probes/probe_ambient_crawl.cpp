// Crawl/shimmer probe for the ambient lighting terms (pass 63): renders a
// fixed view of the real terrain on the CPU with a voxel DDA and the shader's
// own shading terms, then measures how much the image MOVES for a sub-voxel
// camera step. That movement is what the eye reads as moire/sparkle over
// distant, darkened ground - the pass-62 regression this probe pinned down
// (nearest-column atlas reads: 3.5% of the value per 0.05-voxel step; bilinear
// reads with the d = 1 sample dropped: 0.4%).
//
//   g++ -std=c++20 -O2 -ffp-contract=off -I. -Isrc -Itests \
//       probes/probe_ambient_crawl.cpp \
//       src/terrain/{Noise,Noise3D,TerrainGenerator,FarField}.cpp \
//       src/voxel/{Chunk,VoxelTypes,VoxelTextures,World}.cpp -o /tmp/probe_crawl
//
//   /tmp/probe_crawl --shimmer              # the table (crawl per 0.05 voxel)
//   /tmp/probe_crawl --diff --variant bilinear --out diff.bmp
//   /tmp/probe_crawl --term 1 --variant smoothed --out ambient.bmp
//   /tmp/probe_crawl                        # the standard variant BMP set
//
// Terms: 0 full shading, 1 ambient only, 2 direct sun only, 3 voxel AO only,
// 4 normal shading without shadow. Variants: old (pre-62), current (pass 62),
// bilinear (bilinear + ladder from 2 - the shipped pass-63 sampling),
// bilinear8 (that plus 8 azimuths, measured and not taken), smoothed (3x3 tap).
// BMP output (24-bit), no dependencies beyond the repo's own sources.

// Pass-63 diagnosis: a small CPU renderer of the shader's lighting model, so
// the "moire in the dark at distance" can be LOOKED at instead of guessed at.
//
// It marches the real terrain's height atlas (the same "highest solid + 1"
// column field the shader's ambient scan reads, and the same field the air-skip
// marches use), does the same binary sun march, and shades with the shader's
// terms: sky ambient (old view-ray form or the pass-62 sky-visibility form,
// with switchable pieces), direct sun, fresnel rim - plus the per-vertex voxel
// AO the shader applies, approximated from the 8 surrounding voxels.
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include "ambient_mirror.hpp"
#include "terrain/TerrainGenerator.hpp"

namespace {

struct Atlas final {
  int half = 0;
  std::vector<std::uint16_t> top;  // highest solid + 1, 0 = all air

  std::uint16_t at(int x, int z) const {
    const int cx = std::clamp(x, -half, half);
    const int cz = std::clamp(z, -half, half);
    return top[std::size_t(cx + half) * (2 * half + 1) + (cz + half)];
  }
  float bilinear(float fx, float fz) const {
    const float bx = std::floor(fx - 0.5f);
    const float bz = std::floor(fz - 0.5f);
    const float tx = std::clamp(fx - 0.5f - bx, 0.0f, 1.0f);
    const float tz = std::clamp(fz - 0.5f - bz, 0.0f, 1.0f);
    const int x0 = int(bx), z0 = int(bz);
    const float h00 = at(x0, z0), h10 = at(x0 + 1, z0);
    const float h01 = at(x0, z0 + 1), h11 = at(x0 + 1, z0 + 1);
    const float a = h00 + (h10 - h00) * tx;
    const float b = h01 + (h11 - h01) * tx;
    return a + (b - a) * tz;
  }
  // A 3x3 box average of the quantised tops - the "smoothed terrain" the
  // large-scale ambient should be judged against.
  float smoothed(float fx, float fz) const {
    const int x0 = int(std::floor(fx)), z0 = int(std::floor(fz));
    float sum = 0.0f;
    for (int dx = -1; dx <= 1; ++dx) {
      for (int dz = -1; dz <= 1; ++dz) {
        sum += at(x0 + dx, z0 + dz);
      }
    }
    return sum / 9.0f;
  }
};

struct Vec3 {
  float x = 0, y = 0, z = 0;
  const float* v() const { return &x; }
};
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 normalize(Vec3 a) {
  const float l = std::sqrt(dot(a, a));
  return {a.x / l, a.y / l, a.z / l};
}
Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// --- the shader's sky palette ----------------------------------------------
const float kSkyLow[3] = {0.8f, 0.9f, 1.0f};
const float kSkyHigh[3] = {0.45f, 0.62f, 0.95f};
const float kLightColor[3] = {1.0f, 0.95f, 0.85f};

void skyBaseColor(const Vec3& rd, float* out) {
  const float up = std::clamp(rd.y * 0.5f + 0.5f, 0.0f, 1.0f);
  const float t = up * up * (3.0f - 2.0f * up);
  const float horizon = std::pow(1.0f - std::fabs(rd.y), 3.0f);
  for (int i = 0; i < 3; ++i) {
    const float sky = kSkyLow[i] + (kSkyHigh[i] - kSkyLow[i]) * t;
    const float hazeW = 0.12f * horizon + 0.04f * horizon * horizon;
    out[i] = sky + (i == 0 ? 1.0f : (i == 1 ? 0.95f : 0.9f)) * hazeW;
  }
}

// --- the ambient estimate, with switchable pieces --------------------------
// (a copy of the shipped estimate in tests/ambient_mirror.hpp with the
// pass-63 candidates wired in, so variants can be compared side by side)
struct Knobs final {
  bool newAmbient = true;      // false = the pre-62 view-ray formula
  bool bilinear = false;
  bool smoothed = false;       // 3x3 box average of the quantised tops
  bool dropNearest = false;
  bool azimuthal3Tap = false;
  bool smoothMax = false;      // continuous max accumulation instead of hard max
  bool wideLadder = false;     // 2 samples per octave instead of 1
  int azimuths = 6;
  float floorValue = 0.12f;
  bool ao = true;              // the per-vertex voxel AO term
};

constexpr float kHorizonBias = 1.0f;
const float kLadder[6] = {1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f};

// Which term the image shows: 0 = full, 1 = ambient only, 2 = direct only,
// 3 = AO only, 4 = normal shading without shadow.
int g_term = 0;

float columnTop(const Atlas& atlas, const Knobs& k, float fx, float fz) {
  if (k.smoothed) {
    return atlas.smoothed(fx, fz);
  }
  if (k.bilinear) {
    return atlas.bilinear(fx, fz);
  }
  return float(atlas.at(int(std::floor(fx)), int(std::floor(fz))));
}

float skyVisibility(const Atlas& atlas, const Knobs& k, const Vec3& p) {
  const int first = k.dropNearest ? 1 : 0;
  float terms[16];
  float total = 0.0f;
  for (int a = 0; a < k.azimuths; ++a) {
    const float azimuth = float(a) * (6.2831853f / float(k.azimuths));
    const float dx = std::cos(azimuth), dz = std::sin(azimuth);
    float tMax = 0.0f;
    const int count = k.wideLadder ? 11 : 6;
    for (int i = first; i < count; ++i) {
      const float d = k.wideLadder ? (std::pow(1.5f, float(i)) * 1.0f) * kLadder[0]
                                   : kLadder[i];
      const float top = columnTop(atlas, k, p.x + dx * d, p.z + dz * d);
      const float s = std::max(top - p.y - kHorizonBias, 0.0f) / d;
      if (k.smoothMax) {
        // smooth max: 0.5*(a + b + sqrt((a-b)^2 + k)) - continuous, so the
        // horizon does not jump when the argmax switches sample.
        const float diff = tMax - s;
        tMax = 0.5f * (tMax + s + std::sqrt(diff * diff + 2.5e-4f));
      } else {
        tMax = std::max(tMax, s);
      }
    }
    const float sinTheta = tMax / std::sqrt(1.0f + tMax * tMax);
    terms[a] = 1.0f - sinTheta;
  }
  for (int a = 0; a < k.azimuths; ++a) {
    total += k.azimuthal3Tap
                 ? (terms[(a + k.azimuths - 1) % k.azimuths] + 2.0f * terms[a] +
                    terms[(a + 1) % k.azimuths]) *
                       0.25f
                 : terms[a];
  }
  const float rays = 2.0f;  // the two SDF rays escape in this world...
  return std::clamp((total + rays) / 8.0f, 0.0f, 1.0f);
}

struct Hit final {
  bool hit = false;
  Vec3 p;
  Vec3 n;
  int vx = 0, vy = 0, vz = 0;  // the solid voxel entered
};

bool solidAt(const Atlas& atlas, int x, int y, int z) {
  return y < int(atlas.at(x, z));  // the atlas is "highest solid + 1"
}

// A proper voxel DDA, the same traversal the shader's march uses: step to the
// next voxel boundary on the axis with the smallest tMax, and the axis just
// stepped is the face the ray entered through.
Hit dda(const Atlas& atlas, Vec3 origin, Vec3 dir, float maxDist) {
  Hit h;
  const float inv[3] = {1.0f / dir.x, 1.0f / dir.y, 1.0f / dir.z};
  int voxel[3] = {int(std::floor(origin.x)), int(std::floor(origin.y)),
                  int(std::floor(origin.z))};
  const int step[3] = {dir.x >= 0.0f ? 1 : -1, dir.y >= 0.0f ? 1 : -1,
                       dir.z >= 0.0f ? 1 : -1};
  float tMax[3];
  for (int a = 0; a < 3; ++a) {
    const float next = float(voxel[a] + (step[a] > 0 ? 1 : 0));
    tMax[a] = (next - origin.v()[a]) * inv[a];
  }
  const float tDelta[3] = {std::fabs(inv[0]), std::fabs(inv[1]),
                           std::fabs(inv[2])};
  if (solidAt(atlas, voxel[0], voxel[1], voxel[2])) {
    h.hit = true;
    h.p = origin;
    h.n = {0, 1, 0};
    h.vx = voxel[0]; h.vy = voxel[1]; h.vz = voxel[2];
    return h;
  }
  int face = -1;
  float t = 0.0f;
  for (int i = 0; i < 20000; ++i) {
    const int axis = (tMax[0] < tMax[1]) ? ((tMax[0] < tMax[2]) ? 0 : 2)
                                         : ((tMax[1] < tMax[2]) ? 1 : 2);
    t = tMax[axis];
    if (t > maxDist) {
      return h;
    }
    voxel[axis] += step[axis];
    tMax[axis] += tDelta[axis];
    face = axis;
    if (solidAt(atlas, voxel[0], voxel[1], voxel[2])) {
      h.hit = true;
      h.p = origin + dir * t;
      h.n = {0, 0, 0};
      if (axis == 0) h.n = {float(-step[0]), 0, 0};
      else if (axis == 1) h.n = {0, float(-step[1]), 0};
      else h.n = {0, 0, float(-step[2])};
      h.vx = voxel[0]; h.vy = voxel[1]; h.vz = voxel[2];
      return h;
    }
    (void)face;
  }
  return h;
}

// The shader's per-vertex voxel AO (calculateAO + vertexAO), mirrored: the four
// vertices of the hit face, each from its two side neighbours and the diagonal.
float vertexAO(bool side1, bool side2, bool corner) {
  if (side1 && side2) {
    return 0.0f;
  }
  return 1.0f - ((side1 ? 1.0f : 0.0f) + (side2 ? 1.0f : 0.0f) +
                 (corner ? 1.0f : 0.0f)) / 3.0f;
}

float calculateAO(const Atlas& atlas, const Hit& hit) {
  const Vec3 n = hit.n;
  if (std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z) < 0.5f) {
    return 1.0f;
  }
  const int px = hit.vx + int(n.x), py = hit.vy + int(n.y), pz = hit.vz + int(n.z);
  int du[3], dv[3];
  float u = 0.0f, v = 0.0f;
  const Vec3 f = {hit.p.x - std::floor(hit.p.x), hit.p.y - std::floor(hit.p.y),
                  hit.p.z - std::floor(hit.p.z)};
  if (std::fabs(n.x) > 0.5f) {
    du[0] = 0; du[1] = 0; du[2] = 1;
    dv[0] = 0; dv[1] = 1; dv[2] = 0;
    u = f.z; v = f.y;
  } else if (std::fabs(n.y) > 0.5f) {
    du[0] = 1; du[1] = 0; du[2] = 0;
    dv[0] = 0; dv[1] = 0; dv[2] = 1;
    u = f.x; v = f.z;
  } else {
    du[0] = 1; du[1] = 0; du[2] = 0;
    dv[0] = 0; dv[1] = 1; dv[2] = 0;
    u = f.x; v = f.y;
  }
  const auto solid = [&](int su, int sv) {
    return solidAt(atlas, px + su * du[0] + sv * dv[0],
                   py + su * du[1] + sv * dv[1], pz + su * du[2] + sv * dv[2]);
  };
  const bool smU = solid(-1, 0), spU = solid(1, 0);
  const bool smV = solid(0, -1), spV = solid(0, 1);
  const float ao00 = vertexAO(smU, smV, solid(-1, -1));
  const float ao10 = vertexAO(spU, smV, solid(1, -1));
  const float ao01 = vertexAO(smU, spV, solid(-1, 1));
  const float ao11 = vertexAO(spU, spV, solid(1, 1));
  return ao00 * (1.0f - u) * (1.0f - v) + ao10 * u * (1.0f - v) +
         ao01 * (1.0f - u) * v + ao11 * u * v;
}

// Binary sun visibility: the same DDA, walked until the ray is provably above
// every column (maxHeightVoxels).
bool sunVisible(const Atlas& atlas, Vec3 p, Vec3 sun, int maxY) {
  Hit up = dda(atlas, p + sun * 0.02f, sun, 4000.0f);
  if (up.hit && up.p.y < float(maxY)) {
    return false;
  }
  return true;
}

void writeBmp(const std::string& path, int w, int h,
              const std::vector<unsigned char>& rgb) {
  const int rowPad = (4 - (w * 3) % 4) % 4;
  const int dataSize = (w * 3 + rowPad) * h;
  std::vector<unsigned char> header(54 + dataSize, 0);
  header[0] = 'B';
  header[1] = 'M';
  const std::uint32_t fileSize = 54 + std::uint32_t(dataSize);
  std::memcpy(&header[2], &fileSize, 4);
  const std::uint32_t offset = 54;
  std::memcpy(&header[10], &offset, 4);
  const std::uint32_t headerSize = 40;
  std::memcpy(&header[14], &headerSize, 4);
  const std::int32_t width = w, height = h;
  std::memcpy(&header[18], &width, 4);
  std::memcpy(&header[22], &height, 4);
  const std::uint16_t planes = 1, bpp = 24;
  std::memcpy(&header[26], &planes, 2);
  std::memcpy(&header[28], &bpp, 2);
  std::memcpy(&header[34], &dataSize, 4);
  for (int y = 0; y < h; ++y) {
    unsigned char* row = &header[54 + (h - 1 - y) * (w * 3 + rowPad)];
    for (int x = 0; x < w; ++x) {
      const std::size_t i = std::size_t(y) * w + x;
      row[x * 3 + 0] = rgb[i * 3 + 2];  // B
      row[x * 3 + 1] = rgb[i * 3 + 1];  // G
      row[x * 3 + 2] = rgb[i * 3 + 0];  // R
    }
  }
  std::FILE* f = std::fopen(path.c_str(), "wb");
  std::fwrite(header.data(), 1, header.size(), f);
  std::fclose(f);
}

// --- the renderer -----------------------------------------------------------
void render(const Atlas& atlas, const Knobs& k, Vec3 eye, Vec3 target, int w,
            int h, const std::string& path, double* hfEnergy,
            std::vector<float>* outLuma = nullptr) {
  const Vec3 forward = normalize(target - eye);
  const Vec3 right = normalize(cross(forward, {0, 1, 0}));
  const Vec3 up = cross(right, forward);
  const float tanHalfFov = 0.65f;  // ~66 degrees vertical
  const float aspect = float(w) / float(h);
  const Vec3 sun = normalize({0.5f, 1.0f, 0.5f});
  std::vector<unsigned char> image(std::size_t(w) * h * 3, 0);
  std::vector<float> luma(std::size_t(w) * h, 0.0f);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float sx = (2.0f * (float(x) + 0.5f) / float(w) - 1.0f) * tanHalfFov * aspect;
      const float sy = (1.0f - 2.0f * (float(y) + 0.5f) / float(h)) * tanHalfFov;
      const Vec3 rd = normalize(forward + right * sx + up * sy);
      const Hit hit = dda(atlas, eye, rd, 2000.0f);
      float rgb[3];
      if (!hit.hit) {
        skyBaseColor(rd, rgb);
        for (int i = 0; i < 3; ++i) {
          rgb[i] = rgb[i] * 0.8f + 0.2f;  // the sunless sky, roughly
        }
      } else {
        const Vec3 n = hit.n;
        const float ndl = std::max(dot(n, sun), 0.0f);
        const float shadow =
            (ndl > 0.0f && sunVisible(atlas, hit.p, sun, 128)) ? 1.0f : 0.0f;
        const float hemi = std::clamp(n.y * 0.5f + 0.5f, 0.0f, 1.0f);
        const float view = std::max(dot(n, rd * -1.0f), 0.0f);
        const float sunTerm = 0.2f * ndl + 0.8f * std::pow(ndl, 8.0f);
        float skyView[3], skyNormal[3];
        skyBaseColor(rd, skyView);
        skyBaseColor(n, skyNormal);
        const float ao = k.ao ? calculateAO(atlas, hit) : 1.0f;
        const float base = 0.5f;
        if (g_term != 0) {
          // Isolation: one term at a time, everything else white.
          float value = 1.0f;
          if (g_term == 1) {
            value = k.newAmbient
                        ? skyVisibility(atlas, k, hit.p) * (n.y * 0.5f + 0.5f)
                        : 0.5f * (0.55f + 0.45f * shadow);
          } else if (g_term == 2) {
            value = kLightColor[0] *
                    (0.2f * ndl + 0.8f * std::pow(ndl, 8.0f)) * shadow;
          } else if (g_term == 3) {
            value = ao;
          } else if (g_term == 4) {
            value = 0.25f + 0.75f * ndl;
          }
          for (int i = 0; i < 3; ++i) {
            rgb[i] = std::clamp(value, 0.0f, 1.0f);
          }
        } else if (!k.newAmbient) {
          for (int i = 0; i < 3; ++i) {
            const float amb = (skyView[i] * 0.35f +
                               (skyView[i] - skyView[i] * 0.35f) * hemi) *
                              (0.55f + 0.45f * shadow);
            const float rim = skyView[i] * 0.12f * std::pow(1.0f - view, 5.0f);
            rgb[i] = base * (amb + kLightColor[i] * sunTerm * shadow + rim) * ao;
          }
        } else {
          const float vis = skyVisibility(atlas, k, hit.p);
          const float ground[3] = {kSkyLow[0] * 0.45f, kSkyLow[1] * 0.40f,
                                   kSkyLow[2] * 0.32f};
          for (int i = 0; i < 3; ++i) {
            const float dome = ground[i] + (skyNormal[i] - ground[i]) * hemi;
            const float amb = dome * vis + skyNormal[i] * k.floorValue;
            const float rim = skyNormal[i] * 0.12f * std::pow(1.0f - view, 5.0f) * vis;
            rgb[i] = base * (amb + kLightColor[i] * sunTerm * shadow + rim) * ao;
          }
        }
        (void)shadow;
      }
      // Gamma-free clamp to 8 bit (the real pipeline tone-maps; the patterns
      // are what matter here).
      for (int i = 0; i < 3; ++i) {
        const float v = std::clamp(rgb[i], 0.0f, 1.0f);
        image[(std::size_t(y) * w + x) * 3 + i] = (unsigned char)(v * 255.0f + 0.5f);
      }
      luma[std::size_t(y) * w + x] = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
    }
  }
  // High-frequency energy in the DARK half of the image (the user's "moire in
  // darkened areas"): mean |luma - blur3(luma)| there.
  if (hfEnergy != nullptr) {
    double sum = 0.0;
    int count = 0;
    for (int y = 1; y + 1 < h; ++y) {
      for (int x = 1; x + 1 < w; ++x) {
        const float c = luma[std::size_t(y) * w + x];
        const float blur = (luma[std::size_t(y - 1) * w + x] +
                            luma[std::size_t(y + 1) * w + x] +
                            luma[std::size_t(y) * w + x - 1] +
                            luma[std::size_t(y) * w + x + 1] +
                            4.0f * c) / 8.0f;
        if (c < 0.30f) {
          sum += std::fabs(c - blur);
          ++count;
        }
      }
    }
    *hfEnergy = count > 0 ? sum / count : 0.0;
  }
  if (outLuma != nullptr) {
    *outLuma = luma;
  }
  if (!path.empty()) {
    writeBmp(path, w, h, image);
  }
}

} // namespace

int main(int argc, char** argv) {
  const vv::terrain::TerrainConfig config{};
  const vv::terrain::TerrainGenerator gen(config);
  const int half = 420;
  Atlas atlas;
  atlas.half = half;
  atlas.top.assign(std::size_t(2 * half + 1) * (2 * half + 1), 0u);
  for (int x = -half; x <= half; ++x) {
    for (int z = -half; z <= half; ++z) {
      const std::int32_t t = gen.topSolidVoxels(x, z);
      atlas.top[std::size_t(x + half) * (2 * half + 1) + (z + half)] =
          std::uint16_t(t < 0 ? 0 : t + 1);
    }
  }
  // Camera: on the plain near (176, 24), 25 voxels above the ground, looking
  // at the high ridge at (112, 232) - a long, shaded view across the terrain.
  const float eyeY = float(gen.topSolidVoxels(176, 24) + 1) + 25.0f;
  const Vec3 eye{176.5f, eyeY, 24.5f};
  const Vec3 target{112.5f, float(gen.topSolidVoxels(112, 232) + 1), 232.5f};
  const int w = 560, h = 340;

  // Usage: render_probe [--term N] [--variant old|current|bilinear|smoothed]
  //                     [--out FILE]
  std::string variant = "current";
  std::string out;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--term" && i + 1 < argc) {
      g_term = std::atoi(argv[++i]);
    } else if (a == "--variant" && i + 1 < argc) {
      variant = argv[++i];
    } else if (a == "--out" && i + 1 < argc) {
      out = argv[++i];
    }
  }
  if (argc > 1 && std::string(argv[1]) == "--diff") {
    // Visualize the crawl: two frames 0.05 voxel apart, ambient term only, the
    // absolute difference amplified 8x. Static terrain structure cancels; what
    // the eye reads as moire (the part that moves when the camera moves) shows.
    std::string variant = "current";
    std::string out = "/home/user/vv_probe/diff.bmp";
    for (int i = 2; i < argc; ++i) {
      const std::string a = argv[i];
      if (a == "--variant" && i + 1 < argc) variant = argv[++i];
      if (a == "--out" && i + 1 < argc) out = argv[++i];
    }
    Knobs k;
    if (variant == "bilinear") {
      k.bilinear = true;
      k.dropNearest = true;
    } else if (variant == "bilinear_smax") {
      k.bilinear = true;
      k.dropNearest = true;
      k.smoothMax = true;
    }
    k.ao = false;
    g_term = 1;
    std::vector<float> a, b;
    render(atlas, k, eye, target, w, h, "", nullptr, &a);
    render(atlas, k, {eye.x + 0.05f, eye.y, eye.z}, target, w, h, "", nullptr, &b);
    std::vector<unsigned char> image(std::size_t(w) * h * 3, 0);
    double sum = 0.0;
    int count = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
      const float d = std::min(std::fabs(a[i] - b[i]) * 8.0f, 1.0f);
      for (int c = 0; c < 3; ++c) {
        image[i * 3 + c] = (unsigned char)(d * 255.0f + 0.5f);
      }
      if (a[i] < 0.45f) {
        sum += d / 8.0f;
        ++count;
      }
    }
    writeBmp(out, w, h, image);
    std::printf("%s: mean crawl in dark half %.5f -> %s\n", variant.c_str(),
                count ? sum / count : 0.0, out.c_str());
    return 0;
  }

  if (argc > 1 && std::string(argv[1]) == "--shimmer") {
    // The moire signature: a half-voxel camera move must NOT change a distant
    // surface's shading. Measured on the ambient term alone (where the user
    // sees the pattern), in the dark half of the frame.
    struct Candidate final {
      const char* name;
      Knobs knobs;
    };
    Candidate candidates[7];
    candidates[0].name = "pre-62 view-ray formula";
    candidates[0].knobs.newAmbient = false;
    candidates[1].name = "pass-62 nearest sampling";
    candidates[2].name = "bilinear";
    candidates[2].knobs.bilinear = true;
    candidates[3].name = "bilinear + smooth max";
    candidates[3].knobs.bilinear = true;
    candidates[3].knobs.smoothMax = true;
    candidates[4].name = "bilinear + drop d=1";
    candidates[4].knobs.bilinear = true;
    candidates[4].knobs.dropNearest = true;
    candidates[5].name = "bilinear + smax + drop d=1";
    candidates[5].knobs.bilinear = true;
    candidates[5].knobs.smoothMax = true;
    candidates[5].knobs.dropNearest = true;
    candidates[6].name = "bilinear + smax + drop d=1 + 8az";
    candidates[6].knobs.bilinear = true;
    candidates[6].knobs.smoothMax = true;
    candidates[6].knobs.dropNearest = true;
    candidates[6].knobs.azimuths = 8;
    // Keep the ambient term only: no AO, no direct, so the number is the
    // term's own instability.
    for (Candidate& c : candidates) {
      c.knobs.ao = false;
    }
    g_term = 1;
    const float dxs = 0.05f;
    for (Candidate& c : candidates) {
      std::vector<float> a, b;
      render(atlas, c.knobs, eye, target, w, h, "", nullptr, &a);
      render(atlas, c.knobs, {eye.x + dxs, eye.y, eye.z}, target, w, h, "", nullptr, &b);
      double sum = 0.0, level = 0.0;
      int count = 0;
      for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] < 0.45f) {  // the darker half: the user's "darkened areas"
          sum += std::fabs(a[i] - b[i]);
          level += a[i];
          ++count;
        }
      }
      std::printf("%-34s 0.05-voxel camera move: mean |d| %.5f (rel %.1f%%) on %d "
                  "dark px (level %.3f)\n",
                  c.name, count ? sum / count : 0.0,
                  count ? 100.0 * (sum / count) / (level / count) : 0.0, count,
                  count ? level / count : 0.0);
    }
    return 0;
  }

  if (!out.empty()) {
    Knobs k;
    if (variant == "old") {
      k.newAmbient = false;
    } else if (variant == "bilinear") {
      k.bilinear = true;
      k.dropNearest = true;
    } else if (variant == "bilinear8") {
      k.bilinear = true;
      k.dropNearest = true;
      k.azimuths = 8;
    } else if (variant == "smoothed") {
      k.smoothed = true;
      k.azimuthal3Tap = true;
    }
    double energy = 0.0;
    render(atlas, k, eye, target, w, h, out, &energy);
    std::printf("term %d variant %s: dark-area HF energy %.5f -> %s\n", g_term,
                variant.c_str(), energy, out.c_str());
    return 0;
  }

  Knobs old;            old.newAmbient = false;
  Knobs current;        // pass-62 as shipped
  Knobs bilinear = current; bilinear.bilinear = true;
  Knobs smooth = current; smooth.smoothed = true;
  Knobs smoothNoAo = smooth; smoothNoAo.ao = false;
  Knobs oldNoAo = old; oldNoAo.ao = false;

  struct Variant final {
    const char* name;
    const Knobs* knobs;
    const char* file;
  };
  const Variant variants[5] = {
      {"old (pre-62)", &old, "/home/user/vv_probe/view_old.bmp"},
      {"current (pass 62)", &current, "/home/user/vv_probe/view_current.bmp"},
      {"bilinear", &bilinear, "/home/user/vv_probe/view_bilinear.bmp"},
      {"3x3 smoothed", &smooth, "/home/user/vv_probe/view_smooth.bmp"},
      {"smoothed, no AO", &smoothNoAo, "/home/user/vv_probe/view_smooth_noao.bmp"},
  };
  for (const Variant& v : variants) {
    double energy = 0.0;
    render(atlas, *v.knobs, eye, target, w, h, v.file, &energy);
    std::printf("%-18s dark-area HF energy %.5f -> %s\n", v.name, energy, v.file);
  }
  {
    double energy = 0.0;
    render(atlas, oldNoAo, eye, target, w, h, "/home/user/vv_probe/view_old_noao.bmp",
           &energy);
    std::printf("%-18s dark-area HF energy %.5f -> %s\n", "old, no AO", energy,
                "/home/user/vv_probe/view_old_noao.bmp");
  }
  return 0;
}
