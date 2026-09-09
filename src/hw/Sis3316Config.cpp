#include "Sis3316Config.hpp"
#include "Sis3316Daq.hpp"
#include <unistd.h>
#include <vector>

namespace sis {

// per-group register offsets not already in the reg namespace
namespace {
constexpr uint32_t OFF_INPUT_TAP_DELAY = 0x00;
constexpr uint32_t OFF_ANALOG_CTRL     = 0x04;
constexpr uint32_t OFF_DAC_OFFSET_CTRL = 0x08;
constexpr uint32_t OFF_SPI_CTRL        = 0x0C;
constexpr uint32_t OFF_FIR_ENERGY_SETUP = 0xC0;
constexpr uint32_t GLOB_SAMPLE_CLOCK_DIST = 0x50;
constexpr uint32_t GLOB_ACQ_CONTROL       = 0x60;
constexpr uint32_t GLOB_KEY_ADC_FPGA_RESET = 0x434;  // reset ADC-FPGA logic + link
// Trigger Coincidence Lookup Table + internal-trigger feedback (VME FPGA regs).
// Addresses verified against the Struck sis3316.h header on the SIS DVD.
constexpr uint32_t GLOB_LUT_CONTROL       = 0x64;
constexpr uint32_t GLOB_LUT_ADDR          = 0x68;
constexpr uint32_t GLOB_LUT_DATA          = 0x6C;
constexpr uint32_t GLOB_INT_TRIG_FEEDBACK = 0x7C;
constexpr uint32_t ACQ_BIT_FEEDBACK_EXTTRIG = 0x4000;  // bit14: feedback int-trig as ext-trig
constexpr uint32_t FEEDBACK_SELECT_COINC1   = 0x1000000; // bit24: coincidence table 1
constexpr uint32_t EVCFG_EXT_TRIG_BIT       = (1u << 3); // event-config: record on external (coincidence) trigger
}

void applyConfig(Sis3316Daq& daq, const BoardConfig& cfg) {
    using namespace reg;

    // ---- 1. bring the ADC-FPGA logic + data links to a clean, synchronized
    //         state, then re-tune the ADC input tap delay -------------------
    // We use KEY 0x434 (reset ADC-FPGA logic / DDR3 / data link), NOT the bare
    // ADC-clock DCM reset (KEY 0x438). The board is already clocked at 250 MHz;
    // a bare DCM reset without the full oscillator re-init can leave one group's
    // register link stuck reading 0 (observed on group 0). 0x434 re-establishes
    // all four group links cleanly and is the documented recovery for that.
    if (cfg.resetDcmTapDelay) {
        daq.write(GLOB_KEY_ADC_FPGA_RESET, 0);
        usleep(200000);                                      // link re-establish
        uint32_t sc = daq.read(GLOB_SAMPLE_CLOCK_DIST);
        daq.write(GLOB_SAMPLE_CLOCK_DIST, sc & ~0x3u);      // clock distribution = internal
        for (int g = 0; g < N_GROUPS; ++g)                   // tap-delay calibrate
            daq.writeGroupReg(g, OFF_INPUT_TAP_DELAY, 0xF00);
        usleep(20);
        for (int g = 0; g < N_GROUPS; ++g)                   // tap-delay set (250 MHz preset)
            daq.writeGroupReg(g, OFF_INPUT_TAP_DELAY, 0x48 | (0b11u << 8));
        usleep(20);
    }

    // ---- 2. acquisition control: allow external timestamp clear (async) ----
    daq.write(GLOB_ACQ_CONTROL, 0x400);

    // ---- 3. enable ADC chip outputs (SPI) per group ------------------------
    static const uint32_t adcEnable[] = {
        0x81001404, 0x81401404, 0x8100ff01, 0x8140ff01,
        0x81000800, 0x81400800, 0x8100ff01, 0x8140ff01};
    for (int g = 0; g < N_GROUPS; ++g)
        for (uint32_t spell : adcEnable) { daq.writeGroupReg(g, OFF_SPI_CTRL, spell); usleep(10); }

    // ---- 4. analog control: gain + 50 Ohm termination per group ------------
    for (int g = 0; g < N_GROUPS; ++g) {
        uint32_t ana = 0;
        for (int cid = 0; cid < CH_PER_GROUP; ++cid) {
            int ch = g * CH_PER_GROUP + cid;
            ana |= (uint32_t(cfg.gain[ch]) & 0x3u) << (8 * cid);
            if (!cfg.termination[ch]) ana |= (1u << (2 + 8 * cid));   // 1 = disable
        }
        daq.writeGroupReg(g, OFF_ANALOG_CTRL, ana);
    }

    // ---- 5. DAC baseline offset per channel (SPI) --------------------------
    for (int g = 0; g < N_GROUPS; ++g) {
        for (int cid = 0; cid < CH_PER_GROUP; ++cid) {
            int ch = g * CH_PER_GROUP + cid;
            uint32_t chanmask = uint32_t(cid) & 0x3u;
            uint32_t val = uint32_t(cfg.dacOffset[ch]) & 0xFFFFu;
            daq.writeGroupReg(g, OFF_DAC_OFFSET_CTRL, 0x88f00011); usleep(10);
            daq.writeGroupReg(g, OFF_DAC_OFFSET_CTRL, 0x85000000u | (chanmask << 20) | (0x1u << 4)); usleep(10);
            daq.writeGroupReg(g, OFF_DAC_OFFSET_CTRL, 0x82000000u | (chanmask << 20) | (val << 4));   usleep(10);
        }
    }

    // ---- 6. event config (trigger enable / invert) per group ---------------
    for (int g = 0; g < N_GROUPS; ++g) {
        uint32_t ev = 0;
        for (int cid = 0; cid < CH_PER_GROUP; ++cid) {
            int ch = g * CH_PER_GROUP + cid;
            uint32_t f = 0;
            if (cfg.invert[ch])          f |= EVCFG_INPUT_INVERT;      // bit0
            if (cfg.sumTrigger[ch])      f |= (1u << 1);               // bit1 sum trig
            if (cfg.internalTrigger[ch]) f |= EVCFG_INTERNAL_TRIG;     // bit2
            ev |= (f << (8 * cid));
        }
        daq.writeGroupReg(g, EVENT_CONFIG, ev);
    }

    // ---- 7. reset all trigger thresholds first (Struck sequence) -----------
    for (int g = 0; g < N_GROUPS; ++g)
        for (int cid = 0; cid < CH_PER_GROUP; ++cid)
            daq.writeGroupReg(g, triggerThreshold(cid), 0);

    // ---- 8. FIR trigger setup + thresholds + HE per channel ----------------
    for (int g = 0; g < N_GROUPS; ++g) {
        for (int cid = 0; cid < CH_PER_GROUP; ++cid) {
            int ch = g * CH_PER_GROUP + cid;
            // FIR setup: gap<<12 | peaking
            uint32_t fir = (uint32_t(cfg.peaking[ch]) & FIRSETUP_PEAKING_MASK)
                         | ((uint32_t(cfg.gap[ch]) & FIRSETUP_GAP_MASK) << FIRSETUP_GAP_SHIFT);
            daq.writeGroupReg(g, firTriggerSetup(cid), fir);
            // High-energy threshold register (value + 0x8000000)
            uint32_t he = (uint32_t(cfg.heThreshold[ch]) + THRESHOLD_OFFSET) & 0xFFFFFFFFu;
            daq.writeGroupReg(g, heTriggerThreshold(cid), he);
            // Trigger threshold + enable + optional CFD + optional HE suppress
            uint32_t thr = (uint32_t(cfg.threshold[ch]) + THRESHOLD_OFFSET);
            if (cfg.cfd[ch]) thr |= (uint32_t(cfg.cfd[ch]) & 0x3u) << THRESHOLD_CFD_SHIFT;
            if (cfg.cfd[ch] && cfg.heThreshold[ch] > 0) thr |= THRESHOLD_HE_SUPPRESS;
            if (cfg.peaking[ch] > 0 && (cfg.internalTrigger[ch] || cfg.sumTrigger[ch]))
                thr |= THRESHOLD_TRIG_ENABLE;
            daq.writeGroupReg(g, triggerThreshold(cid), thr);
        }
    }

    // ---- 9. raw window / gate window / pre-trigger / addr threshold --------
    for (int g = 0; g < N_GROUPS; ++g) {
        uint32_t rawcfg = ((uint32_t(cfg.sampleLength) & 0xFFFEu) << 16)
                        |  (uint32_t(cfg.sampleStart)  & 0xFFFEu);
        daq.writeGroupReg(g, RAW_DATA_BUFFER_CONFIG, rawcfg);
        daq.writeGroupReg(g, ACTIVE_TRIGGER_GATE_LEN, uint32_t(cfg.gateWindow - 2) & 0xFFFFu);
        daq.writeGroupReg(g, PRE_TRIGGER_DELAY, uint32_t(cfg.preTrigger) & 0x3FEu);
        daq.writeGroupReg(g, END_ADDRESS_THRESHOLD, uint32_t(cfg.addrThreshold / 4) & 0xFFFFFFu);
    }

    // ---- 10. data-format flags per group -----------------------------------
    for (int g = 0; g < N_GROUPS; ++g) {
        uint32_t df = 0;
        for (int cid = 0; cid < CH_PER_GROUP; ++cid) {
            uint32_t f = 0;
            if (cfg.saveAcc16)     f |= (1u << 0);
            if (cfg.saveAcc78)     f |= (1u << 1);
            if (cfg.saveEnergyMaw) f |= (1u << 3);
            df |= (f << (8 * cid));
        }
        daq.writeGroupReg(g, DATAFORMAT_CONFIG, df);
    }

    // ---- 11. accumulator gates (PSD integrals) per group -------------------
    for (int g = 0; g < N_GROUPS; ++g) {
        for (int gate = 1; gate <= 4; ++gate) {
            uint32_t v = ((uint32_t(cfg.accLength[gate-1]) & 0x1FFu) << 16)
                       |  (uint32_t(cfg.accStart[gate-1])  & 0xFFFFu);
            daq.writeGroupReg(g, accumGateConfig(gate), v);
        }
    }

    // ---- 12. energy filter (for energy MAW) per group ----------------------
    for (int g = 0; g < N_GROUPS; ++g) {
        uint32_t en = (uint32_t(cfg.enPeaking) & 0xFFFu)
                    | ((uint32_t(cfg.enGap) & 0x3FFu) << 12);
        daq.writeGroupReg(g, OFF_FIR_ENERGY_SETUP, en);
    }

    // ---- 13. hardware conditional (coincidence) trigger --------------------
    // Program the Trigger Coincidence Lookup Table with the requested AND/OR/
    // multiplicity condition over the member channels, and feed its validation
    // signal back as the ADC-FPGA "external" trigger so the members save an event
    // only when the condition holds. (Struck reference sis3316_root_gui_test1.cpp
    // + trigger_lookupTable_test.cpp; manual §3.6.4.)
    if (cfg.coincMode != 0) {
        std::vector<int> members;
        for (int c = 0; c < 16; ++c) if (cfg.coincMembers[c]) members.push_back(c);
        if (!members.empty()) {
            const int nm = (int)members.size();
            const uint32_t win = uint32_t(cfg.coincWindowSamples);

            // (a) stretch each member's FIR trigger pulse to the coincidence window
            //     (pulse-length field bits [31:24]) so overlapping hits coincide.
            for (int c : members) {
                int g = c / CH_PER_GROUP, cid = c % CH_PER_GROUP;
                uint32_t fir = (uint32_t(cfg.peaking[c]) & FIRSETUP_PEAKING_MASK)
                             | ((uint32_t(cfg.gap[c]) & FIRSETUP_GAP_MASK) << FIRSETUP_GAP_SHIFT)
                             | ((win & 0xFFu) << 24);
                daq.writeGroupReg(g, firTriggerSetup(cid), fir);
            }

            // (b) clear both lookup tables; set the validation output pulse length.
            uint32_t plen = win & 0x1Fu;
            daq.write(GLOB_LUT_CONTROL, 0x80000000u | (plen << 8) | plen);
            for (int p = 0; p < 20000; ++p)
                if (!(daq.read(GLOB_LUT_CONTROL) & 0x80000000u)) break;   // wait for clear

            // (c) validation=1 for every member-trigger combination satisfying the
            //     condition (AND=all, OR=any, MULT=≥coincMinMult).
            uint32_t mask = 0; for (int c : members) mask |= (1u << c);
            for (uint32_t sub = 0; sub < (1u << nm); ++sub) {
                int pop = 0; for (int b = 0; b < nm; ++b) if (sub & (1u << b)) ++pop;
                bool ok = (cfg.coincMode == 1) ? (pop == nm)
                        : (cfg.coincMode == 2) ? (pop >= 1)
                        :                        (pop >= cfg.coincMinMult);
                if (!ok) continue;
                uint32_t addr = 0;
                for (int b = 0; b < nm; ++b) if (sub & (1u << b)) addr |= (1u << members[b]);
                daq.write(GLOB_LUT_ADDR, addr);
                daq.write(GLOB_LUT_DATA, 1);
            }

            // (d) channel trigger mask: which trigger lines index the table.
            daq.write(GLOB_LUT_ADDR, (mask & 0xFFFFu) << 16);
            // (e) feedback source = coincidence table 1.
            daq.write(GLOB_INT_TRIG_FEEDBACK, FEEDBACK_SELECT_COINC1);
            // (f) enable feedback-as-external-trigger (keep async timestamp-clear).
            daq.write(GLOB_ACQ_CONTROL, 0x400u | ACQ_BIT_FEEDBACK_EXTTRIG);

            // (g) members record on the EXTERNAL (coincidence) trigger only;
            //     non-members keep their own self-trigger recording.
            for (int g = 0; g < N_GROUPS; ++g) {
                uint32_t ev = 0;
                for (int cid = 0; cid < CH_PER_GROUP; ++cid) {
                    int c = g * CH_PER_GROUP + cid; uint32_t f = 0;
                    if (cfg.invert[c]) f |= EVCFG_INPUT_INVERT;
                    if (cfg.coincMembers[c]) {
                        f |= EVCFG_EXT_TRIG_BIT;
                    } else {
                        if (cfg.sumTrigger[c])      f |= (1u << 1);
                        if (cfg.internalTrigger[c]) f |= EVCFG_INTERNAL_TRIG;
                    }
                    ev |= (f << (8 * cid));
                }
                daq.writeGroupReg(g, EVENT_CONFIG, ev);
            }
        }
    }
}

} // namespace sis
