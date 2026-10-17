cmake_minimum_required(VERSION 3.25)
project(SoundLogger LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(NOT MSVC)
  message(FATAL_ERROR "MSVC is required (use -G \"Visual Studio 17 2022\" -A x64)")
endif()

# Static CRT everywhere (must be set before FetchContent so deps match).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(cmake/deps.cmake)

# Project flags: set after deps so third-party code keeps its own warning level.
add_compile_options(/W4 /permissive- /utf-8)
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# ---- sl_core: everything except app window code ----
file(GLOB_RECURSE SL_CORE_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/core/*.cpp
  ${CMAKE_SOURCE_DIR}/src/io/*.cpp
  ${CMAKE_SOURCE_DIR}/src/format/*.cpp
  ${CMAKE_SOURCE_DIR}/src/session/*.cpp
  ${CMAKE_SOURCE_DIR}/src/audio/*.cpp)
foreach(f settings.cpp format_text.cpp)
  if(EXISTS ${CMAKE_SOURCE_DIR}/src/app/${f})
    list(APPEND SL_CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/app/${f})
  endif()
endforeach()

add_library(sl_core STATIC ${SL_CORE_SOURCES} src/core/types.h)
target_include_directories(sl_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(sl_core PUBLIC Opus::opus opusenc)

# ---- sl_tests ----
file(GLOB SL_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.cpp)
add_executable(sl_tests ${SL_TEST_SOURCES})
target_link_libraries(sl_tests PRIVATE sl_core)

# ---- sound_logger (enabled in Task 16) ----
# add_executable(sound_logger WIN32 ...)
# target_link_libraries(sound_logger PRIVATE sl_core)
