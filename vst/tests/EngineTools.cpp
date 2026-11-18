// Engine tools for the console test app:
//   --dump-params                 the parameter table as JSON (tests/check-params.ps1 compares it with params.js)
//   --render <preset> <out.wav> [phrase]   offline render; preset = index or "Category/Name"
//   --render-all <folder>         every factory preset, demo phrase, one WAV each
//   --loudness                    loudness of every factory preset (level matching)
//   --bench                       worst-case CPU
//   --brass                       brass realism checks (dynamics -> brightness, breath swells)
#include "TestRender.h"

using namespace bk;
using namespace bk::test;

namespace
{
const char* kindName (Kind k)
{
    switch (k) { case Kind::Bool: return "bool"; case Kind::Choice: return "choice"; case Kind::Int: return "int"; default: return "float"; }
}

const char* unitName (Unit u)
{
    switch (u)
    {
        case Unit::Seconds: return "s";       case Unit::Hz: return "Hz";        case Unit::LfoHz: return "lfohz";
        case Unit::Semitones: return "st";    case Unit::Cents: return "ct";     case Unit::Percent: return "%";
        case Unit::BipolarPercent: return "bip"; case Unit::Decibels: return "dB"; case Unit::Degrees: return "deg";
        case Unit::Octaves: return "oct";     case Unit::Harmonic: return "harm"; case Unit::Ms: return "ms";
        case Unit::Shape: return "shape";     case Unit::Note: return "note";    case Unit::DbPerOct: return "dboct";
        case Unit::Count: return "count";     default: return "";
    }
}

const PresetData* findPreset (const PresetManager& pm, const juce::String& spec)
{
    const auto& f = pm.factory();
    if (spec.containsOnly ("0123456789"))
    {
        const int i = spec.getIntValue();
        return i >= 0 && i < (int) f.size() ? &f[(size_t) i] : nullptr;
    }
    for (const auto& p : f)
        if (p.category + "/" + p.name == spec || p.name == spec) return &p;
    return nullptr;
}

// Audition phrases. Times in seconds.
std::vector<Event> phrase (const juce::String& name, const Values& v, double& length)
{
    const int n = auditionNote (v);
    const bool mono = isMonoPreset (v);
    std::vector<Event> ev;
    auto on  = [&ev] (double t, int note, int vel) { ev.push_back ({ sec (t), Event::On, note, (double) vel }); };
    auto off = [&ev] (double t, int note)          { ev.push_back ({ sec (t), Event::Off, note, 0.0 }); };

    if (name == "velocity")        // same note, soft to hard
    {
        const int vels[] { 25, 50, 75, 100, 127 };
        for (int i = 0; i < 5; ++i) { on (0.1 + i * 1.0, n, vels[i]); off (0.85 + i * 1.0, n); }
        length = 5.6;
    }
    else if (name == "breath")     // one note, breath controller swells and fades
    {
        ev.push_back ({ 0, Event::CC, 2, 0.15 });
        on (0.1, n, 80);
        for (int i = 0; i <= 100; ++i)
        {
            const double t = 0.2 + i * 0.04;    // 4 s: up, then down
            const double x = i <= 60 ? 0.15 + 0.85 * i / 60.0 : 1.0 - 0.8 * (i - 60) / 40.0;
            ev.push_back ({ sec (t), Event::CC, 2, x });
        }
        off (4.4, n);
        length = 5.2;
    }
    else if (name == "articulations")   // each keyswitch once: normal, fall, doit, rip, shake, scoop
    {
        const int ks = (int) std::lround (v[artKsBase]);
        const int order[] { 0, 1, 2, 3, 4, 5 };
        double t = 0.1;
        for (const int a : order)
        {
            on (t - 0.05, ks + a, 100); off (t - 0.04, ks + a);
            on (t, n, 105); off (t + (a == 4 ? 1.2 : 0.6), n);
            t += a == 4 ? 1.9 : 1.3;
        }
        length = t + 0.3;
    }
    else if (name == "legato")     // overlapping line: glide + lip slurs in Legato mode
    {
        const int line[] { 0, 2, 4, 5, 7, 5, 4, 2, 0 };
        for (int i = 0; i < 9; ++i) { on (0.1 + i * 0.35, n + line[i], 90); off (0.1 + i * 0.35 + 0.38, n + line[i]); }
        length = 4.2;
    }
    else if (name == "chord" || ! mono)
    {
        on (0.1, n, 100); on (0.1, n + 4, 100); on (0.1, n + 7, 100);
        off (2.1, n); off (2.1, n + 4); off (2.1, n + 7);
        length = 3.2;
    }
    else                           // "demo" for a mono preset: a short melody
    {
        const int line[] { 0, 4, 7, 12, 7, 4, 0 };
        const int vels[] { 70, 85, 95, 115, 90, 75, 100 };
        for (int i = 0; i < 7; ++i) { on (0.1 + i * 0.45, n + line[i], vels[i]); off (0.1 + i * 0.45 + 0.4, n + line[i]); }
        length = 4.2;
    }
    return ev;
}
} // namespace

