#include "Analysis.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace sis {

PulseMetrics analyzeWaveform(const std::vector<uint16_t>& raw, const AnalysisConfig& cfg) {
    PulseMetrics m;
    const int N = (int)raw.size();
    if (N < cfg.baselineSamples + 4) return m;

    // baseline = mean of the first N pre-trigger samples
    int nb = std::min(cfg.baselineSamples, N);
    double sum = 0, sum2 = 0;
    for (int i = 0; i < nb; ++i) { sum += raw[i]; sum2 += double(raw[i]) * raw[i]; }
    m.baseline = sum / nb;
    double var = sum2 / nb - m.baseline * m.baseline;
    m.noiseRMS = var > 0 ? std::sqrt(var) : 0;

    // overall stats
    int mn = raw[0], mx = raw[0];
    double meanSum = 0;
    for (int i = 0; i < N; ++i) { mn = std::min(mn, int(raw[i])); mx = std::max(mx, int(raw[i])); meanSum += raw[i]; }
    m.minADC = mn; m.maxADC = mx; m.meanADC = meanSum / N; m.p2p = mx - mn;
    double s2 = 0; for (int i = 0; i < N; ++i) { double d = raw[i] - m.meanADC; s2 += d * d; }
    m.stddev = std::sqrt(s2 / N);

    // polarity: auto-detect from which deviation from baseline is larger
    int pol = cfg.polarity;
    if (pol == 0) pol = ((mx - m.baseline) >= (m.baseline - mn)) ? +1 : -1;

    // peak = largest polarity-corrected deviation from baseline
    int peakIdx = 0; double peakDev = -1e30;
    for (int i = 0; i < N; ++i) {
        double dev = pol * (raw[i] - m.baseline);
        if (dev > peakDev) { peakDev = dev; peakIdx = i; }
    }
    m.peakIndex = peakIdx;
    m.peakTimeNs = peakIdx * SAMPLE_NS;
    m.amplitude = peakDev;
    m.snr = m.noiseRMS > 0 ? m.amplitude / m.noiseRMS : 0;

    // charge-comparison gates relative to the peak
    int tStart = std::max(0, peakIdx - cfg.preGate);
    int tEnd   = std::min(N - 1, peakIdx + cfg.totalGate);
    int tailStart = std::min(N - 1, peakIdx + cfg.tailDelay);

    double qTot = 0, qTail = 0;
    for (int i = tStart; i <= tEnd; ++i) {
        double v = pol * (raw[i] - m.baseline);
        qTot += v;
        if (i >= tailStart) qTail += v;
    }
    m.qTotal = qTot * SAMPLE_NS;
    m.qTail  = qTail * SAMPLE_NS;
    m.psd = (qTot > 0) ? (qTail / qTot) : 0.0;

    // FWHM around the peak (half-maximum crossings, linear-interpolated)
    double half = peakDev * cfg.cfdFraction;
    double lo = peakIdx, hi = peakIdx;
    for (int i = peakIdx; i > 0; --i) {
        double a = pol * (raw[i] - m.baseline), b = pol * (raw[i - 1] - m.baseline);
        if (b < half) { lo = (i - 1) + (half - b) / std::max(1e-9, a - b); break; }
    }
    for (int i = peakIdx; i < N - 1; ++i) {
        double a = pol * (raw[i] - m.baseline), b = pol * (raw[i + 1] - m.baseline);
        if (b < half) { hi = i + (a - half) / std::max(1e-9, a - b); break; }
    }
    m.fwhmNs = (hi - lo) * SAMPLE_NS;

    m.valid = true;
    return m;
}

PulseMetrics analyzeEvent(const Sis3316Event& ev, const AnalysisConfig& cfg) {
    return analyzeWaveform(ev.raw, cfg);
}

FpgaPsd computeFpgaPsd(const uint32_t gate[4], int method) {
    (void)method;
    FpgaPsd r;
    const double g1 = double(gate[0]), g2 = double(gate[1]);   // prompt, total
    r.peakSum = g2;                        // total charge / energy proxy
    r.tail    = g2 - g1;                    // slow component
    if (g2 <= 0) return r;                  // no usable energy → invalid
    r.psdRatio = r.tail / r.peakSum;        // (gate2-gate1)/gate2 in [0,1]
    r.psdIndex = r.psdRatio * 65536.0;
    r.valid = true;
    return r;
}

// --------------------------------------------------------------------------
void Hist1D::fill(double x, double w) {
    if (n() == 0 || x < lo || x >= hi) return;
    int i = int((x - lo) / (hi - lo) * n());
    if (i >= 0 && i < n()) bins[i] += w;
}

GaussPeak describePeak(const Hist1D& h, int binLo, int binHi) {
    GaussPeak p;
    binLo = std::max(0, binLo); binHi = std::min(h.n() - 1, binHi);
    if (binHi <= binLo) return p;
    double s = 0, sx = 0, sxx = 0, peak = 0;
    for (int i = binLo; i <= binHi; ++i) {
        double c = h.binCenter(i), w = h.bins[i];
        s += w; sx += w * c; sxx += w * c * c;
        peak = std::max(peak, w);
    }
    if (s <= 0) return p;
    p.mean = sx / s;
    double v = sxx / s - p.mean * p.mean;
    p.sigma = v > 0 ? std::sqrt(v) : 0;
    p.fwhm = 2.3548200 * p.sigma;
    p.amplitude = peak;
    p.binLo = binLo; p.binHi = binHi;
    p.valid = p.sigma > 0;
    return p;
}

FomResult computeFOM(const Hist1D& psd) {
    FomResult r;
    const int N = psd.n();
    if (N < 8) return r;

    // light 3-bin smoothing
    std::vector<double> sm(N, 0.0);
    for (int i = 0; i < N; ++i) {
        double a = psd.bins[std::max(0, i - 1)], b = psd.bins[i], c = psd.bins[std::min(N - 1, i + 1)];
        sm[i] = (a + b + c) / 3.0;
    }
    // find the two highest local maxima separated by a valley
    std::vector<int> peaks;
    for (int i = 1; i < N - 1; ++i)
        if (sm[i] > sm[i - 1] && sm[i] >= sm[i + 1] && sm[i] > 0) peaks.push_back(i);
    if (peaks.size() < 2) return r;
    std::sort(peaks.begin(), peaks.end(),
              [&](int a, int b) { return sm[a] > sm[b]; });
    int p1 = peaks[0], p2 = peaks[1];
    if (p1 > p2) std::swap(p1, p2);                 // p1 = gamma (lower PSD), p2 = neutron

    // valley between the two peaks splits the fit windows
    int valley = p1; double vmin = sm[p1];
    for (int i = p1; i <= p2; ++i) if (sm[i] < vmin) { vmin = sm[i]; valley = i; }

    r.gamma   = describePeak(psd, std::max(0, 2 * p1 - valley), valley);
    r.neutron = describePeak(psd, valley, std::min(N - 1, 2 * p2 - valley));
    if (!r.gamma.valid || !r.neutron.valid) return r;

    r.separation = std::fabs(r.neutron.mean - r.gamma.mean);
    double denom = r.gamma.fwhm + r.neutron.fwhm;
    if (denom <= 0) return r;
    r.fom = r.separation / denom;
    r.valid = true;
    return r;
}

} // namespace sis
