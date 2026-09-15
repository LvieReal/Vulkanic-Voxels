# Agent Notes — Vulkanic Voxels

Working notes for AI agent sessions. **The chat is not persistent — read this
first.** Per-pass details live in the git log (one commit per pass, verbose
messages); this file keeps only what you need to work effectively.

## Project

Voxel world ray-traced in a Vulkan compute shader (one thread per pixel,
`resources/shaders/pixels_rgba.comp`), Qt 6 shell, C++23, CMake ≥ 3.26.
Owner's WGSL reference: `docs/reference_renderer.wgsl` (canonical look).

| Path | Contents |
| --- | --- |
| `src/ui/` | Qt shell (`AppWindow`, `VulkanWidget` — swapchain + game loop) |
| `src/platform/` | OS abstraction (Win32/X11/Wayland/macOS) — keep OS headers out of the rest |
| `src/vulkan/` | `VulkanRenderer` (streaming, far LOD, frame loop), `VoxelResources` (GPU buffers) |
| `src/voxel/` | `World`/`Chunk`/`VoxelTypes`/`VoxelConfig` |
| `src/terrain/` | `Noise(2D/3D)`, `TerrainGenerator` (3D density terrain), `FarField` (far LOD + seam patch) |
| `src/render/` | Scene UBO, push constants, lighting config |
| `tests/` | CPU tests (no Qt/Vulkan), `ctest --test-dir build` |
| `scripts/build-linux-toolchain.sh` | Sandbox toolchain (see below) |

## Working rules (owner-set)

- One problem per pass; passes gated on owner verification.
- No TAA (removed pass 9 after owner decision). No SSAA (removed pass 10).
  One ray per pixel.
- User uploads never reach the sandbox — diagnose from code/math/probes.
- Mouse locked always; cross-platform abstraction stays.
- Uncapped fps default; `VV_PRESENT=fifo` restores vsync.
- Keep agent docs/toolchain inside the repo or home — `/tmp` gets wiped
  and the workspace gets rolled back occasionally (recovery: `git fetch
  origin <branch>` + `git reset --soft` + re-commit).

## Key contracts (sync points between CPU and GPU)

- Voxel atlas: 1 byte/voxel, 4 per u32, slot stride = padded chunk bytes;
  layout X + Y*sizeX + Z*sizeX*worldHeight.
- Height atlas: u16 per column (top+1, 0 = air), packed 2 per u32,
  X + Z*sizeX; `(sizeX*sizeZ+1)/2` words per slot.
- Chunk table: u32 slot per region cell, row-major; **triple-buffered**,
  selected by push `region.w`; `kEmptySlot` = air.
- Far field: u32 per cell = height u16 | type u8 << 16; **double-buffered**,
  selected by push `farParams.y`; `pc.far.z = 0` disables far LOD.
- Push constants (128 B): see `src/render/SceneData.hpp` — must match the
  shader's `Push` block exactly.
- Far grid is world-aligned (512-voxel snap): recenters shift the window,
  never re-quantize. Far seam band is patched from real chunk heightmaps
  (`FarField::patchRegion`, incremental, delta-uploaded).
- Streaming: generation on a worker thread (pump installs + fence-scoped
  uploads only); released slots have a 2-frame cooldown; region swaps are
  wait-free (no device/queue waits); teleport fallback stays synchronous.
- Terrain: density = clamp(g·(target−y)) + fbm3·amp, folds (overhangs) in
  mountains; defaults in `TerrainGenerator.hpp` (lift 36, ceiling 100,
  window (0.92, 0.995), snowLine 82). `maxHeightVoxels()` must stay ≤ 127.

## Environment variables

- `VV_DEBUG_TERM` — color miss pixels by ray-termination cause.
- `VV_DEBUG_HOLE=X,Z` — far-miss pixels over chunk (X,Z): magenta = empty
  far cell (data hole), cyan = data present but ray passed over (height
  too low), yellow = march never crossed the chunk.
