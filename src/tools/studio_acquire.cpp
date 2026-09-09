// ===========================================================================
//  studio_acquire.cpp — headless data-taking for SIS3316 Studio.
//
//  Configures the board, arms it, and streams events straight to a timestamped
//  ROOT file for a fixed duration — using the SAME verified path as the GUI
//  (Sis3316Daq -> applyConfig -> parseBuffer -> analyzeWaveform -> RootWriter).
//  No Qt, no GUI: scriptable batch / remote / cron acquisition and the engine
//  behind automated validation runs.
//
//  Usage:
//    studio_acquire [--ip A] [--port P] [--seconds S] [--out DIR]
//                   [--threshold T] [--invert] [--no-config] [--no-swap]
//                   [--label NAME] [--quiet]
//
//  Exit code 0 on a clean run (>0 events), 1 on error / no data.
// ===========================================================================
#include "../hw/Sis3316Daq.hpp"
#include "../hw/Sis3316Config.hpp"
#include "../daq/Event.hpp"
#include "../analysis/Analysis.hpp"
#include "../io/RootWriter.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

using namespace sis;
using clock_t_ = std::chrono::steady_clock;

static double nowSec(clock_t_::time_point t0) {
    return std::chrono::duration<double>(clock_t_::now() - t0).count();
}

