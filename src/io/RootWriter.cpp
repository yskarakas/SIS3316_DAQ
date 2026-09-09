// ===========================================================================
//  RootWriter.cpp — ROOT-backed implementation (TFile/TTree). All ROOT symbols
//  are confined to this translation unit.
// ===========================================================================
#include "RootWriter.hpp"

#include <TFile.h>
#include <TTree.h>

#include <filesystem>
#include <ctime>
#include <cstring>

namespace fs = std::filesystem;

namespace sis {

// Bump this whenever the branch layout changes. Stored in the file so an
// offline reader can detect schema drift (risk R7).
static constexpr int kSchemaVersion = 2;
// Hard cap on stored raw samples per event (keeps the variable-length branch
// buffer fixed so its address never moves under ROOT).
static constexpr int kMaxRaw = 65536;

int RootWriter::schemaVersion() { return kSchemaVersion; }

struct RootWriter::Impl {
    TFile* file = nullptr;
    TTree* events = nullptr;
    TTree* config = nullptr;
    TTree* summary = nullptr;
    std::string filePath;
    std::string error;
    long long fileSize = -1;

    // ---- Events branch buffers ----
    Int_t    b_channel = 0, b_det = 0, b_pileup = 0, b_nraw = 0;
    ULong64_t b_timestamp = 0;
    Double_t b_timestampSec = 0;
    UInt_t   b_adcMax = 0, b_adcArgmax = 0;
    UInt_t   b_acc1 = 0, b_acc2 = 0, b_acc3 = 0, b_acc4 = 0;
    Double_t b_psdFpga = 0, b_peakSumFpga = 0;
    Double_t b_psdSw = 0, b_qTotalSw = 0, b_qTailSw = 0, b_baselineSw = 0, b_amplitudeSw = 0;
    std::vector<Short_t> b_raw;     // capacity kMaxRaw, fixed address

    // ---- Config branch buffers ----
    Int_t   c_schema = kSchemaVersion;
    UInt_t  c_eventConfig[4]{}, c_channelHeaderId[4]{}, c_dataFormatConfig[4]{};
    UInt_t  c_rawDataConfig[4]{}, c_activeTrigGateLen[4]{}, c_preTrigDelay[4]{}, c_pileupConfig[4]{};
    UInt_t  c_accGate1[4]{}, c_accGate2[4]{}, c_accGate3[4]{}, c_accGate4[4]{};
    UInt_t  c_tofActiveWindow[4]{}, c_generalHistConfig[4]{}, c_tofHistConfig[4]{};
    UInt_t  c_shapeHistXConfig[4]{}, c_shapeHistYConfig[4]{}, c_peakSumHistConfig[4]{};
    UInt_t  c_fwType[4]{}, c_fwVersion[4]{}, c_fwRevision[4]{};
    UInt_t  c_firTrigSetup[16]{}, c_trigThreshold[16]{}, c_heTrigThreshold[16]{};

