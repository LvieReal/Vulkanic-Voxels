# Agent Notes — Vulkanic Voxels

Notes for AI agent sessions (Arena.ai Agent Mode) working in this repository.
**This chat is not persistent — read this file first.** It records what was
done, how to verify work in the sandbox, and the project's roadmap. Keep it
updated when you finish a pass.

## Project overview

Voxel rendering experiment: a single 64×64×64 voxel chunk ray-traced in a
Vulkan **compute shader** (one thread per pixel, `resources/shaders/pixels_rgba.comp`),
presented through a Qt 6 Widgets shell. C++23, CMake ≥ 3.26.

Code map:

| Path | Contents |
| --- | --- |
| `src/ui/` | Qt shell (`AppWindow`, `VulkanWidget` — hosts swapchain + game loop) |
| `src/platform/` | **Platform abstraction layer** (see below) — keep OS headers out of everything else |
| `src/vulkan/` | Renderer, chunk atlas GPU resources (`VoxelResources`), swapchain, device/surface management |
| `src/voxel/` | World/chunk/voxel data model (`VoxelTypes` palette, `VoxelConfig`, chunked `World`) |
| `src/terrain/` | Deterministic noise (`Noise2D`) + `TerrainGenerator` (fBm heightmap, layered types) |
| `src/render/` | Scene uniforms, lighting config, shader data layouts |
| `src/core/` | Camera, timer, runtime paths, file loading |
| `resources/shaders/` | GLSL compute shaders, compiled to SPIR-V at build time by glslang |
| `tests/` | Pure-logic test suite (no Qt/Vulkan); `ctest --test-dir build` |
| `scripts/` | `build-linux-toolchain.sh` — sandbox build toolchain (see below) |
| `cmake/` | Build functions (GameTarget, Tests, Shaders, Packaging, …) |

### Platform abstraction layer (`src/platform/`)

Flow: `VulkanWidget` → `resolveNativeWindow()` (fills `NativeWindow`, a
Qt/Vulkan-free struct of opaque handles) → `VulkanRenderer` →
`requiredVulkanInstanceExtensions()` + `createVulkanSurface()`.

- Backends: Win32, XCB (X11), Wayland, MoltenVK (macOS). On Linux both XCB and
  Wayland are compiled in when headers exist (`VV_HAVE_XCB` / `VV_HAVE_WAYLAND`,
  decided in `cmake/GameTarget.cmake`) and chosen at run time from the QPA
  platform name.
- All WSI entry points are resolved via `vkGetInstanceProcAddr`, never linked
  directly.
- Qt private headers (`Qt6::GuiPrivate` / the QPA native interface) are
  **optional** (`VV_HAVE_QT_QPA`). Without them: Windows/macOS/X11 use public
  APIs (`QWidget::winId()`, `QNativeInterface::QX11Application`); the native
  Wayland backend is unavailable (error message points to XWayland:
  `QT_QPA_PLATFORM=xcb`). Reason: many Qt distributions (e.g. some MSYS2/MinGW
  setups) ship no private dev files.
- To add a new backend: extend `NativeWindowKind`, `NativeWindow`, the resolver
  (`QtNativeWindowResolver.cpp`), and `VulkanSurfaceFactory.cpp`. Nothing else
  should need touching.

### Input model (as of pass 2)

Mouse is locked to window center (hidden cursor) while playing, with
`setMouseTracking(true)` so look works without holding any button. `Esc`
releases the pointer and pauses the camera; `Esc` or a click re-locks. Focus
loss always releases the pointer and clears key state. Mouse grab goes through
`QWindow::setMouseGrabEnabled` and is deferred to the first Expose event if the
window is not yet visible (this is what removed the old
`setMouseGrabEnabled: Not setting mouse grab for invisible window` warning).

### World architecture (as of pass 2)

- Infinite on X/Z, chunked: 32×128×32-voxel chunks (full world height per
  chunk), CPU cache in `vv::voxel::World` keyed by `ChunkCoord` with
  radius+1 hysteresis eviction (`ensureRegion`).
- Terrain: `src/terrain` — hand-rolled deterministic Perlin-style `Noise2D`
  (integer-hash gradients, no deps; SIMD left for the optimization pass),
  `TerrainGenerator` = hilliness-masked fBm heightmap + layered types
  (grass/dirt/stone, sand under `sandLine`, snow above `snowLine`, bedrock at
  y=0). Same seed → identical terrain on every platform.
- GPU: `vulkan/VoxelResources` = one device-local **chunk atlas** storage
  buffer (one byte per VoxelType, packed 4-per-uint32, slot per chunk) + one
  host-visible **chunk table** SSBO (region grid cell → slot index,
  `kEmptySlot`=0xFFFFFFFF). The renderer (`rebuildChunkRegion`) maps chunk
  coords → atlas slots with a free-list, batch-uploads new chunks (single
  staging buffer + `vkQueueWaitIdle` — rare, only on chunk-border crossings)
  and rewrites the table. `updateWorld(cameraPos)` is called every frame and
  no-ops unless the camera chunk changed.
- Shader `pixels_rgba.comp`: DDA over the region AABB; per-step chunk lookup
  (floor-div coords → table slot → packed word fetch); exponential distance
  fog to sky; sky-skip early-out (rays whose region segment stays above
  `maxHeightVoxels()` can never hit — `push.grid.w` carries the bound, and the
  DDA breaks once ascending above it). Colors come from the **voxel palette
  buffer** (binding 4, filled from `kVoxelTypeInfo` at renderer init) — no
  colors are hardcoded in the shader; a bindless texture array is the planned
  replacement. Push constants must match `vv::render::PushConstants`.
