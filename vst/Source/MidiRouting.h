#pragma once
// How a MIDI message reaches the engine. Shared by the plugin (PluginProcessor) and the
// parity tool, and mirrored by SynthProcessor.midi() in vst/webhost/dsp-worklet.js.
#include <juce_audio_basics/juce_audio_basics.h>
#include "SynthEngine.h"

namespace bk
{
inline void routeMidi (SynthEngine& engine, const juce::MidiMessage& m)
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
} // namespace bk
