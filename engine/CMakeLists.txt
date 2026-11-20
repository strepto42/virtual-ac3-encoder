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
  src/Limiter.h
  src/Iec61937.h
  src/DtsBitstream.cpp
  src/DtsBitstream.h
  src/dsp/DspParams.h
  src/dsp/GraphicEq.h
  src/dsp/Compressor.h
  src/dsp/DspChain.h
  src/dsp/DspSettings.h
  src/dsp/PcmDsp.h
  src/dsp/Mailbox.h
  src/dsp/MeterShm.h
  src/dsp/MeterMath.h
  src/dsp/StudioModel.h
  src/dsp/Spectrum.h
  src/dsp/MatrixUpmix.h
  src/dsp/ConfEdit.h
  src/dsp/Loudness.h
  src/dsp/EqImport.h
  src/dsp/AutoProfiles.h
  src/dsp/UndoHistory.h
  src/BootTrace.h
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

# Offline tool: stereo WAV -> 5.1 WAV with the time-domain matrix upmix (the engine's exact code, no FFmpeg).
add_executable(upmix_wav
  test/upmix_wav.cpp
  test/WavFile.cpp
  test/WavFile.h
)
target_include_directories(upmix_wav PRIVATE src test)
if(MSVC)
  target_compile_options(upmix_wav PRIVATE /W3 /permissive- /utf-8)
endif()

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
  src/WasapiPcmOutput.cpp
  src/WasapiPassthrough.h
  src/WasapiPcmOutput.h
)
target_link_libraries(engine PRIVATE ac3core ffmpeg ole32 avrt winmm)
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
# Native Windows GUI for live engine control
# ---------------------------------------------------------------------------
add_executable(control_center WIN32 src/ControlCenter.cpp)
target_link_libraries(control_center PRIVATE comctl32 ole32 propsys shell32 uuid)
if(MSVC)
  target_compile_options(control_center PRIVATE /W4 /permissive- /utf-8)
endif()
# ---------------------------------------------------------------------------
# Unit tests (doctest) — RingBuffer + encoder contract
# ---------------------------------------------------------------------------
enable_testing()
add_executable(unit_tests
  test/tests_main.cpp
  test/test_ringbuffer.cpp
  test/test_encoder.cpp
  test/test_upmix.cpp
  test/test_limiter.cpp
  test/test_iec61937.cpp
  test/test_wavdump.cpp
  test/test_dsp.cpp
  test/test_dsp_settings.cpp
  test/test_studio_logic.cpp
  test/test_eq_shapes.cpp
  test/test_spectrum.cpp
  test/test_matrix_upmix.cpp
  test/test_studio_tools.cpp
)
target_include_directories(unit_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/doctest"
  src
)
target_link_libraries(unit_tests PRIVATE ac3core ffmpeg)
stage_ffmpeg_dlls(unit_tests)
add_test(NAME unit_tests COMMAND unit_tests)

# ---------------------------------------------------------------------------
# Qt 6 control panels in the Breeze style: variant A (mixer) and variant B (mode list).
# Built only when Qt 6 is found: pass -DCMAKE_PREFIX_PATH=<Qt>\6.x.x\msvc2022_64 or use
# build.ps1 -QtDir. Without Qt the engine and the Win32 control_center.exe are still built.
# ---------------------------------------------------------------------------
option(VAC3_BUILD_QT_UI "Build the Qt control panels (variants A and B)" ON)
if(VAC3_BUILD_QT_UI)
  find_package(Qt6 6.2 COMPONENTS Widgets Network QUIET)
  if(Qt6_FOUND)
    set(CMAKE_AUTOMOC ON)
    set(CC_QT_COMMON
      qt/Backend.cpp qt/Backend.h
      qt/BreezeTheme.cpp qt/BreezeTheme.h
      qt/PanelWindow.cpp qt/PanelWindow.h
      qt/AppMain.h)
    qt_add_executable(cc_qt_a WIN32 qt/main_a.cpp qt/MixerWindow.cpp qt/MixerWindow.h qt/LevelBar.cpp qt/LevelBar.h ${CC_QT_COMMON})
    qt_add_executable(cc_qt_b WIN32 qt/main_b.cpp qt/ModesWindow.cpp qt/ModesWindow.h ${CC_QT_COMMON})
    set_target_properties(cc_qt_a PROPERTIES OUTPUT_NAME "ControlCenter_A_Mixer")
    set_target_properties(cc_qt_b PROPERTIES OUTPUT_NAME "ControlCenter_B_Modes")
    foreach(target cc_qt_a cc_qt_b)
      target_include_directories(${target} PRIVATE qt)
      target_link_libraries(${target} PRIVATE Qt6::Widgets Qt6::Network ole32 shell32 uuid propsys)
      if(MSVC)
        target_compile_options(${target} PRIVATE /W3 /permissive- /utf-8)
      endif()
    endforeach()
    # variant A shows level bars next to the faders: it reads the engine's shared-memory level block
    target_include_directories(cc_qt_a PRIVATE src)
    target_compile_definitions(cc_qt_a PRIVATE NOMINMAX)
    # DSP Studio: 30-band EQ with response curve, compressor, level meters (separate exe)
    qt_add_executable(dsp_studio WIN32
      studio/StudioMain.cpp
      studio/StudioWindow.cpp studio/StudioWindow.h
      studio/StudioTheme.cpp studio/StudioTheme.h
      studio/Widgets.cpp studio/Widgets.h
      studio/AutoProfilesDialog.cpp studio/AutoProfilesDialog.h
      qt/BreezeTheme.cpp qt/BreezeTheme.h)
    set_target_properties(dsp_studio PROPERTIES OUTPUT_NAME "VirtualAc3-DSP-Studio")
    target_include_directories(dsp_studio PRIVATE studio qt src)
    target_compile_definitions(dsp_studio PRIVATE NOMINMAX)
    target_link_libraries(dsp_studio PRIVATE Qt6::Widgets Qt6::Network)
    if(MSVC)
      target_compile_options(dsp_studio PRIVATE /W3 /permissive- /utf-8)
    endif()
    message(STATUS "Qt control panels and DSP Studio: enabled")
  else()
    message(STATUS "Qt 6 not found: the Qt control panels are skipped (the Win32 control_center.exe is still built)")
  endif()
endif()