int runDumpParams()
{
    juce::Array<juce::var> list;
    for (const auto& d : paramDefs())
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", d.id);
        o->setProperty ("type", kindName (d.kind));
        o->setProperty ("min", d.min);
        o->setProperty ("max", d.max);
        o->setProperty ("def", d.def);
        o->setProperty ("unit", unitName (d.unit));
        o->setProperty ("log", d.logSkew);
        o->setProperty ("noPreset", d.noPreset);
        juce::Array<juce::var> ch;
        for (const auto& c : d.choices) ch.add (c);
        o->setProperty ("choices", ch);
        list.add (juce::var (o));
    }
    std::printf ("%s\n", juce::JSON::toString (juce::var (list), true).toRawUTF8());
    return 0;
}

int runRender (const juce::String& spec, const juce::File& out, const juce::String& phraseName)
{
    PresetManager pm;
    const auto* p = findPreset (pm, spec);
    if (p == nullptr) { std::printf ("unknown preset %s\n", spec.toRawUTF8()); return 2; }
    const auto v = presetValues (*p);
    double len = 0;
    const auto ev = phrase (phraseName, v, len);
    const auto a = render (v, ev, len);
    if (! writeWav (out, a)) { std::printf ("cannot write %s\n", out.getFullPathName().toRawUTF8()); return 2; }
    std::printf ("%s/%s -> %s  (%.1f s, peak %.3f)\n", p->category.toRawUTF8(), p->name.toRawUTF8(),
                 out.getFullPathName().toRawUTF8(), len, peak (a));
    return 0;
}

int runRenderAll (const juce::File& folder)
{
    PresetManager pm;
    folder.createDirectory();
    int i = 0;
    for (const auto& p : pm.factory())
    {
        const auto v = presetValues (p);
        for (const char* ph : { "demo", "velocity", "breath" })
        {
            double len = 0;
            const auto ev = phrase (ph, v, len);   // sets len: must run before render's arguments are read
            const auto a = render (v, ev, len);
            const auto name = juce::String (i).paddedLeft ('0', 3) + " " + juce::File::createLegalFileName (p.category + " - " + p.name)
                              + " [" + ph + "].wav";
            writeWav (folder.getChildFile (name), a);
        }
        ++i;
    }
    std::printf ("rendered %d presets x 3 phrases into %s\n", i, folder.getFullPathName().toRawUTF8());
    return 0;
}

// Loudest 100 ms window (RMS) of the preset's chord (poly) or note (mono) at velocity 100,
// with master low enough that the soft clipper stays linear, scaled back to master = 1.
double presetLoudness (const PresetData& p)
{
    auto v = presetValues (p, 0.05f);
    const int n = auditionNote (v);
    std::vector<Event> ev { { sec (0.05), Event::On, n, 100.0 } };
    if (! isMonoPreset (v)) { ev.push_back ({ sec (0.05), Event::On, n + 4, 100.0 }); ev.push_back ({ sec (0.05), Event::On, n + 7, 100.0 }); }
    const auto a = render (v, ev, 2.5);
    double best = 0;
    for (int w = 0; w + sec (0.1) <= a.size(); w += sec (0.05)) best = std::max (best, rms (a, w, w + sec (0.1)));
    return best / 0.05;
}

