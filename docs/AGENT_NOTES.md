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
  edges too; pass 38 ports it to the GPU (argmin-seed storage buffer +
  sphere trace), pass 39 fixes the box build (wrong chunk Z stride -> the
  whole region around the camera was fully shadowed).
  Current state = pass 40: the 3D voxel SDF sphere trace (CPU reference in
  src/voxel/SdfField.hpp, box build + chunk-layout walk in
  src/voxel/SdfBox.hpp, GPU mirror in pixels_rgba.comp) hands a ray leaving
  its box over to the 2.5D k*h/t traversal at the box crossing instead of
  calling the box edge open space (pass 39 = the scrambled box build).
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

## Pass 38: GPU port of the 3D voxel SDF soft shadow

STATUS: implemented and committed (owner verifies on-device). The same 3D
voxel SDF as pass 37 now builds on the CPU (a BACKGROUND thread, the same
reference the CPU test pins) and uploads to the GPU, and the SAME sphere
trace runs in the SDF branch of sunShadow (VV_SDF_SHADOWS=1).

DESIGN (mirrors the far-LOD field's background build + fence-scoped upload):
- SDF BOX: a camera-centered box, 2*kSdfHalfChunks (6) chunks on X/Z x the
  full world height (128) = 192x128x192 = 4.7M cells. kSdfHalfChunks is a
  constant in VoxelResources.hpp (bump it to grow the box). The box covers
  the near surface where the side edges are visible; leaving the box = open
  space (casters that reach the sun are within it).
- BUILD (background thread, launchSdfBuild in VulkanRenderer.cpp): called
  from finishRegionMove (the region is complete, so the world chunks under
  the box are installed). It SNAPSHOTS the 36 chunks' voxel types on the
  render thread (the only thread that mutates the world chunk map - the
  worker would race it), then a background thread runs vv::voxel::SdfField
  (the pass-37 CPU reference, chamfer EDT + argmin seed) over the snapshot.
  The nearest-solid-cell seed per cell (0xFFFFFFFF = no solid in view) is
  the uploaded field - the EXACT field the CPU test pins, so the GPU sphere
  trace is a byte-for-byte parity of SdfField.
- UPLOAD (ensureSdfField, called from updateWorld every frame): joins a
  finished build, uploads the seeds (fence-scoped copy, ~19 MB) and PUBLISHES
  the box geometry (origin in world voxels + dims) via a small host-visible
  uniform (binding 13) AFTER the copy lands - so a frame that sees active=1
  always sees the matching field. The box uniform is written on the render
  thread, so it gets a host-write -> shader-read barrier (like the fade
  buffer); the SDF storage buffer (binding 12) needs none (its upload is
  fence-scoped).
- SHADER (pixels_rgba.comp): binding 12 = the seed storage buffer, binding
  13 = the SdfBox uniform (box.xyz origin in world voxels + w active flag,
  dims.xyz cells). sampleSdf3d(p) mirrors SdfField::sample exactly (min over
  the 3x3x3 seed cells of the L2 distance to each seed's solid CUBE - 0
  inside the solid); sunRayEscapesSdf3d mirrors SdfField::sphereTracedShadow
  exactly (160 steps, step max(0.7h, 0.05), fold k*h/t, h < 1e-3 -> hit,
  leaving the box -> open space). sunShadow dispatches to sunRayEscapesSdf3d
  when sdfBox.box.w >= 0, else the pass-34 2.5D top-plane penumbra
  (sunRayEscapesSdf) - the fallback until the first build lands.

COST: the build is ~70-350 ms on the BACKGROUND thread (amortized over the
region lifetime; competes with the generation workers). The upload is a
~10-30 ms hitch ONCE per region change (the 19 MB copy; the region change
is already a hitch). The per-frame cost is one atomic load (the no-op
ensureSdfField check) + the sphere trace in the SDF branch (27 seed reads +
27 cube distances per sample, 160 steps - the same budget the 2.5D path
already pays per column).

ACCEPTANCE (on-device, VV_SDF_SHADOWS=1): the shadow edges running along
vertical/steep casters (the pass-34 hard side edges) are now SOFT and
continuous, matching the top-edge penumbra; the overhang underside stays at
most half-lit; NO light leak (dark-side stays dark); the binary
sunRayEscapes (VV_SDF_SHADOWS=0) is UNTOUCHED (bit-identical); the 2.5D
fallback (before the first build lands) is unchanged.

## Pass 39: FIX - the SDF box was built from scrambled voxels

OWNER REPORT (VV_SDF_SHADOWS=1): "the region around camera is currently fully
shadowed. leftmost edge shows some kind of shadow though, which looks correct
but has bands."

ROOT CAUSE: the box build's chunk-layout walk used the WRONG Z STRIDE. The
renderer indexed each box voxel into its chunk snapshot as

    x + y*chunkSizeX + z*chunkSizeX*chunkSizeZ      (pass 38, WRONG)

while the chunk layout (Chunk::index, the terrain generator's fill, and the
shader's fetchVoxel) is

    x + y*sizeX + z*sizeX*worldHeight               (the sync contract)

With the default config (chunkSizeX/Z = 32, worldHeight = 128) that is a
1024-word stride where the chunks lay out 4096: the SDF was built from a
SCRAMBLED projection of the terrain, not the terrain. Only the box's z slices
with local z == 0 (a 32-voxel-periodic set of 1-voxel-thick slices) happened to
be indexed correctly - which is exactly why a few shadow shapes "looked
correct" while everything around them banded.

MEASURED (probe + test, real terrain seed 1337, the renderer's 6x6-chunk box):
- pass-38 stride: 2,068,241 of 4,718,592 box cells disagree with the chunk
  voxels they claim to describe, and the 400 sampled ground points render
  400/400 FULLY SHADOWED (mean soft visibility 0.000) although the exact
  binary march calls 350 of them lit -> the owner's screenshot.
- fixed stride: 0 cell mismatches; of the 400 samples 350 are exactly lit
  (274 stay > 0.5 lit; 58 fall fully into the k*h/t penumbra - the documented
  over-darkening of LIT penumbra pixels the owner chose over the pass-35/36
  "bright spots"), the 50 exactly-dark ones are all dark, 0 light leaks,
  mean soft 0.695.

FIX (one problem, one pass):
- The box geometry + the snapshot -> field walk moved out of the renderer into
  src/voxel/SdfBox.hpp (SdfBoxGeometry::centeredOn, sdfBoxCellSolid,
  buildSdfBoxField), where the voxel -> chunk mapping uses the REAL layout
  (x + y*chunkSizeX + z*chunkSizeX*worldHeight) and out-of-range/foreign-sized
  snapshots read as air. VulkanRenderer::launchSdfBuild and ensureSdfField now
  use the same SdfBoxGeometry for the constants they have to agree on (dims,
  origin, chunk order), plus a validity guard.
- The shader is NOT touched: sampleSdf3d/sunRayEscapesSdf3d already mirror
  SdfField and take box-local coordinates correctly (world - box.xyz), and the
  seed/box-uniform contract is unchanged.
- New CPU test testSdfBoxBuild pins the walk: (1) the field's zero-distance
  cells must be EXACTLY the chunks' solid voxels (the pass-38 stride gives
  2.07M mismatches -> FAIL), and (2) on real terrain most exactly-lit ground
  must stay lit with no light leak (the pass-38 stride gave 0 of 400 -> FAIL).

WHY THE PASS-38 CPU TEST DID NOT CATCH IT: testSdfSoftShadow3d builds its SDF
straight from its own cell array (no snapshot, no chunk strides), so it never
exercised the box build's index math; the new test drives the real snapshot
path (and would have failed on the old code - verified by re-running it).

ALSO MEASURED, NOT CHANGED (parked):
- BANDING is not a property of the march: on a clean vertical caster the
  sphere trace's penumbra is smooth (max adjacent step 0.079 over 0.25 voxels,
  against the 64-ray sun-disc truth's 0.078). The bands in the report were the
  scrambled field's 32-voxel periodicity.
- BOX COVERAGE: the shader still treats "left the box" as open space, and the
  box (6x6 chunks = 192 voxels) is much smaller than the visible region (25x25
  chunks), so a caster outside the box cannot shadow a point inside it. At the
  game's sun (elevation 54.7 deg) a ray that clears the terrain inside the box
  has already risen above every possible caster before it exits: 0 of the 977
  shadowed ground columns sampled in the box had their blocker outside the box
  footprint, and 0 trace leaks were found - so the box size is currently
  sufficient. It would NOT be for a much lower sun (no day/night cycle today).
  If a shadow cutoff is ever seen at the box faces, the fix is to hand the
  march over to the 2.5D column traversal on leaving the box (thread t0 and
  the accumulated visibility into sunRayEscapesSdf) instead of breaking.
  SUPERSEDED by pass 40 below: that hand-off is now in, for the other half
  of the problem (surfaces outside the box).

## Pass 40: the box edge was open space - hand the ray over instead

FOUND WHILE VERIFYING PASS 39 (first-person view probe, real terrain, seed
1337, 120x40 camera rays): the stride fix removed the whole-region shadowing,
but 53 of the 2946 hit pixels still rendered fully lit where the exact march
is shadowed. All 53 are pixels whose own surface lies OUTSIDE the 6x6-chunk
box (0 of the 2385 in-box surfaces leak): for them the old sunRayEscapesSdf3d
sampled once, found the sample outside the box and returned "open space" - no
SDF shadow at all. Note it is the other half of the box question the pass-39
parked note measured: a surface INSIDE the box blocked by a caster OUTSIDE it
still does not happen at this sun (0 of 2946).

FIX (one problem, one pass): leaving the box now CONTINUES the ray in world
space. sunRayEscapesSdf3d hands over to the 2.5D column traversal
(sunRayEscapesSdf - the whole near region plus far cells, exactly what shadows
the frame until the first box lands) with
- the hand-off point ON the box crossing (analytic slab exit; the origin
  itself when it already starts outside). Handing over at the first SAMPLE
  past the box instead would skip the strip between the last in-box sample
  and the boundary - a march step is up to 0.7 * h, so a caster just outside
  the face could hide in it;
- startT = the distance already marched, so the k*h/t estimate keeps the true
  distance from the shaded surface (penumbra width must not restart);
- startVisibility = the visibility the field accumulated, so both traversals
  fold into one min (never brighter than either alone).
sunRayEscapesSdf now takes (startT, startVisibility) and runs positions off a
local t; with (0, 1) it is bit-identical to the old fallback, and a 160-step
budget that runs out while still inside the box hands over the same way. CPU
mirrors: SdfField::sphereTracedShadowExits reports the crossing point, and the
test's sunRayEscapesSdfMirror takes the same two seed arguments.

MEASURED (probe7 = probes/probe_view.cpp, exact 0.25-step march as the oracle,
output /tmp/probe7.txt; "leaks" = lit where the exact march is dark,
"over-dark" = dark where it is lit):

    pass-38 walk + old shader          53 leaks   2241 over-dark
    pass-39 walk + old shader          53 leaks     21 over-dark
    pass-39 walk + pass-40 hand-off     0 leaks     21 over-dark

The 53 leaks were 0 at in-box surfaces and 53 at out-of-box surfaces; the
hand-off removes all of them. The 21 remaining over-dark pixels are the
pass-34 k*h/t penumbra on exactly-lit penumbra pixels (owner-accepted over the
pass-35/36 "bright spots").

TEST: testSdfBoxHandoff pins it against a field covering only x < 16 with the
caster outside it: 82 shadowed / 78 lit columns, the field alone leaks 56, the
hand-off leaks 0, and outside the field the composite equals the plain 2.5D
march exactly. Leak judging is restricted to columns where real voxels block
the ray (79 of 82; the other 3 are height-field skims - the exact mirror's
height model blocks a ray that merely grazes a column top, which the
cell-accurate field march correctly does not). Full suite green; the exact
binary path (VV_SDF_SHADOWS=0) is untouched and the pre-build 2.5D fallback
call passes (0, 1), so its output is unchanged.

VERIFIED IN THE SANDBOX (toolchain: scripts/build-linux-toolchain.sh under
/tmp/deps): glslangValidator -V resources/shaders/pixels_rgba.comp exits 0
(the hand-off compiles for real), and the project builds warning-free in both
build/release and build/debug (the first in-tree build of SdfBox.hpp and the
renderer's box geometry logging) with ctest green in both and the offscreen
smoke run (QT_QPA_PLATFORM=offscreen) exiting cleanly. On-device rendering is
still the owner's gate.

## Pass 42: the box and the seeds are a PAIR - handover order + no frame stall

OWNER, after pass 41 shipped: "flicker is still there, so i don't know what you
fixed. it looks like it only exaggerated shadow bands. by flicker i meant that
chunks become dark for one frame just before sdf shadows become visible. so i'd
revert and target that + latency."

Pass 41 was reverted first (its own commit): the fixed 64-voxel hand-off made
the field-to-2.5D model switch land ~26 voxels of horizontal travel from the
shaded surface, which reads as harder bands exactly where pass 40 looked soft.
The 8x8 window only bought value-accuracy the symptom never needed. The probes
are kept (tracked) and the measurement is recorded in the revert message.

WHAT WAS ACTUALLY WRONG - three things about WHEN the field is allowed to move.
None of them is the shadow *shape*; all of them are the *handover*:

1. THE BOX AND THE SEEDS ARE A PAIR. The shader resolves "which cell is this
   point in" from the box uniform and indexes the seed buffer with it. A single
   pair cannot be swapped in place while a dispatch that resolved cells against
   the OLD box is still executing, and the GPU reads the box at EXECUTION time,
   not at record time - so there is no moment where rewriting the box in place
   is free. Pass 40 dealt with that by waiting for the copy's fence inside the
   frame (the copy is queued behind those frames, so waiting means they have
   retired); that is correct, and it is also a hitch of a 75 MB host->device
   copy (18.9 M cells x 4 B) plus a queue wait - the owner's "big latency when
   the SDFs get recomputed".

2. THE PUBLISH ORDER INSIDE THE BOX. writeSdfBox wrote origin, then the active
   word, then the dims. A dispatch that caught the write could therefore see
   box.w = 1 (active) next to dims of 0 - the shader then rejects every cell
   and the ray "escapes", i.e. the frame it lands in shades differently from
   both the old and the new field.

3. THE DROPPED LAUNCH. launchSdfBuild early-returns while a build is running,
   and the only call site was finishRegionMove. A region move that completed
   while a build was running lost its launch COMPLETELY: the field stayed
   centered on the chunk the camera had already left until the NEXT region move
   (a whole crossing later - up to a second of streaming, plus the build). For
   that whole time the chunks that had just streamed in were outside the live
   box, so they kept the hard 2.5D shadow look, and when the field finally
   caught up the SDF shadows "appeared".

FIX (one pass, one problem: "the field must change only as a consistent pair,
and it must not cost the frame or lag the camera"):

- The seed buffer is double-buffered (kSdfHalves = 2 copies of the field) and
  the uniform says which half to read (dims.w, the shader's seed base). The
  copy targets the SPARE half, so the live half keeps holding exactly the seeds
  its box describes, and the pairing is always (old box, old half) or (new box,
  new half) - never a mix.
- beginSdfUpload submits the copy and RETURNS; sdfUploadComplete() polls the
  fence. The frame publishes the box in a later frame, once the fence has
  signalled: the copy was queued behind every frame that read the old box, so
  by then none of them can be executing. No wait, no stall, and the invariant
  is the same one pass 40 got by waiting.
- writeSdfBox writes the payload (origin, dims, half index) first and the
  active word last, behind a release fence.
- The retry loop: vv::voxel::SdfHandover (voxel/SdfHandover.hpp) is the step
  decision - join+submit, publish when the copy lands, and RELAUNCH while the
  field does not cover the camera's chunk. finishRegionMove (and the
  synchronous region path) now only record which chunk the field should cover;
  ensureSdfField drives the policy every frame. A swallowed launch is retried
  the moment the pipeline is idle, so the soft shadows follow the camera
  instead of waiting for the next crossing.

TEST: testSdfHandoverPolicy walks the state machine: no build before a region
move has been seen; the first region starts one; a running build is waited for,
never duplicated; a finished build is picked up; the box is NOT published while
the copy is in flight and IS published once it lands; the copy targets the
spare half and the halves alternate; a swallowed launch is retried; a field
that covers the camera is not rebuilt. The CPU field/pack layout contracts stay
pinned by testSdfBoxBuild / testSdfBoxHandoff / testSdfSoftShadow3d.

NOT PROVEN HERE: the sandbox has no Vulkan device, so the frame-level handover
is reasoned (single-queue ordering + the polling predicate) and built, not run.
If the dark frame survives this pass, the next suspects are the STREAMING path
rather than the SDF: the triple-buffered chunk table can be published twice in
one frame (the incremental pump publish plus the final publish in
finishRegionMove advance the half index twice), the slot cooldown guard is
frame-counted rather than fence-counted, and the fade-in compositing of a
newly installed chunk shows whatever is behind it. The diagnostic that splits
these: does the dark frame also happen with VV_SDF_SHADOWS=0 (streaming, i.e.
not the SDF at all), and is it the whole screen or a chunk-wide patch.

VERIFIED IN THE SANDBOX (deps /home/user/.cache/vv-deps): build/release and
build/debug build warning-free, ctest green in both, glslangValidator -V
resources/shaders/pixels_rgba.comp exits 0, the offscreen smoke run exits
cleanly, and the suite prints the new
`sdf handover: box/seed pairing pinned either side of the copy ...` line.
On-device rendering is the owner's gate.

## Pass 43: Qt out, GLFW in - the window is not a toolkit's job

OWNER: "the goal now is, swap qt to glfw completely, eliminate qt dependency."
Reasons given: Qt is a heavy build for what this uses, it does not do
cross-platform keyboard-layout key detection, and it carries Linux quirks.
Decisions taken with the owner before the work: GLFW comes from the system
(GitHub only as the pinned fallback), `stb_image.h` replaces QImage for the
texture PNGs, init failures print to stderr and exit non-zero instead of a
QMessageBox, and the movement keys are bound by physical position with the
layout key accepted on top.

WHAT WENT AWAY: `src/ui/` (AppWindow, VulkanWidget), main.cpp's QApplication,
QtNativeWindowResolver, every `#include <Q...>`, AUTOMOC/AUTORCC/AUTOUIC, the
Qt6 find_package, the QPA private-header probing (including the "install
libQt6GuiPrivate or use XWayland" story), and `qt_generate_deploy_app_script` -
a package is now just the executable, the SPIR-V and the optional textures.

THE HOST, in one place. `vv::core::GameWindow` owns the OS window (GLFW init,
hints, size in PHYSICAL pixels, mouse-lock, raw callbacks) and `vv::core::App`
owns everything else: the renderer, the camera, the frame loop and the input
state. The Qt version had three objects (AppWindow, VulkanWidget, QTimer) doing
that, a `paintEngine()` override to keep the toolkit off the surface, an event
filter just to defer a mouse grab until the window was exposed, and a 0 ms
QTimer as the frame clock. The loop is now explicit: wait for events (so an
unsynced frame loop does not starve the compositor), skip the frame while
iconified, tick, present.

PLATFORM HANDLES come from GLFW's own accessors rather than from the toolkit:
`glfwGetWin32Window` (+ GWLP_HINSTANCE for the HINSTANCE), `glfwGetX11Display`
/ `glfwGetX11Window`, `glfwGetWaylandDisplay` / `glfwGetWaylandWindow`, and
`glfwGetCocoaView`. Two consequences:

* The X11 Vulkan surface is now created with VK_KHR_xlib_surface from the
  Display*/Window pair (pass 42 used XCB with the Qt QPA connection). GLFW
  exposes the Xlib connection, and libX11-xcb would have been one more
  dependency for nothing. `NativeWindowKind::Xcb` became `::X11`.
* Which backends exist is a property of the GLFW library, so the CMake side
  sets VV_WINDOW_BACKEND_X11 / _WAYLAND from how GLFW was built and the code
  compiles only what can exist (plus clear errors for the ones that cannot).
  `-DVV_GLFW_NULL_ONLY=ON` builds GLFW's null backend: that is how the sandbox
  (no X11/Wayland headers, no display) compiles, links and runs everything up
  to "unsupported platform", which is exactly the message it should print.
  `VV_PLATFORM=null` selects that platform explicitly - the null backend is
  never auto-selected, and it is the headless smoke test the toolchain script
  ends with.

INPUT: the keyboard problem the owner named. A key event now carries the
layout key AND the physical scancode, and `vv::core::InputBindings` matches
either (position first, label second): WASD therefore keeps working on AZERTY
(ZQSD), QWERTZ and Dvorak, where the labels move but the positions do not. The
scancode table is per platform - evdev on X11/Wayland, Set-1 make codes on
Windows, Carbon virtual key codes on macOS - and it is unit tested
(testKeyBindings), including the AZERTY/QWERTZ cases, the "unbound label does
not match someone else's position" case and the label-only fallback for
platforms without scancodes. The startup log prints one line per action with
the key number, the layout name `glfwGetKeyName` reports and the scancode: a
"the keyboard behaves differently here" report now carries the numbers.

MOUSE: GLFW_CURSOR_DISABLED replaces the Qt mouse grab. GLFW hides the cursor
and reports it at the window centre, so the camera reads relative deltas (the
first sample after a lock is dropped, not treated as motion), losing focus
drops the lock and the held keys, Escape releases the cursor and pauses, a left
click re-locks.

TEXTURES: `src/render/ImageDecode.cpp` is the one translation unit that
instantiates `third_party/stb_image.h` (STBI_ONLY_PNG - the loader only ever
asks for .png - and STBI_NO_STDIO, so the file is read through std::ifstream
and non-ASCII paths keep working on Windows). testImageDecode writes a PNG byte
by byte in the test (uncompressed deflate, CRC32 and Adler-32 computed there,
one row using the Up filter) and pins the decoded RGBA8 exactly, plus the
missing-file and not-an-image failures.

ERRORS: no toolkit dialog exists any more. A failure prints `[vv] fatal: ...`
on stderr and exits 1; the device-lost path prints the renderer's message once,
releases the mouse, pauses and keeps the (empty) window open so the message can
be read. Device-lost used to be a modal QMessageBox.

VERIFIED IN THE SANDBOX: build/release and build/debug configure and build
warning-free with GLFW 3.5.1 built null-only (`-DVV_GLFW_NULL_ONLY=ON`), ctest
green, and the headless run

  VV_PLATFORM=null LD_LIBRARY_PATH=... timeout 20 ./build/release/bin/game

creates the window, prints the detected platform and size, resolves the native
handles, fails with the documented message and exits 1 without crashing. The
test suite prints the two new lines:

  key bindings: 7 actions, positions matched by scancode (AZERTY/QWERTZ safe),
  labels accepted on top
  image decode: 3x2 RGBA8 reproduced byte-exact (filters none+up),
  missing/garbage rejected

NOT PROVEN HERE: the sandbox has no GPU, no X server and no Wayland compositor,
so the X11 (Xlib) and Wayland surface paths, the Win32/macOS ones, real
keyboard scancodes and the GLFW window behaviour itself are reasoned from
GLFW's documented semantics and built, not run. The owner's gates: a real
window appears on the target machine (X11 and Wayland), the WASD keys work on
the owner's layout, the mouse lock behaves (Escape, alt-tab, click to resume),
and `VV_SDF_SHADOWS=1` still looks as it did in pass 42.

## Pass 44: maximized by default, half the monitor when un-maximized

OWNER (on-device, after pass 43): "pretty sure window is scaled to monitor size
(can't even see the toolbar), it instead should be 2x less monitor size (when
not maximized) + maximized by default."

Pass 43 created a BORDERLESS window sized to the monitor's work area and placed
it at the work area origin. On the owner's desktop that covered the panel
("can't even see the toolbar") and read as fullscreen, because a client that
sizes itself to the work area still gets what it asked for on several window
managers - and on any WM that ignores the request the borderless window ends up
whatever the WM decides. Hand-rolling "maximized" was the mistake: the work
area, the panel, dock autohide and decoration geometry are window-manager
policy, and a client cannot reimplement them portably.

FIX (GameWindow::init):

* Hints: decorated (the default - the borderless hint is gone) and
  GLFW_VISIBLE = false.
* The window is created at HALF the monitor's work area (the video mode when
  there is no work area, 1280x720 if there is no monitor at all), clamped to the
  640x400 minimum, and centred in the work area. This is the size the user gets
  back on un-maximize.
* The maximize state goes through GLFW's OWN create path: the
  GLFW_MAXIMIZED hint, with the window still hidden and glfwShowWindow() last.
  That ordering is the point, and it was checked in the GLFW 3.5.1 sources for
  every backend this supports - x11_window.c sets _NET_WM_STATE before the
  window is mapped, wl_window.c stores wl.maximized and calls
  xdg_toplevel_set_maximized when the (deferred) toplevel is created,
  win32_window.c creates the window with WS_MAXIMIZE (and has a
  maximizeWindowManually path for the hidden case), cocoa_window.m zooms at
  creation - so the window comes up maximized with no flash of the restored
  size, and the size the client asked for is what the WM treats as the geometry
  to restore to. glfwMaximizeWindow() would have taken the "window already
  exists" path in each backend and depended on the same source facts less
  directly.
* glfwSetWindowPos is skipped on Wayland, which does not let a client place its
  windows and reports the attempt as GLFW_FEATURE_UNAVAILABLE (that error would
  otherwise print through the GLFW error callback on every start). The
  compositor places the window; it is maximized a moment later anyway.

The startup log now says both sizes:

  [vv] window: 1920x1040 window pixels -> 1920x1040 framebuffer pixels
               (content scale 1.00x1.00, platform 393221)
  [vv] window: maximized (work area 1920x1040, restores to 960x520),
               decorations on

which also makes the debug title (build id + fps) visible for the first time -
the pass-43 borderless window had no title bar to show it in.

NOT PROVEN HERE: the sandbox has no window manager, so the maximize request, the
restore geometry and the panel behavior are reasoned from GLFW's documented
semantics plus its X11/Wayland source (checked: x11_window.c sets
_NET_WM_STATE on an unmapped window; wl_window.c applies wl.maximized when the
toplevel is created) and built, not run. Owner check: the window comes up
maximized with the panel visible, and un-maximizing gives a window about half
the screen, centred.

## Pass 45: the launch frame is the window's real size

OWNER (on-device, after pass 44): "it's working but it glitches every launch. as
soon as i grab topbar and start moving window, it resolves." The maximize /
half-monitor behavior of pass 44 is accepted; the per-launch glitch is not.

DIAGNOSIS. Two size-related facts met at launch:

* GLFW's window size is the CACHED size it last got from the window manager
  (`glfwGetFramebufferSize` returns `window->x11.width/height` on X11 and
  `window->wl.fbWidth/fbHeight` on Wayland - both are written only when an event
  is processed, x11_window.c ConfigureNotify handler / wl_window.c
  handleToplevelConfigure -> resizeWindow). Pass 44 showed the window and read
  the size immediately, without ever pumping events, so the renderer was created
  for the RESTORE size (half the monitor) while the window manager was already
  maximizing the window. The maximize happens pre-map (that is what the
  GLFW_MAXIMIZED hint buys), so its ConfigureNotify /
  xdg_toplevel configure arrives right after the map request - and nothing had
  read it yet.
* The frame paths in VulkanRenderer::drawFrame recreated the swapchain with
  `m_swapchainExtent` - the extension of the swapchain being replaced, i.e. the
  size we are trying to leave - instead of the window size (drawFrame acquire
  OUT_OF_DATE / present OUT_OF_DATE|SUBOPTIMAL|m_framebufferResized). On X11 the
  driver's `currentExtent` usually overrides the value passed to
  chooseSwapExtent, which hides the mistake; where the surface reports the
  special `(UINT32_MAX, UINT32_MAX)` extent (Wayland) the passed value wins, so
  a recreate could rebuild at the OLD size.

Moving the title bar delivers a fresh configure to the client, which is exactly
why the artifact cleared "as soon as" the owner touched the window.

FIX.

* `GameWindow::settleFramebufferSize(maxWaitSeconds = 0.25)`, called once in
  App::init between `GameWindow::init()` (which shows the window) and
  `resolveNativeWindow()`: pumps `glfwWaitEventsTimeout(10 ms)` until the
  framebuffer size is stable (three identical samples AND at least 30 ms of wall
  time, so a burst of events cannot fake a settled size) or the budget runs out,
  and keeps m_framebufferWidth/Height current on the way. The swapchain is then
  created for the size the window manager itself reported. It logs the answer
  when it changes the size:
  `[vv] window: settled to 1920x1040 framebuffer pixels after 20 ms (the
  maximize request was answered by the window manager)`.
* Size reconciliation is one code path, not a callback-only hope:
  `App::syncRendererSize(width, height)` is called from the framebuffer-size
  hook AND from the frame loop right before `drawFrame()`, and it is a no-op
  until the requested size actually differs from the last one the renderer was
  given (so a callback and the frame check in the same frame cannot rebuild the
  swapchain twice). The frame-loop call runs after `waitEvents()`, i.e. after
  the pending configure has been delivered, so the first frame presented after
  the answer is already correct - with no user action.
* `VulkanRenderer` remembers the window size it was asked for
  (`m_requestedWidth/Height`, set by init()/resize(), floored at 1) and the two
  drawFrame recreate paths use that instead of the previous extent.
  `resize()` clears `m_framebufferResized` when its own rebuild succeeded, so a
  resize no longer costs a second redundant rebuild at present time (the flag
  still stays set when the rebuild failed, so the present path retries).
* A recreate that fails leaves no swapchain; `drawFrame()` now recreates once
  more instead of acquiring from VK_NULL_HANDLE. The frame loop keeps pumping
  events while this retries, so a window that is being resized or un-minimized
  recovers by itself.
* Diagnostics, all on stderr: the launch line prints the swapchain extent next
  to the window's (`[vv] swapchain: 1920x1040 (window reports 1920x1040)`), each
  change prints `[vv] swapchain: A x B -> C x D` followed by
  `[vv] swapchain: now C x D (window C x D)` - so a report that still shows an
  artifact carries the numbers that identify it.

VERIFIED IN THE SANDBOX (deps /home/user/.cache/vv-deps, GLFW 3.5.1 null-only):
build/release and build/debug warning-free; ctest green in both (5.1 s / 24.4 s);
glslangValidator -V exit 0; `VV_PLATFORM=null` smoke prints the two pass-44
window lines and exits 1 through the "no native window handle" path in 34 ms
(the settle costs its 30 ms floor and does not burn the 250 ms budget when the
size never changes). The sandbox has no window manager, so the settle loop was
driven directly by a probe (not committed) against GLFW's null backend, with the
real GameWindow.cpp: (a) nothing changes -> returns in 30 ms at the same size;
(b) window already resized -> reports the new size (1600x900); (c) budgets of
50 ms and 200 ms -> both return in 30 ms; (d) a resize arriving 3 ms into the
settle -> detected in-loop, exits 30 ms later, logs "settled to 1280x1024 ...
after 30 ms", reports the new size; (e) an answer arriving 80 ms AFTER the
settle returned (budget 50 ms) -> the framebuffer-size hook still records it
(1440x900), which is what the frame-loop reconciliation keys off. Probe verdict:
PROBE OK, exit 0.

NOT PROVEN HERE: the real maximize handshake (X11 ConfigureNotify / Wayland
configure) and the visual result - the sandbox has no WM and no GPU. Owner
check: launch looks right from the first frame (no glitch to clear by moving the
window) while the window still comes up maximized and un-maximizes to half the
monitor, on both X11 and Wayland. If an artifact survives, the startup log now
carries the window size, the settled size and the swapchain extent.

## Pass 46: maximize goes through the window manager, not around it

OWNER (on-device, Windows/Win32, after pass 45): "still behaves the same, at
least on windows. it looks like it maximizes, but does not get centered to
screen, instead the top left corner of the window sits in center." With the
log:

  [vv] window: 1920x1009 window pixels -> 1920x1009 framebuffer pixels
               (content scale 1.00x1.00, platform 393217)
  [vv] window: maximized (work area 1920x1032, restores to 960x516),
               decorations on
  [vv] window backend: Win32
  ...
  [vv] swapchain: 1920x1009 (window reports 1920x1009)

Read it carefully, because it splits pass 45's problem in two: the SIZE is
right from the first frame (no "settled to ..." line - the size was already the
maximized one - and the swapchain matches the window), so pass 45's
size/reconciliation work did its job. What is wrong is the PLACEMENT: 1920x1009
of client area against a 1920x1032 work area is a caption-height difference,
i.e. a properly maximized decorated window, but the window's origin is not the
work area origin.

ROOT CAUSE (GLFW 3.5.1 win32_window.c). The GLFW_MAXIMIZED create hint does not
only record a wish: it puts `WS_MAXIMIZE` into the window style at creation
(:1356-1360) and `maximizeWindowManually()` (:481) then sizes the window to the
monitor work area with a SetWindowPos. Windows treats a window created with
WS_MAXIMIZE as maximized from that moment. The pass-44/45 code then ran
`glfwSetWindowPos()` to centre the window in the work area - and that is
`SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOACTIVATE | SWP_NOZORDER |
SWP_NOSIZE)` (:1635) on an ALREADY MAXIMIZED window. A move of a maximized
window keeps its size (hence 1920x1009, "it looks like it maximizes") but its
origin becomes the value passed - half the work area in from the corner, i.e.
"the top left corner of the window sits in center". This is Win32-specific in
symptom (X11 and Wayland apply placement at map time and their window managers
own the maximized rectangle) and unavoidable with the create-hint ordering: the
hint applies the maximize before the app can place the window.

FIX. Placement first, maximize second, both while the window is still hidden:

* The GLFW_MAXIMIZED hint is gone (`GLFW_VISIBLE = false` stays). The window is
  created at the centred restore size, `glfwSetWindowPos()` places it (still
  skipped on Wayland, where the platform reports placement as unavailable), and
  `glfwMaximizeWindow()` is called AFTERWARDS, before `glfwShowWindow()`. Each
  backend then takes its pre-map path: Win32 `maximizeWindowManually()`
  computes the work-area rect itself and only moves/sizes a window that is NOT
  maximized, X11 appends `_NET_WM_STATE_MAXIMIZED_{HORZ,VERT}` to the unmapped
  window (`_glfwMaximizeWindowX11` -> the `!visible` branch), Wayland records
  the state for the toplevel it creates on show, Cocoa zooms. Nothing moves a
  maximized window any more, the maximized rectangle is the window manager's
  policy (work area, panels, decoration) and the centred half-monitor rect
  stays the geometry the window un-maximizes to.
* `GameWindow::verifyMaximizedPlacement()` runs on Win32 only, while the window
  is still hidden: the content area must be at the work area origin (64 px of
  slack for caption/frame, against an error of half the screen), and if it is
  not, the sequence is re-applied - restore, move to the centred rect, maximize
  - exactly what the user does by hand by grabbing the title bar. This is a
  safety net, not the fix; it also exists because on Win32 the position query is
  live for a hidden window (ClientToScreen), while X11 would report the position
  this process requested and Wayland does not report one at all. It does NOT
  gate on GLFW_MAXIMIZED: on Win32 that flag is driven by WM_SIZE messages and
  is therefore still false for a window that has never been shown, even though
  `maximizeWindowManually()` has already applied the maximize.
* One more diagnostic line, printed by `settleFramebufferSize()` after the
  window manager has had its say:

  `[vv] window: 1920x1009 framebuffer pixels, maximized yes, content at 0,0`
  (on Wayland: `... maximized yes (Wayland does not report the position)`).

  A launch-window report from any machine now carries the size the frames use,
  whether the window reports itself maximized, and where its content sits.

VERIFIED IN THE SANDBOX (deps /home/user/.cache/vv-deps rebuilt after an
environment reset wiped it and build/: configure + build clean for Release and
Debug, no warnings; ctest green in both, 5.8 s / 25.5 s; glslangValidator -V
exit 0). `VV_PLATFORM=null` smoke: the null backend applies the synchronous
maximize through the new call order and reports itself maximized
(`[vv] window: 960x400 framebuffer pixels, maximized yes, content at 480,277` -
480,277 is exactly the centred restore rect in the null monitor's work area),
then exits 1 through the documented "no native window handle" path. There is no
window manager in the sandbox, so the Win32 branch itself is reasoned from the
GLFW source above and cannot be run here.

NOT PROVEN HERE: the real Win32 maximize/placement. Owner check: launch on
Windows shows a maximized window filling the screen from the top-left of the
work area (no half-screen offset), un-maximizing gives the centred half-monitor
window, and the new log line says `maximized yes` with `content at` the work
area origin. If the offset is still there, the new line says where the window
is and whether Windows agrees it is maximized - and the "re-applying the
placement" line says whether the safety net had to step in.

## Pass 47: the validation layer is quiet (and on by default for debug runs)

OWNER: "next thing to work on is vulkan validation messages fixing. and enable
VV_VALIDATION through run_debug.bat by default." Five distinct messages, five
distinct root causes; the fixes are all in the barrier/queue-submit plumbing.

1. `pImageMemoryBarriers[0].srcAccessMask (VK_ACCESS_TRANSFER_WRITE_BIT) is not
   supported by stage mask (VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)` (and its
   `dstAccessMask ... SHADER_READ ... TRANSFER` twin), VUID 02819/02820.
   `VoxelResources::createVoxelTextures` built its image transitions with a
   hard-coded `TOP_OF_PIPE -> TRANSFER` pair while the caller passed access
   masks for other stages: the mip chain transitions have a transfer WRITE to
   flush (srcStage must be TRANSFER, not TOP_OF_PIPE) and the final transition
   hands the image to the compute stage (dstStage must be COMPUTE_SHADER, not
   TRANSFER). The stage masks are parameters of the lambda now - TOP_OF_PIPE
   only for the layout-only first transition, TRANSFER for the mip barriers,
   COMPUTE_SHADER for the final one.

2. `pBufferMemoryBarriers[2] VkBuffer ... has a size of 0`, VUID 01188.
   `preComputeBarriers[2]` (the compute output buffer, TRANSFER_READ ->
   SHADER_WRITE) was the only barrier in that array whose `offset`/`size` were
   left at the zero-initialized defaults, i.e. an empty range. It now says
   `offset = 0; size = VK_WHOLE_SIZE;` like its four neighbours.

3. `vkQueueSubmit(): (VkFence ...) submitted in SIGNALED state`, VUID 00063.
   The upload fences are created with `VK_FENCE_CREATE_SIGNALED_BIT` (so the
   first wait in a slot returns immediately) but were only reset *inside* the
   `if (pending)` branch: the FIRST submit in each slot therefore went in on a
   signaled fence - two stream slots at startup, and the same for the far and
   SDF upload paths on the runs that use them. Every submit site now resets its
   fence immediately before `vkQueueSubmit` (four sites: `uploadChunksStreaming`,
   `uploadFarFieldHalf`, `uploadFarFieldDelta`, `beginSdfUpload`), and
   `waitPreviousFarUpload()` waits only - resetting moved to the submit that
   owns the fence. `vkResetFences` on an already-unsignaled fence is a no-op, so
   the unconditional reset is safe in every path.

4. `vkQueueSubmit(): pSubmits[0].pSignalSemaphores[0] ... is being signaled ...
   but it may still be in use by VkSwapchainKHR ... Swapchain image N was
   presented but was not re-acquired`, VUID 00067. The render-finished semaphore
   was per frame in flight (`m_currentFrame`), but the present operation that
   waits on it is not gated by the in-flight fence - presenting only guarantees
   that an image is free when that image is acquired again. The semaphores are
   now per SWAPCHAIN IMAGE (`m_renderFinishedSemaphores[imageIndex]`), created
   by `createPresentSemaphores()` from `createSwapchain()` so the array always
   matches the images in use (the old ones are destroyed rather than kept: a
   present that returned OUT_OF_DATE/SUBOPTIMAL leaves its semaphore signaled
   with nobody waiting on it, and a fresh frame must never signal a signaled
   binary semaphore). The acquire semaphores stay per frame in flight - those
   *are* gated by the in-flight fence. `cleanup()` now destroys the two
   different-sized arrays in separate loops (the old single loop indexed the
   present semaphores by the frame-in-flight count).

5. Same class, not reported because it needs synchronization validation:
   the acquire semaphore's `pWaitDstStageMask` was `COMPUTE_SHADER` only, while
   the submission also transitions and writes the swapchain image in the
   TRANSFER stage. It is `COMPUTE_SHADER | TRANSFER` now, so the acquire
   actually orders every stage that touches the image it hands over.

`run_debug.bat` sets `VV_VALIDATION=1` before launching
`build/debug/bin/game.exe`, so the layer is on for debug runs by default (the
app already switches it on when that variable is present, prints a note when the
layer is missing and keeps going). README documents the variable for other runs.

VERIFIED IN THE SANDBOX (deps /home/user/.cache/vv-deps): Release and Debug
configure and build warning-free, ctest green in both (5.1 s / 23.2 s),
`glslangValidator -V` exit 0, and the `VV_PLATFORM=null` smoke run is unchanged
(two window lines + the geometry line, then the documented no-native-window exit
1). NOT PROVEN HERE: the sandbox has no Vulkan ICD and no validation layer (only
`share/vulkan/registry` in the prefix), so the layer cannot be *run* against the
new code - the fixes are structural (each message maps to the exact call site
above) and the owner's debug run is the verification. Note that the app reaches
`vkCreateInstance` only after a native window exists, so the null-platform smoke
run cannot even exercise the VV_VALIDATION branch.

## Pass 48: zero extents are never handed to the driver (minimize)

OWNER (after pass 47, validation clean): "the only ones i just caught are 0
extents, exactly when minimizing window. needs guards."

```
[vulkan] vkCreateSwapchainKHR(): pCreateInfo->imageExtent (width = 0, height = 0) is invalid.   (VUID ...-01689)
[vulkan] vkCreateBuffer(): pCreateInfo->size is zero.                                           (VUID ...-00912)
[vulkan] vkAllocateMemory(): pAllocateInfo->allocationSize is 0.                                (VUID ...-07897/07899)
```

ONE CAUSE, THREE MESSAGES. A minimized Win32 window has no client area, and the
surface says so: `VkSurfaceCapabilitiesKHR::currentExtent` becomes `(0, 0)` -
*while still not being the special `UINT32_MAX` value* that means "the app
decides". `utils::chooseSwapExtent()` returned `currentExtent` verbatim in that
case, so:

* `vkCreateSwapchainKHR` was called with `imageExtent = 0x0` (01689);
* `m_swapchainExtent` was set to that, and `createStorageResources()` sized the
  compute output from `width * height` -> 0 bytes: `vkCreateBuffer` size 0
  (00912) and the two `vkAllocateMemory` calls that follow it with
  `allocationSize` 0 (07897, 07899).

The trigger chain is GLFW's `WM_SIZE` for the minimize (client `0x0`) -> the
framebuffer-size callback (clamped to 1x1 by GameWindow) -> `App::syncRendererSize`
-> `VulkanRenderer::resize` -> recreate -> `chooseSwapExtent` -> `currentExtent`.

FIX, four layers:

* `utils::chooseSwapExtent()` treats a surface extent with a zero dimension as
  "no current extent" and falls through to the app-chosen size, clamped into
  `[minImageExtent, maxImageExtent]` and floored at 1 - the result is never
  zero, and the `UINT32_MAX` path is unchanged.
* `VulkanRenderer::recreateSwapchain()` asks the surface first
  (`surfaceHasNoSize()`) and DEFERS the rebuild while the surface reports no
  size: the current swapchain is kept, `vkDeviceWaitIdle`/cleanup are not even
  reached, and a single note is printed per episode
  (`[vulkan] swapchain rebuild deferred: the surface reports no size (window
  minimized)`). Nothing is presented while minimized anyway (`canPresent()`),
  and the window comes back at the same size, so the swapchain that is kept is
  the one it needs.
* `VulkanRenderer::createSwapchain()` refuses a zero extent with a message
  that names the reason (`The window has no drawable size (minimized or
  hidden)`), so the init path cannot fall through to the zero-sized buffers
  either - and `createStorageResources()` has its own guard for the same
  reason.
* `App::syncRendererSize()` ignores size reports while the window is minimized,
  so the 1x1 clamped size Win32 produces during the minimize never reaches the
  renderer at all. The size callback fires again on restore and the frame loop
  re-checks the size every frame, so the real size cannot be missed.

VERIFIED IN THE SANDBOX. Release and Debug warning-free; ctest green in both
(5.0 s / 24.0 s); `glslangValidator -V` exit 0; `VV_PLATFORM=null` smoke
unchanged. The guard itself is exercised by a probe (not committed) that links
the REAL `VulkanUtils.cpp` and drives `chooseSwapExtent` with the capability
sets in question: normal Win32 surface (`currentExtent 1920x1009`) -> unchanged;
minimized surface (`currentExtent 0x0`) with a 1x1 or 1920x1009 request -> the
clamped request, never 0x0; minimized surface with `minImageExtent 8x8` -> 8x8;
a 0x0 request -> 1x1; `UINT32_MAX` current extent -> clamped to
`[min, max]` as before; a surface that reports `minImageExtent 0x0` -> the
requested size, still non-zero. Probe verdict: PROBE OK, 9/9 cases. NOT PROVEN
HERE: the real minimize on Win32 (no window manager, no ICD in the sandbox) -
the deferred path needs a surface that reports no size, which cannot be
simulated without a driver. Owner check: minimize/restore with the validation
layer on stays silent, the window comes back at the size it left, and the
"rebuild deferred" note appears once per minimize.
