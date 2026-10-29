cmake_minimum_required(VERSION 3.22)

project(AtomicDAW VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ------------------------------------------------------------------------------
# JUCE
# ------------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE)

FetchContent_MakeAvailable(JUCE)

# ------------------------------------------------------------------------------
# Application target
# ------------------------------------------------------------------------------
juce_add_gui_app(AtomicDAW
    PRODUCT_NAME  "AtomicDAW"
    COMPANY_NAME  "PLSM Studio"
    BUNDLE_ID     "com.plsm.atomicdaw")

juce_generate_juce_header(AtomicDAW)

target_sources(AtomicDAW PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/SelfTest.cpp
    Source/Audio/AudioEngine.cpp
    Source/Audio/MidiInputHub.cpp
    Source/Audio/Mp3Encoder.cpp
    Source/Audio/PlaybackEngine.cpp
    Source/Audio/PluginHost.cpp
    Source/Audio/Recorder.cpp
    Source/Audio/ScopeRender.cpp
    Source/Audio/AudioExport.cpp
    Source/Media/SampleLibrary.cpp
    Source/Model/AutomationLane.cpp
    Source/Model/AutomationModel.cpp
    Source/Model/TempoMap.cpp
    Source/Model/TempoModel.cpp
    Source/Model/HardwareRack.cpp
    Source/Model/LayerModel.cpp
    Source/Model/MixerModel.cpp
    Source/Model/NoteEnvelope.cpp
    Source/Model/ProjectIO.cpp
    Source/UI/AutomationLaneComponent.cpp
    Source/UI/AutomationWindow.cpp
    Source/UI/BpmField.cpp
    Source/UI/EffectWindows.cpp
    Source/UI/ExportDialog.cpp
    Source/UI/FileBrowserPanel.cpp
    Source/UI/GroupPanel.cpp
    Source/UI/LayerPanel.cpp
    Source/UI/MixerPanel.cpp
    Source/UI/MixerWindow.cpp
    Source/UI/NoteEnvelopePanel.cpp
    Source/UI/PianoRollComponent.cpp
    Source/UI/RackPanel.cpp
    Source/UI/SampleEnvelopePanel.cpp
    Source/UI/NameEditor.cpp
    Source/UI/SettingsWindow.cpp)

target_include_directories(AtomicDAW PRIVATE Source)

# Phase 52: MP3 export goes through the Windows Media Foundation sink writer —
# it is part of the OS, so there is no encoder to bundle. These are the three
# libraries that hold MFStartup, the sink writer, and the format GUIDs.
if (WIN32)
    target_link_libraries(AtomicDAW PRIVATE mfplat mfreadwrite mfuuid)
endif()

# Source files are UTF-8 without BOM. Without /utf-8 MSVC decodes them using the
# system code page (936 here) and every Chinese UI string turns into mojibake.
if (MSVC)
    target_compile_options(AtomicDAW PRIVATE /utf-8)

    # Phase 24: 8 MB of stack instead of the 1 MB default.
    #
    # The headless self-test is one very large function and every local in it
    # lives in the same frame; by Phase 24 that frame was within a few kilobytes
    # of the limit, and the failure mode was spectacular — a stack overflow
    # reported inside the NVIDIA driver, several checks before the end, with
    # nothing written. The new blocks are lambdas so their locals are released,
    # but the reserve is the real fix: "add a check" must not be able to crash
    # the harness in a module nobody recognises.
    #
    # Phase 25 raised it again: the optimiser inlines an immediately-invoked
    # [&] lambda, so its locals land in the SAME frame, and the console checks
    # add a MixerModel (128 x 8 insert settings) and a PlaybackEngine (128 insert
    # chains) each. 8 MB overflowed at function ENTRY, before the first check
    # could even print. 64 MB is a reserve, not a commit: Windows only backs the
    # pages that are actually touched, so this costs nothing at run time.
    target_link_options(AtomicDAW PRIVATE /STACK:67108864)
endif()

target_compile_definitions(AtomicDAW PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    # Phase 9: real plugin hosting. VST3 only — it is the one format that is
    # self-contained on all three platforms, and turning the other hosts on
    # would pull in the VST2 SDK headers, which are not redistributable.
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_AU=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:AtomicDAW,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:AtomicDAW,JUCE_VERSION>")

target_link_libraries(AtomicDAW
    PRIVATE
        juce::juce_audio_devices
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# The VST3 scanner inspects plugins in a SEPARATE process (juce_vst3_helper).
# If the helper is not next to the executable JUCE silently falls back to
# loading every plugin inside the DAW instead — a synth that writes logs to the
# desktop or crashes on load then does it to us. Copy it to the exe folder.
add_dependencies(AtomicDAW juce_vst3_helper)

add_custom_command(TARGET AtomicDAW POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:juce_vst3_helper>
            $<TARGET_FILE_DIR:AtomicDAW>
    COMMENT "Copying juce_vst3_helper next to AtomicDAW")

# ------------------------------------------------------------------------------
# Two throwaway VST3s, built so the self-test has a real plugin to host.
#
# Without them "VST3 support" would be untestable on any machine that happens to
# have no plugins installed — which is exactly the machine this is written on.
# They are not installed and never shipped: COPY_PLUGIN_AFTER_BUILD is off, so
# they stay in their own artefact folders and only the self-test points at them.
# ------------------------------------------------------------------------------
# A synth needs MIDI in; VST3 takes the "is a synth" flag from this.
juce_add_plugin(AtomicTestSynth
    FORMATS                  VST3
    IS_SYNTH                 TRUE
    NEEDS_MIDI_INPUT         TRUE
    NEEDS_MIDI_OUTPUT        FALSE
    COPY_PLUGIN_AFTER_BUILD  FALSE
    COMPANY_NAME             "PLSM Studio"
    PLUGIN_MANUFACTURER_CODE Plsm
    PLUGIN_CODE              Atsn
    PRODUCT_NAME             "Atomic Test Synth")

juce_add_plugin(AtomicTestFX
    FORMATS                  VST3
    IS_SYNTH                 FALSE
    NEEDS_MIDI_INPUT         FALSE
    NEEDS_MIDI_OUTPUT        FALSE
    COPY_PLUGIN_AFTER_BUILD  FALSE
    COMPANY_NAME             "PLSM Studio"
    PLUGIN_MANUFACTURER_CODE Plsm
    PLUGIN_CODE              Atfx
    PRODUCT_NAME             "Atomic Test FX")

target_sources(AtomicTestSynth PRIVATE Tools/TestPlugins/TestSynth.cpp)
target_sources(AtomicTestFX    PRIVATE Tools/TestPlugins/TestFX.cpp)

if (MSVC)
    target_compile_options(AtomicTestSynth PRIVATE /utf-8)
    target_compile_options(AtomicTestFX    PRIVATE /utf-8)
endif()

target_compile_definitions(AtomicTestSynth PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_compile_definitions(AtomicTestFX    PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)

target_link_libraries(AtomicTestSynth PRIVATE juce::juce_audio_utils
                                     PUBLIC  juce::juce_recommended_config_flags
                                             juce::juce_recommended_warning_flags)
target_link_libraries(AtomicTestFX    PRIVATE juce::juce_audio_utils
                                     PUBLIC  juce::juce_recommended_config_flags
                                             juce::juce_recommended_warning_flags)
