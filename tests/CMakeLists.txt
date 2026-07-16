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
    dsp/test_kit.cpp
    dsp/test_multitimbral.cpp
    dsp/test_smoothing.cpp
    dsp/test_mono_glide.cpp
    dsp/test_osc6a.cpp
    dsp/test_fx.cpp
    dsp/test_fx_golden.cpp
    dsp/test_golden.cpp
    dsp/test_softclip.cpp
    dsp/test_eq.cpp
    dsp/test_arp.cpp
    dsp/test_stepseq.cpp
    dsp/test_clock_align.cpp
    dsp/test_looper.cpp
    dsp/test_voice_steal.cpp
    dsp/test_audioloop.cpp
    dsp/test_chord.cpp
    dsp/test_rt_safety.cpp
    dsp/test_rtlogring.cpp
    dsp/alloc_hook.cpp)

find_package(Threads REQUIRED)
target_include_directories(dsp_tests PRIVATE ${CMAKE_SOURCE_DIR}/Source/DSP)
target_compile_features(dsp_tests PRIVATE cxx_std_17)
target_link_libraries(dsp_tests PRIVATE Catch2::Catch2WithMain Threads::Threads)

# Where committed golden reference files live.
target_compile_definitions(dsp_tests PRIVATE
    VASYNTH_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

catch_discover_tests(dsp_tests)

# --- performance benchmark (not a CTest gate; wall time is machine-specific) -
add_executable(dsp_bench bench/bench_engine.cpp)
target_include_directories(dsp_bench PRIVATE ${CMAKE_SOURCE_DIR}/Source/DSP)
target_compile_features(dsp_bench PRIVATE cxx_std_17)

# --- Plugin-layer tests (need JUCE + the processor) -------------------------
add_executable(plugin_tests
    plugin/test_state.cpp
    plugin/test_midi_learn.cpp
    plugin/test_output_bounds.cpp
    plugin/test_device_curation.cpp
    plugin/test_input_reliability.cpp
    plugin/test_touch.cpp
    plugin/test_arp_ui.cpp
    plugin/test_layout_mockup.cpp
    plugin/test_drums.cpp
    plugin/test_chord_plugin.cpp
    plugin/test_parts.cpp
    plugin/test_rt_safety.cpp
    plugin/test_smoothing.cpp
    plugin/test_performance_midi.cpp
    plugin/test_audio_device.cpp
    plugin/test_qwerty.cpp
    plugin/test_health_logger.cpp
    plugin/test_editor.cpp
    plugin/test_presets.cpp
    plugin/test_migration.cpp
    plugin/test_fx_plugin.cpp
    plugin/test_midi_profile.cpp
    plugin/test_midi_hotplug.cpp
    plugin/test_presets_factory.cpp
    plugin/test_macros.cpp
    plugin/test_rhythm.cpp
    plugin/test_stuck_notes.cpp
    plugin/test_seq_multitimbral.cpp
    plugin/test_looper_stuck.cpp
    plugin/test_kit_edit.cpp
    plugin/test_voice_isolation.cpp
    plugin/test_load_isolation.cpp
    plugin/test_seq_drives_kit.cpp
    plugin/test_default_scene.cpp
    plugin/test_fx_topology.cpp
    plugin/test_kick_clean.cpp
    plugin/test_scoping_audit.cpp
    plugin/test_looper_focus.cpp
    plugin/test_perpart_mono.cpp
    plugin/test_click_torture.cpp
    plugin/test_editfocus.cpp
    dsp/alloc_hook.cpp)                # global-new counter, shared with dsp_tests

target_include_directories(plugin_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Source
    ${CMAKE_CURRENT_SOURCE_DIR}/dsp)   # for alloc_hook.h
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
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    VASYNTH_DOCS_DIR="${CMAKE_SOURCE_DIR}/docs")

catch_discover_tests(plugin_tests)

# --- soak: memory stability under a MIDI storm (built with the sanitizers) ---
add_executable(soak soak/soak_main.cpp)
target_include_directories(soak PRIVATE ${CMAKE_SOURCE_DIR}/Source)
target_link_libraries(soak PRIVATE
    VASynth
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
target_compile_definitions(soak PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)

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
                     --validate "${CMAKE_BINARY_DIR}/VASynth_artefacts/$<CONFIG>/VST3/synth.vst3")
    set_tests_properties(pluginval_vst3 PROPERTIES TIMEOUT 300)
    message(STATUS "pluginval found: ${PLUGINVAL_EXECUTABLE} (CTest 'pluginval_vst3' enabled)")
else()
    message(WARNING "pluginval not found; the 'pluginval_vst3' CTest will be skipped. "
                    "run-all-checks.sh fetches it automatically.")
endif()
