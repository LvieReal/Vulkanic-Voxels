function(vv_enable_shader_pipeline target_name)
    set(shader_source_dir "${CMAKE_SOURCE_DIR}/resources/shaders")
    set(shader_output_dir "${CMAKE_BINARY_DIR}/resources/shaders")
    set(shader_runtime_dir "$<TARGET_FILE_DIR:${target_name}>/resources/shaders")

    find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)

    file(GLOB shader_sources CONFIGURE_DEPENDS
        "${shader_source_dir}/*.vert"
        "${shader_source_dir}/*.frag"
        "${shader_source_dir}/*.comp"
    )
    if(NOT shader_sources)
        message(FATAL_ERROR "No shader sources found in ${shader_source_dir}")
    endif()

    # SPIR-V debug info in Debug builds only: -g embeds the GLSL source
    # and line tables (OpSource/OpLine/OpName), which Nsight Graphics
    # needs for shader source correlation while profiling. Every other
    # config passes -g0 (glslang's default = strip), so the flag list is
    # never empty (an empty generator-expression argument would leak as
    # a literal "" file argument under Ninja/VERBATIM) and the Release
    # SPIR-V stays byte-identical to a flag-less compile.
    set(shader_debug_flag "$<$<CONFIG:Debug>:-g>$<$<NOT:$<CONFIG:Debug>>:-g0>")
    set(shader_debug_comment "$<$<CONFIG:Debug>: (with debug info)>")

    set(shader_spv_files)
    foreach(shader_file IN LISTS shader_sources)
        get_filename_component(shader_name "${shader_file}" NAME)
        set(shader_spv "${shader_output_dir}/${shader_name}.spv")
        add_custom_command(
            OUTPUT "${shader_spv}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_output_dir}"
            COMMAND "${GLSLANG_VALIDATOR}" -V "${shader_file}" ${shader_debug_flag} -o "${shader_spv}"
            DEPENDS "${shader_file}"
            COMMENT "Compiling ${shader_name} -> ${shader_name}.spv${shader_debug_comment}"
            VERBATIM
        )
        list(APPEND shader_spv_files "${shader_spv}")
    endforeach()

    add_custom_target(${target_name}_shaders ALL DEPENDS ${shader_spv_files})
    add_dependencies(${target_name} ${target_name}_shaders)

    add_custom_command(
        TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_runtime_dir}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${shader_spv_files} "${shader_runtime_dir}"
        COMMENT "Copying SPIR-V shaders to runtime resources/shaders directory"
    )

    set_property(TARGET ${target_name} PROPERTY VV_SHADER_SPV_FILES "${shader_spv_files}")
endfunction()