// Clicks: energy above 16 kHz (4th-order Butterworth high-pass) of the mono mix in 2 ms windows.
// The tones have almost nothing up there (their brightness filters and harmonic layer stop well
// below), while a click is broadband, so periodic waveform corners of low notes don't register
// the way they do in a plain high-order difference. Returns max / median window RMS over
// [from, to), with a floor of -100 dBFS for the median.
static double lastClickTime = 0;   // where the loudest window was (diagnostics)
// `around`: if not empty, only windows within 40 ms of these times (s) are judged.
// `winSec`: window length; at least two periods of the lowest note, because a waveform's sharp
// edge puts a burst of high-band energy into every period.
double clickRatio (const Audio& a, int from, int to, const std::vector<double>& around = {}, double winSec = 0.002)
{
    const auto c1 = designBiquad (bqHighPass, 16000.0, 0.5412, 0, kSr);
    const auto c2 = designBiquad (bqHighPass, 16000.0, 1.3066, 0, kSr);
    Biquad h1, h2;
    std::vector<double> hp ((size_t) a.size());
    for (int i = 0; i < a.size(); ++i)
        hp[(size_t) i] = h2.process (h1.process (0.5 * ((double) a.L[(size_t) i] + a.R[(size_t) i]), c1), c2);
    std::vector<double> w;
    const int win = sec (winSec);
    for (int s = from; s + win <= std::min (to, a.size()); s += win)
    {
        double e = 0;
        for (int i = s; i < s + win; ++i) e += hp[(size_t) i] * hp[(size_t) i];
        w.push_back (std::sqrt (e / win));
    }
    // A click sticks out above BOTH sides; a level change (a brighter note, a glide) only above one.
    // Each window is compared with the larger of the medians of the 10 windows (20 ms) before it
    // and the 10 after it (skipping its direct neighbours).
    const int ctx = 10;
    double best = 0;
    for (int i = ctx + 1; i + ctx + 1 < (int) w.size(); ++i)
    {
        if (! around.empty())
        {
            const double t = (from + (double) i * win) / kSr;
            bool near = false;
            for (const double c : around) near = near || std::abs (t - c) < std::max (0.04, 2 * winSec);
            if (! near) continue;
        }
        auto medianOf = [&w] (int a0, int a1)
        {
            std::vector<double> s (w.begin() + a0, w.begin() + a1);
            std::nth_element (s.begin(), s.begin() + (long) s.size() / 2, s.end());
            return s[s.size() / 2];
        };
        const double ref = std::max (std::max (medianOf (i - ctx - 1, i - 1), medianOf (i + 2, i + ctx + 2)), 1e-5);
        const double r = w[(size_t) i] / ref;
        if (r > best) { best = r; lastClickTime = (from + (double) i * win) / kSr; }
    }
    return best;
}

// Legato glides must not click. Every preset that plays legato (mono, glide) renders the legato
// phrase with its deliberate transients (lip buzz, key click, chiff, breath noise, growl,
// flutter) and reverb off; the click ratio must stay under the threshold. The detector is
// checked first: the same clean render with a tiny injected step (-46 dBFS) must trip it.
constexpr double kClickThreshold = 4.0;   // clean legato presets measure <= 1.3; a -46 dBFS step ~10
// Diagnostics: high-band (>16 kHz) RMS in 2 ms windows between t0 and t1 for a preset's
// legato phrase and for one held note.
int runHfDump (const juce::String& spec, double t0, double t1)
{
    PresetManager pm;
    const PresetData* p = nullptr;
    for (const auto& x : pm.factory()) if (x.category + "/" + x.name == spec || x.name == spec) p = &x;
    if (p == nullptr) return 2;
    auto v = presetValues (*p);
    for (const char* id : { "lipAmt", "keyClick", "chiff", "brNoise", "growlAmt", "flutAmt", "rvOn", "chOn", "vibDepth", "vibAmp" })
        v[(size_t) paramIndex (id)] = 0;
    double len = 0;
    const auto ev = phrase ("legato", v, len);
    for (int run = 0; run < 2; ++run)
    {
        const auto a = run == 0 ? render (v, ev, len) : render (v, { { sec (0.1), Event::On, auditionNote (v), 90.0 } }, len);
        const auto c1 = designBiquad (bqHighPass, 16000.0, 0.5412, 0, kSr), c2 = designBiquad (bqHighPass, 16000.0, 1.3066, 0, kSr);
        Biquad h1, h2;
        std::printf ("%s:\n", run == 0 ? "legato phrase" : "one held note");
        double acc = 0, full = 0; int cnt = 0;
        for (int i = 0; i < sec (t1); ++i)
        {
            const double x = 0.5 * ((double) a.L[(size_t) i] + a.R[(size_t) i]);
            const double h = h2.process (h1.process (x, c1), c2);
            if (i < sec (t0)) continue;
            acc += h * h; full += x * x;
            if (++cnt == sec (0.002)) { std::printf ("  %.3f s  hf %.2e  full %.2e\n", (double) i / kSr, std::sqrt (acc / cnt), std::sqrt (full / cnt)); acc = full = 0; cnt = 0; }
        }
    }
    return 0;
}

