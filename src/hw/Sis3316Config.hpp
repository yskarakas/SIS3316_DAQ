// ===========================================================================
//  Sis3316Config.hpp — full board configuration (the piece that makes the board
//  actually acquire).
//
//  A blank/reset SIS3316 has ALL trigger-enable bits, thresholds and raw-window
//  lengths = 0, so no channel triggers and no waveforms are stored — the board
//  looks "connected but silent". This applies a complete configuration, ported
//  faithfully from the proven Python `module_manager.set_config` +
//  `channel.py`/`group.py`/`triggers.py` sequence, with the EJ-309 16-channel
//  working defaults (sample length 300, threshold 1200, internal trigger on all
//  channels, PSD accumulator gates 1&2, etc.).
//
//  Every register value/bit layout below was cross-checked against those files
//  and the register map; nothing is guessed.
// ===========================================================================
#pragma once
#include <cstdint>
#include <array>
#include <string>

namespace sis {

class Sis3316Daq;

struct BoardConfig {
    // ---- per-channel (index 0..15) ----
    std::array<bool,16> internalTrigger;   // enable internal (self) trigger
    std::array<bool,16> sumTrigger;         // save channel on group sum-trigger
    std::array<bool,16> invert;             // input invert (negative signals)
    std::array<int,16>  threshold;          // trigger threshold ABOVE baseline
    std::array<int,16>  cfd;                // 0=off, 3=CFD 50%
    std::array<int,16>  heThreshold;        // high-energy suppress (0 = disabled)
    std::array<int,16>  peaking;            // FIR trigger peaking time (samples)
    std::array<int,16>  gap;                // FIR trigger gap time (samples)
    std::array<int,16>  dacOffset;          // DAC baseline offset (0..65535)
    std::array<bool,16> termination;        // 50 Ohm termination on/off
    std::array<int,16>  gain;               // 0=5V,1=2V,2=1.9V input range

    // ---- per-group / global ----
    int sampleLength = 300;                 // raw waveform window (samples)
    int sampleStart  = 0;                   // raw buffer start index
    int preTrigger   = 100;                 // pre-trigger delay (samples)
    int gateWindow   = 100;                 // active trigger gate window
    int addrThreshold = 1000000;            // bank almost-full address threshold
    // PSD accumulator gates — NESTED so PSD = (Gate2-Gate1)/Gate2 is a true tail
    // fraction: Gate1 (short/prompt) is a subset of Gate2 (long/total), both
    // starting at the same index. Gate2 spans the whole pulse (prompt + tail);
    // Gate2-Gate1 is the delayed light. Disjoint gates give an unphysical,
    // possibly-negative ratio (see physics review).
    std::array<int,4> accStart  = {0, 0, 0, 0};    // gate start (g1..g4), same start
    std::array<int,4> accLength = {7, 40, 0, 0};   // g1 short (prompt) ⊂ g2 long (total)
    bool saveAcc16 = true;                  // save peak-high + accumulators 1-6
    bool saveAcc78 = true;                  // save accumulators 7,8
    bool saveEnergyMaw = true;              // save start/max energy MAW
    int  enPeaking = 10, enGap = 10;        // energy filter peaking/gap
    bool resetDcmTapDelay = true;           // DCM reset + ADC tap-delay calibrate

    // ---- hardware conditional (coincidence) trigger — Trigger Coincidence Lookup
    //      Table + internal feedback (see Struck reference & manual §3.6.4). When
    //      coincMode != 0 the member channels record ONLY when the condition on
    //      the selected channels is satisfied within the coincidence window. The
    //      member channels still self-trigger (to feed the table) but save an event
    //      only on the coincidence, fed back internally as the "external" trigger.
    int coincMode = 0;                      // 0=off, 1=AND, 2=OR, 3=multiplicity(≥coincMinMult)
    int coincMinMult = 2;                   // for multiplicity mode
    std::array<bool,16> coincMembers{};     // participating channels (ADC readout index)
    int coincWindowSamples = 8;             // coincidence window (FIR trigger pulse length, 4 ns each)

    // EJ-309 16-channel working defaults (matches
    // sample_configs/EJ309_16ch_timestamp_coincidence_multiplicity2.json).
    BoardConfig() {
        for (int c = 0; c < 16; ++c) {
            internalTrigger[c] = true;
            sumTrigger[c]      = true;
            invert[c]          = false;
            threshold[c]       = 1200;
            cfd[c]             = 0;
            heThreshold[c]     = 1500;
            peaking[c]         = 10;
            gap[c]             = 10;
            // DAC baseline offset. Signals here are POSITIVE-going (verified on
            // the real board: pulses rise from baseline), so the baseline must
            // sit LOW to give maximum upward headroom. baseline_ADC ≈ dacOffset/4
            // (measured). dacOffset=10000 → baseline ≈2100 ADC → ~14200 counts of
            // headroom for positive pulses. The old mid-scale value (32768 →
            // baseline ≈8200) wasted the lower half of the range and clipped
            // 1–3 % of events (large/muon pulses) at the 16383 ceiling; this
            // value eliminates that (measured 0.0 % saturation, muons reach
            // ~14000 ADC). For negative-going signals use invert + a high offset.
            dacOffset[c]       = 10000;
            termination[c]     = true;
            gain[c]            = 0;
        }
    }
};

// Apply a full configuration to the board (link must already be open/granted).
// Throws sis::DaqError on any register access failure. Leaves the board
// configured but DISARMED — the caller arms afterwards.
void applyConfig(Sis3316Daq& daq, const BoardConfig& cfg);

} // namespace sis
