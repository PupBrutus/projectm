# Standalone build of the MilkDrop preset lab harness on top of the parent projectM tree.
#   cmake -S milkdrop-lab -B milkdrop-lab/build -A x64
#   cmake --build milkdrop-lab/build --config Release --target preset_render
cmake_minimum_required(VERSION 3.21)
project(milkdrop_lab CXX C)

set(CMAKE_CXX_STANDARD 17)
set(ENABLE_SYSTEM_PROJECTM_EVAL OFF CACHE BOOL "" FORCE)
set(ENABLE_SYSTEM_GLM OFF CACHE BOOL "" FORCE)
set(ENABLE_PLAYLIST OFF CACHE BOOL "" FORCE)
set(ENABLE_SDL_UI OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
# Keep DEBUG log output so shader translation errors reach the report.
set(ENABLE_VERBOSE_LOGGING ON CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. projectm EXCLUDE_FROM_ALL)

add_executable(preset_render src/preset_render.cpp)
target_include_directories(preset_render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/stb_image)
target_link_libraries(preset_render PRIVATE libprojectM::projectM opengl32 user32 gdi32)
target_compile_definitions(preset_render PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
