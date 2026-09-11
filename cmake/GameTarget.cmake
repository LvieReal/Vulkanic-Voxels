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

    if(WIN32)
        target_link_options(${target_name} PRIVATE
            $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU>>:-mwindows>
            $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:/SUBSYSTEM:WINDOWS>
        )
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

    target_link_libraries(${target_name} PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
    target_link_libraries(${target_name} PRIVATE Vulkan::Vulkan)

    if(WIN32)
        target_compile_definitions(${target_name} PRIVATE
            VK_USE_PLATFORM_WIN32_KHR
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
    endif()
endfunction()