int main(int argc, char** argv) {
    std::string ip = "192.168.1.10";
    uint16_t    port = 1234;
    double      seconds = 10.0;
    std::string outDir = ".";
    std::string label;
    int         thrOverride = -1;
    int         dacOverride = -1;
    bool        invertOverride = false, doInvert = false;
    bool        doConfig = true, swap = true, quiet = false;
    int         coincMult = 0;         // 0 = record everything; >=2 = coincidence trigger mode
    double      coincWin  = 40.0;      // coincidence window (ns)
    int         silenceReadout = -1;   // debug: raise threshold of one ADC readout index
    int         rangeOverride = -1;    // input range/gain 0=5V 1=2V 2=1.9V
    std::string hwCoincMode;           // "and" | "or" | "mult" (hardware coincidence trigger)
    std::string hwCoincCh;             // displayed 1-based channel list, e.g. "1,5"
    int         hwCoincMin = 2;        // multiplicity threshold

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); std::exit(2); }
            return argv[++i];
        };
        if      (a == "--ip")        ip = next("--ip");
        else if (a == "--port")      port = (uint16_t)std::stoi(next("--port"));
        else if (a == "--seconds")   seconds = std::stod(next("--seconds"));
        else if (a == "--out")       outDir = next("--out");
        else if (a == "--label")     label = next("--label");
        else if (a == "--threshold") { thrOverride = std::stoi(next("--threshold")); }
        else if (a == "--dac")       { dacOverride = std::stoi(next("--dac")); }
        else if (a == "--coinc-mult")   { coincMult = std::stoi(next("--coinc-mult")); }
        else if (a == "--coinc-window") { coincWin  = std::stod(next("--coinc-window")); }
        else if (a == "--silence-readout") { silenceReadout = std::stoi(next("--silence-readout")); }
        else if (a == "--range")     { rangeOverride = std::stoi(next("--range")); }
        else if (a == "--hwcoinc-mode") { hwCoincMode = next("--hwcoinc-mode"); }
        else if (a == "--hwcoinc-ch")   { hwCoincCh   = next("--hwcoinc-ch"); }
        else if (a == "--hwcoinc-min")  { hwCoincMin  = std::stoi(next("--hwcoinc-min")); }
        else if (a == "--invert")    { invertOverride = true; doInvert = true; }
        else if (a == "--no-config") doConfig = false;
        else if (a == "--no-swap")   swap = false;
        else if (a == "--quiet")     quiet = true;
        else if (a == "-h" || a == "--help") {
            std::printf("Usage: studio_acquire [--ip A] [--port P] [--seconds S] [--out DIR]\n"
                        "                      [--threshold T] [--dac N] [--invert] [--no-config]\n"
                        "                      [--no-swap] [--label NAME] [--quiet]\n"
                        "                      [--coinc-mult M] [--coinc-window NS]\n\n"
                        "  --coinc-mult M    coincidence TRIGGER mode: only record events that fall\n"
                        "                    in a timestamp cluster spanning >= M distinct channels\n"
                        "                    (e.g. 2 for a muon telescope). 0/1 = record everything.\n"
                        "  --coinc-window NS coincidence window in ns (default 40).\n");
            return 0;
        } else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); return 2; }
    }
    auto log = [&](const std::string& s){ if (!quiet) std::printf("%s\n", s.c_str()); std::fflush(stdout); };
    auto connectorOf = [&](int r){ return swap ? (r ^ 1) : r; };

    BoardConfig board;
    if (thrOverride >= 0) for (int c = 0; c < 16; ++c) board.threshold[c] = thrOverride;
    if (silenceReadout >= 0 && silenceReadout < 16) board.threshold[silenceReadout] = 1000000;
    if (dacOverride >= 0) for (int c = 0; c < 16; ++c) board.dacOffset[c] = dacOverride;
    if (rangeOverride >= 0) for (int c = 0; c < 16; ++c) board.gain[c] = rangeOverride;
    if (invertOverride)   for (int c = 0; c < 16; ++c) board.invert[c]    = doInvert;
    if (!hwCoincMode.empty()) {
        board.coincMode = (hwCoincMode=="and")?1 : (hwCoincMode=="or")?2 : (hwCoincMode=="mult")?3 : 0;
        board.coincMinMult = hwCoincMin;
        board.coincMembers.fill(false);
        std::string s = hwCoincCh; size_t p = 0;
        while (p <= s.size()) {
            size_t c = s.find(',', p);
            std::string tok = s.substr(p, c==std::string::npos ? std::string::npos : c-p);
            if (!tok.empty()) { int conn = std::stoi(tok) - 1; int readout = swap ? (conn^1) : conn;
                if (readout>=0 && readout<16) board.coincMembers[readout] = true; }
            if (c==std::string::npos) break; p = c+1;
        }
        std::string mem; for(int c=0;c<16;++c) if(board.coincMembers[c]) mem += std::to_string(c)+" ";
        log("HW coincidence trigger: mode=" + hwCoincMode + " members(readout)=" + mem);
    }

    AnalysisConfig acfg;   // defaults (auto polarity)

    Sis3316Daq daq(ip, port);
    EventFormat fmt[16];
    ConfigSnapshot snapshot;
    try {
        daq.open();
        log("Link OK  id 0x" + [&]{ char b[16]; std::snprintf(b,sizeof b,"%08X",daq.moduleId()); return std::string(b);}()
            + "  " + std::to_string((int)std::lround(daq.temperatureC())) + " C");
        if (doConfig) { log("Configuring board..."); applyConfig(daq, board); }
        FirmwareInfo fw = daq.firmwareInfo(0);
        for (int r = 0; r < 6 && fw.type == 0; ++r) { usleep(50000); fw = daq.firmwareInfo(0); }
        { char b[80]; std::snprintf(b,sizeof b,"ADC firmware: type 0x%04X v0x%02X r0x%02X",fw.type,fw.version,fw.revision); log(b); }
        for (int c = 0; c < 16; ++c) fmt[c] = daq.channelEventFormat(c);
        snapshot = daq.readConfigSnapshot();
        daq.timestampClear();
        daq.disarm(); daq.armBank1(); daq.memToggle();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "connect/arm failed: %s\n", e.what());
        return 1;
    }

    RootWriter writer;
    std::string path = writer.open(outDir);
    if (path.empty()) { std::fprintf(stderr, "cannot open ROOT file: %s\n", writer.lastError().c_str()); return 1; }
    writer.writeConfig(snapshot);
    log("Recording -> " + path);

    std::array<uint64_t,16> counts{};   // per connector (ALL triggers)
    std::array<uint64_t,16> pileup{};
    uint64_t total = 0, recorded = 0;
    double tempAccum = 0; int tempSamples = 0;
    uint64_t rid = 0;
    const bool coincMode = coincMult >= 2;
    const double winTicks = coincWin / SAMPLE_NS;
    if (coincMode)
        log("Coincidence TRIGGER mode: recording only clusters spanning >= " +
            std::to_string(coincMult) + " channels within " + std::to_string((int)coincWin) + " ns");

    // one hit collected from the current bank readout (owns its raw waveform)
    struct Hit { int conn; sis::Sis3316Event ev; };

    auto t0 = clock_t_::now();
    double lastReport = 0;
    while (nowSec(t0) < seconds) {
        try {
            daq.memToggle();
            int prevBank = daq.currentBank() ^ 1;

            // 1) collect the whole batch (all channels) so coincidence gating can
            //    see across channels. A coincidence window (tens of ns) is far
            //    shorter than a bank period, so coincident hits share this batch.
            std::vector<Hit> batch;
            for (int c = 0; c < 16; ++c) {
                if (!fmt[c].valid()) continue;
                const int conn = connectorOf(c);
                std::vector<uint32_t> buf;
                if (daq.readChannelPreviousBank(c, prevBank, buf) == 0) continue;
                auto events = parseBuffer(buf.data(), buf.size(), fmt[c], c, rid);
                for (auto& e : events) {
                    counts[conn]++; total++;
                    if (e.pileup) pileup[conn]++;
                    batch.push_back(Hit{conn, std::move(e)});
                }
            }

            // 2) decide which hits to record. Default: all. Coincidence mode: only
            //    hits in a timestamp cluster spanning >= coincMult distinct channels.
            std::vector<char> keep(batch.size(), coincMode ? 0 : 1);
            if (coincMode && !batch.empty()) {
                std::vector<size_t> idx(batch.size());
                for (size_t k=0;k<idx.size();++k) idx[k]=k;
                std::sort(idx.begin(), idx.end(),
                          [&](size_t a, size_t b){ return batch[a].ev.timestamp < batch[b].ev.timestamp; });
                size_t i=0;
                while (i<idx.size()) {
                    size_t j=i+1;
                    while (j<idx.size() &&
                           double(batch[idx[j]].ev.timestamp) - double(batch[idx[i]].ev.timestamp) <= winTicks) ++j;
                    std::array<bool,16> seen{}; int mult=0;
                    for (size_t k=i;k<j;++k){ int ch=batch[idx[k]].conn;
                        if (ch>=0 && ch<16 && !seen[ch]){ seen[ch]=true; ++mult; } }
                    if (mult >= coincMult) for (size_t k=i;k<j;++k) keep[idx[k]]=1;
                    i=j;
                }
            }

            // 3) write the kept hits
            for (size_t bi=0; bi<batch.size(); ++bi) {
                if (!keep[bi]) continue;
                auto& e = batch[bi].ev; int conn = batch[bi].conn;
                PulseMetrics m = e.raw.empty() ? PulseMetrics{} : analyzeWaveform(e.raw, acfg);
                uint32_t g[4] = {e.gate[0], e.gate[1], e.gate[2], e.gate[3]};
                FpgaPsd fp = computeFpgaPsd(g, 1);
                RootEvent re;
                re.channel = conn; re.det = e.det;
                re.timestamp = e.timestamp; re.timestampSec = e.timestampSeconds();
                re.adcMax = e.adcMax; re.adcArgmax = e.adcArgmax;
                re.acc1 = e.gate[0]; re.acc2 = e.gate[1]; re.acc3 = e.gate[2]; re.acc4 = e.gate[3];
                re.psdFpga = fp.psdRatio; re.peakSumFpga = fp.peakSum;
                re.psdSw = m.valid ? m.psd : 0; re.qTotalSw = m.valid ? m.qTotal : 0;
                re.qTailSw = m.valid ? m.qTail : 0; re.baselineSw = m.valid ? m.baseline : 0;
                re.amplitudeSw = m.valid ? m.amplitude : 0;
                re.pileup = e.pileup;
                re.raw = e.raw.empty() ? nullptr : &e.raw;
                writer.fillEvent(re);
                ++recorded;
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "read error: %s\n", e.what());
        }
        double t = nowSec(t0);
        if (t - lastReport >= 1.0) {
            try { double tc = daq.temperatureC(); tempAccum += tc; ++tempSamples; } catch (...) {}
            char b[128];
            if (coincMode)
                std::snprintf(b,sizeof b,"  t=%5.1fs  triggers=%llu  recorded(coinc)=%llu",
                              t,(unsigned long long)total,(unsigned long long)recorded);
            else
                std::snprintf(b,sizeof b,"  t=%5.1fs  events=%llu  (%.0f Hz)",
                              t, (unsigned long long)total, total/std::max(t,1e-6));
            log(b);
            lastReport = t;
        }
        usleep(6000);
    }

    double dur = nowSec(t0);
    RootRunSummary sum;
    sum.totalEvents = total; sum.durationSec = dur;
    sum.avgTempC = tempSamples ? tempAccum/tempSamples : 0;
    sum.udpPacketsDropped = daq.droppedPackets();
    for (int c = 0; c < 16; ++c) {
        sum.rateHz[c] = counts[c] / std::max(dur,1e-6);
        sum.counts[c] = (uint32_t)counts[c]; sum.pileup[c] = (uint32_t)pileup[c];
    }
    writer.writeRunSummary(sum);
    writer.close();
    try { daq.disarm(); daq.close(); } catch (...) {}

    std::printf("\n=== run complete ===\n");
    std::printf("file      : %s\n", path.c_str());
    std::printf("duration  : %.1f s\n", dur);
    std::printf("triggers  : %llu  (%.0f Hz)\n", (unsigned long long)total, total/std::max(dur,1e-6));
    if (coincMode)
        std::printf("recorded  : %llu coincident events (>= %d ch within %.0f ns)  [%.2f%% of triggers]\n",
                    (unsigned long long)recorded, coincMult, coincWin,
                    total? 100.0*double(recorded)/double(total):0.0);
    else
        std::printf("recorded  : %llu events\n", (unsigned long long)recorded);
    std::printf("dropped   : %llu UDP packets\n", (unsigned long long)daq.droppedPackets());
    std::printf("per front-panel connector (1-based):\n");
    for (int c = 0; c < 16; ++c)
        if (counts[c]) std::printf("   ch %2d : %8llu  (%.0f Hz)%s\n",
                                   c+1, (unsigned long long)counts[c], counts[c]/std::max(dur,1e-6),
                                   pileup[c] ? ("  pileup " + std::to_string(pileup[c])).c_str() : "");
    return total > 0 ? 0 : 1;
}
