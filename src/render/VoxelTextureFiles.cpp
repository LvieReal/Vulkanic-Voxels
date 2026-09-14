#include "render/VoxelTextureFiles.hpp"

#include <QImage>
#include <QString>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

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

// Face ids match voxel/VoxelTextures.hpp: 0 = +Y top, 1 = -Y bottom,
// 2 = +X back, 3 = -X front, 4 = +Z right, 5 = -Z left.
bool faceIdFromName(const std::string& name, std::uint32_t& faceId) {
	if (name == "top") {
		faceId = 0;
	} else if (name == "bottom") {
		faceId = 1;
	} else if (name == "back") {
		faceId = 2;
	} else if (name == "front") {
		faceId = 3;
	} else if (name == "right") {
		faceId = 4;
	} else if (name == "left") {
		faceId = 5;
	} else {
		return false;
	}
	return true;
}

const char* faceNameOfId(std::uint32_t faceId) {
	static const char* kNames[6] = {"top", "bottom", "back",
																	"front", "right", "left"};
	return faceId < 6 ? kNames[faceId] : "top";
}

int voxelTypeFromName(const std::string& name) {
	for (std::uint32_t t = 0; t < vv::voxel::kVoxelTypeCount; ++t) {
		if (name == vv::voxel::kVoxelTypeNames[t]) {
			return static_cast<int>(t);
		}
	}
	return -1;
}

// Resolves a face NAME through a source type's mode to one of the
// source set's image indices (see the header comment).
std::uint32_t resolveSourceFaceIndex(const vv::voxel::VoxelTextureSet& set,
																		 vv::voxel::VoxelTextureMode mode,
																		 const std::string& name) {
	if (!set.textured) {
		return vv::voxel::kNoFaceTexture;
	}
	if (mode == vv::voxel::VoxelTextureMode::Uniform) {
		return set.faceIndex[0];
	}
	if (mode == vv::voxel::VoxelTextureMode::SideUniform) {
		if (name == "top") {
			return set.faceIndex[0];
		}
		if (name == "bottom") {
			return set.faceIndex[1];
		}
		return set.faceIndex[2];  // every side-ish name
	}
	std::uint32_t faceId = 0;
	if (!faceIdFromName(name, faceId)) {
		return vv::voxel::kNoFaceTexture;
	}
	return set.faceIndex[faceId];
}

std::string trim(const std::string& s) {
	const auto begin = s.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos) {
		return "";
	}
	const auto end = s.find_last_not_of(" \t\r\n");
	return s.substr(begin, end - begin + 1);
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

	// Deduplicate repeated file paths across types (aliases and types
	// sharing art upload each file once).
	std::map<std::filesystem::path, std::uint32_t> imageIndexOf;
	std::vector<vv::voxel::VoxelTextureMode> modes(
			vv::voxel::kVoxelTypeCount, vv::voxel::VoxelTextureMode::Uniform);

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
			}
			modes[type] = mode;
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

	// aliases.txt: reuse one type's textures for another type's faces,
	// e.g. "grass bottom = dirt". Applied after file detection (all
	// sources known) and overrides file-based face assignments.
	{
		std::ifstream in(dir / "aliases.txt");
		if (in.good()) {
			std::string line;
			while (std::getline(in, line)) {
				const auto hash = line.find('#');
				if (hash != std::string::npos) {
					line = line.substr(0, hash);
				}
				line = trim(line);
				if (line.empty()) {
					continue;
				}
				const auto eq = line.find('=');
				if (eq == std::string::npos) {
					outLog += "voxel textures: alias skipped (no '='): " + line +
										"\n";
					continue;
				}
				std::istringstream left(trim(line.substr(0, eq)));
				std::istringstream right(trim(line.substr(eq + 1)));
				std::string typeToken, faceToken, sourceToken, sourceFace;
				left >> typeToken >> faceToken;
				right >> sourceToken >> sourceFace;
				if (typeToken.empty() || faceToken.empty() || sourceToken.empty()) {
					outLog += "voxel textures: alias skipped (malformed): " +
										line + "\n";
					continue;
				}
				const int typeIdx = voxelTypeFromName(typeToken);
				const int sourceIdx = voxelTypeFromName(sourceToken);
				if (typeIdx <= 0 || sourceIdx <= 0) {
					outLog += "voxel textures: alias skipped (unknown type): " +
										line + "\n";
					continue;
				}
				std::uint32_t faceMask = 0;
				if (faceToken == "sides" || faceToken == "side") {
					faceMask = 0x3Cu;  // back, front, right, left
				} else if (faceToken == "all") {
					faceMask = 0x3Fu;
				} else {
					std::uint32_t faceId = 0;
					if (!faceIdFromName(faceToken, faceId)) {
						outLog += "voxel textures: alias skipped (unknown face): " +
											line + "\n";
						continue;
					}
					faceMask = 1u << faceId;
				}
				const vv::voxel::VoxelTextureSet& src =
						outSets[static_cast<std::size_t>(sourceIdx)];
				if (!src.textured) {
					outLog += "voxel textures: alias skipped (source has no "
										"textures): " + line + "\n";
					continue;
				}
				bool changed = false;
				for (std::uint32_t faceId = 0; faceId < 6; ++faceId) {
					if ((faceMask & (1u << faceId)) == 0) {
						continue;
					}
					const std::string faceName =
							sourceFace.empty() ? faceNameOfId(faceId) : sourceFace;
					const std::uint32_t idx = resolveSourceFaceIndex(
							src, modes[static_cast<std::size_t>(sourceIdx)], faceName);
					if (idx == vv::voxel::kNoFaceTexture) {
						outLog += "voxel textures: alias face '" + faceName +
											"' not present on source: " + line + "\n";
						continue;
					}
					outSets[static_cast<std::size_t>(typeIdx)].faceIndex[faceId] =
							idx;
					changed = true;
				}
				if (changed) {
					outSets[static_cast<std::size_t>(typeIdx)].textured = true;
					outLog += "voxel textures: alias " + typeToken + " " +
										faceToken + " -> " + sourceToken +
										(sourceFace.empty() ? "" : " " + sourceFace) + "\n";
				}
			}
		}
	}

	// Nominal sizes from the FINAL face assignments (aliases included).
	for (std::size_t t = 0; t < outSets.size(); ++t) {
		vv::voxel::VoxelTextureSet& set = outSets[t];
		std::uint32_t nominal = 32;
		for (std::uint32_t f = 0; f < 6; ++f) {
			if (set.faceIndex[f] != vv::voxel::kNoFaceTexture) {
				const vv::voxel::VoxelTextureImage& img =
						outImages[set.faceIndex[f]];
				nominal = std::max(nominal, std::max(img.width, img.height));
			}
		}
		set.nominalSize = nominal;
	}
	return true;
}

}  // namespace vv::render
