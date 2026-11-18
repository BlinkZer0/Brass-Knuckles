// Console tests (no audio device / window needed).
// Build:  cmake --build %LOCALAPPDATA%\BrassKnucklesVST\build --config Release --target BrassKnucklesTests
// Run without arguments for the full test suite; see EngineTools.cpp for the tool commands.
#include "TestRender.h"

using namespace bk;
using namespace bk::test;

static int failures = 0;
#define CHECK(cond) do { if (! (cond)) { ++failures; std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

int runDumpParams();
int runRender (const juce::String&, const juce::File&, const juce::String&);
int runRenderAll (const juce::File&);
int runLoudness();
int runBench();
int runBrassChecks (bool verbose);
int runHostCheck (const juce::String& path);
int runAliasCheck (bool verbose);

static void testParamTable()
{
    const auto& defs = paramDefs();
    CHECK ((int) defs.size() == kNumParams);
    std::printf ("parameters: %d\n", kNumParams);
    juce::StringArray ids;
    for (const auto& d : defs)
    {
        CHECK (! ids.contains (d.id));
        ids.add (d.id);
        CHECK (d.min < d.max);
        CHECK (d.def >= d.min && d.def <= d.max);
        if (d.kind == Kind::Choice) CHECK (d.choices.size() == (int) d.max + 1);
        if (d.logSkew) CHECK (d.min > 0);
    }
    // index helpers line up with the table
    CHECK (defs[(size_t) oscParam (2, oTEnvKeyTime)].id == "osc3TEnvKeyTime");
    CHECK (defs[(size_t) oscParam (1, oAmpAtk)].id == "osc2AmpAtk");
    CHECK (defs[(size_t) bandParam (2, 4)].id == "addHiLvl");
    CHECK (defs[(size_t) lfoParam (1, 8)].id == "lfo2Amount");
    CHECK (defs[(size_t) modParam (11, 4)].id == "mod12Amt");
    CHECK (defs[(size_t) stWidth].id == "stWidth");
    CHECK (defs[(size_t) dynBreathCC].id == "dynBreathCC" && defs[(size_t) vibWheel].id == "vibWheel");
    CHECK (defs[(size_t) master].noPreset);
}

static void testFactoryPresets (const PresetManager& pm)
{
    const auto& f = pm.factory();
    std::printf ("factory presets: %d\n", (int) f.size());
    CHECK (f.size() >= 12);
    juce::StringArray cats;
    for (const auto& p : f)
    {
        cats.addIfNotAlreadyThere (p.category);
        CHECK (p.name.isNotEmpty());
        for (int i = 0; i < kNumParams; ++i)
        {
            const float v = p.values[(size_t) i];
            if (i == master) { CHECK (std::isnan (v)); continue; }
            if (! std::isnan (v)) CHECK (v >= paramDefs()[(size_t) i].min && v <= paramDefs()[(size_t) i].max);
        }
        CHECK (! std::isnan (p.values[instrument]));   // every preset names its instrument
    }
    std::printf ("categories (%d): %s\n", cats.size(), cats.joinIntoString (", ").toRawUTF8());
    const juce::StringArray expected { "Trumpet", "Flugelhorn", "Trombone", "French Horn", "Tuba/Low Brass", "Soprano Sax",
                                       "Alto Sax", "Tenor Sax", "Baritone Sax", "Sections", "Synth Brass", "Hybrid/FX" };
    CHECK (cats == expected);
    // templates resolved: Lead Trumpet inherits acoustic -> brass
    CHECK (f[0].name == "Lead Trumpet" && f[0].values[voiceMode] == 2.0f && f[0].values[(size_t) oscParam (2, oOn)] == 1.0f
           && f[0].values[(size_t) oscParam (0, oCut)] == 3.0f);
}

