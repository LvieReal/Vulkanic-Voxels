# Voxel textures

Per-type albedo textures, loaded at startup by the game (NOT committed
to the repo - drop your files here locally; a checkout without files
falls back to the plain palette colors).

One file set per voxel type, named after the type (lowercase):
`air` (never sampled), `grass`, `dirt`, `stone`, `sand`, `snow`,
`bedrock`.

Three definition modes per type - the most specific COMPLETE set of
files wins (custom > side-uniform > uniform):

| Mode         | Files                                                    | Faces |
|--------------|----------------------------------------------------------|-------|
| uniform      | `<name>.png`                                             | all 6 faces share one texture |
| side-uniform | `<name>_top.png` `<name>_bottom.png` `<name>_side.png`   | top / bottom / all 4 sides |
| custom       | `<name>_top.png` `<name>_bottom.png` `<name>_px.png` `<name>_nx.png` `<name>_pz.png` `<name>_nz.png` | every face its own |

Notes:

- A mode only applies when ALL of its files exist; a type with no
  complete set uses plain colors.
- Any reasonable PNG/JPG size works (square or not; mip chain is
  generated). Textures should be tileable - they repeat per voxel
  (1 texture unit = 1 voxel).
- Sampling is NEAREST (crisp texels) with linear mip blends; the LOD is
  computed from the pixel footprint, so distant terrain fades to the
  texture's average color (which keeps the far LOD consistent - pick
  textures whose average roughly matches the palette colors in
  `src/voxel/VoxelTypes.hpp` for a seamless near/far look).
- UVs are world-space on each face: +U follows +X (or +Z on X faces),
  +V follows +Z (or +Y on X/Z faces). If a side texture appears flipped
  for your art, mirror the file.
- Textures are the face ALBEDO (they replace the palette color for
  textured faces).
- Where the game looks for them: `resources/textures/voxels` next to
  the executable (copied at build time) and, as a fallback, the same
  path relative to the working directory (the run scripts launch from
  the repo root) - so freshly dropped-in files work without a rebuild
  when launching via run.bat/run.sh/VS Code.