int runClickChecks (bool verbose)
{
    PresetManager pm;
    int fails = 0, checked = 0;
    double worst = 0;
    juce::String worstName;
    // the detector must see a small step (-46 dBFS) in a clean legato sine line
    {
        auto v = defaultValues();
        v[voiceMode] = 2; v[glideOn] = 1; v[(size_t) oscParam (0, oShape)] = 0; v[(size_t) oscParam (0, oCut)] = 64;
        for (const char* id : { "lipAmt", "brNoise", "brsAmt", "dynBrass", "bodyMix", "vibDepth" }) v[(size_t) paramIndex (id)] = 0;
        double len = 0;
        const auto ev = phrase ("legato", v, len);
        auto a = render (v, ev, len);
        std::vector<double> changes;
        for (int i = 1; i < 9; ++i) changes.push_back (0.1 + i * 0.35);
        const double winSec = std::max (0.002, 2.0 / noteHz (auditionNote (v)));
        const double clean = clickRatio (a, sec (0.2), sec (len - 0.6), changes, winSec);
        // a step right at a note change (1.15 s): the kind of click a bad glide would make
        for (int i = sec (1.15); i < a.size(); ++i) { a.L[(size_t) i] += 0.005f; a.R[(size_t) i] += 0.005f; }
        const double withStep = clickRatio (a, sec (0.2), sec (len - 0.6), changes, winSec);
        const bool ok = clean < kClickThreshold && withStep > kClickThreshold * 2;
        std::printf ("%-4s click detector on a sine line: clean %.1f, with a -46 dBFS step %.1f (must exceed %.0f)\n",
                     ok ? "ok" : "FAIL", clean, withStep, kClickThreshold * 2);
        if (! ok) ++fails;
    }
    for (const auto& p : pm.factory())
    {
        auto v = presetValues (p);
        if (v[voiceMode] < 1.5f || v[glideOn] < 0.5f) continue;   // legato presets with glide
        for (const char* id : { "lipAmt", "keyClick", "chiff", "brNoise", "growlAmt", "flutAmt", "rvOn", "chOn", "vibDepth", "vibAmp" })
            v[(size_t) paramIndex (id)] = 0;
        v[oversample] = 2;   // 4x: aliasing above 16 kHz is the aliasing check's business, not this one's
        double len = 0;
        const auto ev = phrase ("legato", v, len);
        const auto a = render (v, ev, len);
        // judged around each legato note change (the phrase changes note every 0.35 s from 0.45 s)
        std::vector<double> changes;
        for (int i = 1; i < 9; ++i) changes.push_back (0.1 + i * 0.35);
        const double winSec = std::max (0.002, 2.0 / noteHz (auditionNote (v) + (int) std::lround (v[transpose])));
        const double r = clickRatio (a, sec (0.2), sec (len - 0.6), changes, winSec);
        ++checked;
        if (r > worst) { worst = r; worstName = p.category + "/" + p.name; }
        const bool ok = r < kClickThreshold;
        if (! ok) ++fails;
        if (verbose || ! ok) std::printf ("%-4s legato clicks %-34s ratio %.1f at %.3f s\n", ok ? "ok" : "FAIL",
                                          (p.category + "/" + p.name).toRawUTF8(), r, lastClickTime);
    }
    std::printf ("%-4s legato glides: %d presets, worst click ratio %.1f (%s), threshold %.0f\n", fails == 0 ? "ok" : "FAIL",
                 checked, worst, worstName.toRawUTF8(), kClickThreshold);
    return fails;
}