static void testPresetFiles (const PresetManager& pm)
{
    const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("bk-test.bkpreset");
    juce::String err;

    // save -> load round trip
    for (const auto& src : pm.factory())
    {
        PresetData out = src;
        out.name = "Round Trip";
        CHECK (PresetManager::writeFile (tmp, out, err));
        PresetData in;
        CHECK (PresetManager::readFile (tmp, in, err));
        CHECK (in.name == "Round Trip");
        for (int i = 0; i < kNumParams; ++i)
        {
            if (i == master) continue;
            const float a = out.values[(size_t) i], b = in.values[(size_t) i];
            if (std::isnan (a)) CHECK (std::isnan (b));
            else CHECK (std::abs (b - a) < 1e-4f * std::max (1.0f, std::abs (a)));
        }
    }
    // the written file names its format and version
    {
        const auto json = juce::JSON::parse (tmp);
        CHECK (json["format"].toString() == "Brass Knuckles Preset" && (int) json["version"] == kPresetVersion);
    }

    // older/partial file (no version): missing params stay NaN (-> defaults), values clamped
    tmp.replaceWithText (R"({"name":"Partial","params":{"osc1Shape":3,"fltCutoff":99999,"unknownParam":5}})");
    PresetData partial;
    CHECK (PresetManager::readFile (tmp, partial, err));
    CHECK (partial.values[(size_t) oscParam (0, oShape)] == 3.0f);
    CHECK (partial.values[fltCutoff] == 20000.0f);
    CHECK (std::isnan (partial.values[ampLevel]));

    // master is never taken from a file
    tmp.replaceWithText (R"({"version":1,"params":{"master":0.1,"vibDepth":30}})");
    PresetData m;
    CHECK (PresetManager::readFile (tmp, m, err));
    CHECK (std::isnan (m.values[master]) && m.values[vibDepth] == 30.0f);

    // garbage and empty settings are rejected with a message
    tmp.replaceWithText ("not json");
    PresetData bad;
    CHECK (! PresetManager::readFile (tmp, bad, err) && err.isNotEmpty());
    tmp.replaceWithText (R"({"name":"Empty","params":{}})");
    err.clear();
    CHECK (! PresetManager::readFile (tmp, bad, err) && err.isNotEmpty());
    tmp.deleteFile();
}

// Every factory preset renders: not silent, not slammed into the clipper.
static void testPresetRenders (const PresetManager& pm)
{
    for (const auto& p : pm.factory())
    {
        const auto v = presetValues (p);
        const int n = auditionNote (v);
        std::vector<Event> ev { { sec (0.02), Event::On, n, 110.0 } };
        if (! isMonoPreset (v)) { ev.push_back ({ sec (0.02), Event::On, n + 4, 110.0 }); ev.push_back ({ sec (0.02), Event::On, n + 7, 110.0 }); }
        ev.push_back ({ sec (1.2), Event::Off, n, 0.0 });
        ev.push_back ({ sec (1.2), Event::Off, n + 4, 0.0 });
        ev.push_back ({ sec (1.2), Event::Off, n + 7, 0.0 });
        const auto a = render (v, ev, 4.0);
        const double level = rms (a, sec (0.3), sec (1.1)), pk = peak (a), tail = rms (a, sec (3.7), sec (4.0));
        const bool ok = level > 0.01 && pk < 0.97 && tail < 1e-4;
        if (! ok) ++failures;
        std::printf ("%-4s render %-34s rms %.3f  peak %.3f  tail %.1e\n", ok ? "ok" : "FAIL",
                     (p.category + "/" + p.name).toRawUTF8(), level, pk, tail);
    }
}

// Engine behaviour that every preset relies on.
static void testEngineBasics()
{
    auto v = defaultValues();
    // velocity-only playing: a note sounds and stops
    {
        const auto a = render (v, { { 0, Event::On, 60, 100.0 }, { sec (0.5), Event::Off, 60, 0.0 } }, 1.2);
        CHECK (rms (a, sec (0.1), sec (0.4)) > 0.01);
        CHECK (rms (a, sec (1.0), sec (1.2)) < 1e-5);
    }
    // breath controller seen -> breath 0 is silent, breath sets the level
    {
        const auto a = render (v, { { 0, Event::CC, 2, 0.0 }, { sec (0.01), Event::On, 60, 127.0 },
                                    { sec (0.5), Event::CC, 2, 0.8 } }, 1.0);
        CHECK (rms (a, sec (0.2), sec (0.45)) < 1e-4);
        CHECK (rms (a, sec (0.7), sec (0.95)) > 0.01);
    }
    // reset-all-controllers forgets the breath controller (velocity plays again)
    {
        const auto a = render (v, { { 0, Event::CC, 2, 0.0 }, { sec (0.01), Event::CC, 121, 0.0 },
                                    { sec (0.02), Event::On, 60, 100.0 } }, 0.5);
        CHECK (rms (a, sec (0.2), sec (0.45)) > 0.01);
    }
    // polyphony limit: 40 notes with polyphony 32 -> 32 sounding
    {
        auto ePtr = std::make_unique<SynthEngine>();
        auto& e = *ePtr;
        e.prepare (kSr);
        e.setParams (makeEngineParams ([&v] (int i) { return v[(size_t) i]; }));
        for (int n = 0; n < 40; ++n) e.noteOn (40 + n, 100);
        std::array<float, 256> L {}, R {};
        e.render (L.data(), R.data(), 256);
        CHECK (e.activeVoices() == 32);
    }
    // legato mode: overlapping notes keep one voice and glide (pitch moves over the glide time)
    {
        auto lv = v;
        lv[voiceMode] = 2; lv[glideOn] = 1; lv[glideTime] = 0.2f; lv[(size_t) oscParam (0, oShape)] = 0; // sine
        lv[vibDepth] = 0; lv[vibAmp] = 0;
        const auto a = render (lv, { { 0, Event::On, 60, 100.0 }, { sec (0.5), Event::On, 72, 100.0 } }, 1.2);
        auto pitchHz = [&a] (int from)   // zero-crossing frequency estimate over 50 ms
        {
            int crossings = 0;
            for (int i = from + 1; i < from + sec (0.05); ++i)
                if (a.L[(size_t) i - 1] <= 0 && a.L[(size_t) i] > 0) ++crossings;
            return crossings / 0.05;
        };
        const double before = pitchHz (sec (0.4)), mid = pitchHz (sec (0.58)), after = pitchHz (sec (1.0));
        CHECK (std::abs (before - 261.6) < 25 && std::abs (after - 523.3) < 25 && mid > before + 30 && mid < after - 30);
    }
}

