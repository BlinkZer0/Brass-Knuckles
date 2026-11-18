#pragma once
// ============================================================================
//  Additive harmonic layer.
//
//  Up to 32 harmonics, generated with the Chebyshev recurrence
//      sin(n t) = 2 cos(t) sin((n-1) t) - sin((n-2) t)
//  from one rotating phasor (cos t, sin t): four multiplies per sample for the
//  whole stack, exactly harmonic, and pitch glides / vibrato are free (the
//  rotation step follows the pitch once per 32-sample chunk).
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
    double logn[kMaxPartials];     // natural log of the harmonic number
    double nodePos[kAddNodes];     // 0..1 position in the spectrum (log2(n) / 5)
    double nodeDly[kAddNodes];     // nodePos^0.8 (spectral delay shape)
    double nodeW[kAddNodes][3];    // low / mid / high band weights
    int spf[kMaxPartials + 1];     // smallest prime factor of n (power laws: f(n) = f(p) f(n/p))

    AddTables()
    {
        spf[0] = spf[1] = 1;
        for (int n = 2; n <= kMaxPartials; ++n)
        {
            int p = 2;
            while (n % p != 0) ++p;
            spf[n] = p;
        }
        for (int k = 0; k < kMaxPartials; ++k)
        {
            const double n = k + 1;
            log2n[k] = std::log2 (n);
            logn[k] = std::log (n);
            int j = 0;
            while (j < kAddNodes - 2 && kAddNodeHarmonic[j + 1] < n) ++j;
            lo[k] = j;
            w[k] = (std::log (n) - std::log (kAddNodeHarmonic[j])) / (std::log (kAddNodeHarmonic[j + 1]) - std::log (kAddNodeHarmonic[j]));
        }
        for (int j = 0; j < kAddNodes; ++j)
        {
            nodePos[j] = std::log2 (kAddNodeHarmonic[j]) / 5.0;
            nodeDly[j] = std::pow (nodePos[j], 0.8);
            bandWeights (nodePos[j], nodeW[j][0], nodeW[j][1], nodeW[j][2]);
        }
    }

    // Low / mid / high band weights for a 0..1 spectrum position.
    static void bandWeights (double x, double& w0, double& w1, double& w2)
    {
        w0 = clampd (1 - x / 0.45, 0.0, 1.0);
        w2 = clampd ((x - 0.55) / 0.45, 0.0, 1.0);
        w1 = 1 - w0 - w2;
    }
};

inline const AddTables& addTables() { static const AddTables t; return t; }

// The normalised spectrum of one note for one chunk. All players of a note share it
// (same dynamics, pitch within a few cents), so it is computed once per note group.
struct AddSpectrum
{
    int group = -1;
    uint64_t stamp = 0;
    int count = 0;
    double lin[kMaxPartials] {};   // already normalised and scaled by the layer level
};

struct AddVoice
{
    // fundamental as a rotating phasor (cos, sin): no sin/cos per sample; the rotation step is
    // set once per chunk and the length renormalised, so it never drifts
    double c = 1, s = 0;
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
