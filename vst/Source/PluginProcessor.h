#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "Params.h"
#include "PresetManager.h"
#include "SynthEngine.h"
#include "EngineParamsFromValues.h"

class BrassKnucklesProcessor : public juce::AudioProcessor
{
public:
    BrassKnucklesProcessor();
    ~BrassKnucklesProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    // The factory presets are exposed to the host as programs.
    int getNumPrograms() override { return (int) presetManager.factory().size(); }
    int getCurrentProgram() override { return currentProgram.load(); }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Presets (message thread)
    struct PresetInfo { juce::String id, name, category; };
    PresetInfo currentPreset() const;
    std::atomic<int> presetVersion { 0 };   // bumps whenever currentPreset() changes

    bool loadPresetById (const juce::String& id, juce::String& error);
    bool loadPresetFile (const juce::File& file, juce::String& error);
    bool saveUserPreset (const juce::String& name, bool overwrite, bool& alreadyExists, juce::String& error);
    bool deleteUserPreset (const juce::String& id, juce::String& error);

    bk::PresetManager presetManager;

    //==============================================================================
    // Used by the editor (message thread)
    juce::AudioProcessorValueTreeState apvts;
    juce::MidiKeyboardState keyboardState;   // UI notes in, all sounding notes out
    juce::RangedAudioParameter* param (int index) const { return params[(size_t) index]; }
    float paramValue (int index) const { return rawValues[(size_t) index]->load(); }
    void requestPanic() { panicRequested = true; }
    void setUiModWheel (float v) { uiModWheel = juce::jlimit (0.0f, 1.0f, v); }
    // 0..1 = on-screen breath wheel; negative = forget the breath controller (velocity plays again)
    void setUiBreath (float v)   { uiBreath = v < 0.0f ? -2.0f : juce::jlimit (0.0f, 1.0f, v); }

    std::atomic<int>   statVoices { 0 };
    std::atomic<float> statPeak { 0.0f };
    std::atomic<bool>  statSustain { false };
    std::atomic<float> statModWheel { 0.0f };
    std::atomic<float> statBpm { 120.0f };
    std::atomic<float> statBreath { -1.0f };      // -1 = no breath controller seen yet
    std::atomic<float> statExpression { -1.0f };
    std::atomic<float> statAftertouch { -1.0f };
    std::atomic<float> statDynamics { 0.0f };
    std::atomic<float> statBend { 0.0f };
    std::atomic<float> statPlunger { 0.5f };

private:
    void applyPreset (const bk::PresetData&);
    void setPresetInfo (const juce::String& id, const juce::String& name, const juce::String& category);
    bk::EngineParams readParams() const;
    void handleMidi (const juce::MidiMessage&);

    std::array<juce::RangedAudioParameter*, bk::kNumParams> params {};
    std::array<std::atomic<float>*, bk::kNumParams> rawValues {};
    bk::SynthEngine engine;
    std::atomic<int> currentProgram { 0 };
    std::atomic<bool> panicRequested { false };
    std::atomic<float> uiModWheel { -1.0f };   // -1 = no pending change
    std::atomic<float> uiBreath { -1.0f };

    mutable juce::CriticalSection presetLock;
    PresetInfo presetInfo;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BrassKnucklesProcessor)
};
