#
# Schwung as a VST3 / AU plugin.
#
#   cmake -B build/plugin -S desktop -DJUCE_DIR=/path/to/JUCE
#   cmake --build build/plugin --config Release
#
# Live hosts VST3 and AU, and NOT CLAP -- which is why JUCE is the wrapper
# here rather than clap-first with a wrapper on top.
#
cmake_minimum_required(VERSION 3.22)
project(SchwungPlugin VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(APPLE)
    # The chain host and every module are built for the host architecture
    # only. A universal plugin would need a universal module tree behind it,
    # which is a fleet-wide decision rather than a plugin flag.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
endif()

# --- JUCE ---------------------------------------------------------------
# Not vendored. Point at a checkout with -DJUCE_DIR=... or $JUCE_DIR.
if(NOT DEFINED JUCE_DIR)
    if(DEFINED ENV{JUCE_DIR})
        set(JUCE_DIR "$ENV{JUCE_DIR}")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../libs/juce/CMakeLists.txt")
        set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libs/juce")
    endif()
endif()

if(NOT JUCE_DIR OR NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    # A HARD FAILURE, not a warning that degrades to "plugin not built".
    # A build step that can be skipped silently defeats every bisect after it
    # -- this repo has paid for that lesson twice already (the link-subscriber
    # sidecar and the manager binary).
    message(FATAL_ERROR
        "JUCE not found. Pass -DJUCE_DIR=/path/to/JUCE or set $JUCE_DIR.\n"
        "  git clone --depth 1 https://github.com/juce-framework/JUCE.git")
endif()

add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)

# --- the plugin ---------------------------------------------------------
juce_add_plugin(SchwungPlugin
    PRODUCT_NAME                "Schwung"
    COMPANY_NAME                "Schwung"
    BUNDLE_ID                   "com.schwung.plugin"
    PLUGIN_MANUFACTURER_CODE    Schw
    PLUGIN_CODE                 Schw
    FORMATS                     AU VST3 Standalone
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD     TRUE)

target_sources(SchwungPlugin PRIVATE
    plugin/PluginProcessor.cpp
    plugin/PluginEditor.cpp
    src/schwung_desktop.c)

target_include_directories(SchwungPlugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src")

target_compile_definitions(SchwungPlugin PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(SchwungPlugin PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags)

# --- render test --------------------------------------------------------
# auval and pluginval both PASS on a plugin that renders pure silence, so
# neither can tell a well-formed wrapper from a connected one. This drives the
# real processor at 48 kHz in 512-frame blocks -- i.e. entirely through
# RateBridge -- and fails on silence, NaN or clipping.
juce_add_console_app(schwung-plugin-test PRODUCT_NAME "schwung-plugin-test")

target_sources(schwung-plugin-test PRIVATE tests/plugin_render_test.cpp)

target_include_directories(schwung-plugin-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src")

target_compile_definitions(schwung-plugin-test PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1)

target_link_libraries(schwung-plugin-test PRIVATE
    SchwungPlugin
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_recommended_config_flags)
