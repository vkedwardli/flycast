cmake_minimum_required(VERSION 3.20)
# Match the main build before CMake initializes compiler availability checks.
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
project(multi_pov_window_test LANGUAGES CXX OBJCXX)
if(NOT APPLE)
    message(FATAL_ERROR "This integration test requires macOS AppKit and a desktop session")
endif()
get_filename_component(FLYCAST_ROOT "../.." ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${FLYCAST_ROOT}/core/deps/SDL" sdl EXCLUDE_FROM_ALL)
add_executable(multi_pov_window_test apple.mm
    "${FLYCAST_ROOT}/core/gdxsv/gdxsv_multi_pov_window_apple.mm")
target_compile_features(multi_pov_window_test PRIVATE cxx_std_17)
target_include_directories(multi_pov_window_test PRIVATE "${FLYCAST_ROOT}/core"
    "${FLYCAST_ROOT}/core/deps/nowide/include")
target_link_libraries(multi_pov_window_test PRIVATE SDL2::SDL2-static
    "-framework Cocoa" "-framework CoreGraphics")
enable_testing()
add_test(NAME native_multi_pov_window COMMAND multi_pov_window_test)
set_tests_properties(native_multi_pov_window PROPERTIES SKIP_RETURN_CODE 77)
