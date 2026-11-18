#pragma once

#include <bitset>
#include "PluginProcessor.h"

// Hosts the HTML/SVG UI (vst/ui, compiled in as BinaryData) in a WebView2 and
// bridges it to the processor:
//   UI -> C++ events : uiReady, setParam{id,value}, gesture{id,begin},
//                      noteOn{note,vel}, noteOff{note}, panic, modWheel{value}, breath{value},
//                      loadPreset{id}, savePreset{name,overwrite}, deletePreset{id},
//                      loadPresetFile, openPresetFolder, paramMenu{id,x,y}
//   C++ -> UI events : params{id:value...}, notes{on[],off[]},
//                      stats{voices,peak,sustain,modWheel,bpm,breath,expression,aftertouch,dynamics,bend},
//                      config{...}, presetList{presets[],folder}, preset{id,name,category},
//                      saveResult{ok,exists,name,error}, message{text}
class BrassKnucklesEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit BrassKnucklesEditor (BrassKnucklesProcessor&);
    ~BrassKnucklesEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    juce::WebBrowserComponent::Options makeBrowserOptions();
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);

    int indexForId (const juce::String& id) const;
    void onUiReady();
    void onSetParam (const juce::var&);
    void onGesture (const juce::var&);
    void sendConfig();
    void sendPresetList();
    void sendPreset();
    void sendMessage (const juce::String& text);
    void chooseAndLoadPresetFile();
    void showParamMenu (const juce::var&);

    BrassKnucklesProcessor& processor;

    std::array<float, bk::kNumParams> lastSent {};
    std::array<bool, bk::kNumParams> inGesture {};
    std::bitset<128> lastNotes;
    int lastPresetVersion = -1;
    int ticks = 0;
    bool uiReady = false;
    std::unique_ptr<juce::FileChooser> fileChooser;

    juce::WebBrowserComponent browser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BrassKnucklesEditor)
};