- `VV_PERF=1` — log frames > 25 ms with the stream/world bucket.
- VV_SHADOW_SHARP=1: exact single-ray sun shadows (no cone penumbra).
- VV_FAR_LOD=1: opt into the coarse terrain LOD field (OFF by default).
- VV_SDF_SHADOWS=1: SDF soft-shadow experiment (pass 33: the exact
  march's traversal + an extended-sun-disk penumbra estimate -
  iquilezles.org/articles/rmshadows/ family. Pass 34 removed the Aaltonen
  two-sphere refinement (it projected a hard "clamped edge" stripe) and
  settled on the plain Quilez k*h/t. Pass 35 tried a unified circle-segment
  but was REVERTED in pass 36 - it fixed k*h/t's over-darkening of lit
  penumbra but the owner read that as "bright spots", and it did not soften
  the steep-cliff side edges (a 1D top-plane limitation). Current state =
  pass 34: k*h/t in the clear branch, hard 0 in the below-top/solid branch.
  kShadowSharpness in the shader tunes the sun's angular size / softness.
  Pass 37 adds a real 3D voxel SDF (src/voxel/SdfField.hpp) validated on the
  CPU (testSdfSoftShadow3d) so the penumbra is soft on the vertical/side
  edges too; the GPU port (3D SDF texture + the same sphere trace) is
  pass 38).
  Exact binary shadows remain the default reference.
  The SDF marcher keeps occupancy/material policy in shadowOpacity() so
  future foliage can attenuate and be marched through instead of
  requiring precise decal projection.
- `VV_PRESENT=fifo` — vsync.

## Sandbox validation

```sh
export PATH=/tmp/deps/venv/bin:/tmp/deps/prefix/bin:$PATH   # symlink -> ~/.cache/vv-deps
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="/tmp/deps/qt6;/tmp/deps/prefix"
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_PREFIX_PATH="/tmp/deps/qt6;/tmp/deps/prefix"
cmake --build build/release && cmake --build build/debug  # warning-free
ctest --test-dir build/release                    # all tests must pass
glslangValidator -V resources/shaders/pixels_rgba.comp -o /tmp/p.spv
QT_QPA_PLATFORM=offscreen LD_LIBRARY_PATH=/tmp/deps/qt6/lib:/tmp/deps/prefix/lib \
    timeout 8 ./build/release/bin/game            # must exit cleanly (Vulkan dialog, no crash)
# CPU tests without the toolchain:
g++ -std=c++20 -O2 -ffp-contract=off -I. -Isrc tests/terrain_world_tests.cpp \
    src/terrain/{Noise,Noise3D,TerrainGenerator,FarField}.cpp \
    src/voxel/{Chunk,VoxelTypes,VoxelTextures,World}.cpp -o /tmp/t && /tmp/t
```

Toolchain: `bash scripts/build-linux-toolchain.sh` installs into
`~/.cache/vv-deps` (survives /tmp wipes) and symlinks `/tmp/deps` to it.

## Current status (pass 27)

Tree state: pass-25 shadows RESTORED on top of the pass-21 revert (the
owner confirmed the revert worked, then asked for the cone tracing to
go too): file-based voxel textures + pass-22 aliases intact, sun
shadows back to the single exact binary march, no light grid, no cone
machinery. Owner-verified base: 3ff725f.