int runLoudness()
{
    PresetManager pm;
    std::printf ("[\n");
    const auto& f = pm.factory();
    for (size_t pi = 0; pi < f.size(); ++pi)
    {
        auto v = presetValues (f[pi], 0.05f);
        const double loud = presetLoudness (f[pi]);
        const double withoutAmp = loud / std::pow (10.0, v[ampLevel] / 20.0);
        std::printf ("  { \"index\": %d, \"name\": \"%s\", \"category\": \"%s\", \"loudness\": %.6f, \"loudnessDb\": %.2f, \"loudnessNoAmp\": %.6f, \"ampLevel\": %.2f }%s\n",
                     (int) pi, f[pi].name.toRawUTF8(), f[pi].category.toRawUTF8(), loud, 20 * std::log10 (std::max (loud, 1e-9)),
                     withoutAmp, v[ampLevel], pi + 1 < f.size() ? "," : "");
    }
    std::printf ("]\n");
    return 0;
}

// Worst case so far: full default polyphony (32 notes), every per-voice feature on:
// 3 oscillators with their own envelopes, 32-harmonic additive layer, voice filter LP24,
// brassiness at 1x / 2x / 4x oversampling, lip transient, breath noise, key click,
// growl, flutter, vibrato, continuous breath, body formants and a plunger mute.
// Grows each phase (section players in phase 3, FX in phase 4).
int runBench()
{
    auto v = defaultValues();
    auto set = [&v] (const char* id, float x) { v[(size_t) paramIndex (id)] = x; };
    set ("osc2On", 1); set ("osc3On", 1); set ("osc2Shape", 2.5f); set ("osc3Shape", 1.5f);
    set ("osc1TEnvCut", 2); set ("osc2TEnvCut", 2); set ("osc3TEnvCut", 2);
    set ("fltOn", 1); set ("fltType", 1); set ("fltEnvAmt", 2);
    set ("osc1AmpRel", 5); set ("osc2AmpRel", 5); set ("osc3AmpRel", 5);
    set ("addOn", 1); set ("addPartials", 32); set ("brsAmt", 0.5f); set ("dynBrass", 0.8f);
    set ("lipAmt", 0.5f); set ("lipDecay", 0.3f); set ("brNoise", 0.3f); set ("keyClick", 0.5f);
    set ("growlAmt", 0.5f); set ("flutAmt", 0.3f); set ("bodyMix", 1); set ("muteType", 4);
    // phase 4: every FX, both LFOs (one per voice, one global), all 12 matrix routes
    set ("fxDrvOn", 1); set ("eqOn", 1); set ("eqMidGain", 3); set ("chOn", 1); set ("rvOn", 1);
    set ("lfo1On", 1); set ("lfo1Mode", 1); set ("lfo2On", 1); set ("lfo2Wave", 5);
    {
        const int routes[12][2] { { srcLfo1, dstPitchAll }, { srcLfo2, dstFltCutoff }, { srcAmpEnv1, dstOsc2Shape }, { srcDynamics, dstHarmTilt },
                                  { srcBreath, dstBrassiness }, { srcModWheel, dstVibDepth }, { srcRandom, dstPan }, { srcKey, dstOsc1Bright },
                                  { srcVibrato, dstAmp }, { srcTmbEnv1, dstOsc3PW }, { srcLfo2, dstPlunger }, { srcModWheel, dstReverbMix } };
        for (int s = 0; s < 12; ++s)
        {
            v[(size_t) modParam (s, 1)] = (float) routes[s][0];
            v[(size_t) modParam (s, 3)] = (float) routes[s][1];
            v[(size_t) modParam (s, 4)] = 0.1f;
        }
    }
    for (int run = 0; run < 5; ++run)
    {
        // runs 0-2: one player per note at 1x / 2x / 4x; runs 3-4: 8-player sections at 4x (32 and 16 notes)
        const int os = std::min (run, 2);
        set ("oversample", (float) os);
        set ("secPlayers", run >= 3 ? 8.0f : 1.0f);
        const int notes = run == 4 ? 16 : 32;
        auto enginePtr = std::make_unique<SynthEngine>(); // ~1.6 MB: too big for the stack
        auto& engine = *enginePtr;
        engine.prepare (kSr);
        std::array<float, 512> L {}, R {};
        const auto P = makeEngineParams ([&v] (int i) { return v[(size_t) i]; });
        engine.setParams (P);
        engine.setController (2, 0.5);
        for (int n = 0; n < notes; ++n) engine.noteOn (36 + n, 100);
        const int blocks = (int) (kSr * 10 / 512);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int b = 0; b < blocks; ++b)
        {
            engine.setParams (P);
            engine.setController (2, 0.5 + 0.4 * std::sin (b * 0.05));
            engine.setTransport (120.0, 0.0, false);
            engine.render (L.data(), R.data(), 512);
        }
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        std::printf ("%2d notes x %d player%s, every voice feature, %dx oversampling: %6.0f ms for 10 s of audio  (%.1f%% of one core, %d notes)\n",
                     notes, run >= 3 ? 8 : 1, run >= 3 ? "s" : " ", os == 0 ? 1 : (os == 1 ? 2 : 4), ms, ms / 100.0, engine.activeVoices());
    }

    // the real thing: every factory preset with section players or unison, a 12-note chord held for 5 s
    PresetManager pm;
    std::printf ("factory presets with several players per note, 12-note chord:\n");
    for (const auto& p : pm.factory())
    {
        const auto pv = presetValues (p);
        const bool multi = pv[secPlayers] > 1.5f || (pv[engineMode] > 0.5f && pv[anaUnison] > 1.5f);
        if (! multi) continue;
        auto e = std::make_unique<SynthEngine>();
        e->prepare (kSr);
        const auto P = makeEngineParams ([&pv] (int i) { return pv[(size_t) i]; });
        e->setParams (P);
        for (int n = 0; n < 12; ++n) e->noteOn (48 + n * 2, 100);
        std::array<float, 512> L {}, R {};
        const int blocks = (int) (kSr * 5 / 512);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int b = 0; b < blocks; ++b) { e->setParams (P); e->setTransport (120.0, 0.0, false); e->render (L.data(), R.data(), 512); }
        std::printf ("   %-30s %5.1f%% of one core\n", (p.category + "/" + p.name).toRawUTF8(), (juce::Time::getMillisecondCounterHiRes() - t0) / 50.0);
    }
    return 0;
}

