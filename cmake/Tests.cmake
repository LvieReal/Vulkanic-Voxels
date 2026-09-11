# Pure-logic test suite for the terrain/world modules. Deliberately free of
# Qt and Vulkan so it also builds and runs in restricted sandboxes (see
# docs/AGENT_NOTES.md).
function(vv_add_tests)
    add_executable(voxel_tests
        tests/terrain_world_tests.cpp
        src/terrain/Noise.cpp
        src/terrain/TerrainGenerator.cpp
        src/voxel/Chunk.cpp
        src/voxel/World.cpp
    )

    target_include_directories(voxel_tests PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src"
    )

    # glm headers only needed if a voxel header starts including them.
    if(VV_GLM_INCLUDE_DIR)
        target_include_directories(voxel_tests SYSTEM PRIVATE
            "${VV_GLM_INCLUDE_DIR}")
    endif()

    target_compile_options(voxel_tests PRIVATE
        $<$<CONFIG:Debug>:-O0 -g>
        $<$<CONFIG:Release>:-O2 -DNDEBUG>
    )

    add_test(NAME voxel_tests COMMAND voxel_tests)
endfunction()
