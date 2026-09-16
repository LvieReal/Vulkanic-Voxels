function(vv_add_game_target target_name)
    file(GLOB_RECURSE vv_sources CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/*.c"
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
    )

    add_executable(${target_name} ${vv_sources})

    target_include_directories(${target_name} PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_BINARY_DIR}/generated"
    )

    # Terrain generation runs in float32 with a bit-exactness contract
    # between the scalar reference and the SSE2 path (see terrain/Noise.hpp):
    # forbid FMA contraction so GCC/Clang cannot fuse a*b+c into fma (which
    # would diverge across compilers/CPUs). MSVC does not contract.
    if(NOT MSVC)
        set_source_files_properties(
            src/terrain/Noise.cpp
            src/terrain/TerrainGenerator.cpp
            PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
    endif()

    target_compile_options(${target_name} PRIVATE
        $<$<CONFIG:Debug>:-O0 -g>
        $<$<CONFIG:Release>:-O3 -DNDEBUG -s>
    )

    target_link_libraries(${target_name} PRIVATE ${VV_GLFW_TARGET} Vulkan::Vulkan)
    target_link_libraries(${target_name} PRIVATE Threads::Threads)

    # Vendored headers (stb_image decodes the voxel textures; see
    # third_party/README.md). Header-only: nothing extra to link.
    target_include_directories(${target_name} SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/third_party")

    # GLFW must not pull in an OpenGL header: presentation goes through
    # Vulkan and no GL symbols are used (GLFW_INCLUDE_NONE is glfw's own
    # switch for that, see its documentation).
    target_compile_definitions(${target_name} PRIVATE GLFW_INCLUDE_NONE=1)

    # glm (header-only) location discovered in Dependencies.cmake.
    if(VV_GLM_INCLUDE_DIR)
        target_include_directories(${target_name} SYSTEM PRIVATE
            "${VV_GLM_INCLUDE_DIR}")
    endif()

    if(WIN32)
        target_link_options(${target_name} PRIVATE
            $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU>>:-mwindows>
            $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:/SUBSYSTEM:WINDOWS>
        )

        target_compile_definitions(${target_name} PRIVATE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
    elseif(APPLE)
        # Nothing special: the MoltenVK surface backend is selected at run
        # time and needs no extra frameworks for surface creation.
    else()
        # Linux/BSD: the native-window handles come from GLFW's headers
        # (glfw3native.h), which declare the X11 (xcb_connection_t,
        # xcb_window_t) and Wayland (wl_display, wl_surface) types behind
        # GLFW_EXPOSE_NATIVE_X11 / _WAYLAND.
        #
        # Which backend exists is decided in Dependencies.cmake: from the
        # development headers that are actually installed (and, for the
        # vendored GLFW, from the backends it was therefore told to build).
        # The definitions below only describe what our own sources have to
        # see - the warning for "neither backend" lives next to that probe.
        if(VV_GLFW_HAS_X11)
            message(STATUS "GLFW native-window backend: X11")
            target_compile_definitions(${target_name} PRIVATE VV_WINDOW_BACKEND_X11=1)
        endif()
        if(VV_GLFW_HAS_WAYLAND)
            message(STATUS "GLFW native-window backend: Wayland")
            target_compile_definitions(${target_name} PRIVATE VV_WINDOW_BACKEND_WAYLAND=1)
            if(VV_GLFW_WAYLAND_HEADERS)
                target_include_directories(${target_name} SYSTEM PRIVATE
                    "${VV_GLFW_WAYLAND_HEADERS}")
            endif()
        endif()
    endif()

    if(EXISTS "${CMAKE_SOURCE_DIR}/resources/textures")
        add_custom_command(
            TARGET ${target_name} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target_name}>/resources/textures"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_SOURCE_DIR}/resources/textures"
                "$<TARGET_FILE_DIR:${target_name}>/resources/textures"
            COMMENT "Copying textures to runtime resources/textures directory"
        )
    endif()
endfunction()
