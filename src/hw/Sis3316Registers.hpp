// ===========================================================================
//  Sis3316Registers.hpp — COMPLETE, DOCUMENTATION-VALIDATED register map for
//  the SIS3316 Neutron/Gamma PSD firmware (ADC FPGA V0250-0202).
//
//  EVERY offset, bit field and physical meaning below was cross-checked against
//  "SIS3316-M-1-1-V102_NeutronGamma_PSD" (the manual shipped with this project).
//  The page reference is given in each block so any value can be re-verified.
//
//  Addressing model (manual §3.1, page 29):
//    - Global VME FPGA registers : 0x0000_0000 .. 0x0000_00FC
//    - Key addresses (write-only) : 0x0000_0400 .. 0x0000_043C
//    - Per-group ADC FPGA regs    : 0x00g0_1000  (g = group 0..3, stride 0x1000)
//        i.e. group base = 0x1000 + g*0x1000 = 0x1000, 0x2000, 0x3000, 0x4000
//    - Per-group memory FIFO       : 0x10_0000 + g*0x10_0000
//
//  A "group" is 4 ADC channels: group 0 = ch1-4, group 1 = ch5-8,
//  group 2 = ch9-12, group 3 = ch13-16. A physical channel 0..15 maps to
//  group = ch/4, channel-in-group (cid) = ch%4.
// ===========================================================================
#pragma once
#include <cstdint>

