# Third-party code

Everything a clone needs to build besides a compiler, CMake, the Vulkan
headers/loader and `glslangValidator`. Nothing here is installed: the
single-header libraries are compiled into the game target directly (one
translation unit each) and GLFW is built as a subproject of the same build.

Keeping it in-tree is pass 58's answer to "why do I have to install every
dependency on my system" - configuring and building needs no `glfw`/`glm`
package and no network access at build time. `-DVV_USE_SYSTEM_DEPS=ON` prefers
a system copy when one exists (see `cmake/Dependencies.cmake`).

## GLFW 3.5.1 - windowing layer

- Upstream: <https://github.com/glfw/glfw> (tag `3.5.1`,
  commit `d9d6f0f1f967807ffade6598ea9a631ebaf37a5e`)
- Tarball vendored from: `https://github.com/glfw/glfw/archive/refs/tags/3.5.1.tar.gz`
  (sha256 `5234f4f29473e9a06bc7847d8371858dd135d38466eeeaa652fdc9f8f9ff0c20`)
- License: zlib/libpng (`LICENSE.md`, kept in place)
- Build: `add_subdirectory(third_party/glfw)` from `cmake/Dependencies.cmake`,
  static, no install step; the version is re-read from `glfw3.h` at configure
  time and checked against `VV_GLFW_EXPECTED_VERSION` in that file.

What is kept: `CMakeLists.txt`, `CMake/`, `include/`, `src/`, `deps/wayland/`
(the protocol XMLs the Wayland backend generates code from), `LICENSE.md` and
`README.md`. What is dropped: `examples/`, `tests/`, `docs/` - none of it is
referenced by the library's own build once `GLFW_BUILD_EXAMPLES`,
`GLFW_BUILD_TESTS` and `GLFW_BUILD_DOCS` are off. The remaining `deps/` files
(`glad`, `nuklear`, `tinycthread`, `getopt`, `linmath`, `stb_image_write`) are
used by those examples and tests only, and are gone with them.

The X11 and Wayland backends need their platform's development packages at
build time - GLFW compiles whichever are present and falls back to its null
backend when neither is (`cmake/Dependencies.cmake` prints a warning and
`-DVV_GLFW_NULL_ONLY=ON` asks for null on purpose). Those headers cannot be
vendored: they are the platform's own ABI.

## glm 1.0.1 - header-only math

- Upstream: <https://github.com/g-truc/glm> (tag `1.0.1`,
  commit `0af55ccecd98d4e5a8d1fad7de25ba429d60e863`)
- Tarball vendored from: `https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz`
  (sha256 `9f3174561fd26904b23f0db5e560971cbf9b3cbda0b280f04d5c379d03bf234c`)
- License: MIT / "Happy Bunny" (`copying.txt`, kept in place)
- Build: the directory is used as an include directory
  (`VV_GLM_INCLUDE_DIR`), nothing is compiled.

What is kept: `glm/` (the headers) and `copying.txt`. What is dropped:
`CMakeLists.txt`, `cmake/`, `doc/`, `test/`, `util/`, `manual.md`,
`readme.md` - 24 MB of documentation and test sources that no build step
touches.

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

## Refreshing a vendored dependency

The upstream tarball above is the source of truth; the copy here is trimmed by
hand, so re-vendoring is: unpack the new tag, copy the kept paths listed for
that dependency, delete the rest, bump `VV_GLFW_EXPECTED_VERSION` in
`cmake/Dependencies.cmake` (GLFW) in the same commit, then build and run
`ctest` on both configs before committing.

To check that the tree has not been touched since it was vendored:

```sh
for d in glfw glm; do
  (cd third_party/$d && find . -type f | LC_ALL=C sort | xargs sha256sum | sha256sum)
done
```

which must print, for this revision:

- `glfw` `0d4c195416c8b0e48f07455af4546373bfce6065199049dd1e67b0828de715fd`
- `glm` `9bc77b6e760c40cd18e9c95a8b7fda146eb9cdfbb1bc60f5d64a5d5347ff077d`

A diff under `third_party/` that is not part of a re-vendoring commit is a bug:
the point of vendoring is that these trees are exactly what upstream published.
