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
int runBenchParts();
int runClickChecks (bool verbose);
int runParity (const juce::File& scenarios);
int runHfDump (const juce::String& spec, double t0, double t1);
double presetLoudness (const PresetData&);

// Level matching holds: every factory preset within +-1 dB of the -18 dB target.
static void testLoudness (const PresetManager& pm)
{
    double lo = 0, hi = -100;
    for (const auto& p : pm.factory())
    {
        const double db = 20 * std::log10 (std::max (presetLoudness (p), 1e-9));
        lo = std::min (lo, db); hi = std::max (hi, db);
        if (std::abs (db + 18.0) > 1.0) { ++failures; std::printf ("FAIL loudness %-34s %.2f dB\n", (p.category + "/" + p.name).toRawUTF8(), db); }
    }
    std::printf ("%-4s loudness of %d presets: %.2f .. %.2f dB (target -18 +- 1)\n", lo >= -19 && hi <= -17 ? "ok" : "FAIL",
                 (int) pm.factory().size(), lo, hi);
}

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
    // phase 7 appended after stWidth; Physical appended to the engine choices
    CHECK (defs[(size_t) pmType].id == "pmType" && defs[(size_t) pmNoise].id == "pmNoise" && pmType == stWidth + 1);
    CHECK (defs[(size_t) engineMode].choices.size() == 3 && defs[(size_t) engineMode].choices[2] == "Physical");
}

