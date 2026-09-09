// ===========================================================================
//  Sis3316Daq.hpp  —  C++ Ethernet driver for the Struck SIS3316 digitizer
//
//  Faithful C++ port of the *newer-firmware* UDP protocol used by the proven
//  Python stack (sis3316_eth_new.py): packet-ID request framing, link-interface
//  reads/writes (0x10/0x11), VME reads/writes (0x20/0x21), grant arbitration,
//  and the chunked DDR-memory FIFO read (0x30) with a congestion window.
//
//  Verified against the real board at 192.168.1.10:1234 (MODID 0x33164001).
//  This is the live-acquisition core of the unified SIS3316 Studio application.
// ===========================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <stdexcept>
#include "../daq/Event.hpp"
#include "Sis3316Registers.hpp"

namespace sis {

// ---- decoded hardware-info structs (for GUI + ROOT config snapshot) -------
struct FirmwareInfo {
    uint32_t raw = 0;
    uint32_t type = 0;        // 0x0250 = 250 MHz ADC
    uint32_t version = 0;     // 0x02 = Neutron/Gamma PSD
    uint32_t revision = 0;    // 0x01 (2015) or 0x02 (2018)
    bool isPsdFirmware() const { return type == 0x0250 && version == 0x02; }
};

struct AdcStatus {
    uint32_t raw = 0;
    bool histClearBusy = false;
    bool dcmOk = false;
    bool memory1Ok = false;
    bool memory2Ok = false;
};

// 11 documented statistic counters per channel-in-group (manual pages 23-24).
struct StatCounters {
    uint32_t c[reg::CH_PER_GROUP][reg::SC_COUNT] = {};
};

// Full configuration snapshot — every PSD-firmware register that defines how
// the board acquires/processes data. Streamed into the ROOT HardwareConfig tree.
struct ConfigSnapshot {
    // per group (4 groups of 4 channels)
    uint32_t eventConfig[4]{};
    uint32_t channelHeaderId[4]{};
    uint32_t dataFormatConfig[4]{};
    uint32_t rawDataConfig[4]{};
    uint32_t activeTrigGateLen[4]{};
    uint32_t preTrigDelay[4]{};
    uint32_t pileupConfig[4]{};
    uint32_t accGateConfig[4][4]{};        // [group][gate 1..4]
    uint32_t tofActiveWindow[4]{};
    uint32_t generalHistConfig[4]{};
    uint32_t tofHistConfig[4]{};
    uint32_t shapeHistXConfig[4]{};
    uint32_t shapeHistYConfig[4]{};
    uint32_t peakSumHistConfig[4]{};
    uint32_t firmwareType[4]{}, firmwareVersion[4]{}, firmwareRevision[4]{};
    // per channel (16)
    uint32_t firTrigSetup[16]{};
    uint32_t trigThreshold[16]{};
    uint32_t heTrigThreshold[16]{};
};

// ---- register map (subset needed for acquisition) -------------------------
constexpr uint32_t REG_MODID                     = 0x04;
constexpr uint32_t REG_HW_VERSION                = 0x1C;
constexpr uint32_t REG_INTERFACE_ARBITRATION     = 0x10;   // link reg
// Temperature register. Manual (PSD V102, page 30, "VME FPGA registers"):
//   0x00000020  R   Temperature register
// The previous value 0x40 actually addressed "Programmable ADC Clock:
// Oscillator I2C register" — reading it returned garbage temperatures.
constexpr uint32_t REG_INTERNAL_TEMPERATURE      = 0x20;
constexpr uint32_t REG_ACQUISITION_CONTROL       = 0x60;
constexpr uint32_t REG_DATA_TRANSFER_GRP_CTRL    = 0x80;   // +4*grp
constexpr uint32_t REG_KEY_DISARM                = 0x414;
constexpr uint32_t REG_KEY_TIMESTAMP_CLEAR       = 0x41C;
constexpr uint32_t REG_KEY_DISARM_AND_ARM_BANK1  = 0x420;
constexpr uint32_t REG_KEY_DISARM_AND_ARM_BANK2  = 0x424;

constexpr uint32_t FPGA_GRP_REG_BASE   = 0x1000;
constexpr uint32_t FPGA_GRP_REG_OFFSET = 0x1000;
constexpr uint32_t FPGA_GRP_MEM_BASE   = 0x100000;
constexpr uint32_t FPGA_GRP_MEM_OFFSET = 0x100000;
constexpr uint32_t PREVIOUS_BANK_SAMPLE_ADDRESS_REG = 0x120;   // per-group, +4*cid
constexpr uint32_t ACTUAL_SAMPLE_ADDRESS_REG        = 0x110;
constexpr uint32_t RAW_DATA_BUFFER_CONFIG_REG       = 0x20;    // per-group
constexpr uint32_t DATAFORMAT_CONFIG_REG            = 0x30;    // per-group, 8*cid field
constexpr uint32_t MAW_TEST_BUFFER_CONFIG_REG       = 0x34;    // per-group

constexpr uint32_t ACQ_BIT_ARMED              = 1u << 16;
constexpr uint32_t ACQ_BIT_BANK               = 1u << 17;
constexpr uint32_t ACQ_BIT_THRESHOLD_OVERRUN  = 1u << 19;
constexpr uint32_t BIT_TRANSFER_BUSY          = 1u << 31;

constexpr int VME_READ_LIMIT  = 64;
constexpr int VME_WRITE_LIMIT = 64;
constexpr int FIFO_READ_LIMIT = 0x40000 / 4;   // 32-bit words
constexpr int CHAN_TOTAL = 16;

inline uint32_t grpReg(uint32_t reg, int gid) {
    return reg + FPGA_GRP_REG_BASE + FPGA_GRP_REG_OFFSET * uint32_t(gid);
}

struct DaqError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Sis3316Daq {
public:
    Sis3316Daq(std::string host, uint16_t port = 1234);
    ~Sis3316Daq();

