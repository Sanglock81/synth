cmake_minimum_required(VERSION 3.22)

project(VASynth VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch JUCE. Pin the tag so builds are reproducible; bump deliberately.
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# One target definition produces BOTH the VST3 and the standalone app.
juce_add_plugin(VASynth
    COMPANY_NAME            "HomeStudio"          # rename freely
    PLUGIN_MANUFACTURER_CODE Hstd
    PLUGIN_CODE              Vasy
    IS_SYNTH                 TRUE
    NEEDS_MIDI_INPUT         TRUE
    NEEDS_MIDI_OUTPUT        FALSE
    IS_MIDI_EFFECT           FALSE
    COPY_PLUGIN_AFTER_BUILD  TRUE                  # installs VST3 to user plugin dir
    FORMATS                  VST3 Standalone
    PRODUCT_NAME             "VA Synth")

target_sources(VASynth PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(VASynth PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    # Standalone: allow runtime selection of audio device (Scarlett 2i2)
    # and enabling multiple MIDI inputs (Korg B2 + Launchkey Mini).
    JUCE_ALSA=1
    JUCE_JACK=1)

target_link_libraries(VASynth PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
