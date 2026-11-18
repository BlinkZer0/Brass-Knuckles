#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace bk;

BrassKnucklesProcessor::BrassKnucklesProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "BrassKnucklesState", createParameterLayout())
{
    for (int i = 0; i < kNumParams; ++i)
    {
        const auto& id = paramDefs()[(size_t) i].id;
        params[(size_t) i] = apvts.getParameter (id);
        rawValues[(size_t) i] = apvts.getRawParameterValue (id);
        jassert (params[(size_t) i] != nullptr && rawValues[(size_t) i] != nullptr);
    }
    setCurrentProgram (0); // a fresh instance starts on the first factory preset
}

//==============================================================================
void BrassKnucklesProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate);
    keyboardState.reset();
}

bool BrassKnucklesProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

EngineParams BrassKnucklesProcessor::readParams() const
{
    return makeEngineParams ([this] (int i) { return paramValue (i); });
}

void BrassKnucklesProcessor::handleMidi (const juce::MidiMessage& m)
{
    const int ch = juce::jmax (0, m.getChannel() - 1);
    if (m.isNoteOn())                  engine.noteOn (m.getNoteNumber(), m.getVelocity(), ch);
    else if (m.isNoteOff())            engine.noteOff (m.getNoteNumber(), ch);
    else if (m.isPitchWheel())         engine.setPitchBend ((m.getPitchWheelValue() - 8192) / 8192.0, ch);
    else if (m.isChannelPressure())    engine.setChannelPressure (m.getChannelPressureValue() / 127.0, ch);
    else if (m.isAftertouch())         engine.setPolyPressure (m.getNoteNumber(), m.getAfterTouchValue() / 127.0, ch);
    else if (m.isAllSoundOff())        engine.panic();
    else if (m.isAllNotesOff())        engine.allNotesOff();
    else if (m.isController())         engine.setController (m.getControllerNumber(), m.getControllerValue() / 127.0, ch);
}

void BrassKnucklesProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();

    engine.setParams (readParams());

    if (panicRequested.exchange (false))
        engine.panic();
    if (const float w = uiModWheel.exchange (-1.0f); w >= 0.0f)   // mod wheel dragged in the UI
        engine.setModWheel (w);
    if (const float b = uiBreath.exchange (-1.0f); b >= 0.0f)     // breath wheel dragged in the UI
        engine.setController (juce::roundToInt (paramValue (dynBreathCC)), b, 0);
    else if (b < -1.5f)                                            // breath wheel double-clicked
        engine.forgetBreath();

    // Merges notes played on the UI keyboard into `midi`, and tracks all
    // sounding notes so the editor can light up the keys.
    keyboardState.processNextMidiBuffer (midi, 0, n, true);

    // Host tempo + song position. No play head -> 120 BPM.
    double bpm = 120.0, ppq = 0.0;
    bool playing = false;
    if (auto* ph = getPlayHead())
    {
        if (const auto pos = ph->getPosition())
        {
            if (const auto b = pos->getBpm()) bpm = *b;
            if (const auto q = pos->getPpqPosition()) ppq = *q;
            playing = pos->getIsPlaying();
        }
    }
    engine.setTransport (bpm, ppq, playing);

    float* L = buffer.getWritePointer (0);
    float* R = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

    // Sample-accurate: render up to each MIDI event, then apply it.
    int pos = 0;
    for (const auto meta : midi)
    {
        const int t = juce::jlimit (0, n, meta.samplePosition);
        if (t > pos)
        {
            engine.render (L + pos, R != nullptr ? R + pos : nullptr, t - pos);
            pos = t;
        }
        const auto msg = meta.getMessage();
        handleMidi (msg);
        if (msg.isPitchWheel()) statBend.store ((float) ((msg.getPitchWheelValue() - 8192) / 8192.0));
    }
    if (pos < n)
        engine.render (L + pos, R != nullptr ? R + pos : nullptr, n - pos);

    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, n);

    statVoices.store (engine.activeVoices());
    statSustain.store (engine.sustainDown());
    statModWheel.store ((float) engine.modWheel());
    statBpm.store ((float) engine.tempo());
    statBreath.store ((float) engine.breathValue());
    statExpression.store ((float) engine.expressionValue());
    statAftertouch.store ((float) engine.aftertouchValue());
    statDynamics.store ((float) engine.lastDynamics());
    statPlunger.store ((float) engine.plungerPosition());
    const float pk = engine.takePeak();
    if (pk > statPeak.load()) statPeak.store (pk);
}

//==============================================================================
// Sets every parameter except master. Anything the preset doesn't specify
// (e.g. an older file saved before a parameter existed) goes to its default.
void BrassKnucklesProcessor::applyPreset (const PresetData& preset)
{
    for (int i = 0; i < kNumParams; ++i)
    {
        if (i == master || paramDefs()[(size_t) i].noPreset) continue;
        const float stored = preset.values[(size_t) i];
        const float v = std::isnan (stored) ? paramDefs()[(size_t) i].def : stored;
        auto* p = params[(size_t) i];
        const float norm = p->convertTo0to1 (v);
        if (std::abs (p->getValue() - norm) < 1e-7f) continue; // unchanged: don't spam the host
        p->beginChangeGesture();
        p->setValueNotifyingHost (norm);
        p->endChangeGesture();
    }
}

