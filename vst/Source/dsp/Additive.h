#pragma once
// ============================================================================
//  Additive harmonic layer.
//
//  Up to 32 harmonics, generated with the Chebyshev recurrence
//      sin(n t) = 2 cos(t) sin((n-1) t) - sin((n-2) t)
//  from one phase accumulator: one sin + one cos per sample for the whole stack,
//  exactly harmonic, and pitch glides / vibrato are free.
//
//  Each harmonic gets its own envelope. To keep that affordable, nine envelope
//  NODES sit at harmonics 1, 2, 3, 4, 6, 9, 14, 21, 32 and every harmonic
//  interpolates between its two neighbours (in log-harmonic). The node times
//  come from the LOW / MID / HIGH band envelopes (blended by position), and the
//  "spectral delay" macro staggers them: higher harmonics start later, swell
//  in more slowly and fade first: the brass signature.
//
//  Amplitudes per harmonic (dB) = instrument spectral envelope at n*f0
//                                + slope(dynamics) * (n - 1) + tilt * log2(n)
//  then odd/even weighting, a fade near Nyquist, and normalisation to unit power,
//  so dynamics changes the colour here and loudness is left to the dynamics range.
// ============================================================================
#include "Envelope.h"
#include "Instruments.h"

namespace bk
{
constexpr int kMaxPartials = 32;
constexpr int kAddNodes = 9;
constexpr double kAddNodeHarmonic[kAddNodes] = { 1, 2, 3, 4, 6, 9, 14, 21, 32 };

struct AddTables
{
    int lo[kMaxPartials];          // node below harmonic n+1
    double w[kMaxPartials];        // weight of the node above
    double log2n[kMaxPartials];
    double nodePos[kAddNodes];     // 0..1 position in the spectrum (log2(n) / 5)

    AddTables()
    {
        for (int k = 0; k < kMaxPartials; ++k)
        {
            const double n = k + 1;
            log2n[k] = std::log2 (n);
            int j = 0;
            while (j < kAddNodes - 2 && kAddNodeHarmonic[j + 1] < n) ++j;
            lo[k] = j;
            w[k] = (std::log (n) - std::log (kAddNodeHarmonic[j])) / (std::log (kAddNodeHarmonic[j + 1]) - std::log (kAddNodeHarmonic[j]));
        }
        for (int j = 0; j < kAddNodes; ++j) nodePos[j] = std::log2 (kAddNodeHarmonic[j]) / 5.0;
    }
};

inline const AddTables& addTables() { static const AddTables t; return t; }

// Low / mid / high band weights for a 0..1 spectrum position.
inline void bandWeights (double x, double& w0, double& w1, double& w2)
{
    w0 = clampd (1 - x / 0.45, 0.0, 1.0);
    w2 = clampd ((x - 0.55) / 0.45, 0.0, 1.0);
    w1 = 1 - w0 - w2;
}

struct AddVoice
{
    double phase = 0;
    double dt = 0;                       // ramp origin
    Envelope node[kAddNodes];
    double nodeLvl[kAddNodes] {};        // node level multipliers (band levels)
    double amp[kMaxPartials] {};         // ramp origin per harmonic
    int count = 0;                       // harmonics rendered last chunk

    bool active() const
    {
        for (const auto& e : node) if (e.active()) return true;
        return false;
    }
    void kill()
    {
        for (auto& e : node) e.kill();
        for (auto& a : amp) a = 0;
        count = 0;
    }
};
} // namespace bk
