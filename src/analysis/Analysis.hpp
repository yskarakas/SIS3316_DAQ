// ===========================================================================
//  Analysis.hpp — shared scientific analysis engine (live + offline)
//
//  Physically-grounded pulse analysis for EJ309 organic scintillators:
//   * waveform metrics (baseline, amplitude, charge, FWHM, SNR, ...),
//   * charge-comparison PSD  PSD = Q_tail / Q_total  (the standard n/γ
//     discrimination method; gates are defined relative to the pulse peak),
//   * Gaussian peak description and the discrimination Figure-of-Merit
//     FOM = |μ_n − μ_γ| / (FWHM_n + FWHM_γ).
//
//  Operates on decoded Sis3316Event objects so the same code serves the live
//  DAQ and the offline .bin/.root analyzer.
// ===========================================================================
#pragma once
#include <cstdint>
#include <vector>
#include "../daq/Event.hpp"

namespace sis {

constexpr double SAMPLE_NS = 4.0;     // 250 MSPS

struct AnalysisConfig {
    int    baselineSamples = 40;      // pre-trigger samples averaged for baseline
    int    polarity = 0;              // +1 positive, -1 negative, 0 = auto-detect
    int    preGate = 4;               // samples before peak included in total gate
    int    totalGate = 80;            // samples after peak for the total integral
    int    tailDelay = 8;             // samples after peak where the tail gate starts
    double cfdFraction = 0.5;         // for FWHM / timing
};

struct PulseMetrics {
    bool   valid = false;
    double baseline = 0;
    double amplitude = 0;             // peak deviation from baseline (polarity-corrected)
    int    peakIndex = 0;
    double peakTimeNs = 0;
    double noiseRMS = 0;
    double snr = 0;
    double qTotal = 0;                // total charge (ADC·ns), baseline-subtracted
    double qTail = 0;                 // tail charge
    double psd = 0;                   // qTail / qTotal
    double fwhmNs = 0;
    double minADC = 0, maxADC = 0, meanADC = 0, stddev = 0, p2p = 0;
};

// Analyse a raw waveform.
PulseMetrics analyzeWaveform(const std::vector<uint16_t>& raw, const AnalysisConfig& cfg);

// Convenience: analyse a parsed event (uses its raw samples).
PulseMetrics analyzeEvent(const Sis3316Event& ev, const AnalysisConfig& cfg);

// ---- FPGA-accumulator PSD (charge-comparison from the on-board integrals) --
//
//  The SIS3316 integrates configurable accumulator gates per event. In the
//  proven EJ-309 configuration, Gate 1 is the prompt/short integral and Gate 2
//  is the longer total integral, and the live analysis defines (matching the
//  reference SIS3316_LiveDAQ code):
//     energy / Peak-Sum = gate2                       (total charge)
//     slow / tail       = gate2 - gate1               (delayed light)
//     PSD               = (gate2 - gate1) / gate2      (tail fraction, 0..1)
//  A larger PSD means more slow scintillation light → neutron-like; smaller →
//  gamma-like. This uses values the FPGA already integrated, so it needs no raw
//  samples and matches the on-board Peak-Sum/PSD histograms.
//
//  gate[0]=Gate1 (prompt), gate[1]=Gate2 (total). gate[2],gate[3] unused here
//  but kept for callers that pass the full accumulator array.
struct FpgaPsd {
    bool   valid = false;
    double peakSum = 0;     // gate2 (total charge / energy proxy)
    double tail = 0;        // gate2 - gate1 (slow component)
    double psdRatio = 0;    // (gate2-gate1)/gate2  (dimensionless, 0..1)
    double psdIndex = 0;    // scaled index (tail/peakSum * 0x10000) if needed
};

// gate[0]=Gate1 prompt, gate[1]=Gate2 total. `method` kept for API stability.
FpgaPsd computeFpgaPsd(const uint32_t gate[4], int method = 1);

// ---- 1-D histogram + Gaussian description --------------------------------
struct Hist1D {
    double lo = 0, hi = 1;
    std::vector<double> bins;
    Hist1D() = default;
    Hist1D(int n, double a, double b) : lo(a), hi(b), bins(n, 0.0) {}
    int n() const { return (int)bins.size(); }
    void fill(double x, double w = 1.0);
    double binCenter(int i) const { return lo + (i + 0.5) * (hi - lo) / n(); }
};

struct GaussPeak {
    bool   valid = false;
    double mean = 0, sigma = 0, fwhm = 0, amplitude = 0;
    int    binLo = 0, binHi = 0;
};

// Describe a single peak inside [binLo,binHi] by weighted moments (robust).
GaussPeak describePeak(const Hist1D& h, int binLo, int binHi);

// Locate the two dominant peaks (γ then n) in a PSD distribution and return
// the discrimination Figure-of-Merit.
struct FomResult {
    bool   valid = false;
    GaussPeak gamma, neutron;
    double separation = 0;
    double fom = 0;
};
FomResult computeFOM(const Hist1D& psdHist);

} // namespace sis
