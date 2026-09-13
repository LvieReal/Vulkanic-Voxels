#include "terrain/FarField.hpp"

#include <chrono>
#include <cmath>
#include <thread>

namespace vv::terrain {

std::size_t FarField::patchRegion(std::vector<std::uint32_t>& cells,
																	std::uint32_t dim,
																	std::uint32_t cellVoxels,
																	std::int32_t originVoxX,
																	std::int32_t originVoxZ,
																	const std::vector<RegionChunkHeights>& chunks,
																	const TerrainGenerator& gen,
																	std::vector<std::uint32_t>* outChangedIndices) {
	const std::int64_t dim64 = static_cast<std::int64_t>(dim);
	const std::int64_t cell64 = static_cast<std::int64_t>(cellVoxels);
	const std::int64_t originX = originVoxX;
	const std::int64_t originZ = originVoxZ;
	if (cells.size() != static_cast<std::size_t>(dim) * dim || cellVoxels == 0) {
		return 0;
	}

	std::vector<std::uint32_t> best(cells.size(), 0u);
	std::vector<std::uint32_t> cover(cells.size(), 0u);
	for (const RegionChunkHeights& chunk : chunks) {
		if (chunk.heights == nullptr || chunk.sizeX == 0 || chunk.sizeZ == 0) {
			continue;
		}
		const std::int64_t baseX = chunk.minVoxX;
		const std::int64_t baseZ = chunk.minVoxZ;
		for (std::uint32_t z = 0; z < chunk.sizeZ; ++z) {
			const std::int64_t fj = (baseZ + z - originZ) / cell64;
			if (fj < 0 || fj >= dim64) {
				continue;
			}
			for (std::uint32_t x = 0; x < chunk.sizeX; ++x) {
				const std::int64_t fi = (baseX + x - originX) / cell64;
				if (fi < 0 || fi >= dim64) {
					continue;
				}
				const std::uint16_t h =
						chunk.heights[x + z * chunk.sizeX];  // top + 1 (0 = air)
				const std::size_t ci = static_cast<std::size_t>(fi) +
																static_cast<std::size_t>(fj) *
																		static_cast<std::size_t>(dim);
				if (h > best[ci]) {
					best[ci] = h;
				}
				++cover[ci];
			}
		}
	}

	const std::uint32_t full = cellVoxels * cellVoxels;
	std::size_t changed = 0;
	for (std::size_t i = 0; i < cells.size(); ++i) {
		const std::uint32_t exact = best[i];
		if (exact == 0u) {
			continue;
		}
		const std::uint32_t current = cells[i] & 0xFFFFu;
		const std::uint32_t height = (cover[i] >= full)
																		 ? exact
																		 : std::max(exact, current);
		if (height == current) {
			continue;
		}
		const std::int32_t top = static_cast<std::int32_t>(height) - 1;
		const auto type = gen.typeForDepth(top, top);
		cells[i] = packColumn(static_cast<std::uint16_t>(height),
														static_cast<std::uint8_t>(type));
		if (outChangedIndices != nullptr) {
			outChangedIndices->push_back(static_cast<std::uint32_t>(i));
		}
		++changed;
	}
	return changed;
}

FarField FarField::build(const TerrainGenerator& gen,
															 std::int32_t centerChunkX,
															 std::int32_t centerChunkZ,
															 std::uint32_t radiusChunks,
															 std::uint32_t cellVoxels,
															 std::uint32_t chunkSize,
															 const FarField* previous) {
	FarField field;
	field.dim = radiusChunks > 0 ? (2 * radiusChunks * chunkSize) / cellVoxels
															 : 0;
	field.cellVoxels = cellVoxels;
	if (field.dim == 0) {
		return field;
	}

	// Center the box near the requested chunk, SNAPPED to a world-aligned
	// 512-voxel grid (a multiple of both the chunk size and the cell
	// footprint). The field is a pure function of world position, so with
	// a fixed grid alignment every recenter only shifts the WINDOW: cells
	// keep their world coordinates and values. Before the snap, each
	// recenter re-quantized the whole distant terrain against a grid that
	// had moved with the camera - visible popping/morphing at every
	// rebuild, which TAA then blended with the old silhouette ("ghosting"
	// during fast flight). Snap offset <= 256 voxels (8 chunks), well
	// inside the recenter hysteresis margins.
	constexpr std::int64_t kSnap = 512;
	const std::int64_t reqX =
			static_cast<std::int64_t>(centerChunkX) * chunkSize + chunkSize / 2;
	const std::int64_t reqZ =
			static_cast<std::int64_t>(centerChunkZ) * chunkSize + chunkSize / 2;
	const auto snapDown = [](std::int64_t v, std::int64_t grid) {
		return v - ((v % grid) + grid) % grid;
	};
	const std::int64_t centerX = snapDown(reqX, kSnap);
	const std::int64_t centerZ = snapDown(reqZ, kSnap);
	const std::int64_t half = (static_cast<std::int64_t>(field.dim) *
															 field.cellVoxels) / 2;
	field.originVoxX = static_cast<std::int32_t>(centerX - half);
	field.originVoxZ = static_cast<std::int32_t>(centerZ - half);
	field.centerVoxX = static_cast<std::int32_t>(centerX);
	field.centerVoxZ = static_cast<std::int32_t>(centerZ);

	// One column evaluation per cell, at the cell center (see header comment
	// for the sampling trade-off). With the 3D density terrain the stored
	// height is the ESTIMATED topmost solid voxel (one fixed-point iteration
	// of the isosurface: 3D noise sampled at the target height) - within a
	// couple of voxels of the true surface, far below the 4-voxel cell
	// footprint, and consistent with the near-region silhouette at the seam
	// to within the existing far quantization. heightAtF is const and
	// thread-safe.
	// Window reuse: with a world-aligned grid, a recenter only shifts the
	// window; cells at the same world position carry the same values (the
	// field is a pure function of position), so they are copied from the
	// previous field and only the exposed strips are recomputed.
	const FarField* prev = nullptr;
	std::int64_t shiftI = 0;
	std::int64_t shiftJ = 0;
	if (previous != nullptr && previous->dim == field.dim &&
			previous->cellVoxels == field.cellVoxels &&
			previous->cells.size() ==
					static_cast<std::size_t>(previous->dim) * previous->dim &&
			(previous->originVoxX - field.originVoxX) %
							static_cast<std::int64_t>(cellVoxels) ==
					0 &&
			(previous->originVoxZ - field.originVoxZ) %
							static_cast<std::int64_t>(cellVoxels) ==
					0) {
		shiftI = (previous->originVoxX - field.originVoxX) /
						 static_cast<std::int64_t>(cellVoxels);
		shiftJ = (previous->originVoxZ - field.originVoxZ) /
						 static_cast<std::int64_t>(cellVoxels);
		if (shiftI > -static_cast<std::int64_t>(field.dim) &&
				shiftI < static_cast<std::int64_t>(field.dim) &&
				shiftJ > -static_cast<std::int64_t>(field.dim) &&
				shiftJ < static_cast<std::int64_t>(field.dim)) {
			prev = previous;
		}
	}

	field.cells.assign(static_cast<std::size_t>(field.dim) * field.dim, 0u);
	for (std::uint32_t j = 0; j < field.dim; ++j) {
		// Cooperative yield: this runs on a background thread and takes
		// seconds at the default radius; without pauses it starves the
		// main thread (and thus the frame rate) on low-core-count
		// machines - one of the pass-6 "stutter when chunks load" causes.
		if ((j & 3u) == 0u && j != 0) {
			std::this_thread::sleep_for(std::chrono::microseconds(300));
		}
		const float wz = static_cast<float>(
				static_cast<std::int64_t>(field.originVoxZ) +
				static_cast<std::int64_t>(j) * cellVoxels + cellVoxels / 2);
		for (std::uint32_t i = 0; i < field.dim; ++i) {
			// Window reuse: copy the previous field's cell at this world
			// position when it exists (the window shifted by (shiftI,
			// shiftJ) cells); only exposed strips fall through to the
			// estimate below.
			if (prev != nullptr) {
				// Index of this world cell in the PREVIOUS window. Note the
				// MINUS: prevIndex = (worldX - prevOrigin)/cell
				//        = i - (prevOrigin - newOrigin)/cell = i - shift.
				// (Pass 14 shipped `i + shift` - every recenter displayed
				// the old terrain shifted by TWICE the window move, i.e. a
				// different landscape: the "LOD switches between two
				// different worlds" report.)
				const std::int64_t si = static_cast<std::int64_t>(i) - shiftI;
				const std::int64_t sj = static_cast<std::int64_t>(j) - shiftJ;
				if (si >= 0 && sj >= 0 &&
						si < static_cast<std::int64_t>(field.dim) &&
						sj < static_cast<std::int64_t>(field.dim)) {
					field.cells[static_cast<std::size_t>(i) +
											static_cast<std::size_t>(j) * field.dim] =
							prev->cells[static_cast<std::size_t>(si) +
													static_cast<std::size_t>(sj) *
															prev->dim];
					continue;
				}
			}
			const float wx = static_cast<float>(
					static_cast<std::int64_t>(field.originVoxX) +
					static_cast<std::int64_t>(i) * cellVoxels + cellVoxels / 2);
			const float target = gen.surfaceTargetF(wx, wz);
			const float mask = gen.mountainMaskF(wx, wz);
			const std::int32_t topSolid =
					gen.estimatedTopSolid(wx, wz, target, mask);
			const auto type = gen.typeForDepth(topSolid, topSolid);
			field.cells[static_cast<std::size_t>(i) +
									static_cast<std::size_t>(j) * field.dim] =
					packColumn(static_cast<std::uint16_t>(topSolid + 1),
										 static_cast<std::uint8_t>(type));
		}
	}
	return field;
}

}  // namespace vv::terrain
