// The binding-13 "SdfBox" uniform, word for word (pass 51).
//
// The shader (resources/shaders/pixels_rgba.comp) declares
//
//     layout(std140, set = 0, binding = 13) uniform SdfBox {
//         ivec4 box;       // 0.. 3: xyz = box origin in world voxels,
//                          //         w = 1 when a field is live, -1 when not
//         uvec4 dims;      // 4.. 7: xyz = box size in cells,
//                          //         w = the live half's base CELL OFFSET
//         uvec4 seedBits;  // 8..11: xyz = bits per axis of the packed seed cell
//     } sdfBox;
//
// and the CPU side fills the same 12 words through this header. That pairing
// is exactly where the GPU path can silently go wrong, so the layout lives
// here as a pure function: the test suite pins every word without a device.
//
// Pass 51 is why: pass 50 added the seedBits words to the uniform, to the
// descriptor range and to VoxelResources::writeSdfBox's signature - but the
// writer never STORED them. The block is zeroed when it is created, so the
// shader read seedBits = (0,0,0), computed both decode masks as
// (1u << 0) - 1 = 0, and turned every 3x3x3 gather into the same degenerate
// voxel: the SDF stopped occluding anything and the 3D shadows disappeared
// from a perfectly healthy bake (the owner's report + logs).
//
// The writer now stores all kWords through storeSdfBoxUniform(): a word can
// only be forgotten if it is also missing from makeSdfBoxUniform(), which the
// tests read word by word.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace vv::voxel {

struct SdfBoxUniform {
	// std140: each vec4-sized member is rounded up to a 16-byte stride, so
	// box + dims + seedBits = 3 * 16 bytes with no padding between them.
	static constexpr std::size_t kWords = 12u;
	static constexpr std::uint32_t kBytes =
			static_cast<std::uint32_t>(kWords * 4u);
	// box.w: the word the shader tests before it touches anything else.
	static constexpr std::size_t kActiveWord = 3u;
	static constexpr std::uint32_t kActive = 1u;
	static constexpr std::uint32_t kInactive = 0xFFFFFFFFu;  // -1 as a float bit pattern

	std::array<std::uint32_t, kWords> words{};
};

// Fills the block for one box. `baseCell` is the live half's base cell offset
// in the seed buffer (dims.w; the caller multiplies the half index by the
// BUFFER's cell count, see the pass-49 note in VoxelResources::writeSdfBox).
// An INACTIVE box is the zero block with box.w = -1 (the shader's first test,
// which then reads no other word): a frame that samples the block while no
// field is live can never resolve a cell.
[[nodiscard]] inline SdfBoxUniform makeSdfBoxUniform(
		std::int32_t boxX, std::int32_t boxY, std::int32_t boxZ,
		std::uint32_t nx, std::uint32_t ny, std::uint32_t nz,
		std::uint32_t seedBitsX, std::uint32_t seedBitsY,
		std::uint32_t seedBitsZ, bool active, std::uint32_t baseCell) {
	SdfBoxUniform uniform;
	uniform.words[0] = active ? static_cast<std::uint32_t>(boxX) : 0u;
	uniform.words[1] = active ? static_cast<std::uint32_t>(boxY) : 0u;
	uniform.words[2] = active ? static_cast<std::uint32_t>(boxZ) : 0u;
	uniform.words[3] =
			active ? SdfBoxUniform::kActive : SdfBoxUniform::kInactive;
	uniform.words[4] = active ? nx : 0u;
	uniform.words[5] = active ? ny : 0u;
	uniform.words[6] = active ? nz : 0u;
	uniform.words[7] = active ? baseCell : 0u;
	uniform.words[8] = active ? seedBitsX : 0u;
	uniform.words[9] = active ? seedBitsY : 0u;
	uniform.words[10] = active ? seedBitsZ : 0u;
	// words[11] is the std140 tail of the uvec4 (the shader never reads it):
	// left 0 - the tests pin that too, so the whole 48-byte block is a
	// deterministic function of the arguments.
	return uniform;
}

// Stores the block into mapped memory in the order the renderer has relied on
// since pass 42: every payload word first, box.w LAST, with a release fence in
// between, so a dispatch that samples the block mid-write cannot see "active"
// next to the previous build's geometry. Free of Vulkan so a test can check
// that all 12 words really land.
inline void storeSdfBoxUniform(const SdfBoxUniform& uniform,
															 std::uint32_t* dst) {
	for (std::size_t i = 0; i < SdfBoxUniform::kWords; ++i) {
		if (i != SdfBoxUniform::kActiveWord) {
			dst[i] = uniform.words[i];
		}
	}
	std::atomic_thread_fence(std::memory_order_release);
	dst[SdfBoxUniform::kActiveWord] = uniform.words[SdfBoxUniform::kActiveWord];
}

}  // namespace vv::voxel
