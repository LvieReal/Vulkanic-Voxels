// Which of the 3D voxel SDF steps the frame must take (pass 42,
// VV_SDF_SHADOWS=1).
//
// The box uniform (binding 13) and the seed buffer (binding 12) are a PAIR:
// the shader resolves "which cell is this point in" from the box and indexes
// the seeds with it, so a box published together with seeds from another
// build shades a whole frame out of a field that describes different terrain -
// the owner's "chunks go dark for one frame, just before the SDF shadows come
// back". The GPU cannot tear the pair by itself (the seeds are a device-local
// copy and the box is host memory), but the CPU can publish out of order, and
// that is what this header pins down.
//
// The order that is safe on a single queue, and the order that keeps the frame
// free of stalls:
//
//   1. the background build finishes -> join (m_sdfPendingReady is only set at
//      the very end, so the join is already done) and SUBMIT the copy. The
//      copy is queued behind every frame already submitted, so it can only
//      land after those frames stopped reading the old pairing. It is NOT
//      waited for here - waiting was the frame hitch the owner felt as
//      "big latency when the SDFs get recomputed".
//   2. the copy's fence has signalled -> publish the box. By (1) no dispatch
//      that resolved cells against the old box can still be executing.
//   3. nothing in flight and the field does not cover the camera's chunk ->
//      build again. A launch that a running build swallowed used to be lost
//      until the NEXT region move, which left the soft shadows up to a whole
//      crossing (a second of streaming, plus the build) behind the camera -
//      the chunks that just streamed in kept the hard 2.5D look for that
//      whole time, and then the SDF shadows "appeared". Retrying here makes
//      the field converge on the camera's chunk as soon as it can.
//
// Nothing here is GPU work or Vulkan state: it is a pure step decision so the
// ordering rules can be unit-tested (see testSdfHandoverPolicy).
#pragma once

#include <cstdint>

namespace vv::voxel {

// The seed buffer holds this many copies of the field. The live half is the
// one the published box points at (the shader's dims.w); the copy fills the
// other, which is what makes the handover safe without a frame wait.
inline constexpr std::uint32_t kSdfHalves = 2;

struct SdfHandover final {
	enum class Step {
		Idle,           // nothing to do this frame
		JoinAndUpload,  // a finished build: join, then SUBMIT the copy
		Publish,        // the copy landed: write the box uniform
		Relaunch,       // the field does not cover the camera: build again
	};

	bool buildRunning = false;    // a background build is on the worker
	bool buildReady = false;      // ...and it has handed its result over
	bool uploadInFlight = false;  // a copy is queued, box not published yet
	bool copyComplete = false;    // ...and its fence has signalled
	bool haveField = false;       // a box is live in the uniform
	bool wantValid = false;       // the camera's chunk is known (a region
	                              // move completed at least once)
	std::int32_t activeCenterX = 0;  // center the live field was built for
	std::int32_t activeCenterZ = 0;
	std::int32_t wantCenterX = 0;  // chunk the camera is on now
	std::int32_t wantCenterZ = 0;
	// The half the LIVE box points at. The copy must target the other one:
	// the live half has to keep holding exactly the seeds its box describes
	// for as long as any frame can still be reading them, and that includes
	// the frames recorded before this build finished.
	std::uint32_t liveHalf = 0;

	std::uint32_t uploadHalf() const {
		return (liveHalf + 1u) % kSdfHalves;
	}

	bool coversCamera() const {
		return haveField && activeCenterX == wantCenterX &&
					 activeCenterZ == wantCenterZ;
	}

	Step step() const {
		if (uploadInFlight) {
			// Never publish before the copy lands: the box would select
			// cells of a seed buffer that still holds the previous field.
			return copyComplete ? Step::Publish : Step::Idle;
		}
		if (buildRunning && buildReady) {
			return Step::JoinAndUpload;
		}
		if (wantValid && !buildRunning && !coversCamera()) {
			return Step::Relaunch;
		}
		return Step::Idle;
	}
};

}  // namespace vv::voxel
