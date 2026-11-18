cmake_minimum_required(VERSION 3.22)

project(BrassKnucklesVST VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# JUCE: use a local checkout if BK_JUCE_DIR is given, otherwise fetch it.
# (build.bat passes E-Piano's existing download when there is one.)
# ---------------------------------------------------------------------------
set(BK_JUCE_DIR "" CACHE PATH "Optional path to a local JUCE checkout")
set(BK_JUCE_TAG "8.0.17-security" CACHE STRING "JUCE git tag to fetch")

if(BK_JUCE_DIR AND EXISTS "${BK_JUCE_DIR}/CMakeLists.txt")
    add_subdirectory("${BK_JUCE_DIR}" JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        ${BK_JUCE_TAG}
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# ---------------------------------------------------------------------------
# Plugin
# ---------------------------------------------------------------------------
juce_add_plugin(BrassKnucklesVST
    COMPANY_NAME            "BrassKnuckles"
    PRODUCT_NAME            "Brass Knuckles"
    PLUGIN_MANUFACTURER_CODE Bknk
    PLUGIN_CODE             Bk01
    FORMATS                 VST3 Standalone
    IS_SYNTH                TRUE
    NEEDS_MIDI_INPUT        TRUE
    NEEDS_MIDI_OUTPUT       FALSE
    IS_MIDI_EFFECT          FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES         Instrument Synth
    NEEDS_WEBVIEW2          TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE)

# The UI (HTML/CSS/JS) and the factory presets are compiled into the binary.
file(GLOB_RECURSE BK_UI_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.css"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.js")
juce_add_binary_data(BrassKnucklesUI SOURCES ${BK_UI_FILES} presets/factory_presets.json)

target_sources(BrassKnucklesVST PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/PresetManager.cpp)

target_include_directories(BrassKnucklesVST PRIVATE Source)

target_compile_definitions(BrassKnucklesVST PUBLIC
    JUCE_WEB_BROWSER=1
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(BrassKnucklesVST
    PRIVATE
        BrassKnucklesUI
        juce::juce_audio_utils
        juce::juce_gui_extra
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Console tests + engine tools:  --target BrassKnucklesTests
# ---------------------------------------------------------------------------
juce_add_console_app(BrassKnucklesTests PRODUCT_NAME "BrassKnucklesTests")
target_sources(BrassKnucklesTests PRIVATE tests/PresetTests.cpp tests/EngineTools.cpp tests/HostCheck.cpp Source/PresetManager.cpp)
target_include_directories(BrassKnucklesTests PRIVATE Source)
# JUCE_PLUGINHOST_VST3: --host-check loads the built .vst3 like a DAW would
target_compile_definitions(BrassKnucklesTests PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_PLUGINHOST_VST3=1)
target_link_libraries(BrassKnucklesTests PRIVATE BrassKnucklesUI juce::juce_audio_processors juce::juce_audio_formats juce::juce_gui_basics
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
