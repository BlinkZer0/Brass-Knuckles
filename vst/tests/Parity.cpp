// --parity <scenarios.json>: renders every scenario with the C++ engine and prints the
// results as JSON. tests/run-parity.ps1 renders the same scenarios with the JS engine
// (vst/webhost/dsp-worklet.js) in headless Edge and compares the two.
//
// Scenario format (tests/parity_scenarios.json):
//   { "name": "...", "preset": "Category/Name"?, "params": { id: value }?, "blocks": N, "bpm": 120?,
//     "events": [ [block, type, args...], ... ] }
// Blocks are 128 samples at 48 kHz. Events are applied at the start of their block, after the
// block's parameter snapshot (as in the plugin). Types:
//   on note vel [ch] · off note [ch] · cc num value(0..1) [ch] · bend value(-1..1) [ch] ·
//   pressure value [ch] · poly note value · sustain 0|1 · wheel value · breath value (<0 = forget) ·
//   art index · midi [status, d1, d2] (through the plugin's MIDI routing) · allOff · panic
// Output per scenario: RMS of each channel in 16 equal windows, the peak, and the render time.
#include "TestRender.h"
#include "MidiRouting.h"

using namespace bk;
using namespace bk::test;

namespace
{
constexpr int kWindows = 16;

const PresetData* findFactory (const PresetManager& pm, const juce::String& spec)
{
    for (const auto& p : pm.factory())
        if (p.category + "/" + p.name == spec || p.name == spec) return &p;
    return nullptr;
}

int argInt (const juce::var& ev, int i, int def = 0) { return ev.size() > i ? (int) ev[i] : def; }
double argNum (const juce::var& ev, int i, double def = 0) { return ev.size() > i ? (double) ev[i] : def; }

void applyScenarioEvent (SynthEngine& e, const juce::var& ev, const Values& values)
{
    const auto type = ev[1].toString();
    if (type == "on")            e.noteOn (argInt (ev, 2), argInt (ev, 3), argInt (ev, 4));
    else if (type == "off")      e.noteOff (argInt (ev, 2), argInt (ev, 3));
    else if (type == "cc")       e.setController (argInt (ev, 2), argNum (ev, 3), argInt (ev, 4));
    else if (type == "bend")     e.setPitchBend (argNum (ev, 2), argInt (ev, 3));
    else if (type == "pressure") e.setChannelPressure (argNum (ev, 2), argInt (ev, 3));
    else if (type == "poly")     e.setPolyPressure (argInt (ev, 2), argNum (ev, 3));
    else if (type == "sustain")  e.setSustain (argInt (ev, 2) != 0);
    else if (type == "wheel")    e.setModWheel (argNum (ev, 2));
    else if (type == "art")      e.setArticulation (argInt (ev, 2));
    else if (type == "allOff")   e.allNotesOff();
    else if (type == "panic")    e.panic();
    else if (type == "breath")   // the on-screen BREATH wheel (PluginProcessor::processBlock)
    {
        const double b = argNum (ev, 2);
        if (b >= 0) e.setController ((int) std::lround (values[(size_t) dynBreathCC]), juce::jlimit (0.0, 1.0, b), 0);
        else e.forgetBreath();
    }
    else if (type == "midi")
    {
        const auto& d = ev[2];
        const auto msg = d.size() >= 3 ? juce::MidiMessage ((int) d[0], (int) d[1], (int) d[2])
                                       : juce::MidiMessage ((int) d[0], (int) d[1]);
        routeMidi (e, msg);
    }
    else std::printf ("unknown event type %s\n", type.toRawUTF8());
}
} // namespace

int runParity (const juce::File& scenarioFile)
{
    const auto json = juce::JSON::parse (scenarioFile);
    if (! json.isArray()) { std::printf ("cannot read %s\n", scenarioFile.getFullPathName().toRawUTF8()); return 2; }

    PresetManager pm;
    juce::Array<juce::var> results;

    for (const auto& sc : *json.getArray())
    {
        auto values = defaultValues();
        if (sc.hasProperty ("preset"))
        {
            const auto* p = findFactory (pm, sc["preset"].toString());
            if (p == nullptr) { std::printf ("unknown preset %s\n", sc["preset"].toString().toRawUTF8()); return 2; }
            applyPresetData (values, *p);
        }
        if (auto* obj = sc["params"].getDynamicObject())
            for (const auto& prop : obj->getProperties())
            {
                const int i = paramIndex (prop.name.toString());
                if (i < 0) { std::printf ("unknown param %s\n", prop.name.toString().toRawUTF8()); return 2; }
                values[(size_t) i] = (float) (double) prop.value;
            }

        const int blocks = (int) sc["blocks"];
        const double bpm = sc.hasProperty ("bpm") ? (double) sc["bpm"] : 120.0;
        const auto* events = sc["events"].getArray();
        const auto P = makeEngineParams ([&values] (int i) { return values[(size_t) i]; });

        auto engine = std::make_unique<SynthEngine>();   // fresh per scenario, like the JS runner (too big for the stack)
        engine->prepare (kSr);
        std::array<float, kBlock> L {}, R {};
        const int per = blocks * kBlock / kWindows;
        double sl[kWindows] {}, sr[kWindows] {}, peak = 0;
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        for (int b = 0; b < blocks; ++b)
        {
            engine->setParams (P);
            engine->setTransport (bpm, 0.0, false);
            if (events != nullptr)
                for (const auto& ev : *events)
                    if ((int) ev[0] == b) applyScenarioEvent (*engine, ev, values);
            engine->render (L.data(), R.data(), kBlock);
            for (int i = 0; i < kBlock; ++i)
            {
                const int w = std::min (kWindows - 1, (b * kBlock + i) / std::max (per, 1));
                sl[w] += (double) L[(size_t) i] * L[(size_t) i];
                sr[w] += (double) R[(size_t) i] * R[(size_t) i];
                peak = std::max (peak, (double) std::max (std::abs (L[(size_t) i]), std::abs (R[(size_t) i])));
            }
        }
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;

        juce::Array<juce::var> rl, rr;
        for (int w = 0; w < kWindows; ++w)
        {
            rl.add (std::sqrt (sl[w] / std::max (per, 1)));
            rr.add (std::sqrt (sr[w] / std::max (per, 1)));
        }
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", sc["name"]);
        o->setProperty ("rmsL", rl);
        o->setProperty ("rmsR", rr);
        o->setProperty ("peak", peak);
        o->setProperty ("ms", ms);
        o->setProperty ("audioMs", blocks * kBlock / kSr * 1000.0);
        results.add (juce::var (o));
    }
    std::printf ("%s\n", juce::JSON::toString (juce::var (results), true, 9).toRawUTF8());
    return 0;
}
