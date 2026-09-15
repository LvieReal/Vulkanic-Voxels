#include "render/ImageDecode.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

// stb_image is compiled here and only here (see third_party/README.md). Only
// the PNG decoder is needed for the voxel textures (<type>[_face].png); stdio
// is disabled on purpose so the path never goes through a narrow-char file
// open that would break non-ASCII paths on Windows - the file is read with
// std::ifstream and decoded from memory.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

namespace vv::render {

bool loadImageFileRGBA(const std::string& path,
											 vv::voxel::VoxelTextureImage& out,
											 std::string* outError) {
	std::ifstream file(path, std::ios::binary);
	if (!file.good()) {
		if (outError != nullptr) {
			*outError = "cannot open '" + path + "'";
		}
		return false;
	}
	std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),
																	 std::istreambuf_iterator<char>());
	if (bytes.empty()) {
		if (outError != nullptr) {
			*outError = "'" + path + "' is empty";
		}
		return false;
	}

	int width = 0;
	int height = 0;
	int channels = 0;
	// stbi_load_from_memory always returns RGBA8 for req_comp = 4.
	unsigned char* pixels =
			stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
														&width, &height, &channels, 4);
	if (pixels == nullptr || width <= 0 || height <= 0) {
		if (outError != nullptr) {
			const char* reason = stbi_failure_reason();
			*outError = "cannot decode '" + path + "': " +
									(reason != nullptr ? reason : "unknown format");
		}
		return false;
	}

	const std::size_t pixelBytes =
			static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
	out.width = static_cast<std::uint32_t>(width);
	out.height = static_cast<std::uint32_t>(height);
	out.rgba.assign(pixelBytes, 255u);
	std::memcpy(out.rgba.data(), pixels, pixelBytes);

	stbi_image_free(pixels);
	(void)channels;
	return true;
}

}  // namespace vv::render
