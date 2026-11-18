#pragma once
// ============================================================================
//  Instrument models: the data that makes a trumpet a trumpet.
//   - spectral envelope (dB at frequency breakpoints) for the additive layer
//   - harmonic slope (dB per harmonic number) when played softly and loudly:
//     real brass spectra are steep when soft and nearly flat when loud
//   - body / bell resonances (bus formant bank): brass bells, sax bodies
//   - breath-noise band
// Values are approximations from published brass / saxophone spectra and
// formant measurements, tuned by ear. Same order as the "instrument" choice.
// The JS engine carries an identical table.
// ============================================================================
#include "Filters.h"

namespace bk
{
constexpr int kNumInstruments = 10;
constexpr int kSpecPoints = 4;
constexpr int kBodyBands = 4;

struct BandSpec { int type; double hz, db, q; };

struct InstrumentModel
{
    double softSlope, loudSlope;          // dB per harmonic at dynamics 0 / 1
    double specHz[kSpecPoints], specDb[kSpecPoints];
    BandSpec body[kBodyBands];
    double noiseHz, noiseQ;
};

inline const InstrumentModel& instrumentModel (int i)
{
    static const InstrumentModel models[kNumInstruments] = {
        // Trumpet: bright formant region 1-3 kHz
        { -7.0, -1.2, { 300, 1200, 3000, 8000 }, { -6, 0, 0, -12 },
          { { bqLowShelf, 250, -4, 0.7 }, { bqPeak, 1300, 5, 1.2 }, { bqPeak, 2600, 3, 1.6 }, { bqHighShelf, 7000, -4, 0.7 } },
          2500, 1.0 },
        // Flugelhorn: conical, darker, rounder
        { -9.0, -3.0, { 250, 800, 2000, 6000 }, { -3, 0, -6, -20 },
          { { bqLowShelf, 150, 2, 0.7 }, { bqPeak, 700, 4, 1.0 }, { bqPeak, 1500, 2, 1.2 }, { bqHighShelf, 3500, -7, 0.7 } },
          1800, 1.0 },
        // Trombone
        { -8.0, -1.5, { 150, 500, 1500, 5000 }, { -6, 0, 0, -12 },
          { { bqLowShelf, 120, -3, 0.7 }, { bqPeak, 520, 5, 1.0 }, { bqPeak, 1200, 3, 1.4 }, { bqHighShelf, 5000, -4, 0.7 } },
          1500, 1.0 },
        // French horn: bell faces away, highs strongly reduced
        { -10.0, -3.5, { 150, 400, 1000, 3000 }, { -3, 0, -4, -20 },
          { { bqLowShelf, 100, 1, 0.7 }, { bqPeak, 380, 5, 0.9 }, { bqPeak, 900, 2, 1.2 }, { bqHighShelf, 1800, -9, 0.7 } },
          1200, 1.0 },
        // Tuba
        { -10.0, -3.5, { 60, 250, 700, 2500 }, { -3, 0, -4, -20 },
          { { bqLowShelf, 50, -2, 0.7 }, { bqPeak, 230, 5, 0.9 }, { bqPeak, 600, 3, 1.2 }, { bqHighShelf, 1400, -8, 0.7 } },
          800, 1.0 },
        // Soprano sax
        { -5.0, -1.0, { 500, 1500, 3500, 9000 }, { -6, 0, 0, -10 },
          { { bqPeak, 900, 2, 1.5 }, { bqPeak, 1600, 5, 2.0 }, { bqPeak, 3200, 4, 2.5 }, { bqHighShelf, 8000, -3, 0.7 } },
          3500, 1.2 },
        // Alto sax
        { -5.5, -1.2, { 400, 1200, 3000, 8000 }, { -6, 0, -1, -12 },
          { { bqPeak, 700, 2, 1.5 }, { bqPeak, 1300, 5, 2.0 }, { bqPeak, 2700, 4, 2.5 }, { bqHighShelf, 7000, -3, 0.7 } },
          3000, 1.2 },
        // Tenor sax
        { -6.0, -1.3, { 250, 900, 2500, 7000 }, { -6, 0, -2, -14 },
          { { bqPeak, 500, 2, 1.5 }, { bqPeak, 950, 5, 2.0 }, { bqPeak, 2100, 4, 2.5 }, { bqHighShelf, 6000, -3, 0.7 } },
          2500, 1.2 },
        // Baritone sax
        { -6.5, -1.5, { 150, 600, 1800, 5000 }, { -6, 0, -3, -15 },
          { { bqPeak, 350, 3, 1.5 }, { bqPeak, 650, 5, 2.0 }, { bqPeak, 1500, 4, 2.5 }, { bqHighShelf, 4500, -4, 0.7 } },
          2000, 1.2 },
        // Synth: neutral (flat spectrum, no body)
        { -6.0, -1.0, { 100, 1000, 5000, 10000 }, { 0, 0, 0, 0 },
          { { bqPeak, 500, 0, 1.0 }, { bqPeak, 1000, 0, 1.0 }, { bqPeak, 2000, 0, 1.0 }, { bqPeak, 4000, 0, 1.0 } },
          3000, 1.0 },
    };
    return models[i < 0 ? 0 : (i >= kNumInstruments ? kNumInstruments - 1 : i)];
}

// Spectral envelope in dB at frequency hz: linear in log-frequency between the
// breakpoints, flat below the first, the last slope continued above the last.
inline double spectralEnvelopeDb (const InstrumentModel& m, double hz)
{
    hz = std::max (hz, 1.0);
    if (hz <= m.specHz[0]) return m.specDb[0];
    for (int i = 1; i < kSpecPoints; ++i)
    {
        if (hz <= m.specHz[i] || i == kSpecPoints - 1)
        {
            const double t = std::log (hz / m.specHz[i - 1]) / std::log (m.specHz[i] / m.specHz[i - 1]);
            return m.specDb[i - 1] + (m.specDb[i] - m.specDb[i - 1]) * t;
        }
    }
    return m.specDb[kSpecPoints - 1];
}

//==============================================================================
// Mutes (bus): up to four bands plus, for the plunger, a moving low-pass.
enum MuteType { muteOpen, muteStraight, muteCup, muteHarmon, mutePlungerType };

struct MuteModel { BandSpec bands[4]; double gainDb; };

inline const MuteModel& muteModel (int type)
{
    static const MuteModel models[5] = {
        { { { bqPeak, 1000, 0, 1 }, { bqPeak, 1000, 0, 1 }, { bqPeak, 1000, 0, 1 }, { bqPeak, 1000, 0, 1 } }, 0 },
        // straight: thin and nasal (low cut, strong 1.8 kHz resonance)
        { { { bqHighPass, 380, 0, 0.7 }, { bqPeak, 1800, 9, 2.2 }, { bqPeak, 5000, 3, 1.5 }, { bqHighShelf, 9000, -6, 0.7 } }, -3 },
        // cup: muffled, round
        { { { bqHighPass, 200, 0, 0.7 }, { bqPeak, 800, 4, 1.5 }, { bqLowPass, 2200, 0, 0.8 }, { bqPeak, 1000, 0, 1 } }, -2 },
        // harmon (stem out): buzzy, intense, far-away
        { { { bqHighPass, 700, 0, 0.8 }, { bqPeak, 1900, 12, 3.5 }, { bqPeak, 4200, 7, 3.0 }, { bqLowPass, 9000, 0, 0.7 } }, -9 },
        // plunger: slight body peak; the wah is the moving low-pass in the engine
        { { { bqPeak, 1100, 2, 1.0 }, { bqPeak, 1000, 0, 1 }, { bqPeak, 1000, 0, 1 }, { bqPeak, 1000, 0, 1 } }, 0 },
    };
    return models[type < 0 ? 0 : (type > 4 ? 4 : type)];
}
} // namespace bk