Pass 29 (textures): SIDE-FACE V AXIS FLIPPED - textures displayed
upside down on every side face. Root cause: the shader built side-face
UVs as (z,y)/(x,y), i.e. V = world +Y, but in this pipeline Vulkan's
V=0 is the image's TOP row (QImage rows upload in order), so the image
top landed at the voxel BOTTOM. Fix: V = -Y on side faces (REPEAT
sampler; fract(-y) = 1 - fract(y) keeps tiling continuous). Top/bottom
faces keep V = +Z (rotation, not flip - art-dependent). README UV
paragraph updated ("textures display UPRIGHT on sides; mirror the file
only for horizontal direction"). Validated: both builds warning-free
(shader recompiled), ctest green, smoke ok.

Pass 28 (textures): PARTIAL TEXTURE SETS NOW WORK PER FACE. Owner
report: grass_top.png + grass_side.png + "grass bottom = dirt" in
aliases.txt still rendered grass as plain colors. Root cause: a mode
only applied when ALL its files existed (the README's own example was
impossible), so grass had no complete side-uniform set -> entirely
plain, and the alias then textured only the rarely-visible bottom face.
Fix: per-face resolution - each face independently uses the first of
its candidate files that exists (top/bottom: _top/_bottom then the
uniform file; side faces: the custom name, then _side, then uniform);
unresolved faces stay plain or take an alias. The startup log now says
per type "N files, M/6 faces (missing: bottom ...)" so gaps point at
their fix. Pure logic (suffix chains, face-name mapping, alias-source
resolution) moved to voxel/VoxelTextures.hpp and unit tested incl. the
exact reported scenario. README/header docs updated. Validated: both
builds warning-free, ctest green, smoke ok.

Pass 27 (first of the fix-one-by-one round): STREAMING PRIORITY WAS
BACKWARDS. The pump stocks m_genRequests by iterating the sorted
pending list in reverse (best first), but the workers popped the BACK -
so the best coords sat at the FRONT forever while every top-up
(appended at the back, always a little worse) was generated first: the
nearest, in-frustum chunks appeared dead last after every region move.
Fix: the workers consume the FRONT (strict FIFO in descending
priority). streamPriority() extracted to src/vulkan/StreamPriority.hpp
(glm-free) and pinned by testStreamPriority, including a full
pump/worker queue simulation (old behavior: 26 priority inversions;
fixed: 0). Validated: both builds warning-free, ctest green, smoke ok.

Note for the fix round: each fix = one pass = one commit, owner
verifies before the next.

Owner's stated next step after the shadow saga: their benchmark /
optimization work (tips delivered in pass 20).

Pass 30 (first optimization pass, owner-requested): HIERARCHICAL DDA.
New per-chunk "block max" atlas: one u16 per 8x8 block of columns = the
MAX column bound in the block (kHeightBlockVoxels in voxel/VoxelTypes.hpp
== kBlockVoxels in the shader; 0 = all-air block), packed two per u32,
8 words per 32x32 slot, uploaded with each chunk (binding 11, the slot
freed by the light-grid revert). In the march's column loop: on entering
a new block, one fetch decides whether the ray provably stays above
every column of the block until it exits - if so the whole block is
crossed with branchless ALU-only DDA steps (no chunk-table resolve, no
column-height fetch, no Y-walk per column). Safety: block exit
mid-column resumes exactly at the exit t; a re-entry guard handles
ulp-level boundary misses (stops skipping that block, normal column
logic advances out); kNoHeightData blocks are never skipped. The skip
condition implies each skipped column's own air-skip condition, so the
image is bit-identical. CPU mirror traceHier + 3-way parity
(old/new/hierarchical, 6000 rays x 2 worlds) green; iteration counts:
world 0 84,033 -> 36,259 (-57%), world 1 69,924 -> 33,668 (-52%).
Block-map packing unit tests added (single-block chunk, 9x9 round-up,
generated 32x32 chunks vs ground truth).

Pass 31 (owner-requested, for Nsight profiling): SPIR-V DEBUG INFO IN
DEBUG BUILDS ONLY. cmake/Shaders.cmake now passes glslangValidator -g in
Debug (embeds the GLSL source + line tables: 753 OpLine, full source
text, 143 KB vs 79 KB) and -g0 (glslang's default = strip) in every
other config - the flag list is never empty because an empty
generator-expression argument leaks as a literal "" file argument under
Ninja/VERBATIM and fails the compile. -g0 is a true no-op: release
SPIR-V is byte-identical to a flag-less compile (verified by hash).
Debug spv loads fine on a real driver (offscreen smoke). To profile:
configure a Debug build dir and run it under Nsight Graphics (Shader
Profiler activity) - the exe picks up the debug spv from its own
resources/shaders. scripts/spirv_dbg.py inspects a spv for debug
opcodes. Note for the optimization hunt: finishRegionMove() still never
clears m_streamActive (the pass-13 bug, lost in the reverts), so the
pump re-runs the full region-table publish every frame AT REST - a
likely CPU-side constant cost; fix parked in 20ca086 as the next pass.

Pass 32 (owner-directed, from the first Nsight profile): KILL THE INTEGER
DIVISIONS + FIX THE STUCK STREAM FLAG. Nsight showed floorDiv's first two
lines ('/' and '%') as the #1/#2 hotspots (14% + 12%) and resolveColumn's
bounds check right behind. Three changes, image bit-identical:
1) floorDiv power-of-two fast path: every divisor here is pow2 (chunk 32,
block 8), and arithmetic >> log2(b) IS floor division for two's
complement (SPIR-V OpShiftRightArithmetic is sign-extending by spec;
identity pinned by a new CPU test). SPIR-V check: OpSDiv 5->1, OpUDiv
3->1 (survivors = generic fallback + far-hole debug), shift count up.
2) resolveColumn hoisted to ONCE per column in the march: the block
bound, height bound and Y-walk all reuse the slot/local pair
(columnHeightAt/blockHeightAt take the resolved pair; the old
columnHeightBound/blockHeightBound re-resolved per call - 2-3
resolveColumns per column). Shadow march ditto. The Y-walk's
voxelTypeAt fallback for unresolved columns was always-0 dead work -
unresolved columns now skip the walk entirely. Block-grid math in the
shader now rounds UP like the CPU builder (no behavior change at 32).
3) finishRegionMove() now adopts m_streamTarget as m_regionCenter and
clears m_streamActive (pass-13 fix, lost in the reverts): without it the
pump re-ran the full finish path EVERY FRAME AT REST, and m_regionCenter
stayed pinned to the last synchronous rebuild (stale fog-cut box with
far-LOD off + far-seam patch scan off-center after streamed moves).
Validated: CPU parity 3-way unchanged, both builds warning-free, ctest
green x2, smoke x2.

