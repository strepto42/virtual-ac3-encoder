cmake_minimum_required(VERSION 3.20)
project(virtual_ac3_encoder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# FFmpeg (prebuilt shared dev libs fetched by scripts/fetch-ffmpeg.ps1)
# ---------------------------------------------------------------------------
set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ffmpeg" CACHE PATH "FFmpeg dev tree")
set(FFMPEG_INC  "${FFMPEG_ROOT}/include")
set(FFMPEG_LIB  "${FFMPEG_ROOT}/lib")
set(FFMPEG_BIN  "${FFMPEG_ROOT}/bin")

if(NOT EXISTS "${FFMPEG_INC}/libavcodec/avcodec.h")
  message(FATAL_ERROR "FFmpeg headers not found at ${FFMPEG_INC}. Run scripts/fetch-ffmpeg.ps1 first.")
endif()

add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg INTERFACE "${FFMPEG_INC}")
foreach(_l avcodec avformat avfilter avutil swresample)
  find_library(FF_${_l} NAMES ${_l} PATHS "${FFMPEG_LIB}" NO_DEFAULT_PATH REQUIRED)
  target_link_libraries(ffmpeg INTERFACE "${FF_${_l}}")
endforeach()

# Collect the runtime DLLs so we can stage them next to executables.
file(GLOB FFMPEG_DLLS "${FFMPEG_BIN}/*.dll")

# Helper: copy FFmpeg DLLs next to a target's output after build.
function(stage_ffmpeg_dlls tgt)
  foreach(_dll ${FFMPEG_DLLS})
    add_custom_command(TARGET ${tgt} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "$<TARGET_FILE_DIR:${tgt}>"
      VERBATIM)
  endforeach()
endfunction()

# ---------------------------------------------------------------------------
# Core encoder library (reused by the test harness and, later, the engine)
# ---------------------------------------------------------------------------
add_library(ac3core STATIC
  src/SpdifEncoder.cpp
  src/SpdifEncoder.h
  src/RingBuffer.h
)
target_include_directories(ac3core PUBLIC src)
target_link_libraries(ac3core PUBLIC ffmpeg)
if(MSVC)
  target_compile_options(ac3core PRIVATE /W4 /permissive-)
endif()

# ---------------------------------------------------------------------------
# Phase 1 offline test harness: WAV -> AC3/IEC61937
# ---------------------------------------------------------------------------
add_executable(encode_wav
  test/encode_wav.cpp
  test/WavFile.cpp
  test/WavFile.h
)
target_link_libraries(encode_wav PRIVATE ac3core ffmpeg)
stage_ffmpeg_dlls(encode_wav)

# ---------------------------------------------------------------------------
# Phase 2 live engine: WASAPI capture -> ring -> AC3 exclusive passthrough
# ---------------------------------------------------------------------------
add_executable(engine
  src/main.cpp
  src/Config.h
  src/ComUtil.cpp
  src/ComUtil.h
  src/Guids.cpp
  src/DeviceEnum.cpp
  src/DeviceEnum.h
  src/WasapiCapture.cpp
  src/WasapiCapture.h
  src/WasapiPassthrough.cpp
  src/WasapiPassthrough.h
)
target_link_libraries(engine PRIVATE ac3core ffmpeg ole32 avrt)
if(MSVC)
  target_compile_options(engine PRIVATE /permissive-)
endif()
# Version string (override from CI: -DVAC3_VERSION=0.1.0). Use a generated header so the
# quoting survives the VS generator (embedded-quote compile defs don't).
set(VAC3_VERSION "0.0.0-dev" CACHE STRING "Engine version string")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h" @ONLY)
target_include_directories(engine PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
stage_ffmpeg_dlls(engine)

# ---------------------------------------------------------------------------
# Unit tests (doctest) — RingBuffer + encoder contract
# ---------------------------------------------------------------------------
enable_testing()
add_executable(unit_tests
  test/tests_main.cpp
  test/test_ringbuffer.cpp
  test/test_encoder.cpp
  test/test_upmix.cpp
)
target_include_directories(unit_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/doctest"
  src
)
target_link_libraries(unit_tests PRIVATE ac3core ffmpeg)
stage_ffmpeg_dlls(unit_tests)
add_test(NAME unit_tests COMMAND unit_tests)