// Profiling aid: the 32 x 8 section case at 2x with one feature switched off at a time.
int runBenchParts()
{
    auto base = defaultValues();
    auto set = [] (Values& v, const char* id, float x) { v[(size_t) paramIndex (id)] = x; };
    for (auto [id, x] : std::initializer_list<std::pair<const char*, double>> {
             { "osc2On", 1 }, { "osc3On", 1 }, { "osc2Shape", 2.5f }, { "osc3Shape", 1.5f }, { "fltOn", 1 }, { "fltType", 1 },
             { "addOn", 1 }, { "addPartials", 32 }, { "brsAmt", 0.5f }, { "dynBrass", 0.8f }, { "lipAmt", 0.5f }, { "lipDecay", 0.3f },
             { "brNoise", 0.3f }, { "keyClick", 0.5f }, { "growlAmt", 0.5f }, { "flutAmt", 0.3f }, { "bodyMix", 1 }, { "muteType", 4 },
             { "osc1AmpRel", 5 }, { "osc2AmpRel", 5 }, { "osc3AmpRel", 5 }, { "secPlayers", 8 }, { "oversample", 1 } })
        set (base, id, (float) x);
    auto time = [] (const Values& v)
    {
        auto e = std::make_unique<SynthEngine>();
        e->prepare (kSr);
        const auto P = makeEngineParams ([&v] (int i) { return v[(size_t) i]; });
        e->setParams (P);
        for (int n = 0; n < 32; ++n) e->noteOn (36 + n, 100);
        std::array<float, 512> L {}, R {};
        const int blocks = (int) (kSr * 3 / 512);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int b = 0; b < blocks; ++b) { e->setParams (P); e->render (L.data(), R.data(), 512); }
        return (juce::Time::getMillisecondCounterHiRes() - t0) / 30.0;   // % of one core
    };
    std::printf ("all on: %.1f%%\n", time (base));
    for (const char* off : { "addOn", "brsAmt", "lipAmt", "brNoise", "keyClick", "growlAmt", "flutAmt", "fltOn", "osc2On", "osc3On" })
    {
        auto v = base;
        set (v, off, 0);
        if (juce::String (off) == "brsAmt") set (v, "dynBrass", 0);
        std::printf ("without %-9s %.1f%%\n", off, time (v));
    }
    return 0;
}

