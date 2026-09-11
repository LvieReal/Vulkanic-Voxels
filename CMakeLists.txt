cmake_minimum_required(VERSION 3.26)
project(game VERSION 0.1.0 LANGUAGES C CXX)

include(cmake/ProjectOptions.cmake)
include(cmake/Dependencies.cmake)
include(cmake/GameTarget.cmake)
include(cmake/Shaders.cmake)
include(cmake/Packaging.cmake)

vv_add_game_target(${PROJECT_NAME})
vv_enable_shader_pipeline(${PROJECT_NAME})
vv_enable_packaging(${PROJECT_NAME})
