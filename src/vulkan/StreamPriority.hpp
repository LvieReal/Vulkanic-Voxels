#pragma once

#include <cmath>
#include <cstdint>

namespace vv::vulkan {

// Streaming priority of one pending chunk (pass 27: extracted from
// VulkanRenderer.cpp so the CPU tests can pin the ordering semantics).
// HIGHER value = generate sooner: NEAR chunks first, and at equal
// distance what is in FRONT of the camera wins (a 96-world-unit forward
// bias, ~3 chunks). Deliberately glm-free so tests/ can include it.
//
// Queue discipline (the pass-27 fix, pinned by testStreamPriority):
// m_streamPending is sorted ASCENDING by this value (worst first); the
// pump stocks m_genRequests by iterating it in REVERSE (best first), so
// the FRONT of m_genRequests is always the best pending coord, and the
// workers consume from the FRONT. (The old back-pop with reverse
// stocking left the best coords stuck at the front forever while the
// workers consumed progressively worse top-ups - "frustum
// prioritization is backwards".)
inline float streamPriority(std::int32_t chunkX, std::int32_t chunkZ,
                            float camX, float camZ, float fwdX, float fwdZ,
                            float chunkWorldSize) {
	const float cx = (static_cast<float>(chunkX) + 0.5f) * chunkWorldSize;
	const float cz = (static_cast<float>(chunkZ) + 0.5f) * chunkWorldSize;
	const float dx = cx - camX;
	const float dz = cz - camZ;
	const float dist = std::max(std::sqrt(dx * dx + dz * dz), 1.0f);
	const float fl = std::sqrt(fwdX * fwdX + fwdZ * fwdZ);
	const float facing =
	    fl > 1e-6f ? (dx / dist * fwdX + dz / dist * fwdZ) / fl : 0.0f;
	return -(dist - facing * 96.0f);
}

}  // namespace vv::vulkan
