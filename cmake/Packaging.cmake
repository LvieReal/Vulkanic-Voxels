include(GNUInstallDirs)

function(vv_enable_packaging target_name)
    install(TARGETS ${target_name}
        RUNTIME DESTINATION .
    )

    if(EXISTS "${CMAKE_SOURCE_DIR}/resources")
        install(DIRECTORY "${CMAKE_SOURCE_DIR}/resources/"
            DESTINATION "resources"
            PATTERN "shaders/*.vert" EXCLUDE
            PATTERN "shaders/*.frag" EXCLUDE
            PATTERN "shaders/*.comp" EXCLUDE
            PATTERN "shaders/*.spv" EXCLUDE
        )
    endif()

    get_property(shader_spv_files TARGET ${target_name} PROPERTY VV_SHADER_SPV_FILES)
    if(shader_spv_files)
        install(FILES ${shader_spv_files}
            DESTINATION "resources/shaders"
        )
    endif()

    # Nothing to deploy for the windowing library: GLFW is either linked
    # statically (the fetched build) or already present on the target system
    # as a shared library its package manager owns. Qt's deployment script
    # (qt_generate_deploy_app_script) went away with Qt in pass 43.

    set(default_config "${CMAKE_BUILD_TYPE}")
    if(NOT default_config)
        set(default_config "Release")
    endif()

    add_custom_target(package_folder
        COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
            --config "$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,${default_config}>"
            --prefix "${CMAKE_BINARY_DIR}/dist/$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,${default_config}>"
        DEPENDS ${target_name}
        USES_TERMINAL
        COMMENT "Creating distributable folder in build/dist"
    )
endfunction()
