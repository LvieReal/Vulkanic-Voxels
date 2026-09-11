cmake_minimum_required(VERSION 3.26)
project(game VERSION 0.1.0 LANGUAGES C CXX)

# Bake the git state into a generated header (core/Version.h.in ->
# ${CMAKE_BINARY_DIR}/generated/core/Version.h) so the window title can name
# the exact build (stale-binary reports are otherwise indistinguishable from
# rendering bugs).
set(VV_BUILD_ID "unknown")
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=7 HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE VV_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE VV_GIT_HASH_RESULT)
    if(VV_GIT_HASH_RESULT EQUAL 0 AND VV_GIT_HASH)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} diff --quiet
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            RESULT_VARIABLE VV_GIT_DIRTY_RESULT)
        if(NOT VV_GIT_DIRTY_RESULT EQUAL 0)
            set(VV_GIT_HASH "${VV_GIT_HASH}-dirty")
        endif()
        set(VV_BUILD_ID "${VV_GIT_HASH}")
    endif()
endif()
configure_file(
    "${CMAKE_SOURCE_DIR}/src/core/Version.h.in"
    "${CMAKE_BINARY_DIR}/generated/core/Version.h"
    @ONLY)

include(cmake/ProjectOptions.cmake)
include(cmake/Dependencies.cmake)
include(cmake/GameTarget.cmake)
include(cmake/Shaders.cmake)
include(cmake/Packaging.cmake)

vv_add_game_target(${PROJECT_NAME})
vv_enable_shader_pipeline(${PROJECT_NAME})
vv_enable_packaging(${PROJECT_NAME})

option(VV_BUILD_TESTS "Build the pure-logic test suite (terrain, world)" ON)
if(VV_BUILD_TESTS)
    include(CTest)
    if(BUILD_TESTING)
        include(cmake/Tests.cmake)
        vv_add_tests()
    endif()
endif()
