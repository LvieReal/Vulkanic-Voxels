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

// Face ids/names and alias-source resolution live in
// voxel/VoxelTextures.hpp now (unit tested there).
int voxelTypeFromName(const std::string& name) {
	for (std::uint32_t t = 0; t < vv::voxel::kVoxelTypeCount; ++t) {
		if (name == vv::voxel::kVoxelTypeNames[t]) {
			return static_cast<int>(t);
		}
	}
	return -1;
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

	// PER-FACE resolution (pass 28): every face independently uses the
	// first of its candidate files that exists (kVoxelFaceSuffixChain,
	// most specific first); a face with no file keeps kNoFaceTexture
	// (plain color) unless an alias fills it below. This replaces the
	// old rule that a mode only applied when ALL its files existed -
	// which made the README's own example (grass_top + grass_side +
	// "grass bottom = dirt") render grass as PLAIN COLORS: no complete
	// side-uniform set, so nothing applied and the alias textured only
	// the rarely-visible bottom face.
	for (std::uint32_t type = 1; type < vv::voxel::kVoxelTypeCount; ++type) {
		const std::string name = vv::voxel::kVoxelTypeNames[type];
		vv::voxel::VoxelTextureSet& set = outSets[type];
		std::uint32_t facesFromFile = 0;
		std::uint32_t fileCount = 0;
		for (std::uint32_t face = 0; face < 6; ++face) {
			for (const char* suffix :
			     vv::voxel::kVoxelFaceSuffixChain[face]) {
				if (suffix == nullptr) {
					break;
				}
				const std::filesystem::path path =
				    dir / (name + suffix + ".png");
				if (!fileExists(path)) {
					continue;
				}
				vv::voxel::VoxelTextureImage img;
				if (!loadImageRGBA(path, img)) {
					continue;  // unreadable: try the next candidate
				}
				std::uint32_t idx = 0;
				auto it = imageIndexOf.find(path);
				if (it == imageIndexOf.end()) {
					idx = static_cast<std::uint32_t>(outImages.size());
					imageIndexOf.emplace(path, idx);
					outImages.push_back(std::move(img));
					++fileCount;
				} else {
					idx = it->second;
				}
				set.faceIndex[face] = idx;
				++facesFromFile;
				break;
			}
		}
		set.textured = facesFromFile > 0;
		if (facesFromFile == 0) {
			outLog += "voxel textures: " + name +
			           " -> plain colors (no files)\n";
		} else if (facesFromFile == 6) {
			outLog += "voxel textures: " + name + " -> " +
			           std::to_string(fileCount) + " files, 6/6 faces\n";
		} else {
			// Partial: say which faces are still untextured so the log
			// points straight at the fix (add the file or an alias).
			outLog += "voxel textures: " + name + " -> " +
			           std::to_string(fileCount) + " files, " +
			           std::to_string(facesFromFile) + "/6 faces (missing:";
			for (std::uint32_t face = 0; face < 6; ++face) {
				if (set.faceIndex[face] == vv::voxel::kNoFaceTexture) {
					outLog += std::string(" ") +
					           vv::voxel::voxelFaceNameOfId(face);
				}
			}
			outLog += ")\n";
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
					if (!vv::voxel::voxelFaceIdFromName(faceToken, faceId)) {
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
							sourceFace.empty()
							    ? vv::voxel::voxelFaceNameOfId(faceId)
							    : sourceFace;
					const std::uint32_t idx = vv::voxel::resolveFaceTextureIndex(
							src, faceName);
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
