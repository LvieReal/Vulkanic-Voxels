# Third-party code

Vendored, single-header libraries. Nothing here is built as a separate target:
the headers are compiled into the game target directly, from one translation
unit each, so there is no dependency to install and no build-system footprint.

## stb_image.h

- Upstream: <https://github.com/nothings/stb> (`stb_image.h`, v2.30)
- Revision pulled from upstream: `013ac3beddff3dbffafd5177e7972067cd2b5083`
  (2024-05-31)
- License: public domain / MIT (dual - see the license block at the end of the
  header)

Used by `src/render/VoxelTextureFiles.cpp` to decode the optional voxel
textures (`<type>[_face].png`), which used to go through Qt's `QImage` (pass 43
removed Qt). The implementation is instantiated in that one translation unit
with `STBI_ONLY_PNG` and `STBI_NO_STDIO` - the file is read through
`std::ifstream` and handed to `stbi_load_from_memory`, so only the PNG decoder
is compiled in and no C-stdio path conversion is involved (a `std::filesystem`
path with non-ASCII characters keeps working).
