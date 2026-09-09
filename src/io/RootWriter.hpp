// ===========================================================================
//  RootWriter.hpp — stable, self-describing ROOT output for SIS3316 Studio.
//
//  ROOT is the primary scientific data format (directive Phase 3.7). This class
//  streams live acquisition directly to a `.root` file — there is NO intermediate
//  .bin format. ROOT headers are fully hidden behind a PIMPL so the Qt/GUI code
//  never includes ROOT (strict separation of concerns, Phase 4).
//
//  File schema (three TTrees) — see RootWriter.cpp for the authoritative branch
//  list and the schema-version constant:
//    * "Events"         one entry per hit (waveform + FPGA + software metrics)
//    * "HardwareConfig" one entry: full register snapshot at acquisition start
//    * "RunSummary"     one entry: run statistics at acquisition stop
// ===========================================================================
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include "../hw/Sis3316Daq.hpp"   // ConfigSnapshot, StatCounters

namespace sis {

// One row of the "Events" tree. `raw` may be null/empty if raw samples were
// not saved for this event.
struct RootEvent {
    int      channel = 0;       // physical channel 0..15
    int      det = 0;           // header Channel ID
    uint64_t timestamp = 0;     // 48-bit FPGA timestamp (4 ns ticks)
    double   timestampSec = 0;
    uint32_t adcMax = 0;
    uint32_t adcArgmax = 0;
    uint32_t acc1 = 0, acc2 = 0, acc3 = 0, acc4 = 0;
    double   psdFpga = 0;       // tail/peakSum from FPGA accumulators
    double   peakSumFpga = 0;   // Acc2 - Acc4
    double   psdSw = 0;         // software charge-comparison PSD (raw waveform)
    double   qTotalSw = 0;
    double   qTailSw = 0;
    double   baselineSw = 0;
    double   amplitudeSw = 0;
    int      pileup = 0;        // bit0 pileup, bit1 repileup, bit2 under, bit3 over
    const std::vector<uint16_t>* raw = nullptr;
};

struct RootRunSummary {
    uint64_t totalEvents = 0;
    double   durationSec = 0;
    double   rateHz[16] = {};       // per-channel software event rate
    uint32_t counts[16] = {};       // per-channel accepted event counts
    uint32_t pileup[16] = {};       // per-channel pile-up flagged events
    double   avgTempC = 0;
    double   deadTimeFrac = 0;
    uint64_t udpPacketsDropped = 0;
};

class RootWriter {
public:
    RootWriter();
    ~RootWriter();
    RootWriter(const RootWriter&) = delete;
    RootWriter& operator=(const RootWriter&) = delete;

    // Create `<studioRoot>/Data/` (if absent) and open a new file named
    // YYYY-MM-DD_HH-MM-SS.root inside it. Returns the absolute file path, or an
    // empty string on failure (see lastError()).
    std::string open(const std::string& studioRoot);

    bool isOpen() const;
    void writeConfig(const ConfigSnapshot& cfg);
    void fillEvent(const RootEvent& e);
    void writeRunSummary(const RootRunSummary& s);
    void close();

    long long entries() const;          // events written so far
    std::string path() const;
    long long fileSizeBytes() const;    // valid after close()
    std::string lastError() const;

    static int schemaVersion();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace sis
