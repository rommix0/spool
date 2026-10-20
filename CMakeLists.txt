cmake_minimum_required(VERSION 3.25)
project(Spool LANGUAGES C CXX)

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

# ---- spool_core: everything except app window code ----
file(GLOB_RECURSE SPOOL_CORE_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/core/*.cpp
  ${CMAKE_SOURCE_DIR}/src/io/*.cpp
  ${CMAKE_SOURCE_DIR}/src/format/*.cpp
  ${CMAKE_SOURCE_DIR}/src/session/*.cpp
  ${CMAKE_SOURCE_DIR}/src/audio/*.cpp)
file(GLOB SPOOL_APP_CORE_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/app/settings.cpp
  ${CMAKE_SOURCE_DIR}/src/app/format_text.cpp
  ${CMAKE_SOURCE_DIR}/src/app/settings_model.cpp
  ${CMAKE_SOURCE_DIR}/src/app/text_util.cpp
  ${CMAKE_SOURCE_DIR}/src/app/lifecycle.cpp)
list(APPEND SPOOL_CORE_SOURCES ${SPOOL_APP_CORE_SOURCES})

add_library(spool_core STATIC ${SPOOL_CORE_SOURCES} src/core/types.h)
target_include_directories(spool_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(spool_core PUBLIC Opus::opus opusenc ole32 avrt uuid)

# ---- spool_tests ----
file(GLOB SPOOL_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.cpp)
add_executable(spool_tests ${SPOOL_TEST_SOURCES})
target_link_libraries(spool_tests PRIVATE spool_core)

# ---- spool: the dashboard app (window code only; settings, format_text, settings_model, text_util and lifecycle are in spool_core) ----
file(GLOB SPOOL_APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/app/*.cpp)
list(REMOVE_ITEM SPOOL_APP_SOURCES ${SPOOL_APP_CORE_SOURCES})
add_executable(spool WIN32 ${SPOOL_APP_SOURCES}
  src/app/spool.rc
  src/app/app.manifest)
target_link_libraries(spool PRIVATE spool_core comctl32 ole32 avrt uuid shell32)