static void testFactoryPresets (const PresetManager& pm)
{
    const auto& f = pm.factory();
    std::printf ("factory presets: %d\n", (int) f.size());
    CHECK (f.size() == 130);   // 12 x 10 (v1.0) + 10 Physical (phase 7)
    juce::StringArray cats, names;
    std::map<juce::String, int> perCat;
    for (const auto& p : f)
    {
        cats.addIfNotAlreadyThere (p.category);
        ++perCat[p.category];
        CHECK (p.name.isNotEmpty());
        CHECK (! names.contains (p.name));   // names are unique across the whole library
        names.add (p.name);
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
                                       "Alto Sax", "Tenor Sax", "Baritone Sax", "Sections", "Synth Brass", "Hybrid/FX", "Physical" };
    CHECK (cats == expected);
    for (const auto& c : expected) CHECK (perCat[c] == 10);
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
        // release + reverb: wait 1.5 decay times (past -80 dB) before the tail must be silent
        const double len = 4.0 + (v[rvOn] >= 0.5f ? 1.5 * v[rvDecay] : 0.0);
        const auto a = render (v, ev, len);
        const double level = rms (a, sec (0.3), sec (1.1)), pk = peak (a), tail = rms (a, sec (len - 0.3), sec (len));
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

// Pitch (Hz) from rising zero crossings of the left channel over [t, t + win).
static double pitchAt (const Audio& a, double t, double win = 0.03)
{
    int first = -1, last = -1, count = 0;
    for (int i = std::max (1, sec (t)); i < std::min (a.size(), sec (t + win)); ++i)
        if (a.L[(size_t) i - 1] <= 0 && a.L[(size_t) i] > 0)
        {
            if (first < 0) first = i;
            last = i;
            ++count;
        }
    return count >= 2 ? (count - 1) * kSr / (last - first) : 0.0;
}

static double semisBetween (double hz, double ref) { return hz > 0 ? 12 * std::log2 (hz / ref) : -99; }

// Phase 3: articulations, section players, MPE, analog engine.
static void testPerformance()
{
    // a pure sine voice so pitch can be read from zero crossings
    auto base = defaultValues();
    auto set = [] (Values& v, const char* id, float x) { v[(size_t) paramIndex (id)] = x; };
    for (const char* id : { "vibDepth", "vibAmp", "lipAmt", "brNoise", "bodyMix", "brsAmt", "dynBrass", "dynBright" }) set (base, id, 0);
    set (base, "osc1Shape", 0); set (base, "osc1Cut", 64);
    const double c4 = noteHz (60);
    const int ks = 12;   // keyswitch base C0

    // keyswitch notes are silent and latch; FALL bends down after release, then the voice ends
    {
        const auto a = render (base, { { 0, Event::On, ks + 1, 100.0 }, { sec (0.02), Event::Off, ks + 1, 0.0 },
                                       { sec (0.1), Event::On, 60, 100.0 }, { sec (0.5), Event::Off, 60, 0.0 } }, 1.4);
        CHECK (rms (a, 0, sec (0.09)) < 1e-6);
        CHECK (std::abs (semisBetween (pitchAt (a, 0.4), c4)) < 0.3);
        const double during = semisBetween (pitchAt (a, 0.5 + 0.35 * 0.6, 0.06), c4);
        std::printf ("     fall: %.2f st at 60%% of the fall\n", during);
        CHECK (during < -1.5);
        CHECK (rms (a, sec (1.25), sec (1.4)) < 1e-5);
    }
    // DOIT bends up after release
    {
        const auto a = render (base, { { 0, Event::On, ks + 2, 100.0 }, { sec (0.1), Event::On, 60, 100.0 }, { sec (0.5), Event::Off, 60, 0.0 } }, 1.0);
        CHECK (semisBetween (pitchAt (a, 0.5 + 0.3 * 0.6, 0.05), c4) > 1.0);
    }
    // SCOOP starts below and arrives; RIP starts far below in steps
    {
        const auto s = render (base, { { 0, Event::On, ks + 5, 100.0 }, { sec (0.1), Event::On, 60, 100.0 } }, 0.6);
        CHECK (semisBetween (pitchAt (s, 0.1 + 0.015, 0.025), c4) < -1.0);
        CHECK (std::abs (semisBetween (pitchAt (s, 0.4), c4)) < 0.3);
        const auto r = render (base, { { 0, Event::On, ks + 3, 100.0 }, { sec (0.1), Event::On, 60, 100.0 } }, 0.6);
        CHECK (semisBetween (pitchAt (r, 0.1, 0.03), c4) < -3.0);
        CHECK (std::abs (semisBetween (pitchAt (r, 0.4), c4)) < 0.3);
    }
    // SHAKE trills up; NORMAL turns articulations off again
    {
        const auto a = render (base, { { 0, Event::On, ks + 4, 100.0 }, { sec (0.1), Event::On, 60, 100.0 } }, 1.2);
        double lo = 99, hi = -99;
        for (double t = 0.5; t < 1.1; t += 0.02) { const double s = semisBetween (pitchAt (a, t, 0.02), c4); lo = std::min (lo, s); hi = std::max (hi, s); }
        std::printf ("     shake: pitch range %.2f .. %.2f st\n", lo, hi);
        CHECK (hi - lo > 2.0 && lo > -0.5);
        const auto n = render (base, { { 0, Event::On, ks + 4, 100.0 }, { sec (0.02), Event::On, ks, 100.0 }, { sec (0.1), Event::On, 60, 100.0 } }, 1.2);
        CHECK (std::abs (semisBetween (pitchAt (n, 0.9, 0.05), c4)) < 0.3);
    }
    // keyswitches off: the C0 key just plays
    {
        auto v = base;
        set (v, "artKs", 0);
        const auto a = render (v, { { 0, Event::On, ks + 1, 100.0 } }, 0.3);
        CHECK (rms (a, sec (0.1), sec (0.3)) > 0.01);
    }
    // AUTO: a soft note after a gap scoops, a medium one doesn't; a hard held note falls on release
    {
        auto v = base;
        set (v, "artAuto", 1);
        const auto soft = render (v, { { 0, Event::On, 60, 30.0 } }, 0.4);
        CHECK (semisBetween (pitchAt (soft, 0.015, 0.025), c4) < -1.0);
        const auto mid = render (v, { { 0, Event::On, 60, 80.0 } }, 0.4);
        CHECK (std::abs (semisBetween (pitchAt (mid, 0.015, 0.025), c4)) < 0.3);
        const auto hard = render (v, { { 0, Event::On, 60, 127.0 }, { sec (0.5), Event::Off, 60, 0.0 } }, 1.0);
        CHECK (semisBetween (pitchAt (hard, 0.5 + 0.35 * 0.6, 0.06), c4) < -1.5);
    }
    // SECTION: 4 players are one note, spread in stereo, about as loud as one player
    {
        auto v = base;
        set (v, "osc1Shape", 2); set (v, "osc1Cut", 6);
        // players a few cents apart beat slowly (~1 Hz): measure over 2 s so the level is an average
        const auto solo = render (v, { { 0, Event::On, 60, 100.0 } }, 2.5);
        set (v, "secPlayers", 4);
        const auto sect = render (v, { { 0, Event::On, 60, 100.0 } }, 2.5);
        double lr = 0, ll = 0, rr = 0;
        for (int i = sec (0.3); i < sec (2.5); ++i) { lr += sect.L[(size_t) i] * sect.R[(size_t) i]; ll += sect.L[(size_t) i] * sect.L[(size_t) i]; rr += sect.R[(size_t) i] * sect.R[(size_t) i]; }
        const double corr = lr / std::sqrt (ll * rr), ratioDb = 20 * std::log10 (rms (sect, sec (0.3), sec (2.5)) / rms (solo, sec (0.3), sec (2.5)));
        std::printf ("     section x4: L/R correlation %.2f, level %+.1f dB vs. one player\n", corr, ratioDb);
        CHECK (corr < 0.95);
        CHECK (std::abs (ratioDb) < 3.0);

        auto ePtr = std::make_unique<SynthEngine>();
        auto& e = *ePtr;
        e.prepare (kSr);
        set (v, "secPlayers", 8);
        e.setParams (makeEngineParams ([&v] (int i) { return v[(size_t) i]; }));
        std::array<float, 256> L {}, R {};
        e.noteOn (60, 100);
        e.render (L.data(), R.data(), 256);
        CHECK (e.activeVoices() == 1);   // counts notes, not players
        for (int n = 0; n < 40; ++n) e.noteOn (30 + n, 100);
        e.render (L.data(), R.data(), 256);
        CHECK (e.activeVoices() == 32);  // 32 notes x 8 players fits the voice pool
    }
    // MPE: a member channel's bend moves only its own note; channel 1 bends everything
    {
        auto v = base;
        set (v, "mpeOn", 1); set (v, "mpeBend", 12);
        auto noteOn = [] (int t, int note, int ch) { Event e { t, Event::On, note, 100.0 }; e.ch = ch; return e; };
        auto bend = [] (int t, double val, int ch) { Event e { t, Event::Bend, 0, val }; e.ch = ch; return e; };
        const auto own = render (v, { noteOn (0, 60, 1), bend (sec (0.2), 1.0, 1) }, 0.6);
        CHECK (std::abs (semisBetween (pitchAt (own, 0.45), c4) - 12) < 0.4);
        const auto other = render (v, { noteOn (0, 60, 2), bend (sec (0.2), 1.0, 1) }, 0.6);
        CHECK (std::abs (semisBetween (pitchAt (other, 0.45), c4)) < 0.3);
        const auto master = render (v, { noteOn (0, 60, 2), bend (sec (0.2), 1.0, 0) }, 0.6);
        CHECK (std::abs (semisBetween (pitchAt (master, 0.45), c4) - 2) < 0.3);   // Bend Range 2
    }
    // ANALOG engine: unison spreads in stereo, the blat makes the attack brighter than the sustain
    {
        auto v = base;
        set (v, "engineMode", 1); set (v, "anaUnison", 3); set (v, "anaSpread", 1); set (v, "anaBlat", 1);
        set (v, "osc1Shape", 2); set (v, "osc1Cut", 3);
        const auto a = render (v, { { 0, Event::On, 48, 120.0 } }, 0.8);
        double lr = 0, ll = 0, rr = 0;
        for (int i = sec (0.3); i < sec (0.8); ++i) { lr += a.L[(size_t) i] * a.R[(size_t) i]; ll += a.L[(size_t) i] * a.L[(size_t) i]; rr += a.R[(size_t) i] * a.R[(size_t) i]; }
        CHECK (lr / std::sqrt (ll * rr) < 0.95);
        const double early = harmonicCentroid (a, sec (0.005), noteHz (48), 2048), late = harmonicCentroid (a, sec (0.5), noteHz (48), 2048);
        std::printf ("     analog blat: harmonic centroid %.0f Hz on the attack, %.0f Hz sustained\n", early, late);
        CHECK (early > late * 1.3);
    }
}

static double correlation (const Audio& a, int from, int to)
{
    double lr = 0, ll = 0, rr = 0;
    for (int i = from; i < std::min (to, a.size()); ++i)
    {
        lr += a.L[(size_t) i] * a.R[(size_t) i]; ll += a.L[(size_t) i] * a.L[(size_t) i]; rr += a.R[(size_t) i] * a.R[(size_t) i];
    }
    return ll > 0 && rr > 0 ? lr / std::sqrt (ll * rr) : 1.0;
}

// Phase 4: LFOs, mod matrix (per-voice and global destinations), FX chain.
static void testModulationFx()
{
    auto base = defaultValues();
    auto set = [] (Values& v, const char* id, float x) { v[(size_t) paramIndex (id)] = x; };
    for (const char* id : { "vibDepth", "vibAmp", "lipAmt", "brNoise", "bodyMix", "brsAmt", "dynBrass", "dynBright" }) set (base, id, 0);
    set (base, "osc1Shape", 0); set (base, "osc1Cut", 64);
    auto pitchRange = [] (const Audio& a, double from, double to)
    {
        double lo = 99, hi = -99;
        for (double t = from; t < to; t += 0.02)
        {
            const double s = semisBetween (pitchAt (a, t, 0.02), noteHz (60));
            lo = std::min (lo, s); hi = std::max (hi, s);
        }
        return hi - lo;
    };

    // LFO 1 (per voice, 5 Hz sine) -> Pitch (All): the pitch wobbles; via Mod Wheel: only with the wheel up
    {
        auto v = base;
        set (v, "lfo1On", 1); set (v, "lfo1Rate", 5); set (v, "lfo1Mode", 1);
        set (v, "mod1Src", srcLfo1); set (v, "mod1Dst", dstPitchAll); set (v, "mod1Amt", 0.05f);   // +-0.6 st
        const auto a = render (v, { { 0, Event::On, 60, 100.0 } }, 1.2);
        CHECK (pitchRange (a, 0.3, 1.1) > 0.7);
        set (v, "mod1Via", srcModWheel);
        const auto still = render (v, { { 0, Event::On, 60, 100.0 } }, 1.2);
        CHECK (pitchRange (still, 0.3, 1.1) < 0.15);
        const auto wheel = render (v, { { 0, Event::CC, 1, 1.0 }, { 0, Event::On, 60, 100.0 } }, 1.2);
        CHECK (pitchRange (wheel, 0.3, 1.1) > 0.7);
    }
    // Random -> Pan: two notes land in different places
    {
        auto v = base;
        set (v, "mod1Src", srcRandom); set (v, "mod1Dst", dstPan); set (v, "mod1Amt", 1);
        const auto b = render (v, { { 0, Event::On, 60, 100.0 }, { sec (0.2), Event::Off, 60, 0.0 }, { sec (0.6), Event::On, 64, 100.0 } }, 1.0);
        auto balance = [] (const Audio& x, double t0, double t1)
        {
            double l = 0, r = 0;
            for (int i = sec (t0); i < sec (t1); ++i) { l += x.L[(size_t) i] * x.L[(size_t) i]; r += x.R[(size_t) i] * x.R[(size_t) i]; }
            return 10 * std::log10 ((l + 1e-12) / (r + 1e-12));
        };
        const double b1 = balance (b, 0.05, 0.18), b2 = balance (b, 0.75, 0.95);
        std::printf ("     random pan: note 1 %+.1f dB L/R, note 2 %+.1f dB L/R\n", b1, b2);
        CHECK (std::abs (b1 - b2) > 1.0);
    }
    // tempo-synced square LFO (1/8 at 120 BPM = 4 Hz) -> Amplitude: 8 gates in 2 s
    {
        auto v = base;
        set (v, "lfo1On", 1); set (v, "lfo1Wave", 2); set (v, "lfo1Sync", 1); set (v, "lfo1Div", 8); set (v, "lfo1Trig", 1);
        set (v, "mod1Src", srcLfo1); set (v, "mod1Dst", dstAmp); set (v, "mod1Amt", -0.9f);
        const auto a = render (v, { { 0, Event::On, 60, 100.0 } }, 2.3);
        int edges = 0;
        bool loud = false;
        const double mid = rms (a, sec (0.2), sec (2.2));
        for (int w = sec (0.2); w + sec (0.01) < sec (2.2); w += sec (0.01))
        {
            const bool now = rms (a, w, w + sec (0.01)) > mid;
            if (now && ! loud) ++edges;
            loud = now;
        }
        std::printf ("     synced LFO: %d gates in 2 s (expect 8)\n", edges);
        CHECK (edges >= 7 && edges <= 9);
    }
    // global destination: Mod Wheel -> Plunger opens a closed plunger
    {
        auto v = base;
        set (v, "osc1Shape", 2); set (v, "osc1Cut", 32);
        set (v, "muteType", 4); set (v, "muteSrc", 0); set (v, "mutePlunger", 0);
        set (v, "mod1Src", srcModWheel); set (v, "mod1Dst", dstPlunger); set (v, "mod1Amt", 1);
        const auto closed = render (v, { { 0, Event::On, 48, 100.0 } }, 0.6);
        const auto opened = render (v, { { 0, Event::CC, 1, 1.0 }, { 0, Event::On, 48, 100.0 } }, 0.6);
        CHECK (centroid (opened, sec (0.3)) > centroid (closed, sec (0.3)) * 1.5);
    }

    // ---- FX ---------------------------------------------------------------------
    auto tone = base;
    set (tone, "osc1Shape", 2); set (tone, "osc1Cut", 8);
    const auto dry = render (tone, { { 0, Event::On, 55, 100.0 } }, 1.5);
    const double dryLevel = rms (dry, sec (0.5), sec (1.5));
    auto levelDb = [&] (const Audio& a) { return 20 * std::log10 (rms (a, sec (0.5), sec (1.5)) / dryLevel); };
    // reverb: a tail after the note, level-matched, and a long silence ends at exactly 0 (no denormals)
    {
        auto v = tone;
        set (v, "rvOn", 1); set (v, "rvDecay", 2); set (v, "rvMix", 0.3f);
        const auto wet = render (v, { { 0, Event::On, 55, 100.0 } }, 1.5);
        std::printf ("     reverb mix 30%%: %+.1f dB vs dry\n", levelDb (wet));
        CHECK (std::abs (levelDb (wet)) < 3.0);
        const auto tail = render (v, { { 0, Event::On, 55, 100.0 }, { sec (0.5), Event::Off, 55, 0.0 } }, 1.4);
        CHECK (rms (tail, sec (1.0), sec (1.3)) > 1e-3);
        set (v, "rvDecay", 0.3f); set (v, "rvType", 0);
        const auto end = render (v, { { 0, Event::On, 55, 100.0 }, { sec (0.2), Event::Off, 55, 0.0 } }, 30.0);
        CHECK (end.L.back() == 0.0f && end.R.back() == 0.0f);
    }
    // drive: level-matched, adds harmonics
    {
        auto v = tone;
        set (v, "osc1Cut", 1.5f);
        const auto clean = render (v, { { 0, Event::On, 55, 100.0 } }, 1.5);
        set (v, "fxDrvOn", 1); set (v, "fxDrvAmt", 0.8f); set (v, "fxDrvTone", 1);
        const auto driven = render (v, { { 0, Event::On, 55, 100.0 } }, 1.5);
        const double db = 20 * std::log10 (rms (driven, sec (0.5), sec (1.5)) / rms (clean, sec (0.5), sec (1.5)));
        // share of the energy above the 4th harmonic (drive adds upper harmonics)
        const double f0 = noteHz (55);
        auto upper = [f0] (const Audio& a) { return bandEnergy (a, sec (0.5), 3.5 * f0, 16000) / bandEnergy (a, sec (0.5), 20, 16000); };
        std::printf ("     drive 80%%: %+.1f dB vs clean, energy above 4th harmonic %.1f%% -> %.1f%%\n", db,
                     100 * upper (clean), 100 * upper (driven));
        CHECK (std::abs (db) < 3.0);
        CHECK (upper (driven) > upper (clean) * 2);
    }
    // EQ: +12 dB low shelf lifts the lows
    {
        auto v = tone;
        set (v, "eqOn", 1); set (v, "eqLoGain", 12); set (v, "eqLoFreq", 300);
        const auto eq = render (v, { { 0, Event::On, 43, 100.0 } }, 1.0);
        const auto flat = render (tone, { { 0, Event::On, 43, 100.0 } }, 1.0);
        CHECK (bandEnergy (eq, sec (0.4), 50, 200) > bandEnergy (flat, sec (0.4), 50, 200) * 6);
    }
    // chorus widens a mono voice and keeps the level; width 0 = mono, width 2 = wider
    {
        auto v = tone;
        set (v, "chOn", 1); set (v, "chMix", 0.6f);
        const auto ch = render (v, { { 0, Event::On, 55, 100.0 } }, 1.5);
        std::printf ("     chorus: L/R correlation %.2f, %+.1f dB\n", correlation (ch, sec (0.5), sec (1.5)), levelDb (ch));
        CHECK (correlation (ch, sec (0.5), sec (1.5)) < 0.95);
        CHECK (std::abs (levelDb (ch)) < 3.0);
        set (v, "stWidth", 0);
        const auto mono = render (v, { { 0, Event::On, 55, 100.0 } }, 1.0);
        double diff = 0;
        for (int i = 0; i < mono.size(); ++i) diff = std::max (diff, (double) std::abs (mono.L[(size_t) i] - mono.R[(size_t) i]));
        CHECK (diff < 1e-6);
        set (v, "stWidth", 2);
        const auto wide = render (v, { { 0, Event::On, 55, 100.0 } }, 1.5);
        CHECK (correlation (wide, sec (0.5), sec (1.5)) < correlation (ch, sec (0.5), sec (1.5)) - 0.05);
    }
}

// ---- physical model (Engine = Physical) ---------------------------------------------------
// Strongest frequency within +-50 cents of f (1-cent steps, DFT), in cents relative to f.
static double centsOff (const Audio& a, double from, double to, double f)
{
    double best = -1; int bestC = 0;
    for (int c = -50; c <= 50; ++c)
    {
        const double w = kTwoPi * f * std::pow (2.0, c / 1200.0) / kSr;
        double re = 0, im = 0;
        for (int i = sec (from); i < std::min (a.size(), sec (to)); ++i) { re += a.L[(size_t) i] * std::cos (w * i); im += a.L[(size_t) i] * std::sin (w * i); }
        if (re * re + im * im > best) { best = re * re + im * im; bestC = c; }
    }
    return bestC;
}

static void testPhysicalModel()
{
    std::printf ("physical model\n");
    auto set = [] (Values& v, const char* id, float x) { v[(size_t) paramIndex (id)] = x; };
    auto base = defaultValues();
    set (base, "engineMode", 2); set (base, "vibDepth", 0); set (base, "vibAmp", 0); set (base, "brNoise", 0);
    set (base, "bodyMix", 0); set (base, "pmNoise", 0); set (base, "osc1PEnvAmt", 0); set (base, "osc1AmpSus", 1);
    for (const int ins : { 0, 6 })   // trumpet (lips), alto sax (reed)
    {
        auto v = base;
        set (v, "instrument", (float) ins);
        double lo = 1e9, hi = -1e9, worstCents = 0;
        for (int note = 36; note <= 84; note += 12)
        {
            const auto a = render (v, { { 0, Event::On, note, 100.0 } }, 0.9);
            const double db = 20 * std::log10 (rms (a, sec (0.4), sec (0.9)) + 1e-12);
            lo = std::min (lo, db); hi = std::max (hi, db);
            worstCents = std::max (worstCents, std::abs (centsOff (a, 0.4, 0.9, noteHz (note))));
        }
        std::printf ("     %s: C2..C6 in tune within %.0f cents, level %.1f .. %.1f dB\n", ins == 0 ? "lips" : "reed", worstCents, lo, hi);
        CHECK (worstCents <= 3);
        CHECK (lo > -40 && hi - lo < 6);
        // dynamics: louder and brighter
        double prevDb = -1e9, c30 = 0;
        for (const int vel : { 30, 80, 127 })
        {
            const auto a = render (v, { { 0, Event::On, 55, (double) vel } }, 0.9);
            const double db = 20 * std::log10 (rms (a, sec (0.4), sec (0.9)) + 1e-12);
            const double hc = harmonicCentroid (a, sec (0.4), noteHz (55));
            std::printf ("     %s vel %3d: %.1f dB, harmonic centroid %.2f\n", ins == 0 ? "lips" : "reed", vel, db, hc);
            CHECK (db > prevDb + 3);
            if (vel == 30) c30 = hc; else CHECK (hc > c30 * 1.08);
            prevDb = db;
        }
    }
    // legato glide lands on the new note; the release ends in silence
    {
        auto v = base;
        set (v, "voiceMode", 2); set (v, "glideOn", 1); set (v, "glideTime", 0.1f); set (v, "osc1AmpRel", 0.1f);
        const auto a = render (v, { { 0, Event::On, 60, 100.0 }, { sec (0.5), Event::On, 67, 100.0 }, { sec (0.52), Event::Off, 60, 0 },
                                    { sec (1.2), Event::Off, 67, 0 } }, 1.8);
        const double c = centsOff (a, 0.8, 1.15, noteHz (67));
        std::printf ("     legato glide C4 -> G4: %+.0f cents at the end, tail %.1e\n", c, rms (a, sec (1.6), sec (1.8)));
        CHECK (std::abs (c) <= 3);
        CHECK (rms (a, sec (1.6), sec (1.8)) < 1e-6);
    }
    // a 4-player section is about as loud as one player
    {
        auto v = base;
        const auto one = render (v, { { 0, Event::On, 60, 100.0 } }, 2.0);
        set (v, "secPlayers", 4);
        const auto four = render (v, { { 0, Event::On, 60, 100.0 } }, 2.0);
        const double d = 20 * std::log10 (rms (four, sec (0.5), sec (2.0)) / rms (one, sec (0.5), sec (2.0)));
        std::printf ("     4 players vs 1: %+.1f dB\n", d);
        CHECK (std::abs (d) < 3);
    }
    // stability at the extremes: lowest / highest notes, full blow, no loss, open bell, full turbulence, both valves,
    // breath swells, engine switched mid-note
    {
        bool finite = true; double pk = 0;
        for (const float lip : { -1.0f, 1.0f })
            for (const int ins : { 0, 6 })
            {
                auto v = base;
                set (v, "instrument", (float) ins); set (v, "pmBlow", 1); set (v, "pmLoss", 0); set (v, "pmLip", lip);
                set (v, "pmBell", 1); set (v, "pmNoise", 1); set (v, "dynBreath", 1);
                std::vector<Event> ev { { 0, Event::On, 21, 127.0 }, { 0, Event::On, 60, 127.0 }, { 0, Event::On, 108, 127.0 } };
                for (int k = 0; k < 20; ++k) ev.push_back ({ sec (0.05 * k), Event::CC, 2, (k % 2) ? 1.0 : 0.1 });
                const auto a = render (v, ev, 1.2);
                for (int i = 0; i < a.size(); ++i) if (! std::isfinite (a.L[(size_t) i]) || ! std::isfinite (a.R[(size_t) i])) finite = false;
                pk = std::max (pk, peak (a));
            }
        std::printf ("     extremes: finite %s, peak %.2f\n", finite ? "yes" : "NO", pk);
        CHECK (finite && pk <= 1.0 && pk > 0.01);
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
    if (cmd == "--bench-parts") return runBenchParts();
    if (cmd == "--clicks") return runClickChecks (true) == 0 ? 0 : 1;
    if (cmd == "--hf" && argc >= 5) return runHfDump (argv[2], juce::String (argv[3]).getDoubleValue(), juce::String (argv[4]).getDoubleValue());
    if (cmd == "--brass") return runBrassChecks (true) + runAliasCheck (true) == 0 ? 0 : 1;
    if (cmd == "--host-check" && argc >= 3) return runHostCheck (argv[2]);
    if (cmd == "--parity" && argc >= 3) return runParity (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));

    PresetManager pm;
    testParamTable();
    testFactoryPresets (pm);
    testPresetFiles (pm);
    testEngineBasics();
    testModeledEngine();
    testPerformance();
    testModulationFx();
    testPhysicalModel();
    testPresetRenders (pm);
    failures += runBrassChecks (false);
    failures += runAliasCheck (false);
    failures += runClickChecks (false);
    testLoudness (pm);

    std::printf ("user preset folder: %s\n", PresetManager::userFolder().getFullPathName().toRawUTF8());
    std::printf (failures == 0 ? "ALL PASSED\n" : "%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
