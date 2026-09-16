# Passes — shipped-feature reports

One entry per pass, oldest first: what was asked for, what shipped, what was
measured, what was rejected and why. This is the project's feature history.

- The git log is the commit-level record (one commit per pass, verbose messages).
- `docs/AGENT_NOTES.md` keeps only the working notes an agent needs (tree map,
  contracts, switches, sandbox recipes, standing rules, what is queued next).
- `docs/UPGRADE_PROPOSALS.md` holds proposals that are **not** shipped features.

Entries are kept as written at the time, so each one describes the tree *at that
pass*; a later pass that changed a decision says so in its own entry (pass 56's
amplitude rule is superseded by pass 57; the toolkit the early entries mention
went away in pass 43). Passes 27-37 were logged as one prose block at the time
and keep that shape here.

## Passes 27-37 (logged in prose at the time)

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

## Pass 49: the SDF rebake - crop the sky, follow only when needed (optimization 1)

Owner task: "we need to optimize SDF shadows for production and fix the still
present latency" - after an inspection pass (probe numbers below), the owner
picked the two rebuild-side changes together (P1+P2 of the inspection report,
`/home/user/sdf_optimization_inspection.md`) with the default recenter margin of
ONE chunk.

### What the inspection measured (probes, not committed)

`/tmp/probe_sdf_cost.cpp` drives the REAL build path (snapshot -> chamfer EDT ->
seed pack -> staging memcpy) at the shipping geometry; `/tmp/probe_sdf_steps.cpp`
is a step-for-step copy of the shader's `sunRayEscapesSdf3d` over a real terrain
field.

- One bake cost ~130-140 ms of WORKER CPU (build 120-137, seed pack 7-11) plus
  ~2.5 ms on the RENDER thread (snapshot 0.5-0.8, staging memcpy ~1.8) and a
  18.9 MB upload. The pack pass was a second full traversal of a 19 MB array for
  a value the build already had.
- The rebuild was requested from `finishRegionMove`, i.e. on EVERY completed
  region move (~every 32 voxels of camera travel), while the box already covers
  +/-96 voxels: 2-3x more bakes than coverage needs, and while the camera moved
  the builder ran a near-continuous rebake loop competing with the frame for
  CPU. Nothing blocks the render thread (the join only happens once the build is
  ready), which is exactly why this shows up as "latency" rather than as an
  obvious stall.
- The GPU march is healthy: mean 22.3 steps/ray on lit top faces, 23.3 on
  sun-facing cliff faces, p90 33, and the 160-step budget is NEVER hit - so no
  field repair/fragmentation pass is needed. The per-step cost is the 3x3x3 seed
  gather (up to 27 u32 fetches + 27 cube distances + sqrt); a real distance field
  would make it 8 taps + a lerp (suggested next, P3).

### What shipped in this pass

1. **Sky crop (`SdfBoxGeometry::cropToBand` + `sdfBoxBandHeight`).** The box's
   TOP is cropped to the highest solid cell of its footprint + `kVoxelSdfBandMargin`
   (16 voxels of penumbra margin, so the softened light right above cliffs and
   overhangs survives). `originY` stays 0: the window must contain every solid
   cell, or the chamfer would under-report a distance it can no longer see -
   solids sit on the ground, so only empty sky can be dropped. Dropping empty
   cells cannot change a retained cell's argmin (the chamfer metric's shortest
   path between two cells of the window is monotone in every axis, so it never
   leaves the window), i.e. the field inside the window is IDENTICAL to the
   full-height one - pinned by `testSdfBoxBand` (exhaustive per-cell distance +
   packed-seed parity against the full build, plus the band height against a
   brute-force scan through the pinned `sdfBoxCellSolid` walk).
   The shader needed no change for this: it already honors `box.y`/`dims.y`, and
   a ray crossing the new top face hands off at the crossing exactly as before.
   On the default terrain the band is 110 of 128 rows (18 rows / 14% of the
   cells, 16.2 MB instead of 18.9 MB to upload); flatter ground drops far more.
2. **The seed pack is gone (`SdfField` stores the shader's encoding).** The
   field's seed array IS `std::vector<std::uint32_t>` with `kSdfEmptySeed`
   (0xFFFFFFFF = no solid in view), so the build's final sweep writes the
   uploaded value and `releaseSeeds()` hands the array over by move: no second
   19 MB array, no ~10 ms pack traversal. The CPU test now pins that exact
   encoding (it decodes seeds the way the shader does).
3. **`dims.w` is the live half's BASE CELL OFFSET, not the half index.** The two
   halves are strided by the BUFFER's cell count (`half * m_sdfCells`), matching
   `beginSdfUpload`'s `dstOffset`; with a per-field stride, a shorter banded
   field copied into the spare half would overlap the live one. `beginSdfUpload`
   accepts a field up to the buffer bound (`seeds.empty() || size > m_sdfCells`
   is the new error), and the publish path validates `ny` against the band
   instead of requiring the full height.
4. **Rebake cadence (`SdfHandover::needsRecenter` + `VulkanRenderer::followSdfField`).**
   The rebuild is armed in `updateWorld` (every frame) and only when the camera's
   chunk has drifted more than `m_sdfMarginChunks` (VV_SDF_MARGIN, default 1,
   clamped to `kSdfHalfChunks - 1` = 2; 0 = the old per-crossing cadence) from
   the LIVE field's center - and never while a region move is streaming (the box
   must be built from fully installed chunks; a mid-stream snapshot would read
   the missing chunks as air, i.e. holes, and holes in an SDF read as open
   space, so the shadows would go light exactly where the new terrain arrives).
   The want is the camera's CURRENT chunk instead of the stream's target, so a
   bake lands where the camera is now. `finishRegionMove` no longer touches the
   want. The pure policy is unit-tested (`needsRecenter`, 9 cases incl. negative
   chunk centers and margin 0/2).
5. **Bake instrumentation (VV_PERF).** `[perf] SDF bake #N: 192x110x192 of 128
   cells (16.2 MB seeds) at chunk (x,z) | worker: band + build ms | render:
   snapshot + upload ms | s since the previous bake` - the worker numbers are off
   the render thread (they cost the frame only as contention), the render numbers
   are frame time. Without VV_PERF the console keeps only the pre-existing
   "3D voxel SDF active" line (now once per bake instead of once per crossing).

### Verification here

