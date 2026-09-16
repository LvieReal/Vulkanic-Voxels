# Upgrade proposals: ambient light, and the SDF bake on the GPU

Research notes for the owner, written after pass 59. **No code in this commit** -
this file is the menu for the next passes, with the measurements and the known
costs attached so a decision does not need a second round of digging. Line
numbers are `resources/shaders/voxels.comp` (the renamed `pixels_rgba.comp`) and
the C++ tree as of `7063297`.

---

## 1. Ambient light: the caves are the problem

### 1.1 What the shader does today

```glsl
// ~:1183-1191
vec3 skyAmb  = skyBaseColor(sRdWorld);       // sky gradient along the VIEW ray
float hemi   = clamp(n.y * 0.5 + 0.5, 0.0, 1.0);
vec3 ambient = mix(skyAmb * 0.35, skyAmb, hemi) * (0.55 + 0.45 * shadow);
vec3 rim     = skyAmb * (0.12 * pow(1.0 - view, 5.0));
vec3 color   = base * (ambient + direct + rim) * ao;
```

- `skyBaseColor(rd)` (~:370) is the sunless sky: `skyLow` → `skyHigh` gradient
  plus horizon haze; the full `skyColor` (~:381, adds the sun disc/glow) is only
  for rays that miss the world.
- `shadow` is the sun visibility from the SDF or the exact march (one ray).
- `ao` (~:421 `calculateAO`) is the **per-vertex voxel AO** ported from the WGSL
  reference: it samples the 8 voxels around the hit face and darkens creases over
  roughly one voxel (`vertexAO`, ~:406). Far-LOD hits skip it.

### 1.2 Why it reads wrong inside a cave

1. **No large-scale occlusion.** The only occlusion term is that one-voxel
   `ao`; the sky ambient itself is unoccluded. A cave floor gets the same ambient
   as an open field, only tinted by `hemi` and cut 45% at most by `shadow`.
2. **The ambient's colour follows the camera ray**, not the surface. `skyAmb`
   is sampled along `sRdWorld`, so turning around changes a wall's ambient
   brightness/hue. For a diffuse ambient term the input should be the *normal's*
   hemisphere; keeping the view ray for `rim` is fine (grazing reflection is
   view-dependent), using it for `ambient` is the bug.
3. **The shadow coupling is the wrong shape for caves.** `(0.55 + 0.45*shadow)`
   removes at most 45% of the ambient, and a cave is `shadow = 0` everywhere, so
   caves keep 55% of a *sky* term - the "there is practically no ambient light
   here" that a cave should show is exactly what is missing. The coupling is also
   not a visibility estimate: the north face of a hill outdoors is `shadow = 0`
   too, and it *is* sky-lit.
4. **The rim term is unoccluded.** `rim` is fed by the same sky ambient and is
   strongest at grazing view angles, which is what a cave wall seen at an angle
   presents.

### 1.3 Proposal A - ambient from the hemisphere, with a ground bounce (no new rays)

Cheap and independent of everything below: sample the sky at the *normal* and
blend in a warm bounce from below.

```glsl
vec3 skyIrradiance  = skyBaseColor(n);                  // dome above the surface
vec3 groundBounce   = scene.skyLow.xyz * kGroundTint;    // warm/dim, from below
vec3 ambient = mix(groundBounce, skyIrradiance, hemi) * kAmbientScale;
```

- `hemi = n.y*0.5+0.5` already exists and is the right weight here.
- Cost: zero taps (one more `skyBaseColor` call, or reuse with the normal
  instead of the view ray). Look change is small but real: undersides and
  overhangs lose the blue and pick up the bounce, and the ambient stops moving
  when the camera turns.
- This alone does **not** fix caves (a cave floor still faces up) - it is the
  prerequisite, because with the view-dependence gone the visibility term below
  is the only thing left modulating the ambient.
- Divergence from the WGSL reference: the reference uses the view-ray form, so
  this needs the owner's eye (and ideally a lever, `--ambient-normal`).

### 1.4 Proposal B - sky visibility, the actual cave fix

