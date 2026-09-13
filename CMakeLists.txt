cmake_minimum_required(VERSION 3.22)
project(PQ_Perfection_of_Match_EQ VERSION 1.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    juce
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.9.zip
)
FetchContent_MakeAvailable(juce)

juce_add_plugin(PQ
    COMPANY_NAME "POURIA"
    BUNDLE_ID "com.pouria.pq"
    PLUGIN_MANUFACTURER_CODE Pour
    PLUGIN_CODE PqEq
    FORMATS VST3
    PRODUCT_NAME "PQ - Perfection of Match EQ"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)

target_sources(PQ PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)

target_compile_definitions(PQ PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(PQ PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
)

if (MINGW)
    target_compile_options(PQ PRIVATE -Wall -Wextra -Wpedantic)
endif()
