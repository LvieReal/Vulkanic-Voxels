# Voxel textures

Per-type albedo textures, loaded at startup by the game (NOT committed
to the repo - drop your files here locally; a checkout without files
falls back to the plain palette colors).

One file set per voxel type, named after the type (lowercase):
`air` (never sampled), `grass`, `dirt`, `stone`, `sand`, `snow`,
`bedrock`.

Face resolution is PER FACE (pass 28): each face independently uses
the most specific file that exists, falling back down this chain:

| Face           | File candidates, in order                                  |
|----------------|------------------------------------------------------------|
| top            | `<name>_top.png` → `<name>.png`                             |
| bottom         | `<name>_bottom.png` → `<name>.png`                          |
| each side face | `<name>_front/_back/_right/_left.png` → `<name>_side.png` → `<name>.png` |

So complete sets behave like the three classic modes (uniform = one
file everywhere; side-uniform = top/bottom/side; custom = six files),
but PARTIAL sets now work too: e.g. `grass_top.png` + `grass_side.png`
gives grass a textured top and sides while the bottom falls back to an
alias or the plain color.

Custom face names map to world axes: **front = −X**, **back = +X**,
**right = +Z**, **left = −Z** (top = +Y, bottom = −Y).

Notes:

- A face with no file (and no alias) uses the plain palette color; a
  type with no files at all is entirely plain. The startup log lists
  per type how many faces got files and which are still missing.
- Reuse another type's textures instead of duplicating files with an
  optional `aliases.txt` in this directory:

  ```
  # <type> <face> = <source type> [<source face>]
  grass bottom = dirt          # grass's bottom face uses dirt's texture
  grass sides = dirt side      # all 4 grass sides use dirt's side art
  ```

  Faces: `top`, `bottom`, `front`, `back`, `right`, `left`, `sides`
  (all 4 sides), `all`. The source face defaults to the target face's
  name and resolves against the source's per-face assignments (a
  uniform source serves any face; `side`/`sides` uses the source's side
  texture). Aliases override file-based assignments for those faces
  and can fill faces the files left plain.
- Any reasonable PNG/JPG size works (square or not; mip chain is
  generated). Textures should be tileable - they repeat per voxel
  (1 texture unit = 1 voxel).
- Sampling is NEAREST (crisp texels) with linear mip blends; the LOD is
  computed from the pixel footprint, so distant terrain fades to the
  texture's average color (which keeps the far LOD consistent - pick
  textures whose average roughly matches the palette colors in
  `src/voxel/VoxelTypes.hpp` for a seamless near/far look).
- UVs are world-space on each face: +U follows +X (or +Z on X faces);
  +V follows +Z on top/bottom faces and −Y on side faces (image top row
  at the voxel top - textures display UPRIGHT on sides). If a texture
  runs the wrong way horizontally for your art, mirror the file.
- Textures are the face ALBEDO (they replace the palette color for
  textured faces).
- Where the game looks for them: `resources/textures/voxels` next to
  the executable (copied at build time) and, as a fallback, the same
  path relative to the working directory (the run scripts launch from
  the repo root) - so freshly dropped-in files work without a rebuild
  when launching via run.bat/run.sh/VS Code.