namespace sis::reg {

// --------------------------------------------------------------------------
//  Group geometry
// --------------------------------------------------------------------------
constexpr int      N_CHANNELS   = 16;
constexpr int      N_GROUPS     = 4;
constexpr int      CH_PER_GROUP = 4;
constexpr double   ADC_CLOCK_HZ = 250'000'000.0;   // 250 MSPS, 4 ns sample period
constexpr double   SAMPLE_NS    = 4.0;

constexpr uint32_t GROUP_REG_BASE   = 0x1000;   // group 0 register base
constexpr uint32_t GROUP_REG_STRIDE = 0x1000;
constexpr uint32_t GROUP_MEM_BASE   = 0x100000; // group 0 memory FIFO base
constexpr uint32_t GROUP_MEM_STRIDE = 0x100000;

// Absolute register address for a per-group register `off` in group `g`.
constexpr uint32_t groupReg(uint32_t off, int g) {
    return GROUP_REG_BASE + GROUP_REG_STRIDE * uint32_t(g) + off;
}

// --------------------------------------------------------------------------
//  GLOBAL VME FPGA REGISTERS  (manual page 30, R access always possible)
// --------------------------------------------------------------------------
constexpr uint32_t CONTROL_STATUS          = 0x00000000;  // J-K control/status
constexpr uint32_t MODID                   = 0x00000004;  // module id + fw rev
constexpr uint32_t IRQ_CONFIG              = 0x00000008;
constexpr uint32_t IRQ_CONTROL             = 0x0000000C;
constexpr uint32_t INTERFACE_ARBITRATION   = 0x00000010;  // link grant
constexpr uint32_t HARDWARE_VERSION        = 0x0000001C;
constexpr uint32_t TEMPERATURE             = 0x00000020;  // R: board temperature
constexpr uint32_t ONEWIRE_CTRL            = 0x00000024;
constexpr uint32_t SERIAL_NUMBER           = 0x00000028;  // R
constexpr uint32_t ADC_CLOCK_OSC_I2C       = 0x00000040;  // NOT temperature!
constexpr uint32_t ADC_SAMPLE_CLOCK_DIST   = 0x00000050;
constexpr uint32_t NIM_IN_CTRL_STATUS      = 0x0000005C;
constexpr uint32_t ACQUISITION_CONTROL     = 0x00000060;  // acq control/status
constexpr uint32_t LEMO_OUT_CO_SELECT      = 0x00000070;
constexpr uint32_t LEMO_OUT_TO_SELECT      = 0x00000074;
constexpr uint32_t LEMO_OUT_UO_SELECT      = 0x00000078;

// Per-group data-transfer control / status (manual page 31). +4*group.
constexpr uint32_t DATA_TRANSFER_CTRL      = 0x00000080;  // +4*g (R/W)
constexpr uint32_t DATA_TRANSFER_STATUS    = 0x00000090;  // +4*g (R)
constexpr uint32_t VME_ADC_DATA_LINK_STAT  = 0x000000A0;

// Acquisition control/status bits (manual standard register; used by driver)
constexpr uint32_t ACQ_BIT_ARMED             = 1u << 16;
constexpr uint32_t ACQ_BIT_BANK              = 1u << 17;  // 0=bank1, 1=bank2 armed
constexpr uint32_t ACQ_BIT_THRESHOLD_OVERRUN = 1u << 19;

// Data-transfer control: command in bits[31:30], mem-select bit 28, word offset.
constexpr uint32_t XFER_CMD_READ        = 0b10u << 30;
constexpr uint32_t XFER_MEM2_SELECT     = 1u << 28;
constexpr uint32_t XFER_STATUS_BUSY     = 1u << 31;
// Data-transfer space selectors written into bits[31:28] of the control reg
// (manual page 24, statistic-counter read example): 0x80000000 | space.
constexpr uint32_t XFER_SPACE_MEMORY    = 0x00000000;
constexpr uint32_t XFER_SPACE_STAT_CTR  = 0x30000000;   // statistic counters

// --------------------------------------------------------------------------
//  KEY ADDRESSES  (manual page 32, write any datum to invoke)
// --------------------------------------------------------------------------
constexpr uint32_t KEY_RESET                 = 0x00000400;
constexpr uint32_t KEY_HISTOGRAMS_CLEAR      = 0x00000404;  // ~35 ms blocking
constexpr uint32_t KEY_ARM_SAMPLE_SINGLE     = 0x00000410;
constexpr uint32_t KEY_DISARM_SAMPLE         = 0x00000414;
constexpr uint32_t KEY_TRIGGER               = 0x00000418;
constexpr uint32_t KEY_TIMESTAMP_CLEAR       = 0x0000041C;
constexpr uint32_t KEY_DISARM_AND_ARM_BANK1  = 0x00000420;
constexpr uint32_t KEY_DISARM_AND_ARM_BANK2  = 0x00000424;
constexpr uint32_t KEY_RESET_ADC_FPGA_LOGIC  = 0x00000434;
constexpr uint32_t KEY_ADC_CLOCK_PLL_RESET   = 0x00000438;

// --------------------------------------------------------------------------
//  PER-GROUP ADC FPGA REGISTERS  (offsets relative to group base; manual
//  pages 33-35). Use groupReg(off, group).
// --------------------------------------------------------------------------
constexpr uint32_t ADC_INPUT_TAP_DELAY       = 0x000;
constexpr uint32_t ADC_GAIN_TERMINATION      = 0x004;
constexpr uint32_t ADC_OFFSET_DAC            = 0x008;
constexpr uint32_t ADC_SPI_CTRL              = 0x00C;
constexpr uint32_t EVENT_CONFIG              = 0x010;  // trigger enable/invert
constexpr uint32_t CHANNEL_HEADER_ID         = 0x014;
constexpr uint32_t END_ADDRESS_THRESHOLD     = 0x018;
constexpr uint32_t ACTIVE_TRIGGER_GATE_LEN   = 0x01C;
constexpr uint32_t RAW_DATA_BUFFER_CONFIG    = 0x020;  // [31:16]=window,[15:0]=start
constexpr uint32_t PILEUP_CONFIG             = 0x024;
constexpr uint32_t PRE_TRIGGER_DELAY         = 0x028;
constexpr uint32_t DATAFORMAT_CONFIG         = 0x030;  // MAW save enable per ch
constexpr uint32_t MAW_TEST_BUFFER_CONFIG    = 0x034;
constexpr uint32_t INTERNAL_TRIGGER_DELAY    = 0x038;
constexpr uint32_t INTERNAL_GATE_LEN_COINC   = 0x03C;

// FIR trigger setup / thresholds — per channel-in-group (cid 0..3), stride 0x10
constexpr uint32_t FIR_TRIGGER_SETUP_BASE    = 0x040;  // +0x10*cid
constexpr uint32_t TRIGGER_THRESHOLD_BASE    = 0x044;  // +0x10*cid
constexpr uint32_t HE_TRIGGER_THRESHOLD_BASE = 0x048;  // +0x10*cid
constexpr uint32_t firTriggerSetup(int cid)    { return FIR_TRIGGER_SETUP_BASE    + 0x10u*uint32_t(cid); }
constexpr uint32_t triggerThreshold(int cid)   { return TRIGGER_THRESHOLD_BASE    + 0x10u*uint32_t(cid); }
constexpr uint32_t heTriggerThreshold(int cid) { return HE_TRIGGER_THRESHOLD_BASE + 0x10u*uint32_t(cid); }

// FIR Trigger Setup bit fields (verified against reference driver triggers.py).
//   peaking time  = bits[11:0]   (0xFFF << 0)
//   gap   time    = bits[23:12]  (0xFFF << 12)
//   NIM out length= bits[31:24]  (0xFE  << 24)
constexpr uint32_t FIRSETUP_PEAKING_MASK = 0xFFF;  constexpr int FIRSETUP_PEAKING_SHIFT = 0;
constexpr uint32_t FIRSETUP_GAP_MASK     = 0xFFF;  constexpr int FIRSETUP_GAP_SHIFT     = 12;
// FIR Trigger Threshold bit fields (verified against reference driver triggers.py).
//   threshold value = bits[27:0]  — COMPARED as (value) to (trapezoid + 0x8000000)
//   cfd enable      = bits[29:28] (0=disabled, >0 enables CFD)
//   high-suppress   = bit 30
//   trigger enable  = bit 31
constexpr uint32_t THRESHOLD_VALUE_MASK     = 0x0FFFFFFF;
constexpr uint32_t THRESHOLD_OFFSET         = 0x8000000;  // trap.filter baseline add (page 9)
constexpr int      THRESHOLD_CFD_SHIFT      = 28;
constexpr uint32_t THRESHOLD_HE_SUPPRESS    = 1u << 30;
constexpr uint32_t THRESHOLD_TRIG_ENABLE    = 1u << 31;

constexpr uint32_t TRIGGER_STAT_COUNTER_MODE = 0x090;  // [Ch1..Ch4]

// Accumulator gate configuration — gate 1..4, stride 0x04 from 0x0A0.
//   D24:16 = Gate Length, D15:0 = Gate Start Index (manual page 39)
constexpr uint32_t ACCUM_GATE_CONFIG_BASE    = 0x0A0;  // +0x04*(gate-1)
constexpr uint32_t accumGateConfig(int gate1to4) { return ACCUM_GATE_CONFIG_BASE + 0x04u*uint32_t(gate1to4-1); }

constexpr uint32_t TOF_ACTIVE_WINDOW_CONFIG  = 0x0C0;
constexpr uint32_t GENERAL_HISTOGRAM_CONFIG  = 0x0CC;  // [Ch1..Ch4]
constexpr uint32_t TOF_HISTOGRAM_CONFIG      = 0x0D0;  // 28-bit divider
constexpr uint32_t SHAPE_HIST_X_CONFIG       = 0x0D4;  // peak-sum axis
constexpr uint32_t SHAPE_HIST_Y_CONFIG       = 0x0D8;  // psd axis
constexpr uint32_t PEAK_SUM_HIST_CONFIG      = 0x0DC;

constexpr uint32_t ADC_FIRMWARE_VERSION      = 0x100;  // R: type/version/revision
constexpr uint32_t ADC_FPGA_STATUS           = 0x104;  // R
constexpr uint32_t ACTUAL_SAMPLE_ADDR_BASE   = 0x110;  // +4*cid (R)
constexpr uint32_t PREVIOUS_BANK_ADDR_BASE   = 0x120;  // +4*cid (R)
constexpr uint32_t actualSampleAddr(int cid)   { return ACTUAL_SAMPLE_ADDR_BASE  + 4u*uint32_t(cid); }
constexpr uint32_t previousBankAddr(int cid)   { return PREVIOUS_BANK_ADDR_BASE  + 4u*uint32_t(cid); }

// --------------------------------------------------------------------------
//  EVENT CONFIG register bit fields (manual page 37). One register per group;
//  the four channels of the group occupy bytes [3:0],[11:8],[19:16],[27:24].
//  For channel-in-group cid the shift is 8*cid.
// --------------------------------------------------------------------------
constexpr uint32_t EVCFG_INPUT_INVERT     = 1u << 0;   // <<(8*cid): negative signals
constexpr uint32_t EVCFG_INTERNAL_TRIG    = 1u << 2;   // <<(8*cid)
constexpr uint32_t EVCFG_EXTERNAL_TRIG    = 1u << 3;   // <<(8*cid)
constexpr uint32_t eventCfgShift(int cid)  { return 8u*uint32_t(cid); }

// --------------------------------------------------------------------------
//  DATA FORMAT CONFIG bit fields (manual page 38). ONLY one bit per channel is
//  defined: "Save MAW Test Buffer Enable" at bit 4/12/20/28 (4 + 8*cid).
//  Bits 0-3, 5-11 ... are RESERVED. The legacy code wrongly treated bits 0-3
//  as accumulator/MAW selectors.
// --------------------------------------------------------------------------
constexpr uint32_t DATAFMT_SAVE_MAW_TEST_SHIFT(int cid) { return 4u + 8u*uint32_t(cid); }

// --------------------------------------------------------------------------
//  GENERAL HISTOGRAM CONFIG bit fields (manual pages 41-42).
// --------------------------------------------------------------------------
constexpr uint32_t GHCFG_HISTOGRAM_ENABLE      = 1u << 0;
constexpr uint32_t GHCFG_WRITE_EVENTS_DISABLE  = 1u << 1;
constexpr uint32_t GHCFG_HIST_CLEAR_DISABLE    = 1u << 2;
constexpr uint32_t GHCFG_PSD_METHOD_SELECT     = 1u << 3;   // 0=Method1, 1=Method2
constexpr uint32_t GHCFG_INCLUDE_PILEUP        = 1u << 4;
constexpr uint32_t GHCFG_INCLUDE_OVERUNDER     = 1u << 5;
constexpr uint32_t GHCFG_ONLY_IF_TOF_ACTIVE    = 1u << 6;
constexpr uint32_t GHCFG_LONG_TOF_WINDOW       = 1u << 7;
constexpr uint32_t GHCFG_2D_AMPL_SELECT        = 1u << 8;   // 0=PSDxTime, 1=AmplxTime
constexpr uint32_t GHCFG_2D_TIMEBASE_MODE      = 1u << 9;   // 0=4ns, 1=4096us bins
constexpr uint32_t GHCFG_2D_CLASS_SELECT_0     = 1u << 10;
constexpr uint32_t GHCFG_2D_CLASS_SELECT_1     = 1u << 11;

// --------------------------------------------------------------------------
//  ADC FPGA STATUS register bit fields (manual page 51).
// --------------------------------------------------------------------------
constexpr uint32_t STAT_HIST_CLEAR_BUSY   = 1u << 24;
constexpr uint32_t STAT_ADC_DCM_RESET     = 1u << 21;
constexpr uint32_t STAT_ADC_DCM_OK        = 1u << 20;
constexpr uint32_t STAT_MEMORY2_OK        = 1u << 17;
constexpr uint32_t STAT_MEMORY1_OK        = 1u << 16;

// --------------------------------------------------------------------------
//  HISTOGRAM MEMORY MAP  (manual pages 27, 43-49). Histograms live in the
//  16 MByte histogram region of each channel's Bank-1 memory.
//
//  Odd  channels (ch1,3,5,..15  -> cid 0,2)  use Memory 1, base byte 0x00C0_0000
//  Even channels (ch2,4,6,..16  -> cid 1,3)  use Memory 2, base byte 0x02C0_0000
//
//  These are 32-bit WORD offsets relative to the channel's histogram base, as
//  used by the data-transfer FIFO read (which addresses 32-bit words).
//  Sizes are in 32-bit bins.
// --------------------------------------------------------------------------
enum class HistType {
    TofGamma,        // 1M bins
    TofNeutron,      // 1M bins
    PeakSumGamma,    // 64K bins
    PeakSumNeutron,  // 64K bins
    PsdAmplTime2D,   // 512 x 1024 bins
    Psd2D,           // 512 x 512 bins
    PsdLookupTable,  // 512 x 512 bins
};

struct HistSpec {
    uint32_t byteOffsetOdd;   // base byte offset within Memory-1 for odd channels
    uint32_t byteOffsetEven;  // base byte offset within Memory-2 for even channels
    uint32_t nbins;           // number of 32-bit bins
};

// Byte offsets per the histogram memory map (manual pages 27, 45-49).
// Odd-channel offsets are 0x00C0_0000 + delta, even-channel 0x02C0_0000 + delta.
inline HistSpec histSpec(HistType t) {
    switch (t) {
        case HistType::TofGamma:       return {0x00C00000, 0x02C00000, 1u<<20};
        case HistType::TofNeutron:     return {0x00D00000, 0x02D00000, 1u<<20};
        case HistType::PeakSumGamma:   return {0x00E00000, 0x02E00000, 64u*1024};
        case HistType::PeakSumNeutron: return {0x00E10000, 0x02E10000, 64u*1024};
        case HistType::PsdAmplTime2D:  return {0x00E80000, 0x02E80000, 512u*1024};
        case HistType::Psd2D:          return {0x00F00000, 0x02F00000, 512u*512};
        case HistType::PsdLookupTable: return {0x00F40000, 0x02F40000, 512u*512};
    }
    return {0,0,0};
}

// Statistic counters: 11 defined counters per channel (manual pages 23-24).
enum StatCounter {
    SC_HIT = 0,
    SC_OUTSIDE_TOF,
    SC_OVER_UNDERFLOW,
    SC_PILEUP_REPILEUP,
    SC_INVALID_PSD_Y,
    SC_INVALID_PSD_X,
    SC_PSD_UNCLASSIFIED,
    SC_PSD_GAMMA,
    SC_PSD_NEUTRON,
    SC_INVALID_PEAKSUM_X,
    SC_INVALID_TIME_X,
    SC_COUNT          // = 11 defined; hardware stores 16 words/channel (5 reserved)
};
constexpr int STAT_WORDS_PER_CHANNEL = 16;   // incl. reserved (manual page 24)

} // namespace sis::reg
