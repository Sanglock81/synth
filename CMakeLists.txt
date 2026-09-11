cmake_minimum_required(VERSION 3.22)

project(VASynth VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC flags the standard std::getenv / fopen as "unsafe" (C4996). We use them
# deliberately and portably (env-gated trace, golden WAV I/O); silence the noise so
# the Windows CI build is clean.
if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    # SynthEngine is a large fixed-size RT-safe object (~344 KB); tests stack-allocate
    # several per function. MSVC defaults to a 1 MB stack (Linux uses 8 MB), which
    # overflows -> give the Windows binaries the same 8 MB reserve so tests match Linux.
    add_link_options(/STACK:8388608)
endif()

# --- Sanitizer builds (memory-leak / UB monitoring) -------------------------
# Applied build-wide (JUCE + our code + tests). Use via run-all-checks.sh
# --sanitize, or: cmake -B build-asan -DVASYNTH_ASAN=ON -DVASYNTH_BUILD_TESTS=ON
option(VASYNTH_ASAN  "Build with Address + Leak sanitizer" OFF)
option(VASYNTH_UBSAN "Build with Undefined Behaviour sanitizer" OFF)
if (VASYNTH_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
endif()
if (VASYNTH_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=undefined)
    # GCC/Clang define no predefined macro for UBSan (unlike __SANITIZE_ADDRESS__),
    # so expose one for tests that must skip wall/CPU-time comparisons under any
    # sanitizer instrumentation (see tests/dsp/test_osc6a.cpp kill-switch timing).
    add_compile_definitions(VASYNTH_UBSAN=1)
endif()

# Third-party UB exemption, scoped to ONE translation unit: the JUCE audio-formats unity
# TU, which is what #includes Xiph's vendored FLAC and libvorbis sources. Both carry UB
# that is theirs, not ours, and that we cannot fix without forking a codec:
#
#   * juce_FlacAudioFormat.cpp + libvorbis psy.c -- left shift of a negative value. UB on
#     paper before C++20, well-defined on every two's-complement target we ship to, and
#     made well-defined outright IN C++20.
#   * libvorbis psy.c:347 -- reads index 17 of a float[17] (a one-past-the-end over-read
#     in the tonemask/ATH interpolation). A known upstream libvorbis 1.3.7 report.
#
# Both surfaced from the REC export tests, the first thing in this project to exercise
# FLAC/Ogg ENCODING. The exemption is deliberately the whole TU rather than a list of
# individual checks: a per-check list is brittle (fixing shift-base merely revealed the
# over-read beneath it) and every line in this TU is vendored code either way.
#
# This does NOT weaken our own gate. Exactly one TU is exempt; all of Source/ and every
# other translation unit stay fully UB-checked. Never widen this to cover our own code --
# see the same policy in tests/lsan.supp.
#
# tests/ubsan.supp states the same intent as a runtime suppression, but GCC's libubsan
# IGNORES suppression files (an LLVM-only feature), so the exemption that actually takes
# effect on our gate is this compile-time one.
#
# Call this from each directory that defines targets consuming the JUCE module (source
# file properties are directory-scoped).
function(vasynth_exempt_thirdparty_codec_ub)
    if (NOT VASYNTH_UBSAN)
        return()
    endif()
    set_source_files_properties(
        ${juce_SOURCE_DIR}/modules/juce_audio_formats/juce_audio_formats.cpp
        PROPERTIES COMPILE_OPTIONS -fno-sanitize=undefined)
endfunction()

# Fetch JUCE. Pin the tag so builds are reproducible; bump deliberately.
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# juce_SOURCE_DIR exists from here on, so the exemption can be applied to this
# directory's targets (VASynth and its artefacts).
vasynth_exempt_thirdparty_codec_ub()

# Vendored libmp3lame -- the embedded MP3 encoder for REC exports (see cmake/lame.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/cmake/lame.cmake)

# Install the built VST3 into the user plugin dir after each build. OFF by default:
# a post-build COPY overwrites the ~/.vst3/synth.vst3 SYMLINK that tools/install-vst3.sh
# creates (the symlink is the never-stale install path). Turn ON for a one-shot copy if
# you prefer that; CI keeps it OFF anyway (the system VST3 dir may be unwritable).
option(VASYNTH_COPY_PLUGIN "Copy the VST3 to the user plugin dir after build" OFF)

# One target definition produces BOTH the VST3 and the standalone app.
juce_add_plugin(VASynth
    COMPANY_NAME            "Farmer"              # author/vendor (shown in DAW plugin lists)
    PLUGIN_MANUFACTURER_CODE Hstd                 # UNCHANGED — plugin identity / session compatibility
    PLUGIN_CODE              Vasy                 # UNCHANGED — same reason
    IS_SYNTH                 TRUE
    NEEDS_MIDI_INPUT         TRUE
    NEEDS_MIDI_OUTPUT        FALSE
    IS_MIDI_EFFECT           FALSE
    COPY_PLUGIN_AFTER_BUILD  ${VASYNTH_COPY_PLUGIN} # installs VST3 to user plugin dir (off in CI)
    FORMATS                  VST3 Standalone
    PRODUCT_NAME             "Synth")               # user-facing name (VST3 file, window title, DAW list)

target_sources(VASynth PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/MasterRecorder.cpp                   # MP3 encode: keeps <lame.h> out of every other TU
    Source/BuildStamp.cpp                       # greppable "VASYNTHBUILD:<hash>" freshness stamp
    Source/DSP/WavetableGen.cpp
    Source/Observability/AudioHealthLogger.cpp)

# The wavetable generator MUST be contraction-free so seed->table bytes are bit-identical across
# platforms (a table is persisted by seed). The #pragma STDC FP_CONTRACT OFF in the .cpp is ignored
# by some GCC builds, so pin it with the real codegen flag on GCC/Clang (MSVC /fp:precise never
# contracts). -Wno-unknown-pragmas silences the (now-belt-and-suspenders) pragma warning.
set_source_files_properties(Source/DSP/WavetableGen.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:GNU,Clang>:-ffp-contract=off;-Wno-unknown-pragmas>")

# Embedded resources: factory MIDI device profiles + factory presets (JSON).
# Linked PUBLIC so the plugin, the standalone, and plugin_tests all resolve
# BinaryData::*. GLOB the presets so adding a JSON needs no CMake edit (configure
# re-runs on change via CONFIGURE_DEPENDS).
file(GLOB VASYNTH_PRESET_JSON CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/resources/presets/*.json")
juce_add_binary_data(VASynthData SOURCES
    resources/midi-profiles/launchkey_mini.json
    ${VASYNTH_PRESET_JSON})
target_link_libraries(VASynth PUBLIC VASynthData)

# Custom standalone app: adds audio-device + MIDI-input logging (and change
# logging) that only the standalone's device manager can see. Compiled ONLY into
# the Standalone target; the VST3 uses the host's device handling untouched.
if (TARGET VASynth_Standalone)
    target_sources(VASynth_Standalone PRIVATE Source/Standalone/VASynthStandaloneApp.cpp)
    target_compile_definitions(VASynth_Standalone PRIVATE JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)
endif()

# Capture the short git hash at configure time for the startup log banner.
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE VASYNTH_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if (NOT VASYNTH_GIT_HASH)
    set(VASYNTH_GIT_HASH "unknown")
endif()

# BUILD-TIME-fresh hash for the on-screen banner: a custom target regenerates VersionInfo.h on
# every build (the configure-time value above goes stale on rebuild). VASynth depends on it.
set(VASYNTH_GEN_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${VASYNTH_GEN_DIR})
execute_process(                                            # seed it once so the first configure has a header
    COMMAND ${CMAKE_COMMAND}
        -DSRCDIR=${CMAKE_SOURCE_DIR}
        -DINFILE=${CMAKE_SOURCE_DIR}/Source/VersionInfo.h.in
        -DOUTFILE=${VASYNTH_GEN_DIR}/VersionInfo.h
        -P ${CMAKE_SOURCE_DIR}/cmake/gen_version.cmake)
add_custom_target(vasynth_version ALL
    BYPRODUCTS ${VASYNTH_GEN_DIR}/VersionInfo.h
    COMMAND ${CMAKE_COMMAND}
        -DSRCDIR=${CMAKE_SOURCE_DIR}
        -DINFILE=${CMAKE_SOURCE_DIR}/Source/VersionInfo.h.in
        -DOUTFILE=${VASYNTH_GEN_DIR}/VersionInfo.h
        -P ${CMAKE_SOURCE_DIR}/cmake/gen_version.cmake
    COMMENT "Refreshing git hash for the version banner")

target_compile_definitions(VASynth PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    # Standalone: allow runtime selection of audio device (your audio interface)
    # and enabling multiple MIDI inputs.
    JUCE_ALSA=1
    JUCE_JACK=1
    JUCE_ASIO=0            # compliance: never compile the proprietary Steinberg ASIO SDK into distributables
    # Startup-banner metadata (observability).
    VASYNTH_VERSION="${PROJECT_VERSION}"
    VASYNTH_GIT_HASH="${VASYNTH_GIT_HASH}"
    VASYNTH_BUILD_TYPE="$<CONFIG>")

add_dependencies(VASynth vasynth_version)
target_include_directories(VASynth PUBLIC ${VASYNTH_GEN_DIR})   # generated VersionInfo.h (build-fresh hash)

# LTO on by default (release artefacts); turn OFF for fast dev rebuilds:
#   cmake -B build -DVASYNTH_ENABLE_LTO=OFF
option(VASYNTH_ENABLE_LTO "Enable JUCE recommended link-time optimisation" ON)

target_link_libraries(VASynth PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    vasynth_mp3lame              # embedded MP3 encoder (MasterRecorder); no runtime dependency
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

if (VASYNTH_ENABLE_LTO)
    target_link_libraries(VASynth PUBLIC juce::juce_recommended_lto_flags)
endif()

# ---------------------------------------------------------------------------
# Tests. Off by default for a plain plugin build; run-all-checks.sh turns it on.
# DSP tests link NO JUCE (Source/DSP/ is dependency-free by design); the
# plugin-layer tests link the shared-code target.
# ---------------------------------------------------------------------------
option(VASYNTH_BUILD_TESTS "Build the VA Synth test suite" OFF)

if (VASYNTH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
