include(FetchContent)

set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)  # opus picks its own CRT; match ours (static)

FetchContent_Declare(opus
  GIT_REPOSITORY https://github.com/xiph/opus
  GIT_TAG v1.5.2
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(opus)

# libopusenc has an autotools build only; fetch the source and build it by hand.
FetchContent_Declare(libopusenc
  GIT_REPOSITORY https://github.com/xiph/libopusenc
  GIT_TAG v0.2.1
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _no_cmake_here)
FetchContent_MakeAvailable(libopusenc)

file(GLOB OPUSENC_SOURCES ${libopusenc_SOURCE_DIR}/src/*.c)
add_library(opusenc STATIC ${OPUSENC_SOURCES})
target_include_directories(opusenc
  PUBLIC ${libopusenc_SOURCE_DIR}/include
  PRIVATE ${libopusenc_SOURCE_DIR}/src)
target_link_libraries(opusenc PUBLIC Opus::opus)
target_compile_definitions(opusenc PRIVATE
  OUTSIDE_SPEEX
  RANDOM_PREFIX=libopusenc
  PACKAGE_NAME="libopusenc"
  PACKAGE_VERSION="0.2.1")
target_compile_options(opusenc PRIVATE /W0)  # third-party code: no warnings