void BrassKnucklesProcessor::setPresetInfo (const juce::String& id, const juce::String& name, const juce::String& category)
{
    {
        const juce::ScopedLock sl (presetLock);
        presetInfo = { id, name, category };
    }
    ++presetVersion;
}

BrassKnucklesProcessor::PresetInfo BrassKnucklesProcessor::currentPreset() const
{
    const juce::ScopedLock sl (presetLock);
    return presetInfo;
}

void BrassKnucklesProcessor::setCurrentProgram (int index)
{
    index = juce::jlimit (0, getNumPrograms() - 1, index);
    currentProgram = index;
    const auto& preset = presetManager.factory()[(size_t) index];
    applyPreset (preset);
    setPresetInfo ("factory:" + juce::String (index), preset.name, preset.category);
    updateHostDisplay (ChangeDetails().withProgramChanged (true));
}

const juce::String BrassKnucklesProcessor::getProgramName (int index)
{
    if (index < 0 || index >= getNumPrograms()) return {};
    const auto& p = presetManager.factory()[(size_t) index];
    return p.category + " - " + p.name;
}

bool BrassKnucklesProcessor::loadPresetById (const juce::String& id, juce::String& error)
{
    if (id.startsWith ("factory:"))
    {
        const int index = id.fromFirstOccurrenceOf (":", false, false).getIntValue();
        if (index < 0 || index >= getNumPrograms()) { error = "Unknown preset"; return false; }
        setCurrentProgram (index);
        return true;
    }

    presetManager.rescanIfChanged();
    if (const auto* entry = presetManager.findUser (id))
    {
        PresetData data;
        if (! PresetManager::readFile (entry->file, data, error)) return false;
        applyPreset (data);
        setPresetInfo (entry->id, entry->name, entry->category);
        return true;
    }
    error = "Preset not found (was it moved or deleted?)";
    return false;
}

bool BrassKnucklesProcessor::loadPresetFile (const juce::File& file, juce::String& error)
{
    PresetData data;
    if (! PresetManager::readFile (file, data, error)) return false;
    applyPreset (data);
    const auto userId = presetManager.idForFile (file);
    if (userId.isNotEmpty())
        setPresetInfo (userId, file.getFileNameWithoutExtension(), presetManager.categoryForFile (file));
    else
        setPresetInfo ("file:" + file.getFullPathName(), data.name, "File");
    return true;
}

bool BrassKnucklesProcessor::saveUserPreset (const juce::String& rawName, bool overwrite, bool& alreadyExists, juce::String& error)
{
    alreadyExists = false;
    const auto name = rawName.trim();
    if (name.isEmpty()) { error = "Enter a name for the preset"; return false; }

    const auto file = presetManager.fileForName (name);
    if (file.existsAsFile() && ! overwrite) { alreadyExists = true; return false; }

    PresetData data;
    data.name = name;
    data.category = "User";
    for (int i = 0; i < kNumParams; ++i)
        if (i != master && ! paramDefs()[(size_t) i].noPreset) data.values[(size_t) i] = paramValue (i);

    if (! PresetManager::writeFile (file, data, error)) return false;
    presetManager.rescanIfChanged();
    setPresetInfo (presetManager.idForFile (file), file.getFileNameWithoutExtension(), presetManager.categoryForFile (file));
    return true;
}

bool BrassKnucklesProcessor::deleteUserPreset (const juce::String& id, juce::String& error)
{
    presetManager.rescanIfChanged();
    const auto* entry = presetManager.findUser (id);
    if (entry == nullptr) { error = "Only user presets can be deleted"; return false; }
    const auto file = entry->file;
    if (! file.moveToTrash() && ! file.deleteFile()) { error = "Couldn't delete " + file.getFullPathName(); return false; }
    presetManager.rescanIfChanged();
    if (currentPreset().id == id)                       // keep the sound, drop the link to the file
        setPresetInfo ({}, currentPreset().name, "User");
    return true;
}

//==============================================================================
void BrassKnucklesProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    const auto info = currentPreset();
    state.setProperty ("program", currentProgram.load(), nullptr);
    state.setProperty ("presetId", info.id, nullptr);
    state.setProperty ("presetName", info.name, nullptr);
    state.setProperty ("presetCategory", info.category, nullptr);
    state.setProperty ("stateVersion", kPresetVersion, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void BrassKnucklesProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto state = juce::ValueTree::fromXml (*xml);
            currentProgram = juce::jlimit (0, getNumPrograms() - 1, (int) state.getProperty ("program", 0));
            const auto name = state.getProperty ("presetName").toString();
            if (name.isEmpty())
            {
                const auto& p = presetManager.factory()[(size_t) currentProgram.load()];
                setPresetInfo ("factory:" + juce::String (currentProgram.load()), p.name, p.category);
            }
            else
            {
                setPresetInfo (state.getProperty ("presetId").toString(), name,
                               state.getProperty ("presetCategory").toString());
            }
            apvts.replaceState (state);
        }
    }
}

//==============================================================================
juce::AudioProcessorEditor* BrassKnucklesProcessor::createEditor()
{
    return new BrassKnucklesEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new BrassKnucklesProcessor();
}