    void open();                 // bind socket + request link grant
    void close();
    bool isOpen() const { return fSock >= 0; }

    // --- single register access -------------------------------------------
    uint32_t read(uint32_t addr);
    void     write(uint32_t addr, uint32_t data);

    // --- board info --------------------------------------------------------
    uint32_t moduleId()        { return readLink(REG_MODID); }
    uint32_t hardwareVersion() { return readLink(REG_HW_VERSION); }
    double   temperatureC();

    // --- acquisition control ----------------------------------------------
    void timestampClear() { write(REG_KEY_TIMESTAMP_CLEAR, 0); }
    void disarm()         { write(REG_KEY_DISARM, 0); }
    void armBank1()       { write(REG_KEY_DISARM_AND_ARM_BANK1, 0); }
    void armBank2()       { write(REG_KEY_DISARM_AND_ARM_BANK2, 0); }

    uint32_t acquisitionStatus() { return read(REG_ACQUISITION_CONTROL); }
    bool     armed()             { return acquisitionStatus() & ACQ_BIT_ARMED; }
    int      currentBank()       { return (acquisitionStatus() & ACQ_BIT_BANK) ? 1 : 0; }
    bool     thresholdOverrun()  { return acquisitionStatus() & ACQ_BIT_THRESHOLD_OVERRUN; }
    void     memToggle();        // arm the opposite bank (no-gap double-bank)

    // Per-channel hit/event format, read from the board's format registers
    // (mirrors channel.event_stats in the Python driver).
    EventFormat channelEventFormat(int channel);

    // Number of 32-bit words stored in the previous bank for a physical
    // channel 0..15.
    uint32_t previousBankWords(int channel);

    // Cumulative count of dropped/out-of-sequence FIFO packets observed during
    // memory reads (health diagnostic, Phase 3.8). Monotonic since open().
    uint64_t droppedPackets() const { return fDroppedPackets; }

    // Read the previous-bank FIFO memory for one channel into `out` (appended).
    // Returns number of 32-bit words read.
    uint32_t readChannelPreviousBank(int channel, int prevBank,
                                     std::vector<uint32_t>& out);

    // --- hardware info / diagnostics --------------------------------------
    FirmwareInfo firmwareInfo(int group);
    AdcStatus    adcStatus(int group);

    // Clear all on-board histograms (key address, ~35 ms blocking). Caller
    // should disarm sampling first if it cares about a clean boundary.
    void histogramsClear() { write(reg::KEY_HISTOGRAMS_CLEAR, 0); }

    // Read the 11 documented statistic counters for the 4 channels of a group
    // via the data-transfer FSM (manual pages 23-24).
    StatCounters readStatisticCounters(int group);

    // Read one on-board histogram for a physical channel into `out` (resized).
    // NOTE: documented per manual pages 27/43-49 but NOT hardware-verified in
    // this environment — see header banner. Returns number of 32-bit bins read.
    uint32_t readHistogram(int channel, reg::HistType type,
                           std::vector<uint32_t>& out);

    // Read every configuration register into a snapshot (for ROOT + the editor).
    ConfigSnapshot readConfigSnapshot();

    // Generic per-group register access (absolute helpers for the config editor).
    uint32_t readGroupReg(int group, uint32_t off)  { return read(reg::groupReg(off, group)); }
    void     writeGroupReg(int group, uint32_t off, uint32_t v) { write(reg::groupReg(off, group), v); }

    // Write a register and read it back; throws if the readback differs (for
    // R/W registers only). Returns the read-back value.
    uint32_t writeVerify(uint32_t addr, uint32_t value);

private:
    // protocol primitives
    void     reqSend(const uint8_t* msg, size_t len);
    int      respRecv(uint8_t* buf, size_t cap, double timeoutSec);
    uint8_t  nextId();

    uint32_t readLink(uint32_t addr);
    void     writeLink(uint32_t addr, uint32_t data);
    uint32_t readVme(uint32_t addr);
    void     readVme(const std::vector<uint32_t>& addrs, std::vector<uint32_t>& out);
    void     writeVme(uint32_t addr, uint32_t data);

    void fifoTransferReset(int gid);
    void fifoTransferReadSetup(int gid, int memNo, uint32_t woffset, uint32_t spaceBits = 0);
    uint32_t fifoRead(int gid, int memNo, uint32_t nwords, uint32_t woffset,
                      std::vector<uint32_t>& out, uint32_t spaceBits = 0);

    void cleanupSocket();

    std::string fHost;
    uint16_t    fPort;
    int         fSock = -1;
    uint8_t     fNextId = 0;
    uint8_t     fReqId = 0;
    double      fDefaultTimeout = 0.1;
    uint64_t    fDroppedPackets = 0;
};

} // namespace sis
