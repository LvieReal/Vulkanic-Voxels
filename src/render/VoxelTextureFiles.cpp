#include "render/VoxelTextureFiles.hpp"

#include <QImage>
#include <QString>

#include <cstring>
#include <map>

#include "voxel/VoxelTypes.hpp"

namespace vv::render {
namespace {

bool fileExists(const std::filesystem::path& p) {
	std::error_code ec;
	return std::filesystem::exists(p, ec) && !ec;
}

// Loads one PNG/JPG as RGBA8. Returns false for missing/unreadable
// files (the caller treats that as "file absent" and falls back).
bool loadImageRGBA(const std::filesystem::path& path,
									 vv::voxel::VoxelTextureImage& out) {
	if (!fileExists(path)) {
		return false;
	}
	QImage img(QString::fromStdString(path.string()));
	if (img.isNull() || img.width() <= 0 || img.height() <= 0) {
		return false;
	}
	img = img.convertToFormat(QImage::Format_RGBA8888);
	if (img.width() <= 0 || img.height() <= 0) {
		return false;
	}
	out.width = static_cast<std::uint32_t>(img.width());
	out.height = static_cast<std::uint32_t>(img.height());
	out.rgba.assign(static_cast<std::size_t>(out.width) * out.height * 4u,
									255);
	// Copy row by row: QImage scanlines may be padded.
	for (std::uint32_t y = 0; y < out.height; ++y) {
		std::memcpy(out.rgba.data() + static_cast<std::size_t>(y) * out.width *
																			4u,
								img.constScanLine(static_cast<int>(y)),
								static_cast<std::size_t>(out.width) * 4u);
	}
	return true;
}

}  // namespace

bool loadVoxelTextureFiles(
		const std::filesystem::path& dir,
		std::vector<vv::voxel::VoxelTextureImage>& outImages,
		std::vector<vv::voxel::VoxelTextureSet>& outSets, std::string& outLog) {
	outImages.clear();
	outSets.assign(vv::voxel::kVoxelTypeCount, vv::voxel::VoxelTextureSet{});
	outLog.clear();

	if (!fileExists(dir)) {
		outLog = "voxel textures: directory '" + dir.string() +
						 "' not found - using plain colors";
		return true;
	}

	// Deduplicate repeated file paths across types (e.g. two types
	// sharing art) so each file is uploaded once.
	std::map<std::filesystem::path, std::uint32_t> imageIndexOf;

	// Most specific mode first: custom (6 files), side-uniform (3),
	// uniform (1). A mode counts only when ALL its files exist.
	const vv::voxel::VoxelTextureMode modesByDetail[] = {
			vv::voxel::VoxelTextureMode::Custom,
			vv::voxel::VoxelTextureMode::SideUniform,
			vv::voxel::VoxelTextureMode::Uniform};

	for (std::uint32_t type = 1; type < vv::voxel::kVoxelTypeCount; ++type) {
		const std::string name = vv::voxel::kVoxelTypeNames[type];
		bool done = false;
		for (vv::voxel::VoxelTextureMode mode : modesByDetail) {
			const std::uint32_t files = vv::voxel::voxelTextureFileCount(mode);

			// Resolve every file of the mode first; bail out on any miss.
			std::vector<vv::voxel::VoxelTextureImage> loaded(files);
			std::vector<std::filesystem::path> paths(files);
			bool complete = true;
			for (std::uint32_t f = 0; f < files; ++f) {
				paths[f] = dir / (name + vv::voxel::voxelTextureSuffix(mode, f) +
													".png");
				if (!loadImageRGBA(paths[f], loaded[f])) {
					complete = false;
					break;
				}
			}
			if (!complete) {
				continue;
			}

			vv::voxel::VoxelTextureSet& set = outSets[type];
			set.textured = true;
			for (std::uint32_t face = 0; face < 6; ++face) {
				const std::uint32_t f = vv::voxel::faceTextureFile(mode, face);
				auto it = imageIndexOf.find(paths[f]);
				if (it == imageIndexOf.end()) {
					it = imageIndexOf
									 .emplace(paths[f],
														static_cast<std::uint32_t>(outImages.size()))
									 .first;
					outImages.push_back(std::move(loaded[f]));
				}
				set.faceIndex[face] = it->second;
				const std::uint32_t w = outImages[it->second].width;
				const std::uint32_t h = outImages[it->second].height;
				set.nominalSize =
						std::max(set.nominalSize, std::max(w, h));
			}
			const char* modeName = mode == vv::voxel::VoxelTextureMode::Custom
																 ? "custom"
																 : (mode == vv::voxel::VoxelTextureMode::SideUniform
																				? "side-uniform"
																				: "uniform");
			outLog += "voxel textures: " + name + " -> " + modeName + " (" +
								std::to_string(files) + " files)\n";
			done = true;
			break;
		}
		if (!done) {
			outLog += "voxel textures: " + name +
								" -> plain colors (no files)\n";
		}
	}
	return true;
}

}  // namespace vv::render
