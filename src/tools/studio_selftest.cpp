// ===========================================================================
//  studio_selftest.cpp — hardware-free regression test for the verifiable core:
//  the event parser, the FPGA-accumulator PSD formula, and ROOT I/O integrity.
//
//  Builds a synthetic STANDARD-firmware event buffer with known field values,
//  parses it, checks every decoded field, checks the PSD formula against a
//  hand-computed value, then writes a ROOT file and reads it back to confirm
//  the schema round-trips. Exit code 0 = all checks passed.
// ===========================================================================
#include "../daq/Event.hpp"
#include "../analysis/Analysis.hpp"
#include "../io/RootWriter.hpp"

#include <TFile.h>
#include <TTree.h>
#include <TLeaf.h>

#include <cstdio>
#include <cmath>
#include <vector>
#include <string>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++g_fail; } \
                              else std::printf("  ok  : %s\n", msg); } while(0)
#define CHECK_EQ(a,b,msg) do { if ((a)!=(b)) { std::printf("  FAIL: %s (got %lld want %lld)\n", msg,(long long)(a),(long long)(b)); ++g_fail; } \
                               else std::printf("  ok  : %s\n", msg); } while(0)
#define CHECK_NEAR(a,b,tol,msg) do { if (std::fabs((a)-(b))>(tol)) { std::printf("  FAIL: %s (got %.6f want %.6f)\n", msg,(double)(a),(double)(b)); ++g_fail; } \
                                     else std::printf("  ok  : %s\n", msg); } while(0)

using namespace sis;