// Aliasing at high dynamics: a high note (C6), velocity 127, full brassiness drive, no
// vibrato / pitch movement, fed by the alias-free harmonics layer so the measurement is
// about the nonlinear stage. Measures the energy that is NOT at a harmonic of the note
// (relative to the total, below 18 kHz) at 1x, 2x and 4x.
// Pass: 4x below -60 dB, 2x at least 6 dB better than 1x, 4x no worse than 2x.
int runAliasCheck (bool verbose)
{
    PresetManager pm;
    auto v = presetValues (pm.factory()[0]);
    auto set = [&v] (const char* id, float x) { v[(size_t) paramIndex (id)] = x; };
    set ("vibDepth", 0); set ("vibAmp", 0); set ("growlAmt", 0); set ("flutAmt", 0);
    set ("brsAmt", 1); set ("dynBrass", 1); set ("brNoise", 0); set ("lipAmt", 0); set ("keyClick", 0); set ("chiff", 0);
    for (int k = 0; k < 3; ++k) { set ((("osc" + juce::String (k + 1)) + "PEnvAmt").toRawUTF8(), 0); set ((("osc" + juce::String (k + 1)) + "BlipAmt").toRawUTF8(), 0); }
    const int note = 84;                       // C6, 1047 Hz
    const double f0 = 440.0 * std::pow (2.0, (note - 69) / 12.0);
    // inharmonic energy / total, dB, over the audible band below 18 kHz (the half-band filters'
    // transition band folds a little energy into 19.5-24 kHz, which nobody hears)
    auto measure = [&] (const Values& w)
    {
        const auto a = render (w, { { 0, Event::On, note, 127.0 } }, 1.0);
        const int N = 16384;
        const auto p = powerSpectrum (a, sec (0.6), N);
        double harm = 0, other = 0;
        for (size_t i = 2; i < p.size(); ++i)
        {
            if ((double) i * kSr / N > 18000.0) break;
            const double h = ((double) i * kSr / N) / f0;
            (std::abs (h - std::round (h)) * f0 < 60.0 && std::round (h) >= 1 ? harm : other) += p[i];
        }
        return 10 * std::log10 (other / std::max (harm + other, 1e-30));
    };

    // the nonlinear stage, fed by the alias-free harmonics layer: what oversampling is for
    auto x = v;
    for (int k = 0; k < 3; ++k) x[(size_t) oscParam (k, oOn)] = 0;
    x[addOn] = 1; x[addPartials] = 32;
    double ratios[3];
    for (int os = 0; os <= 2; ++os) { x[oversample] = (float) os; ratios[os] = measure (x); }
    // at the preset's own brassiness (what you actually hear at velocity 127)
    auto y = presetValues (pm.factory()[0]);
    y[vibDepth] = 0; y[vibAmp] = 0; y[brNoise] = 0; y[lipAmt] = 0;
    for (int k = 0; k < 3; ++k) { y[(size_t) oscParam (k, oOn)] = 0; y[(size_t) oscParam (k, oPEnvAmt)] = 0; y[(size_t) oscParam (k, oBlipAmt)] = 0; }
    y[addOn] = 1; y[oversample] = 2;
    const double presetRatio = measure (y);
    const bool ok = presetRatio < -60.0 && ratios[2] < -50.0 && ratios[1] < ratios[0] - 6.0 && ratios[2] <= ratios[1] + 1.0;
    std::printf ("%-4s aliasing of the brassiness stage at C6, velocity 127: preset drive 4x %.1f dB (< -60); full drive 1x %.1f, 2x %.1f, 4x %.1f dB (4x < -50)\n",
                 ok ? "ok" : "FAIL", presetRatio, ratios[0], ratios[1], ratios[2]);
    if (verbose)   // for information: the PolyBLEP oscillators' own aliasing, and the full preset
    {
        auto w = v; w[oversample] = 2; w[brsAmt] = 0; w[dynBrass] = 0;
        std::printf ("     info: Lead Trumpet oscillators alone (PolyBLEP): %6.1f dB\n", measure (w));
        w = v; w[oversample] = 2;
        std::printf ("     info: Lead Trumpet, full brassiness, 4x:          %6.1f dB\n", measure (w));
    }
    return ok ? 0 : 1;
}