// Energy (sum of power) between lo and hi Hz of the mono mix from `from`.
static double bandEnergy (const Audio& a, int from, double lo, double hi, int n = 8192)
{
    const auto p = powerSpectrum (a, from, n);
    double e = 0;
    for (size_t i = 1; i < p.size(); ++i)
    {
        const double f = (double) i * kSr / n;
        if (f >= lo && f < hi) e += p[i];
    }
    return e;
}

// Phase 2: the modeled engine's parts each do what they claim.
static void testModeledEngine()
{
    // half-band oversampling filters: unity at DC and in the passband, images removed
    for (int factor : { 2, 4 })
    {
        Oversampler os;
        double inPow = 0, outPow = 0;
        for (int i = 0; i < 48000; ++i)
        {
            const double x = 0.5 * std::sin (kTwoPi * 1000.0 * i / kSr);
            // identity "shape" check: tiny input keeps tanhRational linear (error < 1e-6)
            const double y = os.process (x * 1e-3, factor, shapeEdge) / 1.5e-3;
            if (i > 1000) { inPow += x * x; outPow += y * y; }
        }
        CHECK (std::abs (std::sqrt (outPow / inPow) - 1.0) < 0.01);
    }
    {
        const auto& A = halfbandA();
        double dc = 0.5;
        for (int i = 0; i < A.M; ++i) dc += 2 * A.g[i];
        CHECK (std::abs (dc - 1.0) < 1e-12);
    }

    auto base = defaultValues();
    auto set = [] (Values& v, const char* id, float x) { v[(size_t) paramIndex (id)] = x; };
    set (base, "vibDepth", 0); set (base, "vibAmp", 0); set (base, "lipAmt", 0); set (base, "brNoise", 0);
    set (base, "bodyMix", 0); set (base, "brsAmt", 0); set (base, "dynBrass", 0);

    // additive layer alone: sounds, brightens with dynamics, and spectral delay holds the top back
    {
        auto v = base;
        set (v, "osc1On", 0); set (v, "addOn", 1); set (v, "addPartials", 32);
        const auto soft = render (v, { { 0, Event::On, 60, 30.0 } }, 0.8);
        const auto hard = render (v, { { 0, Event::On, 60, 127.0 } }, 0.8);
        CHECK (rms (hard, sec (0.4), sec (0.7)) > 0.01);
        CHECK (centroid (hard, sec (0.4)) > centroid (soft, sec (0.4)) * 1.3);

        auto ratioHigh = [] (const Audio& a, int from) { return bandEnergy (a, from, 1500, 20000, 1024) / bandEnergy (a, from, 100, 1500, 1024); };
        set (v, "addSpecDly", 0);
        const auto even = render (v, { { 0, Event::On, 60, 127.0 } }, 0.6);
        set (v, "addSpecDly", 1);
        const auto staggered = render (v, { { 0, Event::On, 60, 127.0 } }, 0.6);
        // early (10-31 ms): the upper harmonics are much weaker with spectral delay; later they catch up
        CHECK (ratioHigh (staggered, sec (0.01)) < ratioHigh (even, sec (0.01)) * 0.5);
        CHECK (ratioHigh (staggered, sec (0.45)) > ratioHigh (even, sec (0.45)) * 0.5);
    }
    // brassiness adds upper harmonics at high dynamics (source: a dark, nearly sine tone)
    {
        auto v = base;
        set (v, "osc1Cut", 1.2f); set (v, "dynBright", 0);
        const auto clean = render (v, { { 0, Event::On, 60, 127.0 } }, 0.6);
        set (v, "brsAmt", 0.8f); set (v, "dynBrass", 1);
        const auto brassy = render (v, { { 0, Event::On, 60, 127.0 } }, 0.6);
        const double cClean = harmonicCentroid (clean, sec (0.3), noteHz (60)), cBrassy = harmonicCentroid (brassy, sec (0.3), noteHz (60));
        CHECK (cBrassy > cClean * 1.25);   // measured: 3rd harmonic -22.5 dB -> -14 dB re. the fundamental
        CHECK (peak (brassy) < 0.97);
    }
    // straight mute cuts the lows; plunger closed is darker than open
    {
        auto v = base;
        set (v, "osc1Cut", 32);
        const auto open = render (v, { { 0, Event::On, 48, 110.0 } }, 0.6);
        set (v, "muteType", 1);
        const auto straight = render (v, { { 0, Event::On, 48, 110.0 } }, 0.6);
        CHECK (bandEnergy (straight, sec (0.3), 50, 300) < bandEnergy (open, sec (0.3), 50, 300) * 0.3);
        set (v, "muteType", 4); set (v, "muteSrc", 0); set (v, "mutePlunger", 0);
        const auto closed = render (v, { { 0, Event::On, 48, 110.0 } }, 0.6);
        set (v, "mutePlunger", 1);
        const auto opened = render (v, { { 0, Event::On, 48, 110.0 } }, 0.6);
        CHECK (centroid (closed, sec (0.3)) < centroid (opened, sec (0.3)) * 0.6);
    }
    // breath noise adds energy away from the harmonics; reed chiff and key click add attack energy
    {
        auto v = base;
        set (v, "osc1Cut", 1.5f);
        const auto dry = render (v, { { 0, Event::On, 60, 100.0 } }, 0.6);
        set (v, "brNoise", 1);
        const auto airy = render (v, { { 0, Event::On, 60, 100.0 } }, 0.6);
        CHECK (bandEnergy (airy, sec (0.3), 4000, 20000) > bandEnergy (dry, sec (0.3), 4000, 20000) * 4);
        set (v, "brNoise", 0); set (v, "keyClick", 1); set (v, "chiff", 1);
        const auto clicky = render (v, { { 0, Event::On, 60, 100.0 } }, 0.6);
        CHECK (rms (clicky, 0, sec (0.02)) > rms (dry, 0, sec (0.02)) * 1.2);
    }
    // tongue: a hard attack has more onset energy than a soft one; lip buzz decays away
    {
        auto v = base;
        set (v, "lipAmt", 1);
        set (v, "tongue", 0);
        const auto soft = render (v, { { 0, Event::On, 60, 100.0 } }, 0.6);
        set (v, "tongue", 2);
        const auto hard = render (v, { { 0, Event::On, 60, 100.0 } }, 0.6);
        CHECK (rms (hard, 0, sec (0.03)) > rms (soft, 0, sec (0.03)) * 1.1);
        set (v, "lipAmt", 0);
        const auto none = render (v, { { 0, Event::On, 60, 100.0 } }, 0.6);
        CHECK (std::abs (rms (hard, sec (0.4), sec (0.5)) - rms (none, sec (0.4), sec (0.5))) < 0.02 * rms (none, sec (0.4), sec (0.5)));
    }
    // growl: the level wobbles fast (window-to-window spread of 5 ms RMS rises)
    {
        auto spread = [] (const Audio& a)
        {
            double lo = 1e9, hi = 0;
            for (int w = sec (0.3); w < sec (0.6); w += sec (0.005))
            {
                const double r = rms (a, w, w + sec (0.005));
                lo = std::min (lo, r); hi = std::max (hi, r);
            }
            return hi / std::max (lo, 1e-9);
        };
        auto v = base;
        const auto plain = render (v, { { 0, Event::On, 55, 100.0 } }, 0.7);
        set (v, "growlAmt", 1);
        const auto growl = render (v, { { 0, Event::On, 55, 100.0 } }, 0.7);
        CHECK (spread (growl) > spread (plain) * 1.5);
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init; // keeps JUCE leak checks quiet

    const juce::String cmd = argc >= 2 ? juce::String (argv[1]) : juce::String();
    if (cmd == "--dump-params") return runDumpParams();
    if (cmd == "--render" && argc >= 4)
        return runRender (argv[2], juce::File::getCurrentWorkingDirectory().getChildFile (argv[3]), argc >= 5 ? argv[4] : "demo");
    if (cmd == "--render-all" && argc >= 3) return runRenderAll (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
    if (cmd == "--loudness") return runLoudness();
    if (cmd == "--bench") return runBench();
    if (cmd == "--brass") return runBrassChecks (true) + runAliasCheck (true) == 0 ? 0 : 1;
    if (cmd == "--host-check" && argc >= 3) return runHostCheck (argv[2]);

    PresetManager pm;
    testParamTable();
    testFactoryPresets (pm);
    testPresetFiles (pm);
    testEngineBasics();
    testModeledEngine();
    testPresetRenders (pm);
    failures += runBrassChecks (false);
    failures += runAliasCheck (false);

    std::printf ("user preset folder: %s\n", PresetManager::userFolder().getFullPathName().toRawUTF8());
    std::printf (failures == 0 ? "ALL PASSED\n" : "%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