Release + debug warning-free; `ctest` green in both (4.6 s / 22.1 s; the new
band test builds both the full and the cropped field and compares every cell);
`glslangValidator -V` exit 0; `VV_PLATFORM=null` smoke unchanged (exit 1, "no
native window handle"). Probe delta, same terrain, same sandbox CPU: worker per
bake ~130-140 ms -> ~100-113 ms (-25..-30%, from the 14% smaller field plus the
removed pack pass) and the render-thread part stays ~2-3 ms; the bake interval
roughly doubles at margin 1. NOT PROVEN HERE: the on-device look (the sky crop
must be invisible; a ray leaving the shorter box now runs the 2.5D hand-off for
a few extra voxels of open sky, which is the same traversal that already covered
everything above `y = 128`), and the real bake rate/bake-time on the owner's
machine - that is what the VV_PERF line is for.

## Pass 50: the seed decode - bit fields instead of integer divisions

Owner report: after pass 49 the shadows were unchanged with no new artifacts,
and an Nsight capture on his machine put the hottest lines of the frame (up to
13%) at exactly this, inside `sampleSdf3d`:

```glsl
vec3 v = vec3(float(s % nx), float((s / nx) % ny), float(s / (nx * ny)));
```

Integer division and modulo have no instruction on any current GPU: the driver
lowers each one to a long sequence (reciprocal/multiply-high style, ~10-20
instructions). The decode ran up to 27 times per sphere-trace step (the 3x3x3
seed gather) and ~22 steps per traced ray, so those lines were paying for four
division sequences *per fetched seed*.

### What changed

- The seed is now THREE BIT FIELDS: `s = x | (y << bits.x) | (z << (bits.x +
  bits.y))`, with the widths picked per box (`SdfField::seedBitsFor`, the
  smallest that hold each span: 8 + 7 + 8 = 23 bits for the 192 x 110 x 192
  banded shipping box). The shader decodes with
  `uint maskX = (1u << seedBits.x) - 1u;` etc. - two shifts and two masks, all
  uniform-derived and hoisted out of the loop.
- The CELL is unchanged, and so is the chamfer result (only the solid cells'
  seed writes and the decode changed; the relaxation propagates the packed
  value verbatim). The picture cannot move.
- `kMaxSeedBits = 31`: the packing has to stay below 2^31 so an encoded cell can
  never collide with the empty sentinel 0xFFFFFFFF. A box that does not fit is
  REFUSED with a reason in `launchSdfBuild` (and, defensively, `SdfField::build`
  then produces a field of empty seeds = "open space", not a wrong field). Any
  sane config packs in ~23 bits; a body that big would need a ~300 MB seed
  buffer.
- The box uniform (binding 13) grew a third `uvec4 seedBits` (48 bytes).
  `VoxelResources::kSdfBoxUniformBytes` is now the ONE definition used by the
  buffer, `writeSdfBox` and - the bug this caught - the descriptor write in
  `VulkanRenderer`, which still had `range = 2u * 16u` hardcoded: with a 48-byte
  block the shader's `seedBits` read would have been outside the descriptor
  range (a validation error and undefined data on device).
- The publish path additionally validates that the bits the shader will decode
  with are the ones the build used (`seedBitsX + seedBitsY + seedBitsZ <=
  kMaxSeedBits`), so a mismatched pairing can never go live.

### Verification

- SPIR-V, same shader, `glslangValidator -V -H` opcode census: the OLD module
  has `UMod 2 + UDiv 2` in `sampleSdf3d` (glslang CSEs the three source divides
  into two quotient/remainder pairs); the NEW module has **UMod 0, UDiv 0**
  there (the only UDiv left in the whole module is an unrelated pre-existing
  site), with `ShiftRightLogical` and `BitwiseAnd` each up by exactly 2. So the
  divide/remainder sequences are gone from the hot loop, not just moved.
- `testSdfSeedEncoding` (new): width per span (1 -> 0 bits, 192 -> 8, 257 -> 9),
  the shipping box packing to 23 bits, a too-large box being refused rather than
  mis-encoded, and a 200x3x5 field with one solid cell in the far corner whose
  every packed seed decodes (shader arithmetic, mirrored verbatim) to that cell
  with no sentinel collision.
- `testSdfBoxBand` now decodes with the shader's bit arithmetic and also asserts
  the named cell is really solid in the chunk data - the encoder/decoder pair is
  pinned against the voxels, not just against itself.
- Release + debug warning-free, `ctest` green (4.6 s / 22.6 s), `glslangValidator
  -V` exit 0, `VV_PLATFORM=null` smoke unchanged (exit 1).
- NOT PROVEN HERE: the actual frame-time win (no GPU in the sandbox) - the
  owner's Nsight run on the same scene is the measurement that counts.

## Pass 51: the seed bits never reached the shader (the uniform writer)

Owner report, right after pass 50: "sdf (3d) shadows disappeared right after this
change", with his own guess attached - "i bet it's something to do with
shader/uniforms". His logs said the same thing about everything else: the bakes
were normal (192 x 110 x 192 and 192 x 107 x 192, ~15 MB of seeds, band + build
on the worker, snapshot + upload on the render thread). So the field was built,
seeded, packed and uploaded exactly as pass 50 intended, and the GPU still
resolved nothing.

He was right about the uniforms, in the most literal way: `writeSdfBox` took the
three new `seedBits` arguments (pass 50 added them to the signature, to the call
site, to the 48-byte block and to the descriptor range) but never STORED them.
The block's 12 words are only ever touched by this one writer and the buffer is
`memset` to zero when it is created, so words 8..10 - `uvec4 seedBits.xyz` - read
as `(0,0,0)` on the GPU for the entire run. The shader then computed

```glsl
uint seedMaskX = (1u << sdfBox.seedBits.x) - 1u;   // (1u << 0) - 1 == 0
uint seedMaskY = (1u << sdfBox.seedBits.y) - 1u;   // 0
uint seedShiftZ = sdfBox.seedBits.x + sdfBox.seedBits.y;  // 0
```

so every 3x3x3 gather decoded to the same degenerate voxel `(0, 0, s)` instead of
the cell the seed names: `sdfCubeDistance` returned "far away" for essentially
every query, the field stopped occluding, and the SDF contribution went to
visibility 1.0 - no shadows, no artifacts, no error, exactly the report.

### How it was found

- A CPU probe (`/tmp/probe_seed_decode.cpp`, not committed: it builds the
  shipping banded 192 x 110 x 192 field with the real CPU code, then runs the
  shader's `sampleSdf3d` + the shadow march over 3481 upward rays from the
  terrain surface) first ruled out the field itself: with the pass-50 decode the
  packed seeds give 12.5% of rays blocked and mean visibility 0.695; the
  pass-49 pair (linear index decode over a linear-index field) is bit-identical,
  and the old divisions applied to the new packed seeds see nothing (0.0%,
  1.000). Encoder and decoder agreed - which meant the shader was not reading
  what the probe was reading.
- Reading `writeSdfBox` next to the block declaration is where it shows: the
  function stored words 0..7 and the active word, and the block declares three
  more. Adding the missing leg to the probe - the shader's own arithmetic, but
  with the masks read out of the block's stored words - reproduces the owner's
  picture: `ZEROBITS (as shipped) 0.0% blocked, mean visibility 1.000`.

### What changed

- `src/voxel/SdfUniform.hpp` (new): the binding-13 block as a word-for-word
  layout definition - `SdfBoxUniform` (`kWords = 12`, `kBytes = 48`, box.w is
  `kActiveWord`), `makeSdfBoxUniform()` (assigns all twelve words; an inactive
  box is the zero block with box.w = -1, the shader's first test) and
  `storeSdfBoxUniform()` (payload first, box.w LAST behind the release fence -
  the pass-42 ordering rule now lives inside the writer instead of being
  restated at the call site). It is pure C++ - no Vulkan, no device - so the
  test suite can walk it.
- `VoxelResources::writeSdfBox` is now a call to those two functions, and
  `kSdfBoxUniformBytes` is `SdfBoxUniform::kBytes`: the buffer size, the
  descriptor range and the writer all come from the one layout. A word can only
  go missing now if `makeSdfBoxUniform` does not assign it, and the tests read
  all twelve.

### Verification

- `testSdfBoxUniform` (new): the block is 12 words / 48 bytes / 3 x 16 with
  box.w at word 3; the shipping box's words carry the origin, dims, the live
  half's base cell offset and the 8/7/8 bit widths, with the tail padding zero;
  the writer's own store path fills all twelve words (the destination is
  poisoned with 0xDEADBEEF first, so a forgotten word stays visible); the
  inactive block is zero except box.w = -1; and 3000 seeds of a 200 x 3 x 5
  field decode through the bits READ BACK OUT OF THE STORED WORDS.
- Mutation check of that test: making `storeSdfBoxUniform` skip words 8..10 (the
  pass-51 bug itself) fails 3 checks; assigning zero to words[8] fails 3;
  restored, the suite is green again.
- CPU probe, five decoders over the same shipping field and rays: NEW (pass 50)
  12.5% blocked / 0.695 mean visibility, ZEROBITS (as shipped) 0.0% / 1.000,
  UNIFORM (pass 51 - masks read out of the block the fixed writer stores)
  12.5% / 0.695, i.e. bit-identical to what pass 50 intended. The old divisions
  over the new packed bytes stay blind (0.0% / 1.000), which is what a stale
  `pixels_rgba.comp.spv` would also look like - see the next paragraph.
- Release + debug warning-free, `ctest` green (4.7 s / 22.6 s),
  `glslangValidator -V` exit 0.

### The other way a shader change can fail to reach the game

Pass 50 changed the shader AND the CPU, so the executable relinked and the
POST_BUILD copy of the SPIR-V ran with it - but that is luck, not a rule:
`cmake/Shaders.cmake` copied `*.spv` next to the binary only as a POST_BUILD
step of the game target, which runs when the target LINKS. A shader-only rebuild
(the glslang step does not touch the link) left `build/.../resources/shaders`
newer than the `bin/resources/shaders` copy the renderer loads
(`VulkanRenderer::createComputePipeline` -> `core::executableDir()`, no version
check anywhere), so the game would keep rendering with the old shader while the
source said otherwise. Reproduced by mtime (compiled 06:53:15, runtime copy still
06:40:45), then made a build step: `${target_name}_shader_assets` is an
always-run custom target that copies `*.spv` into `$<TARGET_FILE_DIR>`, with the
shader compile as its dependency, and the game target depends on it. Verified
with a real shader change and no C++ change: `[2/3] Copying SPIR-V shaders...`
with no link step in the log, the exe's mtime untouched and both copies back in
sync - the case that used to silently keep the old shader.

## Pass 52: the corner gather - eight cells instead of twenty-seven (optimization P3)

Owner task: the P3 item of the optimization inspection ("u16/u8 distance field
per cell, ~8 taps + a lerp per step instead of the 27-tap gather"), greenlit
with "we can proceed to distance-field upload optimization". Before writing any
upload code the CPU probe (`/tmp/probe_df.cpp`, not committed - build with the
usual `g++ -std=c++20 -O2 -I src ... src/terrain/*.cpp src/voxel/*.cpp`) was
extended to put every candidate sampler through the SHADOW MARCH the shader
runs: the shipping banded box (192 x 110 x 192, 4.05 M cells, 16.2 MB), sun
`normalize(0.5, 1, 0.5)`, surface origins lifted exactly like `sunShadow`, step
`max(h * 0.7, 0.05)`, the 160-step budget and the hand-off at the box crossing
all as in the shader.

### What the probe measured (31 k rays at stride 1; stride 3 = 3481 rays in
parentheses)

| sampler | blocked | mean visibility | steps/ray | taps/ray |
| --- | --- | --- | --- | --- |
| seeds, 3x3x3 (shipped) | 12.9% (12.5%) | 0.690 (0.695) | 22.0 (22.0) | 592.8 (592.9) |
| seeds, 8 corners + 1 sqrt | 12.8% (12.5%) | 0.690 (0.695) | 21.8 (21.8) | **174.2** (174.2) |
| seeds, 14 = 8 corners + 6 face | 12.9% | 0.690 | 21.9 | 307.2 |
| u16 trilinear DF, own argmin | 11.3% (11.2%) | 0.857 (0.858) | 18.7 | 149.2 |
| u16 trilinear DF, cellDistance - 0.5 | 11.2% | 0.866 | 18.0 | 144.3 |

The 8-corner gather moves 9 verdicts of 31 k rays (2 of 3481), mean |dvis|
0.0003, max 0.075 - measurement noise on the rays that graze a cave mouth, and
the step count is unchanged, so the 0.7 safety factor stays exactly as
conservative as it was. This is the P3 tap target (8 taps per step instead of
27, one sqrt instead of eight) reached with a shader edit: no new buffer, no new
upload, no change to `SdfBoxUniform`, `SdfField`'s packed seeds or the pass-42
publish pairing.

### Why the candidates can be cut and why the picture cannot move

The field is cell-centred: every cell stores the nearest solid voxel inside it,
so the candidates that can carry the min at `p` are the cells whose CENTRES
surround it - `floor(p - 0.5)` and `+1`. The shader's own clamp into the box is
equivalent to skipping an out-of-range corner (a duplicate candidate cannot
change a min), and the squared-distance comparison is only a change of units.
The 3x3x3's farther cells do carry the min somewhere in the field: over the
shipping-style banded field 294 912 probes agree exactly on 88.12% of samples,
the other 35 040 are ABOVE the 8-cell min (mean +0.725, worst +12.655 voxels)
and none is ever below it (a subset min cannot be). The disagreements are all in
open sky - the points that are already tens of voxels away from any surface,
where `k*h/t` is clamped to 1 and the march has nothing left to decide, which is
why its verdicts and its accumulated visibility do not move. (The test terrain,
being hillier, agrees on 95.2% of its own samples, up to +14.589.) The 14-candidate variant (8 corners + the 6
face neighbours of `p`'s own cell) halves the gap again at 307 taps and 4 moved
verdicts; it is measured and documented here but not shipped, because the
8-candidate version already reaches the tap target with the picture bound.

### Implementation

- `resources/shaders/pixels_rgba.comp` (`sampleSdf3d`): the decode is unchanged
  (pass 50's shifts/masks); the gather is the eight corners of the interpolation
  cell compared as squared distances with a single `sqrt` at the end, and
  `bestSq >= 1e29 -> 1e30` reproduces the old "no solid among the candidates"
  value. The 0.7 step factor, the 160-step budget, `sunRayEscapesSdf3d`'s
  hand-off at the box crossing and the `box.w < 0` gate are untouched, as is
  `shadowPenumbra`.
- `src/voxel/SdfField.hpp`: `sampleCorners()` is the CPU mirror of the new
  gather (same `floor(p - 0.5)`, same clamps, same squared comparison) so the
  change is testable without a device; `sample()` stays as the 3x3x3 reference
  the tests march against. Nothing on the build/upload side changed - the
  pass-50 packed seeds are read by both gathers.

### Verification

- `testSdfCornerGather` (new). (a) A field with ONE solid cell: the cube is the
  only candidate either gather can find, so 600 probes must agree exactly -
  which pins the candidate set and the squared comparison. (b) The shipping
  banded field (band crop, real terrain): 15 884 samples, **none below the
  27-cell min** (the subset invariant), 756 above it (0.646 mean / 14.589 max,
  677 of them by more than 0.05 - all in open sky); and 576 surface rays marched
  with the shader's own arithmetic: 0 verdict diffs, mean visibility 0.9181 ->
  0.9185, mean |dvis| 0.0009, max 0.154 (tolerances 99% / 0.005 / 0.20).
- Release + debug builds warning-free, `ctest` 100% in both,
  `glslangValidator -V` exit 0 on the shader; the runtime copy is back in sync
  (`build/release/bin/resources/shaders/pixels_rgba.comp.spv`, md5
  7aeccb2deb7c6f28b720ef33831658f1).

### The distance field itself (P3 as written) was measured and left on the shelf

A true per-cell distance field does cut the taps - and it changes the picture,
which is the bar here. Any real DF (own-argmin cube distance, or the chamfer's
`cellDistance - 0.5`, or the trilinear read of either) lightens the shadows:
mean visibility 0.695 -> 0.858 on the shipping field, 46-48 of 3481 rays
changing verdict. The reason is geometric, not a bug: the 3x3x3 min
OVER-reports the distance near corners and edges (each cell's stored voxel is
the nearest one in that cell, so the min over a neighbourhood is a lower bound
of the local distance but the single-cell values are not), and it is that
over-report which keeps the current penumbra dark - remove it and the soft
shadow thins out. A bias cannot put the darkness back: it only shortens steps,
so 0 -> 0.25 -> 1.0 moves blocked rays from 11.2% to 7.8% and the step count
from 18.7 to 58.4 (the bigger field over-reports relative to the march, so every
step gets shorter). An "eroded" DF built from the sampler's own values
reproduces the picture (12.5% / 0.695) but it is not a distance field: it costs
1.9 steps per ray against the reference march and 748 verdict diffs, i.e. it
matches only because it is the sampler's own picture. Quantize + pack for a
real DF was affordable (28-33 ms exact cube distance, 13-18 ms for
`cellDistance - 0.5`, on top of the existing bake), so the upload side is
ready whenever the owner wants the LIGHTER picture - but on the "picture
quality is the acceptance bar" rule the 8-corner gather dominates it: the same
8 taps per step, no new buffer, and no visible change.

### Levers left on the march (measured, picture-changing, not shipped)

- Step factor 0.7 -> 0.95: 22.3 -> 18.1 steps per ray on the frame probe's
  rays (-19%), but rays that graze the corner of a neighbouring column stop
  being shadowed (the "hit within the first voxel" share drops 12.3% -> 4.9%
  with a 1-voxel start offset, 12.1% for the factor alone) - a real shadow
  change, so it stays opt-in.
- Early-out when the running visibility is already <= 0.02: 0.6 of 18.1 steps
  per ray. Same rule: it changes which rays keep marching.
- P4 (incremental sliding rebuild, ~30 ms bakes) is still open; it needs the P2
  region plumbing and a CPU parity test.

## Pass 53: the bake's sweeps, merged (P4, re-aimed by measurement)

### Follow-up: the boundary column is relaxed in the reference's own order

The merged sweeps split the boundary column (x = 0 forward, x = nx-1 backward)
onto the generic seven-candidate path, and the first cut ran that column LAST in
the backward pass (after the row's fast loop) where the pre-pass-53 sweep ran it
FIRST. Everything measured said the two orders agree - the parity test against
the frozen sequential form was green on all shapes, and an independent
end-to-end probe (the pre-53 `SdfField.hpp` from `e58bc8f` against the current
one, shipping banded box, 4 055 040 cells) produced the same seed fingerprint
`8e6aa0319555dd00` - because a diagonal candidate covers the one route the late
column would have opened. It is still the wrong thing to leave resting on an
observation: the backward pass now relaxes x = nx-1 before the row's fast loop,
so the sweep order and the candidate order are LITERALLY the reference's, and
"the uploaded array is bit-identical" is a property of the code instead of a
measurement. Same fingerprint, same 52-57 ms bake, with the forward column
already first.

The owner picked P4 (the sliding rebuild) after verifying pass 52. Before any
engine code, the probe `/tmp/probe_slide.cpp` (not committed) answered P4's two
questions on the shipping 192 x 110 x 192 banded box (4 055 040 cells):

**Where does the bake go?** clear 14, source scan 20, forward sweep 71, backward
sweep 62 = 167 ms of worker time in the probe (the engine's own number in the
pass-49 logs was 96-113 ms on the owner's machine, same box shape) - the two
chamfer sweeps are 76-79% of it.

**Can a shifted copy of the live field be repaired locally into the field a full
rebuild produces?** Measured no, twice:

- Shifted copy + both reference passes: 154.7 ms against 152.6 ms for a full
  build (the copy costs what the initialization it replaces costs), and the two
  fields are **not** bit-identical at shipping size - 52 of 4 055 040 seed words
  differ, because a fixed number of min-plus sweeps depends on the state it
  starts from. That is an approximation of the picture, not a rewrite.
- Shifted copy + a worklist repair iterated to convergence: 537-1177 ms and
  2.4-5.5% of seed words different, distances off by up to 16 voxels. The
  converged fixed point is not the reference field either: the shipped two-pass
  chamfer is NOT converged (it spends two sweeps, not a Bellman-Ford closure),
  so iterating its 14 local rules to convergence finds strictly shorter paths
  than the two passes ever do - a *better* field, and a different picture.

So P4's premise (the interesting work is local and repairable) does not hold for
this operator: the sweeps are inherently sequential, they have to run over the
whole array to reproduce the shipped field, and they are the cost. The pass
shipped the one exact win the measurement did leave open - the sweeps
themselves.

### What changed

`SdfField::build`: a cell's seven candidates are scanned in one go and the cell
is written once, instead of seven `relax()` calls that each loaded, compared and
stored `dist_[i]`/`seed_[i]`. Same candidates, same order, same strict `<`
against the running value (so a tie still keeps the earliest candidate) - a
rewrite, not a new algorithm. The x bounds tests are hoisted out of the innermost
loop by splitting the first (forward) / last (backward) column onto a generic
path, and the row base is computed once per row instead of per cell. The
candidate lists (`kForwardSteps`/`kBackwardSteps`) and the weights (`kW1/W2/W3`)
are now named constants, so the order that decides ties is visible in one place.

Measured in the probe, three runs each, interleaved, same field:

| build | run 0 | run 1 | run 2 | parity |
| --- | --- | --- | --- | --- |
| old sequential form | 161.9 ms | 184.7 ms | 185.6 ms | - |
| shipped merged form | 60.0 ms | 66.2 ms | 68.1 ms | 0 seed diffs, 0 exact-float distance diffs of 4 055 040 |

The uploaded seed array is bit-identical, so the picture cannot move - and no
shader, uniform, upload or cadence code was touched.

Left on the table, measured and exact (the next pass if the owner wants it): the
clear (14 ms of the remaining ~62) and the source scan (20 ms - `sdfBoxCellSolid`
does two integer divisions and a per-cell size check, all hoistable by walking
the chunks instead of the cells).

### Verification

- `testSdfChamferParity` (new) freezes the pre-pass-53 sequential form in the
  test file and requires bit-for-bit agreement, every seed word and every
  distance: a 33x17x40 shape with a floor, pillars, a floating slab (overhang),
  a hill, an isolated tower and a hollow box; degenerate spans (1x9x5, 9x1x9,
  2x2x2); an all-air box (every seed must stay `kSdfEmptySeed`); and the REAL
  banded terrain through `buildSdfBoxField` (64x107x64 = 438 272 cells, 438 272
  seeds identical, 0 distance diffs).
- Mutation check: replacing the interior corner weight `kW3` with `kW2` (one
  live constant) fails 3 checks and shows 79 529 distance diffs on the real box;
  restored, the suite is green. (A first attempt mutated a `Step` array entry
  that the x=0 column can never consult - x-negative candidates are always out
  of range there - which is why the mutation was invisible; the live path is
  what the check has to hit.)
- Release + debug builds warning-free, `ctest` 100% in both.

## Pass 54 (REJECTED on-device, reverted in pass 55): the march's step cap

The owner's directive for this pass: the 3D SDF shadows show (a) stair-stepping
where a shadow contact is hard and (b) banding across penumbrae, which he
believed dithering would fix. He asked for the proper fixes to be researched
rather than guessed, so the pass began on the web and ended in a probe.

**Outcome: rejected.** On-device verdict: "looks like it did not solve anything,
only performs worse now." The cap cost 21.9 -> 77.3 of the march's 160 field
samples per ray and bought nothing the eye could see, so pass 55 reverts it
(`t += max(h * 0.7, 0.05)` again, `kMaxSdfStep` gone from the shader, the mirror
and the tests) and replaces it with the ray jitter the owner actually meant.
Everything below is kept as measurement, not as a shipped design: the probe
numbers are real (the profile *was* smoother), they just did not predict what the
picture looks like - which is the lesson this pass cost.

### What the research says

- **Banding in a ray-traced SDF is a resolution/scampling artifact.**
  arXiv 2210.06160 (ray-traced SDFs) and the follow-ups name three mitigations:
  the Aaltonen/iq triangulated closest-approach estimate, **restricting the
  maximum step size**, and jittering the ray plus TAA. iquilezles.org/articles/
  rmshadows is the reference for both estimates; shadergif.com's AA guide and
  the r/GraphicsProgramming SDF threads are where the cone/footprint hit
  threshold comes from (treat the ray as a cone: hit when `h < t*tan`).
- **Dithering cannot be the banding fix here**: `packColor` already applies the
  IGN `+/-0.5 LSB` debanding, and the banding is in the float visibility
  profile, not in the 8-bit output - so there is nothing for dithering to hide.
- TAA was rejected by the owner at pass 9, and Aaltonen's *two-sphere* form at
  pass 34; the triangulated form is distinct but had to prove itself.

### The probe (not committed; `/tmp/probe_shadow_art*.cpp`)

`SdfField::sampleCorners` on fields built by the shipping CPU code (the real
band box: origin (-96,0,-96), 192x110x192, seedBits (8,7,8)) plus a synthetic
scene (floor, a DIAGONAL wall whose face steps 1 voxel in x every 2 in z, and a
tall thin pillar). Metrics: terrain mean visibility / share of rays that move;
for the contact edge, the terminator's 0.5-crossing per row (interpolated, so
the scan's own resolution cannot masquerade as a staircase), the ramp width, the
worst neighbour step, and the summed second difference ("wobble"); for the
penumbra, the same on a fine scan. An `exact` control marches the analytic voxel
solid at 0.02 voxels - the ground truth the SDF path is supposed to match.

Three measurement traps were hit and are worth remembering: a row spacing of 10
voxels samples a 2:1 voxel staircase at the same phase every time and reads a
*perfect* line; an all-dark profile reads as "staircase 0.000" (no crossing
found) rather than as "everything is black"; and a pre-rewrite probe marched
every ray to the budget without stopping at the box exit, which produced 134
steps/ray and a mean visibility of 0.06 instead of 22 steps and 0.66.

### What the probe measured (3721 terrain rays at stride 2; 60 contact rows at

0.02 voxels; a 0.02-voxel penumbra scan)

| variant | terrain meanvis | rays moved >1/255 | steps/ray | contact wobble | worst step | terminator staircase |
| --- | --- | --- | --- | --- | --- | --- |
| shipped (uncapped) | 0.6556 | - | 21.9 | 0.29 | 0.037 | 0.081 |
| Aaltonen triangulation | 0.6540 | 3.8% | 21.9 | 0.32 | 0.094 | 0.107 |
| footprint/cone threshold | 0.6524 | 15.9% | 19.8 | 0.29 | - | 0.081 |
| minimum penumbra via `t` clamp | 0.0070 | 80.5% | 21.9 | - | - | - |
| post-loop penumbra floor | 0.6563 | 11.9% | 21.9 | 1.53 | 0.995 | 0.081 |
| softer kK (4 / 6) | 0.6332 / 0.6480 | 21.7% / 18.6% | 21.9 | 0.19 / 0.23 | 0.025 / 0.028 | 0.113 / 0.104 |
| step cap 1.5 | 0.6553 | 2.4% | 56.4 | 0.21 | 0.028 | 0.085 |
| step cap 1.0 | 0.6553 | 2.1% | 77.3 | 0.13 | 0.017 | 0.104 |
| step cap 0.5 | 0.6552 | 2.3% | 141.7 | 0.07 | 0.017 | 0.089 |
| exact voxel control | 1.0000 | - | - | 2.00 | 1.000 | 0.000 |

### What that rules out, and what it leaves

- **The triangulation is measured out**: 0 extra samples, but on the terrain the
  8-corner gather already compares the closest of eight candidates, so
  interpolating between two samples changes 3.8% of rays by 0.0016 and makes the
  synthetic contact edge *worse* (wobble 0.29 -> 0.32). Do not ship it.
- **The cone/footprint threshold is measured out**: it fattens the shadow
  (blocked 19.2% -> 26.2%) instead of anti-aliasing it, because a footprint
  epsilon on a point-light ray moves the hit test outward.
- **Both "minimum penumbra" forms are measured out**: flooring the march
  distance `t` makes the ray's own surface a caster and drops the terrain to
  mean 0.007; a post-loop floor cannot lighten a hit, and where it does act it
  puts a hard kink in the ramp (wobble 1.53, worst step 0.995).
- **Softer `kShadowSharpness`** does soften contacts, but it softens *every*
  penumbra in proportion (the fake penumbra is 0.125*t wide) and darkens the
  picture, not lightens it: 4.0 moves 21.7% of rays. It is the honest "how soft
  do you want contacts" knob, not a fix - left for the owner to ask for.
- **The step cap worked on paper and failed on the device.** The visibility is
  a min over samples, and a step longer than the one voxel the field resolves
  lands on an arbitrary subset of samples - so the terminator wobbles with the
  sampling. Capping at 1.0 voxel did smooth the profile (contact wobble
  0.29 -> 0.13, worst step 0.037 -> 0.017, terrain picture moving by 0.0004 mean
  visibility, no ray cut off by the budget) - and the owner saw neither of the
  two artifacts improve. It costs 77.3 vs 21.9 field samples per ray, i.e. real
  frames per second, for a change below the visible threshold. Do not re-ship it
  and do not offer the 1.5 / 0.5 variants as follow-ups.
- **Jittering the OUTPUT was never the fix** (his correction, pass 55): "by
  dithering i meant jittering the actual rays, not the output image (that's
  different, color banding)". The IGN debanding in `packColor` is not evidence
  against ray jitter - different error, different scale.

### Honest limits

- The terminator still sits ~0.9 voxels wider than the exact geometry (the fake
  penumbra's own creep) and still has a +/-0.17-voxel sawtooth locked to the
  caster's voxel grid; the cap dilutes the *profile's* wobble (what the edge
  looks like) but does not move the terminator or remove that sawtooth. Removing
  the creep means changing the penumbra form (a look decision), and removing the
  sawtooth means a finer field or TAA.
- The exact reference's own contact edge is *harder* (0.016-voxel ramp, worst
  step 1.0) than the SDF's. Where a shadow contact is genuinely hard, no
  march-side change can make it soft - only a wider penumbra can, which is the
  kK trade above.
- The cap costs samples: 21.9 -> 77.3 of the field phase's 160. That phase is
  roughly an eighth of a shadow ray's taps (pass 52: 174 taps/ray), so this is
  ~30% of the SDF shadow pass. 1.5 costs 20% and gets ~70% of the smoothing;
  0.5 is 6.5x for the last 15%. The constant is one line.

### Verification (as it stood in pass 54)

- `testSdfSoftShadow3d` gained (4), a 0.05-voxel scan across the mesa's west
  shadow edge on the ground pinning the capped profile's wobble, the uncapped
  march being measurably worse on the same scan, and the 160-sample budget.
- `testSdfShaderMirrorConstants` (new) read `resources/shaders/pixels_rgba.comp`
  through `VV_SHADER_DIR` (cmake/Tests.cmake) and pinned `kShadowSharpness`,
  `kMaxSdfStep` and the capped step expression, then proved the mirror's
  *defaults* were those constants by comparing a default call against an
  explicit (8.0, 160, 1.0) call bit for bit. That pairing test survives - pass 55
  points it at the jitter instead (and at the renderer that writes the lever).
- Mutation check: setting the mirror's default cap to 1e9 failed exactly two
  checks and nothing else; restored, the suite was green. Both ctests 100% at
  `efd798f`.

## Pass 55: jitter the shadow ray's DIRECTION

The owner's corrected directive: "by dithering i meant jittering the actual rays, not the output image (that's different, color banding)".

**What it does.** `shadowRayJitter(hitPosVox, sunV)` tilts the sun by a
disc-uniform offset of slope `kShadowJitterDefault = 0.02` (a 1.1-degree cone,
16% of the 0.125-radian sun disc `kShadowSharpness = 8` models), hashed from the
shaded point's world position quantised to `kShadowJitterGrain = 8` cells per
voxel. Uniform in the disc area (`r = slope * sqrt(u1)`), zero mean, and the
direction keeps its LENGTH: the march's `t` is a distance along `sunV`, so a
rescaled direction would rescale the whole penumbra. It is applied once, in
`sunShadow`, to the two SOFT paths only (`sunRayEscapesSdf3d` inside the box,
`sunRayEscapesSdf` for the 2.5D fallback); the exact binary `sunRayEscapes` path
keeps the true sun and stays bit-identical, which is the standing constraint.

**Why this and not the cap.** The estimate's error is *coherent*: the visibility
is a min over discrete samples, so two neighbouring shaded points walk almost the
same sample set, their errors agree, and the error reads as structure - bands
across a penumbra, steps along a contact. Jitter makes the same error
*incoherent* without changing its magnitude; the cap tried to make the error
smaller and paid 3.5x the samples for something no eye could see. With TAA
rejected (pass 9) the noise is not resolved away, so the magnitude has to be
chosen to look like grain rather than like blotches - hence the lever.

**The lever.** `VV_SHADOW_JITTER=<slope>` (renderer: `m_shadowJitter`, clamped to
[0, 0.5]) rides to the shader in `pc.camera.w`, whose `.z` was already unused
(`camera.w` was written as 0.0 every frame, so nothing else reads it). Unset
(`< 0`) = `kShadowJitterDefault`; `0` = the pre-pass-55 estimate, bit-identical;
the startup log prints which. `pc.camera.w` is the *only* new push-constant
input, and `testSdfShaderMirrorConstants` now reads `VulkanRenderer.cpp` too
(`VV_SRC_DIR`) to check that the writer exists - pass 51's failure mode was a
shader input nobody wrote.

**Measured** (`testSdfSoftShadow3d` block (4), 0.05-voxel scan across the mesa's
west shadow edge, 181 samples; the mirror now defaults to the shipped jitter, so
the tests march the picture the GPU renders):

| | plain (jitter 0) | shipped (0.02) |
| --- | --- | --- |
| mean visibility | 0.5184 | 0.5177 |
| samples moved | - | 36 of 181 (max 0.135) |
| field samples/ray | 23.98 | 23.22 |

Zero bias (|d| 0.0007 - the disc-uniform offset is what buys that), the tilt
stays inside the slope by construction (max 0.0200 rad measured), the length
error is 1.05e-07 (float-level), and the jitter *costs no samples* - it slightly
reduces them, since a tilted ray can leave the box a step earlier. The
`shader mirror` test adds a second scan (400 random surface rays off the near
box): 53 of 400 rays move, mean visibility |d| 0.0001.

**What it is not.** Not output dithering (that is `packColor`'s IGN debanding,
already there, and it cannot reach a 0.01-0.15 error in the float visibility).
Not TAA (owner-rejected, one ray per pixel): the noise is meant to be seen as
grain, and the default is deliberately at the small end - if it reads as noise on
the device, 0 turns it off without a rebuild.

**Honest limits.** The jitter decorrelates the error but does not remove it, and
it does not move the terminator: the SDF contact edge still sits ~0.3-1 voxel
wider than the exact geometry (the fake penumbra's creep) and still carries a
+/-0.17-voxel sawtooth locked to the caster's voxel grid (measured in pass 54's
contact scan; the exact path's own edge is a straight line with a 0.016-voxel
ramp, i.e. genuinely harder than the SDF's). Removing either means changing the
penumbra form (a look decision) or a finer field - both out of scope here.
Blocked-ray share moves a little (terrain probe: 19.2% -> 23.8% at 0.02) because
a tilted ray can now strike a caster it previously grazed past; the mean
visibility rises by the same token, i.e. the shadow *area* is preserved.

**Verification.** Release + debug builds warning-free, `ctest` 100% in both;
`glslangValidator` exit 0 for the shader; the runtime `.spv` copies refreshed
(release `da672af3e0184c981cf6a1dfd9384853`, debug `e63e2cdca70238319cdef4e87b5b4d60`)
- a shader edit that never reaches `bin/resources` was a real failure mode here.

## Pass 56: the jitter acts on the ray's ORIGIN, one pixel of world

**The verdict that set this up.** Pass 55 (direction jitter) shipped and the
owner ran it on the device: the noise came "in small squares", was "noticeable
at 0.02-0.04" while "0.01 is still too high", and "at contacts (small penumbra)
there's no jitter at all" - then the question that is the spec: "so, i think
it's supposed to jitter per pixel?". Mechanism, not magnitude: he asked for the
constant to move *below* 0.01 even though the visual artifact was already
"too much", which is the signature of a wrong-shaped noise field, not a loud one.

**What the three symptoms said, in order.** (1) "Small squares": the pass-55
hash cell was a FIXED 1/8 voxel, i.e. ~2.5-5 screen pixels at typical
20-40 px/voxel distances. The grain has to be one pixel of the *picture*, so the
cell must be the pixel's own footprint (the same `voxelsPerPixel` the texture
LOD already computes from `sTHit`, so it is free). (2) "0.01 is still too high"
while the noise was already visible at 0.02-0.04: a direction tilt displaces the
sample by `slope * t`, and the visibility it feeds (`kShadowSharpness * h / t`)
responds by `kShadowSharpness * slope` = 0.08-0.16 at those slopes - a uniform
8-16% brightness noise in every penumbra, independent of distance. (3) "At
contacts there's no jitter at all": exactly the same equation, read at t ~= 0 -
a tilt moves a contact ray by nothing, while a contact needs the *largest* move
because its penumbra is sharper than a pixel.

**The mechanism now.** `shadowRayOrigin()` starts the soft path from a per-pixel
DISPLACED origin: a disc-uniform offset of radius `min(amount * footprintVox,
1 voxel)` (amount = `pc.camera.w`, `VV_SHADOW_JITTER`, default
`kShadowJitterDefault = 0.5` pixel footprints), hashed with hash13 from the
shaded point quantised in FOOTPRINT-sized cells, applied in the SURFACE's
tangent plane. Each choice was measured, not argued:

- *Origin, not direction*: the sample moves by a fixed distance at every t, so
  the visibility moves by `k * offset / t` - strongest at contacts, ~1% across a
  wide penumbra. On the synthetic contact edge (0.02-voxel scan of the
  staircase wall, 60 rows) the worst step in the edge profile goes 0.037 (base)
  -> 0.042 (0.02 vox offset) -> 0.080 (0.05) -> 0.136 (0.1) -> 0.307 (0.25),
  i.e. the hard step becomes a band of partial samples that grows with the
  offset, and the mean edge position does not move (47.293 -> 47.291 at 0.05).
- *Tangent plane, not the sun's plane*: a displacement perpendicular to the sun
  with the origin's normal lift raised by the same distance looks harmless and
  biases the picture - on 3721 terrain rays, mean visibility 0.6556 (base) ->
  0.7173 at 0.25 voxels of lift (+6%), because a lifted origin clears casters
  the surface point should still be blocked by. The same radius sliding ALONG
  the surface keeps the mean in check (0.6631 at 0.05 vox, +1.1%; 0.6727 at
  0.1, +2.6%) which is also why the default is half a pixel and not a whole one.
- *Footprint cell, not a fixed world cell*: the grain is one pixel wide at any
  distance; a screen-space hash (the literal reading of "per pixel") was
  rejected because it would stick to the screen and crawl over the world as the
  camera moves, and because the tangent-plane hash already decorrelates the
  neighbours. The test pins the consequence: two probes 0.3 footprints apart in
  the same cell get bit-identical offsets, 1023 of 1024 neighbouring cells get
  different ones, every offset stays inside its disc (max 0.125 of 0.125 vox at
  a 0.25-voxel footprint) and the disc is zero-mean (|mean| 0.004 vox, 3.2% of
  the radius).
- *Lever*: 0 is bit-identical to the un-jittered estimate (the offset vector is
  all zeros, the march is byte-for-byte pass 53's), 0.5 is the default, 4 the
  clamp. The renderer prints the resolved value at startup next to the SDF
  margin line, and `push.camera.w` carries it to the shader.

**Honest limits.** The jitter decorrelates the min-over-samples error into
per-pixel noise; it does not move the terminator. The SDF contact edge still
sits ~0.3-1 voxel wider than the exact geometry (the fake penumbra's creep) and
still carries the +/-0.17-voxel sawtooth locked to the caster's voxel grid: on
the 60-row contact scan the row-to-row residual stays 0.081 at every offset up
to 0.1 voxels (it only scrambles at 0.25: 0.128). The exact path's own edge is a
straight line with a 0.016-voxel ramp, i.e. genuinely harder than the SDF's, so
what the jitter buys at a contact is the *look* of the edge (a band of partial
samples instead of a clean, comb-like step), not its position. The terrain
picture's mean visibility is preserved to 0.0000 on the mirror test's 400 rays
and 0.0020 on the 181-ray mesa scan; the blocked share on the 3721-ray terrain
probe still rises a little at larger offsets (19.2% -> 20.7% at 0.02 vox,
24.0% at 0.05) because the sampling variance is not symmetric in a *hard* shadow.

**Verification.** `glslangValidator` exit 0; release + debug `ctest` 100%
(release 5.95 s, debug 25.28 s); the runtime `.spv` copies refreshed (release
`3414d9c31630e0b65266ada9a45d306a`, debug `d5da838557627ad348d22eedc703d1ab`
- see the file's own md5, a shader edit that never reaches `bin/resources` was a
real failure mode here). `testSdfShaderMirrorConstants` now pairs the shader
file (VV_SHADER_DIR) and the renderer writer (VV_SRC_DIR) with the CPU mirror's
constants and pins the *text* of the mechanism (the displaced soft origin, the
untouched exact origin, the footprint hash cell, the `pc.camera.w` lever, the
`t += max(h * 0.7, 0.05)` step that pass 54's cap must not come back through),
and it fails if the shader's `kShadowJitterDefault` stops being 0.5.
`testSdfSoftShadow3d` block 4 pins the offset's arithmetic and properties plus
its effect on the scan. The A/B lever is the point of the pass: the owner can
sweep 0 / 0.25 / 0.5 / 1 / 2 without a rebuild and the startup log says what he
is looking at.

## Pass 57: the jitter is normalized where the shadow is formed

**The verdict that set this up.** Pass 56's mechanism came back verified - the
owner: "looks correct now" - with two defects named in one breath: its strength
("0.5 is too small, 5 is enough to eliminate banding and stepping") and, the
real find, its distance dependence: "noise scales with distance, so up close
it's still not enough, and too far it's too much".

**Diagnosis, before any code.** Pass 56 scaled the origin offset by the shaded
point's PIXEL FOOTPRINT - the one quantity in the shader that grows with the
camera - and the visibility the offset feeds moves by `k * offset / t`, where t
is the distance to the caster. So the noise grew with the camera exactly as
reported: on the 3721-ray terrain probe (footprint = d * 2 * tan35 / 1080) the
mean |dvis| ran 0.0076 / 0.0181 / 0.0461 / 0.0798 / 0.1669 (22x) at camera
distances 5 / 15 / 45 / 135 / 400, with the moved share creeping 17.7% -> 28.3%.
Same probe, same rays: a direction CONE that moves the sample by `slope * t` at
the caster holds 0.0210 - 0.0216 (flat) across that whole range, because
`k * (slope * t) / t = k * slope` wherever the shadow is formed. That is also
why a cone alone cannot fix a contact (the owner's pass-55 finding): at a contact
the caster is at t ~= 0, so `slope * t` ~= 0 again.

**The mechanism now.** `shadowRayJitter(hitPosVox, n, sunV, footprintVox,
outOrigin)` returns the direction to march (the same LENGTH as `sunV`, since the
march's t is a distance) and writes the origin, from ONE hashed azimuth
(hash13, twice, in pixel-footprint-sized cells of the hit position - pass 56's
accepted per-pixel grain):

- *Cone*: `dir + normalize(tilt) * (slope * magnitude)`, where `tilt` is that
  azimuth projected perpendicular to the sun (a tilt along the sun would change
  t and nothing else) and `magnitude = sqrt(u1)` is uniform inside the cone.
  The noise is one amplitude at every camera distance.
- *Contact floor*: the origin slides along the SURFACE tangent plane by
  `floorVox * magnitude`. A fixed world distance, deliberately not a footprint -
  repeating that scaling is the bug above - and the tangent plane is pass 56's
  accepted choice: a displaced origin can never start inside the solid it stands
  on, and the ray keeps its t.
- *Constants*: `kShadowJitterDefault = 0.002` (the cone slope the owner picked on
  the device: "VV_SHADOW_JITTER=0.002 feels just right, it's enough to hide
  stepping and bands"), `kShadowJitterFloor = 0.02` voxel at that slope - i.e.
  the floor is 10x the slope, capped by `kShadowJitterFloorMax = 1.0` (reached
  at slope 0.10). The owner's "5" was pass 56's footprint lever, whose value at a
  typical view is about one voxel of displacement; the shipped default is a
  quarter of that lever's "too small" setting, and the probe numbers below use
  0.05 - 25x the default - where the contact metric can resolve the effect.
- *Lever*: `VV_SHADOW_JITTER` is now the CONE SLOPE (renderer clamp [0, 0.5]),
  and moves both terms; `0` is still bit-identical to the un-jittered estimate
  (the early-out returns `sunV` and the plain lifted origin), and the startup
  log prints the slope and the resolved floor in voxels.

**Measured, at the 0.05 lever (25x the shipped default: cone slope 0.05,
floor 0.50 vox).**

- *Contacts* (60 rows of the staircase wall, 0.05-voxel x sampling, the
  terminator's interpolated 0.5-crossing fitted per row). The exact reference's
  edge is a STRAIGHT line - residual 0.000, parity-locked part 0.000, mean edge
  48.260 - while the SDF march's edge sits ~1.0 voxel on the other side (47.293)
  with a zigzag locked to the caster's voxel rows (parity mean 0.162 voxel: this
  is the "stair-stepping"). The jitter cuts that locked part to 0.063 (2.6x) and
  turns the profile into per-sample grain (summed |2nd difference| 0.29 -> 6.42,
  worst neighbour step 0.037 -> 0.957), at the price of moving the mean edge
  0.485 voxel further from the reference (47.293 -> 46.808): the edge folds
  toward the lit side, i.e. contacts read marginally lighter and noisier, which
  is the same trade pass 56 made at his "5".
- *Terrain* (3721 rays): 18.2% of rays change by more than 1/255, mean |dvis|
  0.068, mean visibility 0.6556 -> 0.7122 (+0.057) and the blocked share 19.2% ->
  25.4%. A binary hit is not a smooth ramp, so a symmetric perturbation does not
  leave the mean alone; pass 56 documented the same asymmetry (19.2% -> 24.0% at
  a 0.05-voxel offset) and the lever is the answer, not the form.
- *Penumbra* (the pillar's top-corner scan at 0.02 voxel): the deep-penumbra
  mean goes 0.028 -> 0.109 (partial samples replace the clamped floor), the
  flat-plateau share 77.6% -> 77.1% and the profile's roughness 2.02 -> 15.9.
- *Cost*: two hash13 and a handful of ALU per shadow RAY against ~174 field taps
  - below the probe's timing noise. The march's samples and the exact
  `sunRayEscapes` path are untouched.

**At the shipped default (0.002).** The owner swept the lever on-device and
picked the gentlest setting that still reads as fixed. Measured at 0.002: the
penumbra profile's exactly-flat plateaus fall from 47.2% of neighbouring samples
to 28.1% (this is the banding dissolving into sub-quantisation dither), the
terrain probe moves 16.8% of rays by more than 1/255 with mean |dvis| 0.0078 and
mean visibility 0.6556 -> 0.6600, and the blocked share 19.2% -> 22.4%. The
contact scan's parity-locked zigzag does NOT improve at this strength (0.162 ->
0.222): a gentle jitter dithers the profile samples (summed |2nd difference| 0.29
-> 0.48, worst neighbour step 0.037 -> 0.064) without moving the terminator's
row-to-row structure. What the eye reads as a stepped contact is the plateau/jump
structure, which is what the default breaks; the geometry-derived zigzag needs
the 0.05-ish lever to actually move, and then it costs the half-voxel edge fold
documented above. Both levers are recorded in the probe table at the end of this
section.

**Honest limits.** The jitter does not fix the SDF's ~1-voxel contact creep; it
makes the edge noisier and, measured at 0.05, folds its mean by half a voxel
toward the light. The tangential slide can start inside a step-up that shares
the point's voxel-face normal (a face normal is axis-aligned, so a horizontal
slide on a floor can enter the next column), which produces occasional dark
specks at contacts - present since pass 56 at its largest offsets, bounded here
by the 1-voxel cap. A field-validated origin (one extra sample: if `h(origin)`
is under the lift, keep the un-displaced origin) is the pass-58 candidate if it
shows on screen. And the grain is a footprint cell, so at a grazing view it
projects to many pixels: pass 56's accepted choice, unchanged.

**Verification.** `glslangValidator` exit 0. Release + debug `ctest` 100%
(release 6.73 s, debug 28.66 s) with the runtime `.spv` copies refreshed
(release `18995e91048ca1bf434eb3332625d698`, debug
`9d806f59f501d736aef9e878dbc56a2a`). `testSdfShaderMirrorConstants` pins the
shader's *text* for this mechanism (the tangent-plane slide, the tilt projected
perpendicular to the sun, the footprint hash cell, the `pc.camera.w` slope, the
`t += max(h * 0.7, 0.05)` step pass 54's cap must not return through) and its
constants against the CPU mirror (`0.05 / 0.50 / 1.0`), then drives 400 random
terrain rays through the mirror's own arithmetic: the march's defaults are still
8.0 / 160, 99 of 400 rays move, and the mean visibility moves 0.033 - the bound
is 0.06 now, documented in the test, because pass 56's "does not move the mean"
(0.0000) was measured at a *smaller* default and any jitter that acts on an edge
moves that edge. `testSdfSoftShadow3d` block 4 pins the floor's arithmetic (0
implies exactly 0, linear in the lever, capped), the footprint-independence of
the displacement bound, the tangent-slide property (the normal component of the
offset is 0) and the 181-ray mesa scan (42 rays move, mean shift 0.024 with the
scan window crossing the edge).

**Follow-up commit (same pass): the default is the owner's pick.** He swept the
`VV_SHADOW_JITTER` lever on the device and reported "confirmed, stays consistent
with distance now. `VV_SHADOW_JITTER=0.002` feels just right, it's enough to hide
stepping and bands." The shipped constants are therefore
`kShadowJitterDefault = 0.002` and `kShadowJitterFloor = 0.02` voxel (the floor
stays 10x the slope, capped at one voxel at slope 0.10), i.e. exactly the setting
he verified - the arithmetic is unchanged, only the default moved, so his A/B
still reproduces byte for byte. The suite's effect half now runs its scan and its
400-ray check at an explicit 0.05 lever (a constant named in the test) instead of
at the default, because at 0.002 only a fraction of a scan's rays are brushed; the
constants half still pins the shader's own default. Rebuilt and re-tested at that
commit: release + debug `ctest` 100% (6.84 s / 30.11 s), runtime `.spv` md5
release `74ad055c3070a1a1d6eb168ae5e770a0`, debug
`82dc95a2423f9a30d1692374d23be95b`.

## Pass 58: glfw and glm are vendored, not installed

**The report:** "we should vendor glfw and glm, someone complained that they should
not have to install every dependency in their system." Pass 43 had chosen the
other answer: use a system GLFW when CMake finds one, otherwise `FetchContent`
the pinned 3.5.1 from GitHub at configure time. Both halves are gone - a clone
now carries its dependencies, and the build needs no network.

**What is in the tree.** `third_party/glfw` is upstream 3.5.1 (tag commit
`d9d6f0f1f967807ffade6598ea9a631ebaf37a56`, tarball sha256 `5234f4f2…`) with
`examples/`, `tests/`, `docs/` and the `deps/` files those use removed, and
`deps/wayland/` kept (the Wayland backend generates its protocol code from those
XMLs). `third_party/glm` is upstream 1.0.1 (commit `0af55cc…`, tarball
`9f317456…`) reduced to the `glm/` headers plus its license - the 24 MB of
`doc/`, `test/`, `cmake/` it ships are not part of anything we build.
`stb_image.h` was already vendored. `third_party/README.md` records the upstreams,
what was trimmed and why, and a tree digest for each directory so "did someone
edit vendored code" is one command.

**How they are wired.** `cmake/Dependencies.cmake` was rewritten around the
vendored trees as the default and a system pair as the opt-in:

- `VV_USE_SYSTEM_DEPS=OFF` (new, default) skips `find_package(glfw3)` entirely, so
  a stale `glfw3_DIR` in an existing build directory cannot pull the system copy
  back in, and `add_subdirectory(third_party/glfw … EXCLUDE_FROM_ALL)` builds it
  as a subproject (static, examples/tests/docs/install off, output under
  `build/.../third_party/glfw`).
- `VV_USE_SYSTEM_DEPS=ON` restores the pass-43 preference for a packager's build.
- The version is not taken on faith: the configure reads `GLFW_VERSION_*` out of
  `third_party/glfw/include/GLFW/glfw3.h` and fails if it is not
  `VV_GLFW_EXPECTED_VERSION` (3.5.1), so a partial re-vendor is a hard error
  rather than a silently different library.
- glm's include probe got a fresh variable name (`VV_GLM_INCLUDE_DIR` is `unset`
  in the cache first): the pass-43 configure cached it as a PATH, and a stale
  entry would otherwise shadow the vendored copy in an existing build directory.
  `cmake/GameTarget.cmake` and `cmake/Tests.cmake` already consumed that variable,
  so they only lost their dead `if()` guard's false branch.
- Which native backends exist moved here from `GameTarget.cmake`: the vendored
  GLFW is told to build X11/Wayland only for the development packages actually
  installed (`X11/Xlib.h`, `wayland-client-core.h`), and with neither it is built
  null-only and CMake warns with the package list instead of the old hard error.
  `-DVV_GLFW_NULL_ONLY=ON` still forces null for restricted environments.
- `scripts/build-linux-toolchain.sh` lost its GLFW and glm build steps (the
  sandbox prefix is Vulkan + glslang now).

**Verification.**

- Three build directories, none of which had a system glfw/glm to find (this
  sandbox has neither package nor X11/Wayland headers): `build/vendor` and
  `build/fresh` configured from scratch (`glfw: vendored 3.5.1 (third_party/glfw)`,
  `glm: vendored (third_party/glm, upstream 1.0.1)`), built and passed `ctest`
  (6.70 s / 6.72 s); `build/release` and `build/debug` reconfigured and rebuilt
  on the same CMake (100%, 6.77 s / 28.47 s).
- The link is the vendored library: `build/*/third_party/glfw/src/libglfw3.a` is
  built by this build and the game binary carries 283 glfw symbols.
- The shaders are untouched by this pass: the runtime `.spv` md5s are the same as
  at pass 57's pin (`74ad055c3070a1a1d6eb168ae5e770a0` release,
  `82dc95a2423f9a30d1692374d23be95b` debug).
- Headless smoke run on a null-only build prints the documented path
  (`VV_PLATFORM=null`: window created, then "Failed to obtain a native window
  handle … Unsupported GLFW platform 'null'").
- `scripts/build-linux-toolchain.sh` passes `sh -n`.

**Honest limits.** This box has no X11 or Wayland development headers, so only
GLFW's null backend was compiled here; the X11/Wayland/win32 backends are the same
upstream sources that were being fetched before, and the digests in
`third_party/README.md` pin the tree. GLFW's X11/Wayland builds still need their
platform development packages (they are the platform's ABI and cannot be
vendored); the README now says exactly that, and lists what is no longer needed
(`libglfw3-dev`, `mingw-w64-ucrt-x86_64-glfw`, `libglm-dev`, `glm-devel`,
`brew install glfw`). Vulkan headers/loader and `glslangValidator` remain system
requirements, unchanged by this pass.

## Pass 59: the shader is `voxels.comp` (it was `pixels_rgba.comp`)

**Why.** `pixels_rgba.comp` was the name from the 2D compute pixel-drawing era;
the file has been the voxel ray tracer since the first 3D pass, and the name was
the last thing still saying otherwise.

**What changed.** `git mv resources/shaders/pixels_rgba.comp
resources/shaders/voxels.comp` and every reference that means "the shader today":
the loader path in `VulkanRenderer::createComputePipeline`
(`voxels.comp.spv`, `resources/shaders/`), the comments that point a reader at the
shader (`src/render/SceneData.hpp` x2, `src/render/LightingConfig.hpp`,
`src/voxel/SdfUniform.hpp`, `src/voxel/SdfField.hpp`, `src/voxel/VoxelTypes.hpp`),
the mirror test's `VV_SHADER_DIR` file name and its failure message
(`testSdfShaderMirrorConstants` reads the file to pin the shadow arithmetic), and
the `cmake/Tests.cmake` comment. Nothing else loads it by name: the shader
pipeline globs `*.comp`, the runtime copy uses the same glob, and the packaging
target installs `VV_SHADER_SPV_FILES`.

**What did not change.** Not one byte of the shader: `git show HEAD:resources/
shaders/pixels_rgba.comp | md5sum` and the renamed file's md5 are identical, so
the rebuilt SPIR-V is identical too - and the name is not part of the SPIR-V, so
the runtime behaviour cannot differ.

**Verification.** `glslangValidator -V resources/shaders/voxels.comp` exits 0;
release + debug rebuilt and `ctest` 100% (the mirror test now opens the new name -
it fails loudly if the file cannot be read, which is the point of the explicit
`check(in.good(), ...)`); the runtime `resources/shaders` copy holds
`voxels.comp.spv`, the stale `pixels_rgba.comp.spv` was removed from those
directories, and neither binary contains the old name any more (the Debug build
shows the literal `voxels.comp.spv`; the Release build inlines short literals
into mov immediates, so a byte search there only finds `voxels.c`/`comp.spv`
fragments - checked with a byte search over both, `pixels_rgba` is gone from
each). The Release `.spv` is
byte-identical to the pass-57/58 one (`74ad055c3070a1a1d6eb168ae5e770a0`) - the
strongest statement that this pass changed no arithmetic - while the Debug `.spv`
becomes `b44d94e98c515686a37b45fd6202b544`: Debug builds pass `-g`, and glslang
embeds the SOURCE FILE NAME in the debug information (OpSource/OpName), so a
rename is supposed to show up there and nowhere else.

**Docs, later (pass 60).** This entry was written while the agent notes still
carried a stale "Project" header (a toolkit shell that had been gone since pass
43) and the shipped-feature reports lived inside the notes file; pass 60 split
those apart - this file is the feature-report home now, and `docs/AGENT_NOTES.md`
keeps only the working notes.

## Pass 61: the switches are command-line options (they were environment variables)

OWNER: "someone complained about environment variables. they want commands to be
parsed instead."

**What shipped.** One parser, one struct, one place that touches the environment:

- `src/core/CommandLine.hpp/.cpp` (new, dependency-free, pure): `parseCommandLine`
  turns `argv` into `vv::core::GameOptions`, `optionsFromEnvironment` fills the
  same struct from the historical `VV_*` variables, `commandLineUsage` is the
  `--help` text, `describeOptions` is the one-line startup summary. `main()`
  parses before anything else, prints the usage and exits 0 for `--help`, and
  reports a bad option on stderr with exit 2 instead of starting the game with
  settings the user did not ask for.
- Every `getenv` outside `CommandLine.cpp` is gone: the renderer
  (`--sdf-shadows`, `--shadow-sharp`, `--shadow-jitter`, `--sdf-margin`,
  `--far-lod`, `--validation`, `--perf`, `--debug-term`, `--debug-hole`), the
  swapchain's present mode (`--present`), and the window platform
  (`--platform`) now read the parsed struct through `vv::core::options()`.
- `--platform` is a superset of the old `VV_PLATFORM`: it takes
  `auto|x11|wayland|null|cocoa|win32` (the old variable only understood `null`)
  and goes through `glfwInitHint(GLFW_PLATFORM, …)` - no environment mutation.
  A platform the GLFW build does not have fails at `glfwInit` with GLFW's own
  message (verified here: `--platform x11` on a null-only build prints "This
  binary only supports the Null platform" and exits non-zero).
- `--present` accepts `immediate|mailbox|fifo` and also `uncapped` as a spelling
  of `immediate` (the default), so describing the default in a command line is
  not an error.
- Boolean flags take a `--no-` prefix (`--no-far-lod`), and the last one on the
  command line wins. The environment still seeds the struct and a flag overrides
  the variable with the same meaning - the owner sweeps `VV_SHADOW_JITTER` on the
  device without a rebuild, and CI and `run_debug.bat` set `VV_VALIDATION`, so
  removing the variables would have broken workflows for no gain.
- `run.bat` forwards its arguments, `run_debug.bat` passes `--validation` instead
  of setting a variable, and the toolchain script's headless hint is
  `--platform null`.

**Startup log.** `[vv] options: sdf-shadows, shadow-jitter 0.050, sdf-margin 2,
perf, platform null` - only the switches that differ from the defaults, which is
the line a bug report should quote.

**Tests** (`testCommandLine`, in the pure-logic suite): the empty command line,
every flag at once, the value forms, the `--no-` forms, "last one wins",
`--help`/`-h` and that the usage names every flag, the failure modes (unknown
flag, missing value, non-numeric slope, fractional chunk count, unknown present
mode, unknown platform, unreadable chunk coordinates), the precedence rule
(a flag beats its variable, an unrelated variable survives), the startup
summary, and the `setOptions`/`options` round trip. The environment is cleared
and set explicitly by the test, so the suite behaves the same in any shell.

The pass-57 pin in `testSdfShaderMirrorConstants` (the renderer must be the one
that reads the jitter lever - pass 51's lesson) now checks the new plumbing: the
renderer consumes `options().shadowJitter*`, and `CommandLine.cpp` is the file
that reads `VV_SHADOW_JITTER` and defines `--shadow-jitter`. The failure mode
just moved one indirection out; it did not go away.

**Verified in the sandbox.** Release + debug configured and built warning-free,
`ctest` 100% on both. `--help` prints the usage on stdout and exits 0; `--nope`
and `--shadow-jitter loud` print `[vv] fatal: …` on stderr and exit 2. A headless
run (`--platform null`) prints `[vv] options: platform null` and then the
documented windowless failure, `VV_PLATFORM=null` alone still does the same, and
`VV_FAR_LOD=1 game --no-far-lod` reports only `platform null` - the flag won.
This box has no window, so the renderer's own `[vulkan] …` lines are not
reachable here; the renderer wiring is covered by the suite's source pins.

## Pass 62: the ambient is the sky the SURFACE sees (it was the sky the CAMERA ray saw)

OWNER: "proceed with ambient light upgrade. i pick the recommended approach
(b1 + 2 sdf rays)." (the recommendation in `docs/UPGRADE_PROPOSALS.md` §1, the
pass-59 proposal).

**What was wrong.** The ambient was

```glsl
vec3 skyAmb  = skyBaseColor(sRdWorld);   // the sky along the VIEW ray
vec3 ambient = mix(skyAmb * 0.35, skyAmb, hemi) * (0.55 + 0.45 * shadow);
```

Two defects, both visible:

- **It followed the camera.** The sky was sampled along the pixel's view ray, so
  the ambient on a given wall changed when the camera turned (worst at grazing
  angles, where the ray's elevation swings most). A diffuse ambient term is
  view-independent by definition - the rim keeps the view ray, the ambient must
  not have it.
- **It was not an occlusion term.** `(0.55 + 0.45 * shadow)` removes at most 45%
  of a *sky* value, and a cave is `shadow = 0` everywhere, so a cave kept 55% of
  a sky term. "There is practically no ambient light here" could never appear.
  The coupling is also the wrong shape: the north face of a hill is `shadow = 0`
  too, and it *is* sky-lit.

**What shipped** (`resources/shaders/voxels.comp`, a new "Ambient sky
visibility" block; `scene.ambient` in the scene UBO; on by default):

- The dome term is sampled **at the hit normal**, `skyBaseColor(n)`, blended with
  a dim warm ground bounce (`scene.skyLow * vec3(0.45, 0.40, 0.32)`) by the
  existing hemispheric weight `hemi = n.y*0.5+0.5`. No view ray anywhere in it.
- The sky is scaled by a per-pixel **sky visibility**:
  - **B1 - the horizon scan**: 6 azimuths x 6 distances (1, 2, 4, 8, 16, 32
    voxels) against the near region's per-column height atlas (binding 5, the
    same `ColumnHeights` the marches read, resolved through `resolveColumn`).
    Each azimuth's horizon is the *maximum* over its samples of
    `tan = (columnTop - shadingPoint.y - 1) / d` - tan is monotonic in the
    elevation, so the max is the highest horizon - and one `sqrt` turns it into
    `sin`. The scan reports the SUM over azimuths of `1 - sin`.
  - **2 SDF rays**: one up the normal (a cosine-weighted hemisphere carries most
    of its energy overhead), one into the scan's worst azimuth *just above* the
    horizon it reported. `visibility = (scanSum + ray1 + ray2) / 8` - one mean of
    eight, so the rays carry 2/8 of the weight and a ray with nothing to hit
    reports the sky it sees (from the bottom of a slot canyon the zenith really
    is visible).
  - The **bias of one voxel** in the horizon is what makes a flat plain read a
    horizon of exactly 0: the atlas stores *highest solid + 1*, so a column flush
    with the shading point still reads one voxel high, and without the bias a
    flat field would lose ~55% of its sky in every azimuth.
- A **floor** (`scene.ambient.y`, `--ambient-floor`, default 0.12) is added on
  top, so a fully occluded point keeps a fraction of the sky it would have seen -
  dark, not black.
- The **rim** is sky light too, so it follows the same visibility (it used to be
  the one unoccluded sky term, strongest exactly where a cave wall is seen at a
  grazing angle).

**Levers** (flags, pass-61 style; both are new and flag-only):

- `--no-ambient` is the **control**: it selects the pre-62 formula verbatim
  (`skyAmb = skyBaseColor(sRdWorld)`, the same `mix(...) * (0.55 + 0.45*shadow)`
  and the same unoccluded rim), so the shipped picture can be compared against
  the upgrade without a rebuild. `--ambient` restores the default (last one
  wins).
- `--ambient-floor <0..1>` sets the floor (clamped; `0` = black caves, `1` = a
  flat fill for orientation).

`scene.ambient.x` carries the switch, `scene.ambient.y` the floor (`< 0` = the
shader's own default, so "unset" is spelled once). The uniform is filled in the
renderer's per-frame `SceneUniform::update` from `vv::core::options()`; the tests
pin the writer, the parser and the shader text (the pass-51 lesson).

**Measured, on the real terrain** (the game's default `TerrainConfig`, seed 1337;
`tests/ambient_mirror.hpp` is the CPU mirror, driven by a scratch probe over
`topSolidVoxels` columns; the game's SDF box is 192x110x192, the probe uses
96^3):

| scene | 6-azimuth scan mean | sky visibility (scan + 2 rays) |
| --- | --- | --- |
| noon open field | 0.953 | 0.965 |
| mountain ridge (highest) | 1.000 | 1.000 |
| hillside, normal leans | 0.501 | 0.625 |
| valley floor (lowest in a 512-voxel window) | 0.530 | 0.647 |
| darkest surface point in a 321x321-voxel window | 0.093 | 0.320 |

The shelters this world has (valleys, the lee of ridges, notches) lose a third to
two thirds of their ambient; open ground and ridges keep all of it. The old
formula could not express the difference: its dark end was 0.55 *everywhere*
(`shadow = 0`), its bright end 1.0 - and both moved when the camera turned.

**Finding: the shipped terrain has no caves, overhangs or tunnels.** Every column
solid-below / air-above, checked with 1-voxel scans of `typeAt` and
`topSolidVoxels` in six regions (56k+ columns total, plus a 17-voxel sweep over
+/-1200 voxels: 20,164 more) - zero air cells under a solid top. The "overhang
warp" in the density model folds the *surface*; it does not put rock over air. So
the two SDF rays escape in every scene above (they act as the zenith term), and
"cave" acceptance cannot be judged on the current world: the darkest thing the
terrain offers is the 0.093 notch in the table. The rays are still the
3D-correct half of the design and the unit tests pin them on a synthetic cave
under a live field (both rays meet the ceiling after the lift step, `t/32 = 0.156`
each, the point reads 0.04 instead of 0.25) - if the terrain ever grows a roof,
the term is already there.

**Look change on the noon field, measured** (sunlit, up-facing surface; ambient
plus the sun term, not ambient alone): red -21..-23%, green -13..-15%, blue
-2..+5% depending on the view ray. The old ambient on the field was the
*below-horizon* sky sampled through the camera (red ~0.75-0.78); the new value
(0.504 / 0.694 / 1.064) is within a few percent of the cosine-weighted sky
integral for an up-facing surface (0.51 / 0.67 / 0.97 + the horizon haze band) -
i.e. the field got *more correct*, a little darker and less washed out. Shadowed
open ground moves the other way, +23% in red, because it is sky-lit and always
was: the old 0.55 cut was standing in for occlusion. `--no-ambient` restores the
old numbers exactly if the owner prefers them.

**Tests** (`testAmbientVisibility`, new; `tests/ambient_mirror.hpp`, new). The
shader's text is pinned (every constant, both formulas, the scan, the two rays,
the mean of eight, and - mechanically - that the pass-62 branch contains no
`sRdWorld`, which is the 360-degree-turn acceptance criterion). The mirror is
checked against exact hand-derived values: a flat plain is exactly 6.0 (every
azimuth unobstructed) and one voxel up is too; a single spire at exactly the
first azimuth's d = 1 cell gives 5 + (1 - 9.5/sqrt(1+9.5^2)); a roof 16 voxels up
scans to 0.0021; a synthetic cave under a live 24^3 field is exactly
`(6*scan + 2*(5/32)) / 8`; the floor defaults, clamps and its two ends. The
command-line tests cover `--no-ambient`, `--ambient`, last-one-wins, the clamped
floor values, the non-numeric and missing-value failures and the startup line.

**Verified in the sandbox.** Release + debug configured and built warning-free,
`ctest` 100% (1/1) on both; the standalone g++ suite is green; the shader
compiles with `glslangValidator` and through the build's own step (release SPV
md5 `fe1ca032e9ce4fe8a3ee90dbc36ff269`, debug `b999c61149cfebed7a62b0b628c5e71d`;
`voxels.comp` itself `0e30ad219e400c63afc41cb3f4252167`). This box has no GPU or
window, so the on-device look and the 360-degree-turn check belong to the owner.

**Not done, deliberately.** The scan samples the near region only (binding 5, via
`resolveColumn`); the far-LOD heights (binding 6) are not in it, because the near
region is authoritative at 32 voxels and the far fields are camera-anchored - a
scan that reached into them would pop as the region moves. The ambient *gain*
(the overall level) is not a lever yet: `--ambient-floor` moves the cave end, and
`--no-ambient` is the control. If the field reads too dark, the next pass adds
`--ambient-gain` rather than re-tuning the shipped numbers.
