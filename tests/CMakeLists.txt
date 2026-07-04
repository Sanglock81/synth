# ===========================================================================
# VA Synth test suite.
#
#  * dsp_tests    — links NOTHING but Catch2. Includes only Source/DSP/, which
#                   is standard-library-only by design. If this target ever
#                   needs to link JUCE, that's a design regression.
#  * plugin_tests — links the plugin shared-code target (JUCE). Exercises the
#                   APVTS state round-trip and MIDI-learn.
#
# Both are registered with CTest. pluginval and the golden render run from
# run-all-checks.sh / dedicated CTest entries.
# ===========================================================================

include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# --- JUCE-free DSP tests ----------------------------------------------------
add_executable(dsp_tests
    dsp/test_smoke.cpp
    dsp/test_oscillator.cpp
    dsp/test_filter.cpp
    dsp/test_envelope.cpp
    dsp/test_lfo.cpp
    dsp/test_engine.cpp
    dsp/test_golden.cpp
    dsp/test_rt_safety.cpp
    dsp/alloc_hook.cpp)

target_include_directories(dsp_tests PRIVATE ${CMAKE_SOURCE_DIR}/Source/DSP)
target_compile_features(dsp_tests PRIVATE cxx_std_17)
target_link_libraries(dsp_tests PRIVATE Catch2::Catch2WithMain)

# Where committed golden reference files live.
target_compile_definitions(dsp_tests PRIVATE
    VASYNTH_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

catch_discover_tests(dsp_tests)

# --- Plugin-layer tests (need JUCE + the processor) -------------------------
add_executable(plugin_tests
    plugin/test_state.cpp
    plugin/test_midi_learn.cpp)

target_include_directories(plugin_tests PRIVATE ${CMAKE_SOURCE_DIR}/Source)
target_link_libraries(plugin_tests PRIVATE
    Catch2::Catch2WithMain
    VASynth                       # plugin shared-code (processor/editor object code)
    juce::juce_audio_utils        # JUCE module include dirs + libs (INTERFACE)
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# The shared-code target is compiled with the plugin's JucePlugin_* defines;
# the test TU includes only the processor header, which needs the module paths
# above. Mirror the plugin's compile defs so headers agree.
target_compile_definitions(plugin_tests PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)

catch_discover_tests(plugin_tests)

# --- pluginval integration test ---------------------------------------------
# Validates the built VST3 at strictness 8. run-all-checks.sh fetches pluginval
# into tools/ before configuring, so find_program picks it up here.
find_program(PLUGINVAL_EXECUTABLE NAMES pluginval
             PATHS ${CMAKE_SOURCE_DIR}/tools NO_DEFAULT_PATH)
if (NOT PLUGINVAL_EXECUTABLE)
    find_program(PLUGINVAL_EXECUTABLE NAMES pluginval)
endif()

if (PLUGINVAL_EXECUTABLE)
    add_test(NAME pluginval_vst3
             COMMAND ${PLUGINVAL_EXECUTABLE}
                     --strictness-level 8
                     --validate "${CMAKE_BINARY_DIR}/VASynth_artefacts/$<CONFIG>/VST3/VA Synth.vst3")
    set_tests_properties(pluginval_vst3 PROPERTIES TIMEOUT 300)
    message(STATUS "pluginval found: ${PLUGINVAL_EXECUTABLE} (CTest 'pluginval_vst3' enabled)")
else()
    message(WARNING "pluginval not found; the 'pluginval_vst3' CTest will be skipped. "
                    "run-all-checks.sh fetches it automatically.")
endif()