Multiply the sky part of the ambient by a per-pixel **sky visibility** `skyVis`
(the fraction of the hemisphere that actually sees sky), and keep a small floor
so caves stay readable instead of black:

```glsl
vec3 ambient = mix(groundBounce, skyIrradiance, hemi) * skyVis * kAmbientScale
             + skyIrradiance * kAmbientFloor;      // kAmbientFloor ~ 0.10-0.18
```

**B1 - horizon scan against the height atlases (cheap, 2.5D).** The near columns
already have a per-column u16 height (`columnHeightAt`, ~:268, binding 5) and the
far region has the same at a coarse resolution (binding 6). For each of K
horizontal directions (recommend 6: 0°, 60°, … 300°), sample the height at
increasing distances (e.g. 1, 2, 4, 8, 16, 32, 64 voxels) and take the maximum
elevation angle `theta_i = atan2(h - p.y, d)`; then

```glsl
skyVis = clamp(mean_i (1 - sin(clamp(theta_i, 0, kMaxTheta))) ^ kVisGamma, 0, 1);
```

Cost: ~K*7 = 40 scalar fetches from buffers the shadow march already reads, plus
a handful of ALU - the same order as one extra shadow ray (the SDF march is
~19-22 samples/ray with `t += max(h*0.7, 0.05)`). Caves are handled correctly
because the roof *is* the column height above the point, and the floor/valley/
mountain cases come out as expected. It cannot see overhangs/arches (a 3D caster
that the 2.5D column misses) - for a voxel world with arcs and tunnels that is a
real gap, but a small one at the distances that matter for ambient.

**B2 - a few SDF rays (3D-correct).** 4-8 short rays through the existing SDF
(binding 12, same `sampleSdf3d` the shadow march uses), bounded to ~32 voxels:
`skyVis = mean over rays of (ray escaped the bound ? 1 : t/32)`. Handles
overhangs and tunnels. Cost: 4-8 x a shadow ray's *bounded* march (shorter range,
so fewer samples: ~8-12 per ray) - the most expensive option here, and the one to
reach for only if B1's misses are visible.

Practical recommendation: **B1 + 2 SDF rays** (one up-ish along the normal, one
along the dominant horizontal silhouette) or B1 alone first, with the lever
`--ambient`/`--no-ambient` and `--ambient-floor` for the look. Everything is
per-pixel and A/B-able without a rebuild, like `--shadow-jitter` (switches are
command-line flags since pass 61).

### 1.5 Proposal C - bake the visibility channel (if B costs too much on screen)

The SDF bake already walks a 192x110x192 box (4,055,040 cells) on a background
thread and produces one 4-byte seed per cell. A **downsampled** visibility
channel (e.g. one sample per 4x4x4 voxels = ~63k samples, `0..255`) computed once
per bake from the *finished* SDF (8-16 short rays per sample) would cost an
estimated 10-25 ms of worker time and then the shading side costs one trilinear
gather (the shader already gathers 8 neighbouring cells for the SDF itself, so
the machinery and its cost profile are known). Zero per-frame rays, smooth by
construction, and it is exactly the same "bake a volume, sample it in the march"
shape as the field itself. Same caveat as every baked channel: it must re-bake
with the field (the field follows the camera within a chunk of drift, so the
channel would be as fresh as the shadows are).

### 1.6 Proposal D - block light (the real cave answer, a feature, not a pass)

A torch/emissive-block light volume (one `u8` per voxel or per 2x2x2), seeded by
emissive voxels and flooded by max-propagation or jump-flooding, added as
`blockLight * blockColor` to the ambient, plus a night/dusk scale on the sky
term. This is what makes caves *interesting* rather than merely dark, and the
flood is a compute pass over the same chunk data as the SDF bake - it belongs
with the GPU work in section 2, not with the ambient pass. Worth doing after the
visibility term exists, because that term is what stops the sky from leaking
into a torch-lit tunnel.

### 1.7 What not to do

- **Do not** simply scale the global ambient down until caves look dark: that
  darkens the whole outdoors and leaves no local contrast.