    // ---- Summary branch buffers ----
    ULong64_t s_totalEvents = 0, s_udpDropped = 0;
    Double_t  s_durationSec = 0, s_avgTempC = 0, s_deadTimeFrac = 0;
    Double_t  s_rateHz[16]{};
    UInt_t    s_counts[16]{}, s_pileup[16]{};
};

RootWriter::RootWriter() : d(std::make_unique<Impl>()) {
    d->b_raw.reserve(kMaxRaw);
    d->b_raw.resize(kMaxRaw);     // fix the backing storage address
}

RootWriter::~RootWriter() { close(); }

std::string RootWriter::open(const std::string& studioRoot) {
    try {
        fs::path base = studioRoot.empty() ? fs::current_path() : fs::path(studioRoot);
        fs::path dataDir = base / "Data";
        std::error_code ec;
        fs::create_directories(dataDir, ec);
        if (ec) { d->error = "cannot create Data dir: " + ec.message(); return {}; }

        std::time_t t = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char name[64];
        std::strftime(name, sizeof(name), "%Y-%m-%d_%H-%M-%S.root", &tm);
        fs::path full = dataDir / name;
        d->filePath = full.string();

        d->file = TFile::Open(d->filePath.c_str(), "RECREATE");
        if (!d->file || d->file->IsZombie()) {
            d->error = "TFile::Open failed for " + d->filePath;
            delete d->file; d->file = nullptr; return {};
        }

        // ---------------- Events tree ----------------
        d->events = new TTree("Events", "SIS3316 hits");
        d->events->Branch("channel",      &d->b_channel,      "channel/I");
        d->events->Branch("det",          &d->b_det,          "det/I");
        d->events->Branch("timestamp",    &d->b_timestamp,    "timestamp/l");
        d->events->Branch("timestampSec", &d->b_timestampSec, "timestampSec/D");
        d->events->Branch("adcMax",       &d->b_adcMax,       "adcMax/i");
        d->events->Branch("adcArgmax",    &d->b_adcArgmax,    "adcArgmax/i");
        d->events->Branch("acc1",         &d->b_acc1,         "acc1/i");
        d->events->Branch("acc2",         &d->b_acc2,         "acc2/i");
        d->events->Branch("acc3",         &d->b_acc3,         "acc3/i");
        d->events->Branch("acc4",         &d->b_acc4,         "acc4/i");
        d->events->Branch("psdFpga",      &d->b_psdFpga,      "psdFpga/D");
        d->events->Branch("peakSumFpga",  &d->b_peakSumFpga,  "peakSumFpga/D");
        d->events->Branch("psdSw",        &d->b_psdSw,        "psdSw/D");
        d->events->Branch("qTotalSw",     &d->b_qTotalSw,     "qTotalSw/D");
        d->events->Branch("qTailSw",      &d->b_qTailSw,      "qTailSw/D");
        d->events->Branch("baselineSw",   &d->b_baselineSw,   "baselineSw/D");
        d->events->Branch("amplitudeSw",  &d->b_amplitudeSw,  "amplitudeSw/D");
        d->events->Branch("pileup",       &d->b_pileup,       "pileup/I");
        d->events->Branch("nraw",         &d->b_nraw,         "nraw/I");
        d->events->Branch("raw",          d->b_raw.data(),    "raw[nraw]/S");

        // ---------------- HardwareConfig tree ----------------
        d->config = new TTree("HardwareConfig", "Register snapshot at run start");
        d->config->Branch("schemaVersion",     &d->c_schema,            "schemaVersion/I");
        d->config->Branch("eventConfig",        d->c_eventConfig,       "eventConfig[4]/i");
        d->config->Branch("channelHeaderId",    d->c_channelHeaderId,   "channelHeaderId[4]/i");
        d->config->Branch("dataFormatConfig",   d->c_dataFormatConfig,  "dataFormatConfig[4]/i");
        d->config->Branch("rawDataConfig",      d->c_rawDataConfig,     "rawDataConfig[4]/i");
        d->config->Branch("activeTrigGateLen",  d->c_activeTrigGateLen, "activeTrigGateLen[4]/i");
        d->config->Branch("preTrigDelay",       d->c_preTrigDelay,      "preTrigDelay[4]/i");
        d->config->Branch("pileupConfig",       d->c_pileupConfig,      "pileupConfig[4]/i");
        d->config->Branch("accGate1Config",     d->c_accGate1,          "accGate1Config[4]/i");
        d->config->Branch("accGate2Config",     d->c_accGate2,          "accGate2Config[4]/i");
        d->config->Branch("accGate3Config",     d->c_accGate3,          "accGate3Config[4]/i");
        d->config->Branch("accGate4Config",     d->c_accGate4,          "accGate4Config[4]/i");
        d->config->Branch("tofActiveWindow",    d->c_tofActiveWindow,   "tofActiveWindow[4]/i");
        d->config->Branch("generalHistConfig",  d->c_generalHistConfig, "generalHistConfig[4]/i");
        d->config->Branch("tofHistConfig",      d->c_tofHistConfig,     "tofHistConfig[4]/i");
        d->config->Branch("shapeHistXConfig",   d->c_shapeHistXConfig,  "shapeHistXConfig[4]/i");
        d->config->Branch("shapeHistYConfig",   d->c_shapeHistYConfig,  "shapeHistYConfig[4]/i");
        d->config->Branch("peakSumHistConfig",  d->c_peakSumHistConfig, "peakSumHistConfig[4]/i");
        d->config->Branch("firmwareType",       d->c_fwType,            "firmwareType[4]/i");
        d->config->Branch("firmwareVersion",    d->c_fwVersion,         "firmwareVersion[4]/i");
        d->config->Branch("firmwareRevision",   d->c_fwRevision,        "firmwareRevision[4]/i");
        d->config->Branch("firTrigSetup",       d->c_firTrigSetup,      "firTrigSetup[16]/i");
        d->config->Branch("trigThreshold",      d->c_trigThreshold,     "trigThreshold[16]/i");
        d->config->Branch("heTrigThreshold",    d->c_heTrigThreshold,   "heTrigThreshold[16]/i");

        // ---------------- RunSummary tree ----------------
        d->summary = new TTree("RunSummary", "Run statistics at run stop");
        d->summary->Branch("totalEvents",  &d->s_totalEvents,  "totalEvents/l");
        d->summary->Branch("durationSec",  &d->s_durationSec,  "durationSec/D");
        d->summary->Branch("rateHz",        d->s_rateHz,       "rateHz[16]/D");
        d->summary->Branch("counts",        d->s_counts,       "counts[16]/i");
        d->summary->Branch("pileup",        d->s_pileup,       "pileup[16]/i");
        d->summary->Branch("avgTempC",     &d->s_avgTempC,     "avgTempC/D");
        d->summary->Branch("deadTimeFrac", &d->s_deadTimeFrac, "deadTimeFrac/D");
        d->summary->Branch("udpPacketsDropped", &d->s_udpDropped, "udpPacketsDropped/l");

        d->error.clear();
        return d->filePath;
    } catch (const std::exception& e) {
        d->error = e.what();
        return {};
    }
}

bool RootWriter::isOpen() const { return d->file && !d->file->IsZombie(); }

void RootWriter::writeConfig(const ConfigSnapshot& cfg) {
    if (!isOpen()) return;
    for (int g = 0; g < 4; ++g) {
        d->c_eventConfig[g]       = cfg.eventConfig[g];
        d->c_channelHeaderId[g]   = cfg.channelHeaderId[g];
        d->c_dataFormatConfig[g]  = cfg.dataFormatConfig[g];
        d->c_rawDataConfig[g]     = cfg.rawDataConfig[g];
        d->c_activeTrigGateLen[g] = cfg.activeTrigGateLen[g];
        d->c_preTrigDelay[g]      = cfg.preTrigDelay[g];
        d->c_pileupConfig[g]      = cfg.pileupConfig[g];
        d->c_accGate1[g]          = cfg.accGateConfig[g][0];
        d->c_accGate2[g]          = cfg.accGateConfig[g][1];
        d->c_accGate3[g]          = cfg.accGateConfig[g][2];
        d->c_accGate4[g]          = cfg.accGateConfig[g][3];
        d->c_tofActiveWindow[g]   = cfg.tofActiveWindow[g];
        d->c_generalHistConfig[g] = cfg.generalHistConfig[g];
        d->c_tofHistConfig[g]     = cfg.tofHistConfig[g];
        d->c_shapeHistXConfig[g]  = cfg.shapeHistXConfig[g];
        d->c_shapeHistYConfig[g]  = cfg.shapeHistYConfig[g];
        d->c_peakSumHistConfig[g] = cfg.peakSumHistConfig[g];
        d->c_fwType[g]            = cfg.firmwareType[g];
        d->c_fwVersion[g]         = cfg.firmwareVersion[g];
        d->c_fwRevision[g]        = cfg.firmwareRevision[g];
    }
    for (int ch = 0; ch < 16; ++ch) {
        d->c_firTrigSetup[ch]   = cfg.firTrigSetup[ch];
        d->c_trigThreshold[ch]  = cfg.trigThreshold[ch];
        d->c_heTrigThreshold[ch] = cfg.heTrigThreshold[ch];
    }
    d->config->Fill();
}

void RootWriter::fillEvent(const RootEvent& e) {
    if (!isOpen()) return;
    d->b_channel = e.channel; d->b_det = e.det;
    d->b_timestamp = e.timestamp; d->b_timestampSec = e.timestampSec;
    d->b_adcMax = e.adcMax; d->b_adcArgmax = e.adcArgmax;
    d->b_acc1 = e.acc1; d->b_acc2 = e.acc2; d->b_acc3 = e.acc3; d->b_acc4 = e.acc4;
    d->b_psdFpga = e.psdFpga; d->b_peakSumFpga = e.peakSumFpga;
    d->b_psdSw = e.psdSw; d->b_qTotalSw = e.qTotalSw; d->b_qTailSw = e.qTailSw;
    d->b_baselineSw = e.baselineSw; d->b_amplitudeSw = e.amplitudeSw;
    d->b_pileup = e.pileup;

    int n = 0;
    if (e.raw && !e.raw->empty()) {
        n = (int)std::min<size_t>(e.raw->size(), (size_t)kMaxRaw);
        for (int i = 0; i < n; ++i) d->b_raw[i] = (Short_t)(*e.raw)[i];
    }
    d->b_nraw = n;
    d->events->Fill();
}

void RootWriter::writeRunSummary(const RootRunSummary& s) {
    if (!isOpen()) return;
    d->s_totalEvents = s.totalEvents;
    d->s_durationSec = s.durationSec;
    d->s_avgTempC = s.avgTempC;
    d->s_deadTimeFrac = s.deadTimeFrac;
    d->s_udpDropped = s.udpPacketsDropped;
    for (int c = 0; c < 16; ++c) {
        d->s_rateHz[c] = s.rateHz[c];
        d->s_counts[c] = s.counts[c];
        d->s_pileup[c] = s.pileup[c];
    }
    d->summary->Fill();
}

void RootWriter::close() {
    if (!d->file) return;
    if (!d->file->IsZombie()) {
        d->file->cd();
        if (d->events)  d->events->Write();
        if (d->config)  d->config->Write();
        if (d->summary) d->summary->Write();
        d->fileSize = (long long)d->file->GetSize();
    }
    d->file->Close();
    delete d->file;
    d->file = nullptr;
    d->events = d->config = d->summary = nullptr;
}

long long RootWriter::entries() const { return d->events ? d->events->GetEntries() : 0; }
std::string RootWriter::path() const { return d->filePath; }
long long RootWriter::fileSizeBytes() const { return d->fileSize; }
std::string RootWriter::lastError() const { return d->error; }

} // namespace sis