Pass 33 (owner-reported artifacts, the "sdf shadows" rewrite): SDF SOFT
SHADOWS REWRITTEN ON THE EXACT TRAVERSAL. The previous sphere-traced
attempt had two compounding bugs: (1) the Aaltonen triangulation's
d2 = max(d^2 - y^2, 0) clamp - when the distance to the surface GROWS
(the shadow edge, exactly where the penumbra lives) y > d, d2 clamps to
0, penumbra = 0, and min() pins visibility to BLACK for the rest of the
march (the canonical formulation gets NaN there and min() no-ops - the
clamp turned the safe no-op into a permanent blackout, so every grazing
pixel rendered darker than the binary shadow); (2) the march stepped by
the "SDF" value (floored 0.05), crawling at ~0.04 voxels/step near
surfaces, so the 128-step budget covered only ~50 voxels vs the exact
march's 256 columns - shadows appeared/disappeared at an unexplained
distance. Rewrite: sunRayEscapesSdf now runs the EXACT march's
traversal (ascending column DDA + height-bound cell walk + coarse far
cells, identical occlusion events and 256-column budget, so nothing the
binary march shadows can leak) and takes softness from shadowPenumbra -
Quilez's k*h/t improved with Aaltonen's two-sphere triangulation
(iquilezles.org/articles/rmshadows/), sampled at every cleared column
from the distance to its top plane; degenerate triangles (y >= t or
h^2 <= y^2, i.e. growing distance) skip the update explicitly instead of
clamping to zero. The fake per-sample distances (0.25 overhang air,
0.05 floor, 1e30 sentinels) and nextShadowCellDistance are gone; the
overhang-shaft case (walk finds no solid) advances without a sample -
the topmost solid sits above the ascending ray, no relevant surface.
kShadowSharpness = 8 (~7-degree light, few-voxel penumbra) is named and
documented for tuning. shadowOpacity() keeps the foliage hook
(attenuate + continue through the column). CPU mirror
shadowPenumbraMirror/sunRayEscapesSdfMirror + testSunShadowSdfMarch:
occlusion parity with the exact mirror (0 leaks over 3000 rays), [0,1]
range, penumbra present but a minority, and the penumbra shape pinned
against the known wall (hard shadow under the top, partial grazing, lit
well clear, monotonic). Validated: CPU tests green (sdf shadow: 2191
shadowed, 9 penumbral, 800 lit of 3000; wall gradient 1,1,0.40,0,0),
both builds warning-free, ctest green x2, smoke x2.