- **Do not** reuse `shadow` (sun visibility) as the cave test: north faces
  outdoors are sun-shadowed and sky-lit, and a cave's *entrance* is sun-shadowed
  too.
- **Do not** replace the per-vertex `ao`: it works at a different scale (one
  voxel, creases) and is the part that reads well already. The upgrade is a
  *second*, larger-scale term multiplied in with it.

### 1.8 How to judge it

Scenes worth a look on-device, in this order: a cave interior (roof within a few
voxels, and a bigger cavern), a tunnel mouth (the transition), a valley between
hills, an overhang/arch, and open ground at noon. Then turn the camera 360°
without moving: the ambient on a *given* surface must not change (that is
proposal A's acceptance test). Perf: the same Nsight capture as pass 50 - if the
ambient addition shows up, `--no-ambient` isolates it, and B1's 40 fetches should
be noise next to the existing march.

---

## 2. Can the 3D voxel SDF be built on the GPU?

### 2.1 What the bake is today

- **CPU, background thread.** `vv::voxel::SdfField::build` (`src/voxel/SdfField.hpp`)
  initialises the solid cells as their own seeds and runs a **two-pass chamfer
  relaxation** (W1 = 1 face, sqrt(2) edge, sqrt(3) corner), merged to one
  7-candidate scan per cell in pass 53. Shipping box: 192x110x192 = 4,055,040
  cells, worker time **57 ms** (down from 109), measured on the owner's terrain.
- **Output**: one **4-byte argmin seed** per cell (the coordinate of the nearest
  solid cell, bit-packed, `kSdfEmptySeed` for "no solid in view"); the array the
  worker builds *is* the array that is uploaded (pass 49 fused the packing in).
- **Upload**: render-thread staging copy of **16.2 MB** per bake into a
  double-buffered device buffer (binding 12; box geometry binding 13). `--perf`
  already logs the split: `worker: band + build ms | render: snapshot + upload ms`.
- **Consumer**: the shader gathers the 8 neighbouring cells' seeds
  (`sampleSdf3d`, pass 52) and takes the min cube distance; the sphere trace
  folds `k*h/t` per step.

### 2.2 Verdict

**Yes - and the GPU is where it belongs**, but not for the reason one would
expect (speed). The source data (the voxel atlas, binding 0 + `ChunkTable`) is
already on the GPU, the output is already consumed there as a plain storage
buffer, and the field's whole purpose is to be sampled by a compute shader. There
are no images, samplers or layout transitions anywhere in the interface, so a
compute pass that writes binding 12 in the box's layout is the entire contract.

Two things to be honest about up front:

1. **It is not obviously faster.** A direct port of the *sweep* is not
   GPU-shaped (the relaxation order is sequential in z, y and x; slice-per-dispatch
   still forces serial y and x walks, and a slice granularity is ~200-400
   dispatch+barrier pairs). The standard GPU answer is **jump flooding**: seed
   the volume with the solid cells, then `2*log2(max dim)` ≈ 16 passes of
   27-neighbour lookups at a halved stride. For 4.05M cells that is ~1.7 G taps
   and on the order of 5-25 ms of GPU time on a mid-range card (this sandbox has
   no Vulkan device, so that is an estimate from the arithmetic, not a
   measurement - the first thing the experiment must do is measure it).
2. **JFA is not bit-identical to the chamfer.** It yields the true nearest seed
   (which is arguably *better* than the W1/W2/W3 weighted walk) but the field
   differs slightly, so the picture differs slightly: it needs an A/B and a
   `--sdf-gpu` fallback with the CPU bake kept as the reference - exactly the
   pattern the shadow work already uses.

### 2.3 What it buys

1. **No snapshot, no 16 MB upload, no worker thread.** The bake reads the live
   chunk buffers instead of a copy taken on the render thread, and nothing is
   copied host→device per recenter. The render-thread part of a bake disappears.
2. **Freshness.** The field currently follows the camera with a 1-chunk margin
   and a 57 ms background job; on the GPU it can be rebuilt per recenter (or
   amortised one JFA pass per frame over 16 frames) with no thread to join.
3. **A bigger or finer box becomes affordable.** The reason the box is 6x6
   chunks at 1 voxel per cell is the bake budget. On the GPU the same cost buys a
   taller box (currently band-cropped to 110 cells), a wider radius, or a finer
   cell size - i.e. better shadows at range and less banding at the box edge.
4. **It is the same infrastructure a light/AO volume needs** (section 1.5/1.6):
   all three are volume computes over the same chunk data. Building the GPU bake
   is building the road the ambient work wants anyway.

### 2.4 What it costs and risks

1. **A new pipeline + descriptor set + barriers.** The JFA passes need memory
   barriers between them and a barrier (or a separate submission) before the
   shadow pass reads the result. The owner's debug runs keep the validation layer
   on (pass 47/48), so this must be validation-clean - and pass 51 is the
   cautionary tale: a plumbing mistake here does not crash, it silently removes
   the shadows (or leaves the old field in place).
2. **Frame-time competition.** The CPU bake hides on a worker thread (owner
   verified "no hitch"). A 5-25 ms GPU bake is 5-25 ms the *frame* cannot use
   unless it is spread over frames, which adds latency between "the chunks
   arrived" and "the field describes them". Either way it is a visible-budget
   decision, not a free win.
3. **Testing.** There is no GPU in the CI path: `SdfField` must stay the CPU
   reference, and the GPU result should be verified by a readback diff (copy the
   seed buffer back, compare against the CPU bake on the same box) - the same
   shape as the existing CPU/shader mirror tests and pass 53's bit-identical
   check.
4. **Determinism.** JFA is deterministic for a fixed dispatch layout, but it is
   a *different* field than the chamfer's, so any picture comparison has to be
   per-config, and the pass-53 contract ("bit-identical to the CPU sweeps") does
   not survive by construction.
5. **The exact path is unaffected** either way: `--shadow-sharp` (or no `--sdf-shadows`) never touches
   this code.

### 2.5 Where it belongs in the tree

- A second compute shader next to `resources/shaders/voxels.comp` (e.g.
  `sdf_bake.comp`): the shader pipeline globs `*.comp`, compiles each into the
  runtime `resources/shaders` dir and the packaging target installs the whole
  list, so a new file is picked up by the existing machinery.
- A pipeline + descriptor set beside the existing compute pipeline in
  `src/vulkan/VulkanRenderer.*` (or a small `SdfBakePipeline` helper), writing
  the same seed buffer bindings the march already reads, with the existing
  double-buffered handover (`SdfHandover`) deciding when a bake goes live.
- The dispatch sizing, the 16 passes and the neighbour mask belong in the shader
  and in a small CPU mirror struct next to `SdfField` so the two can be compared
  from the tests.

### 2.6 Recommended order

1. **Measure first, in this order**: `--perf` already prints
   `worker: band + build | render: snapshot + upload`, so the first question is
   how much of the current cost is the *upload*, not the bake. If 16.2 MB of
   staging per recenter is a real share, the cheap win is to stop re-uploading
   the whole box: a recenter moves the box by whole chunks, so the unchanged
   region could be copied device-to-device (`vkCmdCopyBuffer` of the two halves
   of the band) and only the newly exposed slab uploaded. That is a small,
   safe change with no new shader, and it answers "how often does the field
   really need to be recomputed" before committing to a GPU builder.
2. If the goal is a bigger/fresher box rather than the upload, prototype the
   **JFA bake behind `--sdf-gpu`** with the CPU path as the reference, a
   readback diff, and a frame-amortised schedule (one pass per frame). Compare
   worker ms, upload ms, frame ms and the picture against the current field on
   the owner's terrain.
3. Then (and only then) move the ambient visibility channel (1.5) and later the
   block-light volume (1.6) onto the same road, since they are the same kind of
   volume compute over the same data.

### 2.7 Bottom line

The GPU bake is a good architectural fit and the only path to a bigger, fresher
field - but it is a *feature* purchase, not a speed fix: the CPU bake is already
off the render thread, and the likeliest immediate win is the upload path, not
the sweeps. The ambient visibility term (section 1) is the change with the
biggest visible payoff per line, and it does not need the GPU at all.