int main() {
    std::printf("== SIS3316 Studio self-test ==\n");

    // ---- 1. Build a synthetic standard-firmware event (acc1 layout) --------
    // event length (16-bit words) = 6 + nraw16 + 14(acc1) ; nraw16 = 20 -> 10 raw 32b words
    EventFormat fmt;
    fmt.acc1 = true; fmt.nraw16 = 20;
    fmt.eventLen32 = (6 + fmt.nraw16 + 14) / 2;   // = 20
    CHECK_EQ(fmt.eventLen32, 20, "event length (32-bit words) = 20");

    const uint64_t TS   = 0x0000ABCDEF12ULL;       // 48-bit timestamp
    const int      DET  = 7;
    const uint32_t A1=100, A2=1000, A3=300, A4=50; // accumulators
    const uint32_t ADCMAX = 0x1F40, ARGMAX = 0x0030, INFO = 0b0001; // pileup bit set

    std::vector<uint32_t> w(fmt.eventLen32, 0);
    w[0] = uint32_t((TS >> 32) << 16) | uint32_t((DET & 0xFFF) << 4) | 0xA;
    w[1] = uint32_t(TS & 0xFFFFFFFF);
    w[2] = (ARGMAX << 16) | ADCMAX;
    w[3] = (INFO << 28) | (A1 & 0x00FFFFFF);
    w[4] = A2; w[5] = A3; w[6] = A4; w[7] = 0; w[8] = 0;      // gate2..gate6
    const uint32_t nraw32 = 10;
    w[9] = (0xEu << 28) | nraw32;                            // marker + raw count
    for (uint32_t k = 0; k < nraw32; ++k)
        w[10 + k] = ((2*k+1) << 16) | (2*k);                 // samples 0,1,2,...,19

    uint64_t rid = 0;
    auto evts = parseBuffer(w.data(), w.size(), fmt, /*channel*/3, rid);
    CHECK_EQ(evts.size(), 1, "parsed exactly one event");
    if (!evts.empty()) {
        const auto& e = evts[0];
        CHECK_EQ(e.channel, 3, "physical channel preserved");
        CHECK_EQ(e.det, DET, "header det field decoded");
        CHECK_EQ((long long)e.timestamp, (long long)TS, "48-bit timestamp decoded");
        CHECK_EQ(e.adcMax, ADCMAX, "adcMax decoded");
        CHECK_EQ(e.adcArgmax, ARGMAX, "adcArgmax decoded");
        CHECK_EQ(e.gate[0], A1, "gate1 (Acc1) decoded");
        CHECK_EQ(e.gate[1], A2, "gate2 (Acc2) decoded");
        CHECK_EQ(e.gate[2], A3, "gate3 (Acc3) decoded");
        CHECK_EQ(e.gate[3], A4, "gate4 (Acc4) decoded");
        CHECK_EQ(e.raw.size(), 20, "20 raw samples decoded");
        bool rawOk = true;
        for (int i = 0; i < 20; ++i) if (e.raw[i] != i) rawOk = false;
        CHECK(rawOk, "raw sample values 0..19 round-trip (low/high split)");
        CHECK(e.pileup != 0, "pileup flag decoded from info nibble");

        // ---- 2. FPGA PSD formula (charge-comparison, gate2 vs gate1) ------
        // Convention (matches reference SIS3316_LiveDAQ EJ-309 config):
        //   gate[0]=Gate1 prompt, gate[1]=Gate2 total
        //   peakSum = gate2, tail = gate2-gate1, PSD = (gate2-gate1)/gate2
        uint32_t g[4] = {e.gate[0], e.gate[1], e.gate[2], e.gate[3]};
        FpgaPsd fp = computeFpgaPsd(g, 1);
        CHECK(fp.valid, "FPGA PSD valid");
        CHECK_NEAR(fp.peakSum, double(A2), 1e-9, "peakSum = gate2 = 1000");
        CHECK_NEAR(fp.tail,    double(A2)-double(A1), 1e-9, "tail = gate2-gate1 = 900");
        CHECK_NEAR(fp.psdRatio, (double(A2)-double(A1))/double(A2), 1e-9, "psdRatio = (gate2-gate1)/gate2");
        CHECK_NEAR(fp.psdIndex, ((double(A2)-double(A1))/double(A2))*65536.0, 1e-6, "PSD index (scaled)");
    }

    // ---- 3. ROOT I/O round-trip ------------------------------------------
    std::string testRoot = "/tmp/sis_studio_selftest_root";
    RootWriter wr;
    std::string path = wr.open(testRoot);
    CHECK(!path.empty(), "RootWriter opened a file");
    if (!path.empty()) {
        ConfigSnapshot cfg;
        cfg.eventConfig[0] = 0x12345678; cfg.trigThreshold[5] = 0x08001234;
        cfg.firmwareType[0] = 0x0250; cfg.firmwareVersion[0] = 0x02;
        wr.writeConfig(cfg);
        std::vector<uint16_t> raw = {1,2,3,4,5,6};
        for (int i = 0; i < 100; ++i) {
            RootEvent re;
            re.channel = i % 16; re.det = i; re.timestamp = 1000 + i;
            re.timestampSec = (1000.0 + i) / 250e6;
            re.acc1 = 100; re.acc2 = 1000; re.acc3 = 300; re.acc4 = 50;
            re.psdFpga = 0.21; re.peakSumFpga = 950; re.amplitudeSw = 1234;
            re.raw = &raw;
            wr.fillEvent(re);
        }
        CHECK_EQ(wr.entries(), 100, "100 events filled");
        RootRunSummary sum; sum.totalEvents = 100; sum.durationSec = 12.5;
        sum.avgTempC = 42.0; sum.counts[3] = 555;
        wr.writeRunSummary(sum);
        wr.close();

        // reopen and verify
        TFile* f = TFile::Open(path.c_str(), "READ");
        CHECK(f && !f->IsZombie(), "file reopened");
        if (f && !f->IsZombie()) {
            auto* ev = (TTree*)f->Get("Events");
            auto* cf = (TTree*)f->Get("HardwareConfig");
            auto* su = (TTree*)f->Get("RunSummary");
            CHECK(ev && cf && su, "all three trees present");
            if (ev) CHECK_EQ(ev->GetEntries(), 100, "Events tree has 100 entries");
            if (cf) {
                CHECK_EQ(cf->GetEntries(), 1, "HardwareConfig has 1 entry");
                cf->GetEntry(0);
                CHECK_EQ((long long)cf->GetLeaf("eventConfig")->GetValue(0),
                         (long long)0x12345678, "eventConfig[0] round-trip");
                CHECK_EQ((long long)cf->GetLeaf("firmwareType")->GetValue(0),
                         (long long)0x0250, "firmwareType round-trip");
            }
            if (su) {
                su->GetEntry(0);
                CHECK_EQ((long long)su->GetLeaf("totalEvents")->GetValue(),
                         (long long)100, "RunSummary totalEvents round-trip");
                CHECK_NEAR(su->GetLeaf("avgTempC")->GetValue(), 42.0, 1e-6,
                           "RunSummary avgTempC round-trip");
            }
            if (ev) {
                ev->GetEntry(0);
                CHECK_EQ((long long)ev->GetLeaf("nraw")->GetValue(), (long long)6,
                         "Events nraw round-trip");
                CHECK_NEAR(ev->GetLeaf("peakSumFpga")->GetValue(), 950.0, 1e-6,
                           "Events peakSumFpga round-trip");
            }
            f->Close(); delete f;
        }
    }

    std::printf("\n== %s ==\n", g_fail == 0 ? "ALL CHECKS PASSED" : "FAILURES PRESENT");
    return g_fail == 0 ? 0 : 1;
}
