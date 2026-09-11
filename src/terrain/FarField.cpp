#include "terrain/FarField.hpp"

#include <cmath>

namespace vv::terrain {

FarField FarField::build(const TerrainGenerator& gen,
												 std::int32_t centerChunkX,
												 std::int32_t centerChunkZ,
												 std::uint32_t radiusChunks,
												 std::uint32_t cellVoxels,
												 std::uint32_t chunkSize) {
	FarField field;
	field.dim = radiusChunks > 0 ? (2 * radiusChunks * chunkSize) / cellVoxels
															 : 0;
	field.cellVoxels = cellVoxels;
	if (field.dim == 0) {
		return field;
	}

	// Center the box on the center chunk's center voxel; the min corner
	// lands on a cell boundary because dim * cellVoxels is even.
	const std::int64_t centerX =
			static_cast<std::int64_t>(centerChunkX) * chunkSize + chunkSize / 2;
	const std::int64_t centerZ =
			static_cast<std::int64_t>(centerChunkZ) * chunkSize + chunkSize / 2;
	const std::int64_t half = (static_cast<std::int64_t>(field.dim) *
														 field.cellVoxels) / 2;
	field.originVoxX = static_cast<std::int32_t>(centerX - half);
	field.originVoxZ = static_cast<std::int32_t>(centerZ - half);

	// One noise evaluation per cell, at the cell center (see header comment
	// for the sampling trade-off). heightAtF is const and thread-safe; the
	// 4-wide path would help here but lanes share no state, so the scalar
	// call is fine on the background thread.
	field.cells.assign(static_cast<std::size_t>(field.dim) * field.dim, 0u);
	for (std::uint32_t j = 0; j < field.dim; ++j) {
		const float wz = static_cast<float>(
				static_cast<std::int64_t>(field.originVoxZ) +
				static_cast<std::int64_t>(j) * cellVoxels + cellVoxels / 2);
		for (std::uint32_t i = 0; i < field.dim; ++i) {
			const float wx = static_cast<float>(
					static_cast<std::int64_t>(field.originVoxX) +
					static_cast<std::int64_t>(i) * cellVoxels + cellVoxels / 2);
			const float height = gen.heightAtF(wx, wz);
			const std::int32_t surface =
					static_cast<std::int32_t>(std::floor(height));
			const auto type =
					gen.typeForColumn(surface, height);
			field.cells[static_cast<std::size_t>(i) +
									static_cast<std::size_t>(j) * field.dim] =
					packColumn(static_cast<std::uint16_t>(surface + 1),
										 static_cast<std::uint8_t>(type));
		}
	}
	return field;
}

}  // namespace vv::terrain