- Default view: render radius 6 chunks (13×13, ~22 MB atlas), fog density
  derived from the radius in `computeFogDensity()`, maxTraceSteps 512.

### GPU error handling (as of pass 2.1)

After a user-reported driver crash ("renderer freezes, no error messages"):
- All `vkAcquireNextImageKHR` / `vkQueueSubmit` / `vkQueuePresentKHR` /
  record failures now set a device-lost state with a message; the widget
  shows one message box and stops rendering (`VulkanRenderer::deviceLost()`).
- Optional `VK_EXT_debug_utils` messenger routes validation messages to
  stderr; the Khronos validation layer is enabled only when the
  `VV_VALIDATION` env var is set and the layer is installed.
- Fixed a real bug found during the investigation: with an exactly
  region-sized atlas, hysteresis eviction lags demand, so the first chunk
  crossing after startup always failed (silently) and the region never
  re-centered. `rebuildChunkRegion` now (a) collects *all* slotless region
  chunks (CPU-cached chunks that lost their slot only need a re-upload), and
  (b) falls back to releasing slots outside the region when free slots run
  short — always makes progress, teleports included.
- TDR mitigation: maxTraceSteps 1024 → 512, sky-skip early-out (pass-1's
  random chunk hit within ~2 steps; air traversal of 400+ cells was the new
  worst case). If device loss persists on weaker GPUs, next steps: reduce
  `renderRadiusChunks`, then the heightmap-guided skipping planned for the
  optimization pass.

### Pass 2.2: the "noisy rings" regression

After 2.1 the user reported the world rendering as "noisy rings" instead of
voxels (screenshots did not sync into the sandbox — uploads may not reach
the filesystem; debug from code when that happens). Root cause: the new
palette fill used `source[type * 3 + c]`, but `VoxelTypeInfo` is
9 floats per type (`{top, side, bottom}`), so every material got a
scrambled brown/green color — voxel structure became invisible and only the
distance-fog banding (concentric rings around the view) remained visible.

- Fix: palette building extracted into the pure, unit-tested
  `vv::voxel::buildVoxelPalette()` (`src/voxel/VoxelTypes.{hpp,cpp}`);
  `VoxelResources::createPalette` just uploads its output. The test would
  have caught the original bug.
- Lesson: GPU-facing data-layout code should be built by small pure
  functions covered by the headless test suite, not raw pointer arithmetic
  inside Vulkan setup code.

### Pass 2.3: "pattern on top of voxels" + distance cropping (superseded by 2.4)

After the palette fix the user still saw a noisy pattern over the terrain
and terrain cropping that shrank when close/low and grew when high/far
("maybe rays miss" — exactly right). One root cause, two symptoms:

- **The DDA step budget counts cell crossings, not distance.** 512 steps
  reach only ~296–362 world units (a 3D-diagonal ray spends √3 crossings
  per unit). A ray that exhausts its budget outputs pure sky while a
  neighbor that hits terrain just inside the boundary is only 92–96%
  fogged → a per-pixel-varying 4–8% discontinuity = the noise pattern.
- **The fog was thinner than the region** (99.8% opaque at 711 units vs.
  588 region diagonal), so fog never caught up before rays were cut —
  making the cut visible as a crop shell around the camera (from low
  altitude, horizontal rays burn their budget on air and miss far terrain;
  from higher up rays steepen and reach further).

Fix (fog is now the primary ray terminator):

- `computeFogDensity()` = `kFogTail / regionWidth` — fog is 99.8% opaque
  (`kFogTail = 6.215 = -ln(0.002)`) exactly at the region width.
- The shader cuts every ray at the 99.8%-fog distance (`fogCutT =
  kFogTail / fogDensity`; `tEnd = min(tEnd, fogCutT - tEntryOffset)`).
  Beyond it, sky and fogged terrain are indistinguishable, so budget-,
  region- and fog-cuts all produce the same color — no artifact, by
  construction.
- `maxTraceSteps` is a pure safety net: default 1024, and the renderer
  raises it to ≥ 1.75× the region width (in voxels) so it never binds
  before the fog does (worst case ~√3 crossings per unit).
- Trade-off: fog is ~1.7× thicker than before (95% at ~200 units for the
  default radius 6). More view = raise `renderRadiusChunks` (more GPU
  work; fog and budget scale with it automatically).
- `kFogTail` is duplicated in shader and C++ with a KEEP-IN-SYNC note
  (push constants could carry it, but one constant is not worth it yet).

### Pass 2.4: fog cut vs. region box geometry (real bug, secondary)

The 2.3 fix (fog cut at the full region WIDTH) did not remove the artifact:
the user retested and saw the same noisy rings + cropping. Re-auditing the
geometry found the real bug, which had survived all previous rounds:

- The loaded region is a BOX around the camera's chunk. The camera is only
  ~[radius, radius+1] chunk extents (192-224 world units at radius 6) from
  the NEAREST side faces - about HALF the region width the fog was tuned to.