// Brass realism checks on every factory preset. Returns the number of failures.
//  1. dynamics -> brightness: the harmonic centroid (brightness of the tone itself, not
//     the breath noise) of a held note rises with velocity
//     (acoustic presets: at least 8% per step, three steps)
//  2. dynamics -> loudness: RMS rises with velocity
//  3. breath control works over time: holding a note and pushing CC2 makes it louder AND brighter
int runBrassChecks (bool verbose)
{
    PresetManager pm;
    int fails = 0;
    for (const auto& p : pm.factory())
    {
        // the preset's dynamic response, measured on its tone: one player (section detune moves the
        // upper harmonics out of the measurement windows), no vibrato, no matrix routes
        auto v = presetValues (p);
        v[secPlayers] = 1; v[vibDepth] = 0; v[vibAmp] = 0; v[brNoise] = 0; v[growlAmt] = 0;
        v[pmNoise] = 0;   // the physical model's turbulence is breath noise too
        for (int s = 0; s < kNumModSlotsParam; ++s) v[(size_t) modParam (s, 0)] = 0;
        const bool acoustic = std::lround (v[instrument]) != 9;
        const int n = auditionNote (v);
        const double f0 = noteHz (n + (int) std::lround (v[transpose]));
        // a plunger on a controller starts closed (a 350 Hz low-pass); test it half open
        const bool plungerOnCtl = std::lround (v[muteType]) == 4 && std::lround (v[muteSrc]) != 0;
        const Event openPlunger { 0, Event::CC, std::lround (v[muteSrc]) == 5 ? (int) std::lround (v[muteCC]) : 1, 0.5 };
        double cents[3], levels[3];
        const int vels[] { 40, 80, 120 };
        for (int k = 0; k < 3; ++k)
        {
            std::vector<Event> evs { { sec (0.02), Event::On, n, (double) vels[k] } };
            if (plungerOnCtl) evs.push_back (openPlunger);
            const auto a = render (v, evs, 0.9);
            cents[k] = harmonicCentroid (a, sec (0.5), f0);
            levels[k] = rms (a, sec (0.5), sec (0.85));
        }
        // breath swell: same note, breath 0.2 then 0.9, measured mid-note
        std::vector<Event> ev { { 0, Event::CC, 2, 0.2 }, { sec (0.02), Event::On, n, 80.0 }, { sec (0.9), Event::CC, 2, 0.9 } };
        if (plungerOnCtl) ev.push_back (openPlunger);
        const auto b = render (v, ev, 1.8);
        const double cLow = harmonicCentroid (b, sec (0.55), f0), cHigh = harmonicCentroid (b, sec (1.45), f0);
        const double lLow = rms (b, sec (0.55), sec (0.85)), lHigh = rms (b, sec (1.45), sec (1.75));

        const bool brightOk = ! acoustic || (cents[1] > cents[0] * 1.08 && cents[2] > cents[1] * 1.08);
        const bool loudOk = levels[1] > levels[0] && levels[2] > levels[1];
        const bool breathOk = lHigh > lLow * 1.5 && (! acoustic || cHigh > cLow * 1.08);
        const bool ok = brightOk && loudOk && breathOk;
        if (! ok) ++fails;
        if (verbose || ! ok)
            std::printf ("%-4s %-34s centroid %5.0f/%5.0f/%5.0f Hz  rms %.3f/%.3f/%.3f  breath: %5.0f->%5.0f Hz, %.3f->%.3f%s%s%s\n",
                         ok ? "ok" : "FAIL", (p.category + "/" + p.name).toRawUTF8(), cents[0], cents[1], cents[2],
                         levels[0], levels[1], levels[2], cLow, cHigh, lLow, lHigh,
                         brightOk ? "" : "  [brightness]", loudOk ? "" : "  [loudness]", breathOk ? "" : "  [breath]");
    }
    return fails;
}
