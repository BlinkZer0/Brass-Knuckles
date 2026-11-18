#pragma once
// Offline rendering + analysis helpers shared by the console tests and tools.
#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <complex>
#include "EngineParamsFromValues.h"
#include "PresetManager.h"

namespace bk::test
{
constexpr double kSr = 48000.0;
constexpr int kBlock = 128;

using Values = std::array<float, kNumParams>;

inline Values defaultValues()
{
    Values v {};
    for (int i = 0; i < kNumParams; ++i) v[(size_t) i] = paramDefs()[(size_t) i].def;
    return v;
}

inline void applyPresetData (Values& v, const PresetData& p)
{
    for (int i = 0; i < kNumParams; ++i)
        if (i != master && ! std::isnan (p.values[(size_t) i])) v[(size_t) i] = p.values[(size_t) i];
}

inline int paramIndex (const juce::String& id)
{
    for (int i = 0; i < kNumParams; ++i)
        if (paramDefs()[(size_t) i].id == id) return i;
    return -1;
}

inline Values presetValues (const PresetData& p, float masterLevel = 0.7f)
{
    auto v = defaultValues();
    applyPresetData (v, p);
    v[master] = masterLevel;
    return v;
}

// Timed MIDI-ish events (sample position).
struct Event
{
    enum Type { On, Off, CC, Bend, Pressure, PolyPressure, Sustain };
    int time;
    Type type;
    int a = 0;       // note / cc number
    double b = 0;    // velocity (0..127) or value (0..1, bend -1..1)
    int ch = 0;      // MIDI channel index 0..15 (0 = channel 1)
};

struct Audio
{
    std::vector<float> L, R;
    int size() const { return (int) L.size(); }
};

inline void applyEvent (SynthEngine& e, const Event& ev)
{
    switch (ev.type)
    {
        case Event::On:           e.noteOn (ev.a, (int) ev.b, ev.ch); break;
        case Event::Off:          e.noteOff (ev.a, ev.ch); break;
        case Event::CC:           e.setController (ev.a, ev.b, ev.ch); break;
        case Event::Bend:         e.setPitchBend (ev.b, ev.ch); break;
        case Event::Pressure:     e.setChannelPressure (ev.b, ev.ch); break;
        case Event::PolyPressure: e.setPolyPressure (ev.a, ev.b, ev.ch); break;
        case Event::Sustain:      e.setSustain (ev.b >= 0.5); break;
    }
}

// Renders `seconds` of audio, applying events at their exact sample (like the plugin).
inline Audio render (const Values& values, std::vector<Event> events, double seconds)
{
    std::stable_sort (events.begin(), events.end(), [] (const Event& a, const Event& b) { return a.time < b.time; });
    auto enginePtr = std::make_unique<SynthEngine>(); // ~1.6 MB: too big for the stack
    auto& engine = *enginePtr;
    engine.prepare (kSr);
    const auto P = makeEngineParams ([&values] (int i) { return values[(size_t) i]; });
    const int total = (int) (seconds * kSr);
    Audio out;
    out.L.assign ((size_t) total, 0.0f);
    out.R.assign ((size_t) total, 0.0f);
    size_t ei = 0;
    for (int start = 0; start < total; start += kBlock)
    {
        const int end = std::min (total, start + kBlock);
        engine.setParams (P);
        engine.setTransport (120.0, 0.0, false);
        int pos = start;
        while (ei < events.size() && events[ei].time < end)
        {
            const int t = std::max (pos, events[ei].time);
            if (t > pos) { engine.render (out.L.data() + pos, out.R.data() + pos, t - pos); pos = t; }
            applyEvent (engine, events[ei++]);
        }
        if (end > pos) engine.render (out.L.data() + pos, out.R.data() + pos, end - pos);
    }
    return out;
}

inline int sec (double s) { return (int) (s * kSr); }

inline double rms (const Audio& a, int from, int to)
{
    from = std::max (0, from); to = std::min (a.size(), to);
    double acc = 0;
    for (int i = from; i < to; ++i) acc += 0.5 * ((double) a.L[(size_t) i] * a.L[(size_t) i] + (double) a.R[(size_t) i] * a.R[(size_t) i]);
    return to > from ? std::sqrt (acc / (to - from)) : 0.0;
}

inline double peak (const Audio& a)
{
    double p = 0;
    for (int i = 0; i < a.size(); ++i) p = std::max (p, (double) std::max (std::abs (a.L[(size_t) i]), std::abs (a.R[(size_t) i])));
    return p;
}

// In-place radix-2 FFT.
inline void fft (std::vector<std::complex<double>>& x)
{
    const size_t n = x.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (x[i], x[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2 * kPi / (double) len;
        const std::complex<double> wl (std::cos (ang), std::sin (ang));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w (1);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const auto u = x[i + k], v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Power spectrum (Hann window) of the mono mix starting at `from`, n = power of two.
inline std::vector<double> powerSpectrum (const Audio& a, int from, int n = 8192)
{
    std::vector<std::complex<double>> x ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const int j = std::min (a.size() - 1, from + i);
        const double w = 0.5 - 0.5 * std::cos (2 * kPi * i / (n - 1));
        x[(size_t) i] = 0.5 * ((double) a.L[(size_t) j] + a.R[(size_t) j]) * w;
    }
    fft (x);
    std::vector<double> p ((size_t) n / 2);
    for (int i = 0; i < n / 2; ++i) p[(size_t) i] = std::norm (x[(size_t) i]);
    return p;
}

// Spectral centroid in Hz.
inline double centroid (const Audio& a, int from, int n = 8192)
{
    const auto p = powerSpectrum (a, from, n);
    double num = 0, den = 0;
    for (size_t i = 1; i < p.size(); ++i)
    {
        const double f = (double) i * kSr / n;
        const double m = std::sqrt (p[i]);
        num += f * m; den += m;
    }
    return den > 0 ? num / den : 0.0;
}

// Brightness of the TONE: amplitude-weighted mean frequency of the harmonics of f0 (below
// 16 kHz), each measured in a window around k*f0. Unlike the plain spectral centroid it
// isn't pulled up by broadband breath noise or growl sidebands.
inline double harmonicCentroid (const Audio& a, int from, double f0, int n = 8192)
{
    const auto p = powerSpectrum (a, from, n);
    const double binHz = kSr / n, half = std::min (25.0, f0 * 0.35);
    double num = 0, den = 0;
    for (int k = 1; k * f0 < 16000.0; ++k)
    {
        const double fk = k * f0;
        double e = 0;
        for (int i = std::max (1, (int) ((fk - half) / binHz)); i <= (int) ((fk + half) / binHz) && i < (int) p.size(); ++i) e += p[(size_t) i];
        const double amp = std::sqrt (e);
        num += fk * amp; den += amp;
    }
    return den > 0 ? num / den : 0.0;
}

inline double noteHz (int note) { return 440.0 * std::pow (2.0, (note - 69) / 12.0); }

// The note a preset is auditioned on: mid-range for its instrument.
inline int auditionNote (const Values& v)
{
    static const int notes[] { 67, 62, 53, 55, 41, 72, 63, 56, 49, 60 };
    const int i = juce::jlimit (0, 9, (int) std::lround (v[instrument]));
    return notes[i];
}

inline bool isMonoPreset (const Values& v) { return v[voiceMode] >= 0.5f; }

inline bool writeWav (const juce::File& f, const Audio& a)
{
    f.deleteFile();
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream> (f);
    const auto opts = juce::AudioFormatWriterOptions{}.withSampleRate (kSr).withNumChannels (2).withBitsPerSample (24);
    auto w = fmt.createWriterFor (out, opts);
    if (w == nullptr) return false;
    juce::AudioBuffer<float> b (2, a.size());
    b.copyFrom (0, 0, a.L.data(), a.size());
    b.copyFrom (1, 0, a.R.data(), a.size());
    return w->writeFromAudioSampleBuffer (b, 0, a.size());
}
} // namespace bk::test