Pass 33 follow-up (owner: "much better, but some stripes" + "are hard
shadows combined with hard shadows?"): PIXEL-INTEGRATED SHADOW JITTER.
The residual stripes are the classic "marching-step banding" the
rmshadows article warns about: the penumbra is a lower envelope over
the march's per-column samples, so evaluating it exactly at the pixel
center stripes the shadow at ~1-column spacing (Aaltonen's triangulation
helps but does not remove it). Fix: in SDF mode only, shadeSurfaceHit
jitters the shadow ray origin to a stable pseudo-random sub-pixel
position (integer hash of gl_GlobalInvocationID, no sin-hash, no
temporal term - no shimmer, no TAA needed) within the hit face's plane
(tangent axes from the normal, same convention as calculateAO; the face
is a plane so the jittered origin can never go below the surface - no
self-shadow acne). Each pixel then samples the envelope at a random
sub-pixel phase, turning the regular stripes into fine dither noise.
The exact-binary path is untouched (jitter = 0), so the reference stays
bit-identical. Re: hard shadows - there is NO double application:
sunShadow runs either the SDF march or the binary march (one if), and
the result is applied once to ambient + direct. The hard edge is the
umbra (fully-blocked sun), which is binary in ANY SDF soft shadow (see
the article's reference images); only the penumbra (partial block) is
soft. Validated: glslangValidator clean, both builds warning-free,
ctest green, offscreen smoke identical to baseline (dialog, 0% CPU).

Pass 34 (owner: "that wasn't it. the stripes/lines are too strong, jitter
cannot fix it. these honestly look like clamped edges"): THE AALTONEN
TRIANGULATION WAS THE STRIPE - NOT THE SAMPLING POSITION. The pass-33
follow-up's jitter dithered the sample position by a sub-pixel amount, but the
stripe's period is a FULL DDA COLUMN (~10-30 px), so a +-0.5 px jitter cannot
touch it (the owner was right that jitter can't fix it). Measured on the CPU
mirror (flat ground + wall, fine sub-column scan): the Aaltonen estimate has a
near-vertical cliff at h/prev ~= 2 (the distance DOUBLING in one step, where
y = h^2/(2 prev) = h and d = sqrt(h^2 - y^2) -> 0), swinging a single
per-column estimate from ~0 to 1 over an infinitesimal range of h/prev. As the
pixel moves, the column where h/prev = 2 slides; folded into the min over the
march's columns it projects a hard STRAIGHT edge across the lit terrain - a
1-column-period comb of ~7% dips in the lit region plus a chaotic,
non-monotonic penumbra at the shadow edge (the "clamped edge"). Fix:
shadowPenumbra is now the PLAIN Quilez estimate k*h/t (the article's original
technique) - a smooth, well-conditioned function of (h, t) with no cliff; the
dense per-column sampling (every column the ray clears) already resolves the
closest approach well within a pixel, so the refinement's light-leak
correction is not needed. The march's `previous` tracking is gone. The pass-33
follow-up jitter is removed too (its stated purpose was this artifact; the
estimate fix makes it moot, and it no longer had a role). The exact-binary
reference (sunRayEscapes) is untouched. The CPU mirrors
shadowPenumbraMirror/sunRayEscapesSdfMirror are updated in lockstep (same
2-arg estimate, no `previous`). Measured (CPU mirror, flat ground + wall):
the penumbra is now a SMOOTH MONOTONIC ramp (1.0 -> 0.93 -> 0.77 -> 0.59 ->
0.41 -> 0.21 -> 0.00 over ~1.4 columns), the lit region is clean (no comb),
and the shadow is fully dark under the wall top. Wall penumbra shape pinned:
1.0, 1.0, 0.409, 0.0, 0.0 (lit, lit, partial, hard, hard).

Pass 35 (REVERTED in pass 36 - owner rejected on-device): THE UNIFIED
CIRCLE-SEGMENT PENUMBRA. Owner: "sides of the SDF shadow right now are
completely sharp... limitation or can be fixed?" This pass attempted the fix
with the extended-sun-disk circle-segment sampled in BOTH branches. It was
committed (0ba0fee) and pushed, but the owner's on-device screenshot showed
the sides STILL sharp AND new "weird bright spots". Pass 36 diagnosed the
bright spots (k*h/t over-darkening, see below) and REVERTED the functional
files to the pass-34 plain k*h/t state. The circle-segment design notes are
kept below for reference only. SDF experimental path only (VV_SDF_SHADOWS /
scene.misc.w > 0.5); the exact binary sunRayEscapes is untouched and stays
the bit-identical reference.

ROOT CAUSE: the penumbra term was only evaluated in the CLEAR branch (ray
above the column's top plane, h = y0 - bound); the BELOW-top/solid branch
(y0 < bound, the ray crosses the caster's side - the shadow's side/leading
edge) did `return 0.0` with NO penumbra term, so every side edge was a hard
0->1. k*h/t can never soften it: h is height over the top plane, constant
along the caster's side, and 0 at the top.

THE FIX (validated against 168-ray brute-force ground truth before the
shader): shadowPenumbra is now the EXTENDED-SUN-DISK circle segment, one
formula used in BOTH branches (and both near + far). d = bound - y0 is the
SIGNED height of the sample over the column's top plane; a = d/(t*alpha)
with alpha = 1/kShadowSharpness is the top's offset from the sun center in
sun-disk radii; the lit fraction = 0.5 - (asin a + a*sqrt(1-a^2))/pi (the
circle-segment cap (1/pi)(acos a - a*sqrt(1-a^2)), clamped a to [-1,1]).
It is the exact extended-disk answer for a heightfield top and is
CONTINUOUS (0.5) at the top, so a shadow edge - where the march
transitions clear<->below-top - gets a smooth ramp on BOTH sides instead of
a jump (lit side in (0.5,1), blocked side in (0,0.5)). Sampled at every
DDA column and folded into the min (closest approach). When a solid column
blocks, the march returns the accumulated visibility (the penumbra of that
top, <= 0.5) instead of a hard 0.0.

WHY NOT THE ALTERNATIVES (all measured against the brute-force truth):
- corner-based circle segment (distance to the caster's top CORNER): ~0 on
  the side edge (the ray drifts in z so the corner sits far off-axis).
- deficit-only (bound - y0) with k*h/t in the clear branch: 0 on the side
  edge (rays escape EAST of the vertical corner, not above the top plane).
- trailing-edgeDist (marchSdfEdge, diag15/16): DEAD END - inverted ramp /
  lit-in-umbra. Do not retry.
- A true vertical wall (a box's side) is still sharp: the 1D top-plane SDF
  has no data for a vertical face. That is a known limitation of this
  approximation, NOT of soft shadows; the owner's terrain is a heightfield
  (sloped sides) where this formula is continuous and soft. A proper 3D
  voxel SDF would be needed for true vertical casters - parked, not done.

VALIDATED (CPU mirror first, then shader): CPU tests green - sdf shadow:
2191 shadowed, 231 penumbral, 578 lit of 3000 (the penumbra is now a real
minority, was 7 under k*h/t), 0 leaks (blocked pixels are at most half-lit,
never fully lit), visibility in [0,1]. Wall ramp pinned at
[1.0, 1.0, 0.751, 0.212, 0.0] (lit, lit, grazing-light-side, under-top,
deep) - a smooth ramp straddling the top at 0.5, no hard jump. X-sweep
(far/top edge) against 168-ray truth: unified tracks truth far better than
k*h/t (x14 0.633 vs 0.211; x15 0.358 vs 0.000). Shader compiles to valid
SPIR-V (glslangValidator -V + the build pipeline). Both builds config clean,
ctest green, offscreen smoke: no crash (idle on the Vulkan error dialog, no
ICD in the sandbox).

NOTE (stale section above replaced): the earlier "trailing-edge = CONFIRMED
CORRECT MECHANISM / TUNING TODO" text in this pass's draft was WRONG
(contradicted by diag15/16) and was removed. (The circle-segment fix itself
was then reverted in pass 36 - see below.)

Pass 36 (owner rejected pass 35 on-device -> diagnose + REVERT): OWNER
SCREENSHOT ("looks like it did not affect sides at all, only produces weird
bright spots now. could you debug this?"). Two CPU diagnostics (diag36/37,
a 96x96x64 heightfield with rolling hills + a cliff, and a finite MESA, all
checked against the exact binary march) pinned the bright spots down:

ROOT CAUSE OF THE BRIGHT SPOTS: pass-34 plain k*h/t OVER-DARKENS LIT
PENUMBRA PIXELS. The min over the DDA columns lands on the grazing column
(where (y0-bound)/t is tiny), so k*h/t -> ~0 even though the ray CLEARS the
terrain (exact march = lit). On the real terrain that is ~16% of lit
pixels. The circle-segment "fixed" that over-darkening by lighting those
same pixels to 0.5-0.95 - and that is exactly the change the owner saw as
"weird bright spots" (pixels that were near-black in pass 34 are now half-
to nearly-lit). It is a physically-correct correction of a k*h/t artifact,
but it is NOT the look the owner wants, and it did not soften the sides.

WHY THE SIDES DID NOT SOFTEN: the owner's sharp "sides" are the shadow
edges that run along the steep/vertical cliff faces. The 1D top-plane SDF
has no data for a vertical face, so those edges are unchanged by any
top-plane penumbra (circle-segment OR k*h/t) - the mesa z-edges that ARE
soft are a different, already-soft feature. Softening a true vertical
caster needs a proper 3D voxel SDF (distance to the actual surface,
including side faces) - a larger rewrite, not a penumbra-term change.

DECISION: revert the functional files (pixels_rgba.comp,
terrain_world_tests.cpp, README.md, VulkanRenderer.hpp) to the pass-34
plain k*h/t state (git checkout 7ea15fd) to remove the bright spots and
restore the last owner-verified look. The circle-segment math stays in the
pass-35 section above for reference. The sides remain a documented
limitation of the 1D top-plane SDF; a 3D voxel SDF is the parked next step
if the owner wants true-soft vertical casters.

VALIDATED: after the revert, CPU tests green (plain k*h/t expectations),
shader compiles to valid SPIR-V, release build clean, ctest green,
offscreen smoke: no crash.

Pass 37 (owner green-lit, option (a)): 3D VOXEL SDF FOR THE SDF SHADOW
(CPU REFERENCE). Owner green-lit a "real and proper" 3D voxel SDF (based on
established references, not reinvented) so the penumbra is soft on ALL
edges - including the side/vertical edges the 1D top-plane model (pass-34
k*h/t) cannot soften. SDF experimental path only (VV_SDF_SHADOWS /
scene.misc.w > 0.5); the exact binary sunRayEscapes stays the bit-identical
reference; no lighter lit-side penumbra (owner rule from pass 36).

TECHNIQUE (the established real-time voxel-SDF soft-shadow pipeline:
voxelize -> 3D distance field -> sphere-traced soft shadow; the k*h/t
estimate from iquilezles.org/articles/rmshadows/):
- src/voxel/SdfField.hpp (header-only; the CPU reference and the parity
  source for the pass-38 GPU port): SdfField builds a 3D SDF from the
  solid/air voxel grid. A two-pass chamfer distance transform (W1=1,
  W2=sqrt(2), W3=sqrt(3), 26-neighbor) tracks, per cell, the NEAREST SOLID
  CELL (the argmin seed). sample(p) is the exact L2 distance from p to the
  nearest solid CUBE (min over the 3x3x3 cell neighborhood's seeds):
  exactly 0 inside the solid, >0 in air. THE BUG THE FIRST ATTEMPT HIT: a
  trilinear field of cell-center distances is NON-ZERO inside solid cells
  near a boundary (it treats voxels as points), so the sphere trace skipped
  over the surface (129 light leaks); the cube SDF (0 inside solid) fixes
  it.
- sphereTracedShadow(sdf, o, d, k=8, steps=160): sphere-trace toward the
  sun; h = sdf.sample; hit (h < 1e-3) -> 0; fold k*h/t into the running min;
  conservative step t += max(0.7*h, 0.05) (the chamfer field can slightly
  over-estimate, so a factor <1 keeps the march from overshooting a surface
  - the standard sphere-trace safety); low sun (d.y <= 0.05) -> 1; leaving
  the field -> break (open space).

TEST (testSdfSoftShadow3d, shadow-mirror namespace): makeSdfTestWorld =
64x48x64 rolling ground (type 1) + MESA x[20,24) z[12,44) y[0,40) (type 2)
+ OVERHANG slab x[44,52) z[20,30) y[30,34) (type 3, air beneath),
farDim=0 so the SDF box and the exact march see identical geometry. Invariants:
(1) 2500 points over 4 surface kinds (ground top / mesa -x vertical face /
mesa top / under the overhang): NO light leak (soft <= 0.5+1e-3 wherever the
exact march is blocked), range [0,1], both shadowed and lit well exercised.
(2) The mesa's SIDE edge (the shadow boundary running along its west face
x=20): scan the GROUND SURFACE across it (x=5..14 at z=28.5, on the actual
column top). Must cross both shadow (<0.25) and light (>0.75) and make no
>0.7 jump between adjacent columns (a hard edge). Measured penumbra:
1.0 -> 0.810 -> 0.153 -> 0.0 across x=9,10,11 (a real soft ramp, max step
0.657); the 2.5D top-plane path has no data for the vertical face and makes
a hard jump here. (The first draft of this check scanned a fixed y=16 line,
which crosses the terrain surface itself - a hard in/out-of-ground edge, not
a caster's shadow - and was wrong; it now follows the ground top.)
(3) The overhang's UNDERSIDE (48,16,25): the exact march blocks it and the
3D SDF keeps it at most half-lit (the 2.5D top-plane path sees only the
column's top and can light it).

RESULTS: CPU green - sdf3d shadow: 852 shadowed, 1648 lit of 2500; 0 leaks,
0 out-of-range; the side scan crosses shadow+light with no hard jump; the
overhang underside <= 0.5. Every other test green (no regressions).

NEXT (pass 38): GPU port - build the same 3D SDF on the GPU (3D texture,
or in-register) and run the same sphere trace in the SDF branch of
sunShadow; the CPU binary is the parity source. Keep soft <= 0.5 wherever
the exact march is dark; do NOT add a lighter lit-side penumbra term.
