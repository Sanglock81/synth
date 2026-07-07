cmake_minimum_required(VERSION 3.22)

project(VASynth VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
endif()

# Fetch JUCE. Pin the tag so builds are reproducible; bump deliberately.
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# Install the built VST3 into the user plugin dir after each build (handy for
# local dev). OFF in CI, where the system VST3 dir may be unwritable.
option(VASYNTH_COPY_PLUGIN "Copy the VST3 to the user plugin dir after build" ON)

# One target definition produces BOTH the VST3 and the standalone app.
juce_add_plugin(VASynth
    COMPANY_NAME            "HomeStudio"          # rename freely
    PLUGIN_MANUFACTURER_CODE Hstd
    PLUGIN_CODE              Vasy
    IS_SYNTH                 TRUE
    NEEDS_MIDI_INPUT         TRUE
    NEEDS_MIDI_OUTPUT        FALSE
    IS_MIDI_EFFECT           FALSE
    COPY_PLUGIN_AFTER_BUILD  ${VASYNTH_COPY_PLUGIN} # installs VST3 to user plugin dir (off in CI)
    FORMATS                  VST3 Standalone
    PRODUCT_NAME             "synth")               # user-facing name (VST3 file, window title)

target_sources(VASynth PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Observability/AudioHealthLogger.cpp)

# Embedded resources: factory MIDI device profiles + factory presets (JSON).
# Linked PUBLIC so the plugin, the standalone, and plugin_tests all resolve
# BinaryData::*. GLOB the presets so adding a JSON needs no CMake edit (configure
# re-runs on change via CONFIGURE_DEPENDS).
file(GLOB VASYNTH_PRESET_JSON CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/resources/presets/*.json")
juce_add_binary_data(VASynthData SOURCES
    resources/midi-profiles/launchkey_mini.json
    resources/midi-profiles/korg_b2.json
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

target_compile_definitions(VASynth PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    # Standalone: allow runtime selection of audio device (Scarlett 2i2)
    # and enabling multiple MIDI inputs (Korg B2 + Launchkey Mini).
    JUCE_ALSA=1
    JUCE_JACK=1
    # Startup-banner metadata (observability).
    VASYNTH_VERSION="${PROJECT_VERSION}"
    VASYNTH_GIT_HASH="${VASYNTH_GIT_HASH}"
    VASYNTH_BUILD_TYPE="$<CONFIG>")

# LTO on by default (release artefacts); turn OFF for fast dev rebuilds:
#   cmake -B build -DVASYNTH_ENABLE_LTO=OFF
option(VASYNTH_ENABLE_LTO "Enable JUCE recommended link-time optimisation" ON)

target_link_libraries(VASynth PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
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
