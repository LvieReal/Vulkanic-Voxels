# Pure-logic test suite for the terrain/world modules. Deliberately free of
# windowing state and Vulkan so it also builds and runs in restricted
# sandboxes (see docs/AGENT_NOTES.md); GLFW is only linked for its key
# constants - nothing in it is initialized.
function(vv_add_tests)
    add_executable(voxel_tests
        tests/terrain_world_tests.cpp
        src/terrain/FarField.cpp
        src/terrain/Noise.cpp
        src/terrain/Noise3D.cpp
        src/terrain/TerrainGenerator.cpp
        src/voxel/Chunk.cpp
        src/voxel/VoxelTypes.cpp
        src/voxel/VoxelTextures.cpp
        src/voxel/World.cpp
        src/render/ImageDecode.cpp
        src/core/InputBindings.cpp
    )

    target_include_directories(voxel_tests PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src"
    )

    # GLFW (key constants for the binding tests) and the vendored headers
    # (stb_image, exercised by the decoder test). Linking GLFW is safe: its
    # test-only use here never calls glfwInit.
    target_include_directories(voxel_tests SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/third_party")
    target_link_libraries(voxel_tests PRIVATE ${VV_GLFW_TARGET} Threads::Threads)
    target_compile_definitions(voxel_tests PRIVATE GLFW_INCLUDE_NONE=1)

    # The SDF shadow tests mirror resources/shaders/voxels.comp, and
    # testSdfShaderMirrorConstants reads that file to pin the constants the
    # mirror hardcodes (pass 54; the pass-51 uniform bug is the same failure
    # mode: a GPU-side contract drifting away from its CPU counterpart).
    # VV_SRC_DIR lets the same test read VulkanRenderer.cpp: pass 55's jitter
    # slope reaches the shader through pc.camera.w, so the WRITER is part of
    # the contract too (pass 51 again: a shader uniform nobody wrote).
    target_compile_definitions(voxel_tests PRIVATE
        VV_SHADER_DIR="${CMAKE_SOURCE_DIR}/resources/shaders"
        VV_SRC_DIR="${CMAKE_SOURCE_DIR}/src")

    # Same float32 bit-exactness contract as the game target
    # (terrain/Noise.hpp): no FMA contraction on GCC/Clang.
    if(NOT MSVC)
        set_source_files_properties(
            src/terrain/Noise.cpp
            src/terrain/Noise3D.cpp
            src/terrain/TerrainGenerator.cpp
            PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
    endif()

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