- Rays toward a near face exited the region at ~200 units, far inside the
  416-unit fog cut, and the miss path returns UNFOGGED sky. Terrain just
  inside the boundary was only ~94-96% fogged. So the region boundary was
  plainly visible as: a square, direction-asymmetric "crop" (near faces end
  at ~200u, corner directions at ~300-416u - matches "the higher/farther I
  am, the more I see it", including the ground-plane cut on Y), plus a
  per-direction discontinuity between 0% and ~5% terrain visibility at the
  boundary = the residual noise/rings. 2.3 only shrank the mismatch
  (17.5% -> ~5%); it did not remove the boundary itself.
- Fix: the fog cut is now the PER-FRAME distance from the camera to the
  nearest region SIDE face (`VulkanRenderer::fogCutDistance()`), pushed as
  `fogDensity = 1 / cutDistance`. Any ray's region exit lies at >= the
  perpendicular distance to the face it crosses, i.e. >= cutDistance, so
  EVERY termination (hit, budget, region exit, fog cut) now happens where
  the fog is >= 99.8% opaque - the boundary is invisible by construction,
  from every direction. (Top face: no terrain above worldHeight, sky there
  is correct. Bottom face: y=0 is solid bedrock, nothing exits through it.)
- Fog curve changed from `1 - exp(-density*d)` to
  `1 - exp(-kFogTail * (d/cut)^4)` (kFogTail = 6.215 = -ln 0.002 lives ONLY
  in the shader now). The exponent-4 ramp keeps mid-distance terrain much
  clearer (~32% fog at half the cut vs ~95% for the linear curve at the old
  density) while still hitting 99.8% exactly at the cut. This also directly
  addresses "losing depth on Y": ground below a high camera is far less
  fogged than before.
- The C++/shader sync contract simplified: `fogDensity = 1/cutDistance`,
  shader cuts at `1/fogDensity` and assumes 99.8% opacity there. No shared
  constant anymore.

Also eliminated during the audit (do not re-investigate):

- Sky-skip ceiling is a TRUE bound: fbm is amplitude-normalized to [-1,1],
  hilliness mask is in [0.35, 1.2], so heightAt <= 44 + 1.2*26 = 75.2 <
  maxHeightVoxels() = 80.
- Shader staleness: CMake compiles .comp -> .spv with a proper DEPENDS and
  POST_BUILD copy_if_different into the exe dir; the 2.2 palette fix (CPU
  data only) visibly changing the image proves the current shader was
  active on the user's machine.
- Single fog writer verified (only drawFrame writes push.camera.y).

Remote-debugging instrumentation added (uploads/screenshots do not reach
the sandbox, so the user's machine must report itself):

- Window title (1 Hz) + one stderr line at startup:
  `build <git-hash[-dirty]> | fogCut=<u> fog=<density> steps=<n>
  region=WxH@(cx,cz) slots=used/total | <fps>`. Generated header
  `core/Version.h.in` -> `${CMAKE_BINARY_DIR}/generated/core/Version.h`.
  First thing to check on any report: does the hash match the expected
  commit? A stale binary otherwise perfectly mimics a rendering bug.
- `VV_DEBUG_TERM=1`: miss pixels false-colored by termination cause -
  red=budget exhausted, green=region exit, blue=fog cut, magenta=sky-skip,
  yellow=ascended above terrain, near-black=no region intersection. Terrain
  hits render normally. Reading the colors of any remaining artifact
  identifies the mechanism immediately.
- `VV_DEBUG_SSAA=1`: 4 jittered rays per pixel (rotated grid). If the
  "noisy pattern" disappears under SSAA it is sub-pixel aliasing/moire
  (expected for 1 ray/pixel over a voxel grid at distance), not a logic
  bug - the proper fix then is TAA/jitter, not more terminator tuning.
  Costs ~4x compute; lower renderRadiusChunks if the GPU objects.

### Pass 2.5: THE root cause - sky-skip ceiling was 0 (unassigned member)

The 2.4 fix was verified-correct but the user still saw the same noisy
rings + cropping (and correctly thinner fog). The VV_DEBUG_TERM
instrumentation added in 2.4 finally localized it:

- User report: rings render MAGENTA (= TERM_SKY_SKIP), and green / red /
  yellow never appear. The absence of green/red/yellow is geometric proof
  the fog-cut side works (region exit can never beat a fog cut placed at
  the nearest-face distance; the budget cannot be exhausted within it;
  ascending rays are sky-skipped before they could trigger the ascend
  break). So the sky-skip early-out itself was claiming rays that should
  hit terrain.
- Root cause: `m_maxTerrainVoxelY` was declared `= 0` in the header,
  read once in the push-constant write (`push.grid.w`), and NEVER
  ASSIGNED anywhere. The 2.1 commit intended
  `min(maxHeightVoxels(), worldHeight-1)` (= 80) but the assignment was
  lost, most likely to the same-file parallel-edit race that struck three
  times before. The sky-skip ceiling on every build since 2.1 was 0.
- Effect: sky-skip fired for ANY ray that does not descend below the
  bedrock floor (y=0) before leaving the region - including rays that
  would hit terrain well inside it. From a camera at y~56, only steep
  rays (diving below y=0 within the region) were traced at all: visible
  terrain was an angle-limited patch whose boundary follows the terrain's
  own height contours = the "noisy rings ON TOP of voxels", and hilltops
  passed over by shallow rays were clipped = the "cropping" (worst on Y,
  worse when higher/farther). This single bug explains every report
  since 2.1 (when sky-skip was introduced - "noisy rings instead of
  voxels"); 2.2's palette fix revealed the visible patch, 2.3/2.4's fog
  work was real but secondary.
- Fix: assign m_maxTerrainVoxelY in createVoxelWorldAndUpload right after
  the world is created: clamp(terrain.maxHeightVoxels(), 1,
  worldHeight-1). debugStats() now also prints skyCeil= (this would have
  caught the bug immediately).
- Audited all 48 VulkanRenderer members for the same landmine
  (declared-but-never-assigned): all others are written via out-params,
  increments, or are intentionally default-constructed objects.
- Lessons: (1) a member default that silently flows into a push constant
  is a correctness trap - push values should either be computed at the
  write site or the init must be grep-verifiable; (2) the remote debug
  instrumentation paid for itself in one round-trip: a color-coded
  terminator + absent-colors reasoning pinned the mechanism without a
  single local repro; (3) the user's "it's a real traversal artifact" was
  exactly right.

### Pass 3: optimizations - heightmap-guided GPU traversal + vectorized CPU generation

User-verified clean after 2.5 ("cropping and noisy rings are gone, build
matches"); scope agreed: heightmap-guided air-skipping, vectorization
(SIMD), with assembly inspection explicitly deferred to a later pass.

**3a. Heightmap-guided traversal (GPU).** The old tracer stepped through
every voxel cell, air included (~200-700 storage fetches per ray). Now:

- CPU: `Chunk::heightMap()` = per-column (highest solid voxel Y + 1, 0 =
  all-air), a ground-truth scan of the actual voxel data (lazy, invalidated
  by `set()`), packed two-u16-per-u32 (`heightMapWords()`). Uploaded as a
  new SSBO (binding 5) alongside each chunk in `uploadChunks`.
- Shader: 2D DDA over XZ *columns*. A column whose whole ray segment stays
  at/above the height bound costs ONE step (the air-skip). Columns the ray
  can reach get a bounded cell walk that starts no higher than the bound
  (cells above it are provably air), with the atlas slot resolved once per
  column. The bound is CONSERVATIVE, so overhangs/caves/edits stay correct
  (they only make the walk longer, never wrong).
- **Parity guarantee**: CPU mirrors of both algorithms live in the test
  suite; 12,000 rays over a pure heightfield AND over
  overhang content (floating slab, wall, carved shaft) produce identical
  hit cell / t / entry face / type. The rewrite cannot change the image.
- Work per ray is now ~(columns crossed <= ~sqrt(2) x fog cut) + (cells in
  non-skipped columns <= |rd.y| x fog cut + 1 each); the step budget
  (1024) counts column steps and never binds before the fog cut.
- Sync contracts (breaking = silent corruption): heightmap slot stride in
  u32 words = (chunkSizeX * chunkSizeZ + 1) / 2 in THREE places (shader
  computes it from push constants; Chunk::heightMapWordStride();
  VoxelResources::heightSlotWordStride()); u16 pair packing: column i even
  -> low half of word i>>1 (explicit CPU-side packing, endian-independent);
  heightmap row-major X + Z*chunkSizeX; 0xFFFF sentinel = column not
  loaded (forces the exact walk - can never hide geometry).

**3b. Vectorization (CPU).** Terrain generation was the last double-based
scalar hot path:

- `Noise2D` is now float32: `noiseF`/`fbmF` (scalar reference) and `fbm4`
  (4-lane SSE2; x86-64 baseline, no dispatch needed - the 32-bit multiply
  is emulated from 16-bit lanes since SSE2 lacks pmulld). NEON is future
  work; other platforms fall back to the scalar reference.
- **Bit-exactness contract**: `fbm4` lanes == `fbmF` bit-for-bit (same op
  order; `-ffp-contract=off` on the terrain TUs in both CMake targets so
  GCC/Clang cannot FMA-contract differently per target; MSVC never
  contracts). Verified by unit test incl. lattice-line coordinates. This
  is what keeps `testChunkMatchesGenerator` meaningful: chunks generated
  via `heightAt4` match `typeAt` via `heightAtF` exactly.
- `World::generateChunk` fills 4 columns at a time and writes only the
  solid range [0, surface] (air stays from the Chunk constructor) instead
  of all 128 cells.
- Terrain VALUES shift microscopically vs pass 2.5 (float32 vs float64
  noise) - same seed still produces identical terrain for a given build
  (the determinism promise), but old and new builds do not produce
  bit-identical worlds. Documented in Noise.hpp; float32 is exact for
  lattice coords up to ~2^24 (region coords are ~600).
- Measured on the 2-core sandbox (169-chunk region): 2.0-2.4 ms/chunk ->
  0.8-1.1 ms/chunk (~2.5x). Startup region build ~400ms -> ~160ms; chunk
  border crossings ~10ms -> ~4ms.

**Pass 3.1 hotfix: "voxels disappeared" - heightmap copied into the voxel
atlas.** First user run of pass 3 showed a fully empty world (pure sky).
Root cause (user's bet was correct - the air-skip pipeline, not SIMD noise;
noise mathematically cannot blank terrain since height = 44 +- 37 is always
solid): `uploadChunks` built ONE region list containing both the voxel
regions and the new heightmap regions and issued ONE vkCmdCopyBuffer
targeting the voxel atlas. A VkBufferCopy region does not carry its target
buffer - the CALL chooses it - so every chunk's 2 KB of heightmap bytes
landed in the first 2 KB of its voxel atlas slot (corrupting it), and the
height atlas (binding 5) was never written: uninitialized device memory
read as zeros -> every column bound 0 -> every ray air-skipped -> all rays
miss -> no voxels. Fix: two region vectors + two copy commands (voxel
regions -> voxel atlas, height regions -> height atlas), with a comment
explaining why they must never be mixed. Lesson added to the GPU-data-
layout rule: multi-buffer uploads need per-target region lists; a mixed
list silently retargets data. (Invisible to the headless suite by
construction - it is Vulkan command wiring, not logic; the parity test
remains valid for the traversal algorithm itself.)

Deferred (user decision): assembly inspection ("too far, maybe later").
NEON path for ARM/macOS; AVX2 8-wide noise (marginal over SSE2 for this
hash-heavy workload); TAA/jitter for sub-pixel aliasing if it ever shows
up (user: it is NOT the current artifact class - agreed, 2.5 proved it).

### Pass 3.5: fps cap, 2x render distance, AO, sky, debanding

User confirmed pass 3.1 ("voxels are back") and asked for, before LOD:
uncap the 60 fps ceiling, double the render distance, port the vertex-AO
from their WGSL reference renderer (saved verbatim as
docs/reference_renderer.wgsl), a brighter sky, and dithered debanding.

- **FPS cap - there were TWO independent caps.** (1) The widget's QTimer
  ticked every 16 ms -> max ~62.5 fps by itself; now 0 ms (tick on every
  event-loop pass). (2) choosePresentMode preferred MAILBOX -> FIFO, both
  of which sync to the compositor (60 Hz). Now IMMEDIATE > MAILBOX > FIFO
  (uncapped by default; tearing is possible with IMMEDIATE), overridable
  with VV_PRESENT=immediate|mailbox|fifo (fifo restores vsync).
- **Render distance 2x**: renderRadiusChunks 6 -> 12 (region 25x25 = 625
  slots, atlas ~83 MB). Fog cut, step budget and slot count all scale
  automatically (cut = 384-416 u from the camera to the nearest region
  face). Startup generates 625 chunks (~0.7 s, one-time); each border
  crossing streams 2(2r+1)-1 = 49 chunks (~45 ms hitch) - the LOD pass
  will make streaming incremental.
- **Ambient occlusion** (ported from the WGSL reference): vertexAO corner
  darkening (1 - (side1+side2+corner)/3, two sides -> 0) + calculateAO -
  8 voxels sampled around the hit face in the plane one cell out along
  the normal, four corner AO values bilinearly blended by the hit's
  fractional position on the face; multiplies the lit color. AO fades to
  1 only in unloaded cells (treated as air), which the fog already hides.
  CPU truth-table test added (note: 1-1/3 != 2/3 in float32 - 1 ulp - so
  the test uses tolerance; the shader formula itself is the port target).
- **Sky** (ported compute_env): horizon/zenith smoothstep gradient +
  horizon haze (1,0.95,0.9) x (0.12 h + 0.04 h^2) + sun glow
  (pow(sun,48) x 0.8) and core (pow(sun,512) x 8), using the configured
  sun dir/color. The old LightingConfig defaults were effectively
  night-dark ((0.05,0.08,0.12)/(0.2,0.3,0.5)) - hence "too dark". Now the
  reference palette: horizon (0.8,0.9,1.0), zenith (0.45,0.62,0.95), warm
  sun (1,0.95,0.85), sun dir normalize(0.5,1,0.5). The sky doubles as
  the fog target, so terrain fades into exactly the sky it occludes,
  including sun haze.
- **Debanding**: interleaved gradient noise (+-0.5 LSB) added in
  packColor before the 8-bit quantization - converts sky gradient bands
  into imperceptible noise.
- Not ported (yet): selection outline, ground grid, the reference's
  ambient/direct/rim shading split (our lighting kept; AO applied on
  top).

### Pass 4: far-LOD height field (2k-unit view distance)

User asked for LOD + "infinite" render distance, accepting an arbitrary
huge distance for the first test (full detail at that range would need
~1.25 GB; the far field needs 4 MB).

Architecture - two-tier LOD that leaves the near tier untouched:

- NEAR (unchanged, parity-tested): full-detail voxel region, radius 12
  chunks, fog hidden the region boundary (when far is off, exactly as in
  3.5).
- FAR (new, binding 6): a coarse height field covering a square of
  2*farLodRadiusChunks chunks per side (default 64 -> +/-2048 units),
  cells of farLodCellVoxels (4) voxels. One u32 per cell:
  u16 (max solid height + 1) | u8 surface type << 16, row-major
  X + Z*dim - see terrain/FarField.{hpp,cpp} (pure, unit-tested; one
  heightAtF per cell at the cell CENTER). 1024x1024 cells = 4 MB.
- SHADER: after a ray exits the near region without a hit (and pc.far.z
  > 0), a second 2D DDA marches the far grid (one u32 fetch + compares
  per cell; budget = dim.x + dim.y). Side-face hit when entering a cell
  below its surface top; top-face hit when crossing the top plane inside
  a cell; height 0 = no surface (never hit - a ray below y=0 over an
  air/outside cell must NOT hit; found by the DDA-vs-brute test).
  Far hits shade with the cell's surface type (palette) + normal + fog,
  no AO. The near traversal is byte-identical (its tEnd is still
  min(region exit, fog cut); with far active the cut >= 1536 > region
  exit ~588, so the near loop always ends at the region exit).
- FOG: with the far field active, fogCutDistance() measures the FAR box
  faces (cut in [1536, 2560] as the camera wanders within the recenter
  hysteresis; the wall breathes subtly). The near-region boundary is
  hidden GEOMETRICALLY (the far field continues the same terrain), not
  by fog. Invariant (enforced in VoxelConfig::isValid):
  far half-extent >= recenter hysteresis + near-region half-diagonal
  -> farRadius >= 2*renderRadius + 4, so the cut always exceeds the
  near-region exit along any ray (no gap between the two marches).
- CPU: FarField::build runs on a std::thread (1M noise evals ~0.3-0.6 s
  background; renders start immediately with the old 400u fog wall and
  the far field pops in once). VulkanRenderer owns the thread state:
  launchFarFieldBuild / ensureFarField (called from updateWorld);
  the thread only touches m_farPending (released via m_farPendingReady)
  and the const terrain generator; cleanup() joins it BEFORE the world
  is destroyed. Recenter: rebuild when the camera strays > max(4,
  far/4) chunks (= 512u) from the field center; the old field keeps
  rendering until the new one uploads (camera always inside both).
  VoxelResources::uploadFarField: staging + single copy, queue idle
  (rare).
- Push constants grew to exactly 128 B (spec minimum guarantee):
  ivec4 far (origin vox X/Z, cell dims; z=0 = off) + vec4 farParams
  (cell footprint). static_assert in SceneData.hpp; the far buffer
  always exists (4-byte dummy when disabled) so binding 6 is always a
  valid descriptor.
- debugStats() title now shows far=on/building/off(dims@center).

Testing: testFarField (builder conventions, packing, determinism) and
testFarMarch (CPU mirror of the far DDA vs dense brute force over the
same synthetic grid incl. towers/holes; hit/t agreement on 4000 rays).
Two reference bugs found while writing it (both fixed in test AND
shader semantics): outside-grid cells must be "no data", and height-0
cells have no surface - a ray below y=0 there must not hit. Also mind
C++ int+unsigned promotion when mixing negative origins with uint32
math (the test hit this: -48 + 4u wrapped to 4.29e9).

Known trade-offs (first test, by design):
- Far silhouettes quantize by ~ +-cellVoxels/2 of height (center
  sampling); visible as slight steps on distant ridgelines against the
  sky. Mitigation later: conservative max-of-samples seam band or a
  finer (C=2) inner far ring.
- The seam ring (near region edge, ~400u, fog ~1-2%) can show small
  up/down steps where coarse hands off to fine.
- No shadows/AO in the far field; the sun lights it flat.

### Pass 4.1: altitude crop fix + incremental (frustum-prioritized) streaming

Two user reports on pass 4: terrain crops away when flying high, and
border crossings stutter (~50 ms).

**The altitude crop** was a far-LOD integration bug, found by reading
("the higher I am" => camera above y=128 => outside the near-region
box, the only situation where these fire): two pre-far-LOD early sky
returns - the near-region AABB MISS return and the sky-skip return -
never reached the far march. From high altitude, shallow downward rays
either miss the near box entirely or stay above maxTerrainY while
crossing it, and both returned sky even though the far field continues
the terrain beyond. Fix: both conditions now skip only the NEAR march
(nearMarch = intersect && !skySkip) and fall through to the far march;
with far LOD off they still return sky immediately (unchanged
behavior). The near march's tEndRegion is 0 for missed-box rays, so
its loop no-ops cleanly.

**Incremental streaming** removes the border-crossing hitch. Design
(main-thread time-slicing, no new threads - deliberate after this
codebase's history of edit/threading races):

- The chunk atlas gains ONE SPARE RING of slots ((2r+3)^2 = 729 at
  r=12; region table stays (2r+1)^2). Streamed chunks upload into
  spare slots the ACTIVE table never references, so in-flight frames
  cannot tear and the uploads need no full-device stall.
- updateWorld: on a border crossing, beginRegionMove() computes the
  pending coords (new-region coords not already in slots), sorted by
  frustum priority (camera-facing first, then distance; the sort key
  is a small static function). The OLD region keeps rendering
  untouched while pumpRegionStreaming(3 ms/frame) generates
  (World::ensureChunk, new idempotent primitive) and uploads in
  batches of 4 (each uploadChunks does its own queue idle - ~1 ms at
  these sizes).
- finishRegionMove() (pending empty): release out-of-region slots,
  evict the CPU cache beyond the usual +1 ring
  (World::evictOutside, new primitive), one deviceWaitIdle (table
  swap safety), rewrite the region table, move the center.
- Fallbacks: if pending > free slots (teleport-scale move) or the
  camera outruns the stream by > 2 chunks, fall back to the
  synchronous rebuildChunkRegion (unchanged legacy path, also used at
  init). Mid-stream target changes re-aim the pending list; already-
  streamed chunks keep their slots.
- Why the streaming edge is invisible: the gap between old and new
  regions renders as FAR terrain (the coarse field continues the same
  heightmap), so the swap is a coarse-to-fine refinement at ~350-400 u
  where fog is a few percent - not a hole.
- Steady-state costs unchanged: same 625 resident slots + 104 spare
  (atlas 96 MB), same CPU cache hysteresis ((2r+2)(2r+1) chunks).

Tests: testWorldEnsureChunk (ensureChunk idempotence, evictOutside
hysteresis parity with ensureRegion); all existing suites unchanged.
The renderer streaming logic is Vulkan-wiring (headless-untestable),
kept small and reviewed; the far march/parity tests still cover the
traversal semantics.

### Pass 5: out-of-bounds crop, branchless DDA, WGSL lighting, sun shadows, SSAA

**The last altitude crop** ("still there exactly when I fly out of
bounds"): with the camera above y=128, horizontal rays miss the near
box entirely (tEndRegion = 0), and the fog-cut early return
`if (tEnd <= 0) return sky` fired BEFORE the far march - one more
pre-far-LOD return living on the box-miss path. Now it returns sky only
when far LOD is off; otherwise the ray falls through to the far march.

**Branchless DDA** (cf. shadertoy.com/view/4dX3zl, user-requested): the
2D XZ steps in both marches (near columns + far cells) now advance via
masked selects (takeX ? delta : 0) instead of divergent if/else. Tie
rule preserved (X only on strictly-less) so the CPU parity mirrors are
unchanged and still pass. The vertical cell walks keep their loops
(short by construction).

**Anti-aliasing** - why not cones (Amanatides): the far LOD already IS
the cone approximation - 4-voxel cells match the pixel footprint at
~500u, so distance does not shimmer; full per-voxel cone tracing costs
the most exactly where it buys the least (near field, footprint < 1
voxel). The practical near-field answer is supersampling: VV_SSAA=1
(also VV_DEBUG_SSAA) enables the existing 4-rays-per-pixel path as a
supported mode (~4x compute; at 200-1000 fps there is headroom). TAA
(needs a history buffer) remains the future option if SSAA's cost ever
matters.

**Lighting** ported from the owner's WGSL reference (the last piece of
it): hemispheric sky ambient (mix(sky*0.35, sky, n.y*0.5+0.5)), direct
sun = lightColor * (0.2*lambert + 0.8*pow(lambert, 8)) (broad lobe),
fresnel rim = sky * 0.12 * pow(1-view, 5); AO scales everything,
shadows scale only the direct term. The ambient uses the sky in the
VIEW direction, so lit terrain and its fog background always agree.
Shadowed faces keep 35%+ sky ambient - no more pitch-black north faces.

**Sun shadows** - one shadow ray per hit pixel toward the sun:
- Near region: 2D column DDA (unit columns); per column ONE height-
  atlas fetch decides whether a blocker can exist (bound > ray height);
  only then a short exact-voxel walk (ascending ray, so the walk is
  0-2 cells). Cheap because the default sun (0.408, 0.816, 0.408)
  lifts the ray above all terrain (maxTerrainY+1) within ~20-40
  columns; that ascend check ends the march lit.
- Beyond the near region: coarse far-cell test (far height > ray
  height at column entry -> blocked). Conservative in the blocker's
  favor; fog hides the coarseness.
- Cap 256 columns; sun elevations <= ~3 degrees skip shadows (no cheap
  ascend bound). Acne guarded by normal + sun offsets off the surface.
- Parity test: testSunShadowMarch - CPU mirror vs dense brute force
  over a two-tier synthetic world (wall, tower, tall far ridge casting
  shadows INTO the near region); 3000 origins agree. Writing it found
  the usual reference-sampler trap: grazes shallower than sun.y*dt are
  invisible to the brute - the mirror is exact, so disagreements are
  re-sampled at dt/100 before counting.

**Verified**: shader compiles; Release+Debug warning-free; all tests
pass (12k traversal-parity rays, far march, shadow march, heightmaps,
SIMD bit-exactness, world APIs).

## Roadmap status

**Pass 1 — done (commit "Cross-platform platform layer…"):**
- [x] Linux/macOS support via the platform abstraction layer (no unconditional
      `windows.h` anywhere)
- [x] Scraped the Qt escape/pause menu (native in-engine UI comes later);
      Escape now toggles pointer lock + pause
- [x] Fixed the mouse-grab warning (deferred grab, see above)
- [x] README with per-OS build instructions
- [x] Fix: `setMouseTracking(true)` so first-person look needs no button held
- [x] Docs kept in-repo (this file + toolchain script) per user request

**Pass 2 — done (pending user verification):**
- [x] Voxel types (grass, dirt, stone, sand, snow, bedrock) with per-face
      colors instead of random colors
- [x] Terrain generation with noise (hand-rolled scalar Perlin fBm; pulling a
      web library was deemed unnecessary — revisit for SIMD pass)
- [x] Infinite worlds via chunking (X/Z only, full-height chunks, GPU chunk
      atlas + region management, distance fog)
- [x] Pure-logic test suite (`tests/`, runs headless in sandbox)

**Pass 5 — done (pending user verification):**
- [x] Out-of-bounds altitude crop fixed (fog-cut early return bypassed
      the far march on box-miss rays)
- [x] Branchless DDA stepping in both marches (parity-preserved ties)
- [x] AA: VV_SSAA=1 4x supersampling mode; far LOD doubles as the
      distance cone filter (cones-vs-SSAA reasoning in the pass notes)
- [x] WGSL ambient/direct/rim lighting model (shadow-aware ambient)
- [x] Sun shadows (near-exact column march + coarse far test), parity
      tested against dense sampling

**Pass 4.1 — done (user-verified: "much better, no holes"):**
- [x] Altitude crop fixed (early sky returns bypassed the far march
      from above y=128)
- [x] Incremental chunk streaming: frustum-prioritized, 3 ms/frame
      time-sliced generation + batched tear-free uploads into a spare
      atlas ring; sync fallback for teleports

**Pass 4 — done (user-verified: "working seamlessly"):**
- [x] Far-LOD height field: 2k-unit view distance (radius 64 chunks,
      4x4-voxel cells, 4 MB) on top of the unchanged near region;
      background-thread build + upload, automatic recentering, fog cut
      moved to the far boundary
- [x] Far march unit tests (builder + DDA-vs-brute-force parity)
- [x] Future: finer inner far ring / conservative seam band; true
      "infinite" via nested far rings (clipmap)

**Pass 3.5 — done (user-verified):**
- [x] FPS uncapped (0ms tick timer + IMMEDIATE present mode; VV_PRESENT
      overrides, fifo restores vsync)
- [x] Render distance 2x (radius 12; fog/budget/slots scale automatically)
- [x] Per-vertex simulated AO ported from the user's WGSL reference
      (docs/reference_renderer.wgsl)
- [x] Bright sky: reference palette + haze + sun glow/core; sky is the fog
      target
- [x] Debanding: IGN dither before 8-bit quantization

**Pass 3 — done (pending user verification):**
- [x] Heightmap-guided air-skipping on the GPU (column DDA + conservative
      per-column height bound; provably image-identical via the traversal
      parity test - 12k rays incl. overhang content)
- [x] Vectorized CPU terrain generation (4-lane SSE2 float32 fBm,
      bit-identical to the scalar reference; solid-range-only column fill;
      ~2.5x faster chunk generation)
- [x] Assembly inspection deferred to a later pass (user decision)

**Pass 2.1 — done (crash investigation after user report):**
- [x] Region slot-exhaustion bug fixed (first border crossing always failed
      silently; see "GPU error handling" above)
- [x] Vulkan errors surfaced (device-lost state + UI message box); previously
      every GPU error was swallowed — matched the "no error messages" report
- [x] TDR mitigation: maxTraceSteps 512, sky-skip early-out
- [x] Debug messenger (stderr) + opt-in validation layer via VV_VALIDATION=1
- [x] Palette moved out of the shader into a data buffer (binding 4); note
      added that a bindless texture array is planned

**Pass 3 — planned, START ONLY AFTER USER VERIFIES PASS 2:**
- Optimizations: SIMD noise, bitwise ops, faster traversal for large worlds
- Infinite render distance via level of detail
- (Later) native in-engine UI instead of Qt widgets for the escape menu

## Sandbox facts (Arena.ai environment)

- **`/tmp` is ephemeral between prompts.** Anything placed there (including the
  whole Linux toolchain) can vanish between messages. Only the workspace
  (`/home/user/Vulkanic-Voxels`) persists, via snapshots — and snapshot-capped
  directories (`build/`, `.cache/`, `node_modules/`, …) do not persist either.
  Don't rely on a previous prompt's build tree existing; re-run the toolchain
  script and rebuild when in doubt.
- **Network:** only `github.com` / `codeload.github.com` / `api.github.com` /
  `pypi.org` are reachable. `apt` and Debian mirrors are blocked,
  freedesktop.org is unreachable (no real xcb/wayland headers downloadable —
  the toolchain script installs ABI-identical compile-validation shims
  instead, clearly marked inside the files).
- **No GPU / no display server.** Never expect the game to actually render in
  the sandbox; validate that it *builds, links, and fails gracefully* at run
  time.

### Verifying a Linux build in the sandbox

```sh
# One-time (or after /tmp was wiped), ~20-30 min on 2 cores:
scripts/build-linux-toolchain.sh /tmp/deps

export PATH="/tmp/deps/venv/bin:/tmp/deps/prefix/bin:$PATH"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="/tmp/deps/qt6;/tmp/deps/prefix"
cmake --build build            # must be warning-free
ctest --test-dir build         # terrain/world logic tests (no GPU needed)
cmake --build build --target package_folder   # packaging check

# Headless smoke test: no GPU -> the game must show the graceful
# Vulkan error dialog (modal, so it runs until timeout) instead of crashing.
QT_QPA_PLATFORM=offscreen \
LD_LIBRARY_PATH="/tmp/deps/qt6/lib:/tmp/deps/prefix/lib" \
    timeout 5 ./build/bin/game
```

The script builds: pip CMake 3.31 + Ninja, zlib, minimal Qt 6.8.3
(offscreen/minimal QPA only — no xcb/OpenGL in Qt itself), Vulkan headers +
loader (WSI off; WSI functions resolved at run time), glm, glslang, and the
xcb/wayland compile shims. It is idempotent — re-running skips finished parts.

### Session conventions

- Work happens on the session branch (`arena/…`, shown in the environment);
  never touch other branches. Commit per pass; push with
  `git push origin <session-branch>`.
- `git push` / `gh` can fail with an authentication error when the session
  token expires (observed after ~1h). Retry on a later turn; if it persists,
  ask the user to reconnect GitHub in Arena settings.
- **Never issue parallel `edit_file` calls against the same file** — they
  race and silently drop each other's changes (observed in pass 2). Parallel
  edits to *different* files are fine.
- `/tmp` can be wiped **mid-turn**, not just between prompts (observed in
  pass 2): don't cache toolchain assumptions even within one turn.
- Code style: `.clang-format` is Google-based, **tabs**, 2-width. New UI/core
  files follow it; the older `vulkan/` and `render/` files use 2-space indent
  (don't mass-reformat, match the file you edit).

### Known caveats

- The sandbox Qt is built without xcb, so the resolver's public X11 fallback
  (`QNativeInterface::QX11Application`) cannot be compile-tested there beyond a
  manual `-DQT_FEATURE_xcb=1` object-file check; real X11 runs happen on user
  machines.
- Wayland rendering into a QWidget is best-effort (needs Qt private headers);
  X11 is the battle-tested path.
- macOS and Windows runtime behavior is verified by the user, not the sandbox.
