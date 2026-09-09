// ===========================================================================
//  Event.hpp — decoded SIS3316 hit/event + on-the-fly buffer parser
//
//  Faithful C++ port of processing/parser.py (the proven Python parser),
//  including the self-describing raw-sample count and the SIS3316 hit format:
//
//    w0 : [31:16] timestamp[47:32]   [15:4] det   [3:0] format
//    w1 : timestamp[31:0]
//    -- if Accumulator Gates 1-6 flag --
//    w2 : [15:0] peak ADC max   [31:16] peak index (argmax)
//    w3 : [23:0] gate1          [31:28] info (pileup/over/underflow)
//    w4..w8 : gate2..gate6
//    -- if Accumulator Gates 7-8 flag --
//    w9,w10 : gate7,gate8
//    -- (optional MAW / MAW-energy blocks) --
//    wK : [25:0] raw-sample 32-bit-word count   [31:26] 0xE marker / status
//    raw 32-bit words (each = two 16-bit ADC samples, low then high)
// ===========================================================================
#pragma once
#include <cstdint>
#include <vector>

namespace sis {

// Per-channel hit/event format, derived from the board's format registers.
struct EventFormat {
    bool acc1 = false;        // Accumulator Gates 1-6
    bool acc2 = false;        // Accumulator Gates 7-8
    bool maw = false;         // MAW test values
    bool mawEnergy = false;   // start/max energy MAW
    bool mawEna = false;      // MAW samples saved
    int  nraw16 = 0;          // raw samples (16-bit words), from raw_window
    int  nmaw = 0;            // MAW test buffer length
    int  eventLen32 = 0;      // 32-bit words per event (event boundary)

    bool valid() const { return eventLen32 > 0; }
};

struct Sis3316Event {
    uint64_t rid = 0;
    int      channel = 0;     // physical channel 0..15 (from readout, authoritative)
    int      det = 0;         // header det field
    uint64_t timestamp = 0;   // 48-bit
    uint32_t adcMax = 0;
    uint32_t adcArgmax = 0;
    uint32_t gate[8] = {0,0,0,0,0,0,0,0};   // gate1..gate8 (0 if absent)
    bool     hasAcc1 = false;
    bool     hasAcc2 = false;
    int      pileup = 0;
    std::vector<uint16_t> raw;              // raw ADC samples

    double timestampSeconds() const { return double(timestamp) / 250000000.0; }
};

// Parse one channel's previous-bank buffer (32-bit words) into events.
// `ridCounter` is advanced like the Python parser's running event_id.
std::vector<Sis3316Event> parseBuffer(const uint32_t* words, size_t nwords,
                                      const EventFormat& fmt, int channel,
                                      uint64_t& ridCounter);

} // namespace sis
