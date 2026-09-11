function(vv_add_game_target target_name)
    file(GLOB_RECURSE vv_sources CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/*.c"
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
    )

    add_executable(${target_name} ${vv_sources})

    target_include_directories(${target_name} PRIVATE
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_SOURCE_DIR}/src"
    )

    target_compile_options(${target_name} PRIVATE
        $<$<CONFIG:Debug>:-O0 -g>
        $<$<CONFIG:Release>:-O3 -DNDEBUG -s>
    )

    target_link_libraries(${target_name} PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
    target_link_libraries(${target_name} PRIVATE Vulkan::Vulkan)

    # The UI layer queries native window handles (XCB connection, Wayland
    # wl_surface, Cocoa NSView, ...) through Qt's QPA native interface, which
    # lives in the (installed) private QtGui headers.
    target_link_libraries(${target_name} PRIVATE Qt6::GuiPrivate)

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
        # Linux/BSD: both the X11 (XCB) and Wayland Vulkan surface backends
        # are compiled in when their headers are available. Only headers are
        # needed; the WSI entry points come from the Vulkan loader.
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(PC_XCB QUIET xcb)
            pkg_check_modules(PC_WAYLAND_CLIENT QUIET wayland-client)
        endif()

        find_path(VV_XCB_INCLUDE_DIR xcb/xcb.h HINTS ${PC_XCB_INCLUDE_DIRS})
        find_path(VV_WAYLAND_INCLUDE_DIR wayland-client-core.h
            HINTS ${PC_WAYLAND_CLIENT_INCLUDE_DIRS})

        if(VV_XCB_INCLUDE_DIR)
            message(STATUS "Vulkan X11/XCB surface backend: enabled")
            target_compile_definitions(${target_name} PRIVATE VV_HAVE_XCB=1)
            target_include_directories(${target_name} SYSTEM PRIVATE
                "${VV_XCB_INCLUDE_DIR}")
        else()
            message(WARNING
                "xcb headers not found - the X11 Vulkan surface backend is "
                "disabled. Install libxcb1-dev (Debian/Ubuntu) or "
                "libxcb-devel (Fedora) and reconfigure.")
        endif()

        if(VV_WAYLAND_INCLUDE_DIR)
            message(STATUS "Vulkan Wayland surface backend: enabled")
            target_compile_definitions(${target_name} PRIVATE VV_HAVE_WAYLAND=1)
            target_include_directories(${target_name} SYSTEM PRIVATE
                "${VV_WAYLAND_INCLUDE_DIR}")
        else()
            message(WARNING
                "Wayland client headers not found - the Wayland Vulkan "
                "surface backend is disabled. Install libwayland-dev "
                "(Debian/Ubuntu) or wayland-devel (Fedora) and reconfigure.")
        endif()

        if(NOT VV_XCB_INCLUDE_DIR AND NOT VV_WAYLAND_INCLUDE_DIR)
            message(FATAL_ERROR
                "Neither X11 nor Wayland headers were found: the game cannot "
                "create a Vulkan surface on this system. Install at least one "
                "of: libxcb1-dev / libx11-dev, libwayland-dev.")
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
