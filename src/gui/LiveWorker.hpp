// ===========================================================================
//  LiveWorker.hpp — live acquisition worker (QThread) for SIS3316 Studio.
//
//  Drives the verified C++ stack (Sis3316Daq -> parseBuffer -> analysis) and
//  emits lightweight per-batch results to the GUI: latest waveform per channel
//  (persistence scope), per-event hits (channel, timestamp, amplitude, FPGA &
//  software PSD, gates), per-channel rates (software + FPGA statistic counter),
//  board telemetry, windowed time-coincidence pairs (TOF), recording state and
//  health diagnostics (UDP packet loss, dead time).
//
//  All ROOT file writing happens inside this worker thread (never the GUI
//  thread) via the PIMPL'd RootWriter — strict separation, no GUI stalls.
// ===========================================================================
#pragma once
#include <QObject>
#include <QVector>
#include <QString>
#include <array>
#include <atomic>
#include <memory>
#include "../hw/Sis3316Daq.hpp"
#include "../hw/Sis3316Config.hpp"
#include "../daq/Event.hpp"
#include "../analysis/Analysis.hpp"
#include "../io/RootWriter.hpp"

struct LiveHit {
    int      ch = 0;
    quint64  ts = 0;
    double   amplitude = 0;     // software amplitude (raw) or FPGA peakSum fallback
    double   psd = 0;           // FPGA-accumulator PSD if valid, else software PSD
    double   psdSw = 0;         // software charge-comparison PSD
    double   peakSum = 0;       // FPGA Acc2-Acc4
    double   qTotal = 0;
    bool     pileup = false;
    bool     fpgaPsdValid = false;
};

struct CoincPair {
    int chA = 0, chB = 0;
    double tofNs = 0;           // signed time difference chB - chA
    double ampA = 0, ampB = 0;
    double psdA = 0, psdB = 0;
};

struct LiveStats {
    double elapsed = 0;
    quint64 totalEvents = 0;
    std::array<double, 16> rateHz{};        // software per-channel event rate
    std::array<quint64, 16> chCounts{};     // cumulative counts per channel
    std::array<quint64, 16> pileup{};       // cumulative pile-up flagged events
    std::array<bool, 16> active{};
    double temp = 0;
    quint64 coincTotal = 0;
    // recording + diagnostics
    bool    recording = false;
    QString recordPath;
    quint64 recordEntries = 0;
    quint64 packetsDropped = 0;
    double  deadTimeFrac = 0;
};

Q_DECLARE_METATYPE(LiveHit)
Q_DECLARE_METATYPE(QVector<LiveHit>)
Q_DECLARE_METATYPE(CoincPair)
Q_DECLARE_METATYPE(QVector<CoincPair>)
Q_DECLARE_METATYPE(LiveStats)

struct LiveConfig {
    QString ip = "192.168.1.10";
    quint16 port = 1234;
    sis::AnalysisConfig analysis;
    double  coincWindowNs = 100.0;   // windowed coincidence (NOT exact-tick)
    int     coincRecordMult = 0;     // 0/1 = record all; >=2 = coincidence TRIGGER
                                     // mode (only record hits in a timestamp
                                     // cluster spanning >= this many channels)
    bool    clearTimestamp = true;
    int     psdMethod = 1;           // FPGA PSD method (1 or 2)
    QString dataRoot;                // where Data/ is created for recordings
    std::array<bool,16> invert{};    // per-channel display inversion (handled in GUI)
    bool    configureOnStart = true; // apply full board config before arming
    sis::BoardConfig board;          // configuration to apply

    // Front-panel connector ↔ ADC readout mapping. On this SIS3316 the analog
    // input connectors are wired to the ADC channels swapped WITHIN each pair
    // (connector 1↔ADC 2, 3↔4, …), verified against the hardware. With
    // swapChannelPairs=true, readout index r is labelled as front-panel
    // connector (r XOR 1)+1 so the displayed channel matches the cable.
    bool    swapChannelPairs = true;
    // returns the 0-based front-panel connector for a 0-based readout index
    int connectorOf(int readoutIndex) const {
        return swapChannelPairs ? (readoutIndex ^ 1) : readoutIndex;
    }
};

class LiveWorker : public QObject {
    Q_OBJECT
public:
    explicit LiveWorker(const LiveConfig& cfg);

public slots:
    void run();
    void stop() { fRunning = false; }
    void startRecording() { fRecordRequested = true; }
    void stopRecording()  { fRecordRequested = false; }

signals:
    void started();
    void stopped();
    void failed(const QString& msg);
    void logMsg(const QString& msg);
    void recordingChanged(bool on, const QString& path);
    void runSummaryReady(const QString& text);
    void waveforms(const QVector<int>& channels, const QVector<QVector<double>>& wfs);
    void hits(const QVector<LiveHit>& batch);
    void coincidences(const QVector<CoincPair>& pairs);
    void stats(const LiveStats& s);

private:
    LiveConfig fCfg;
    std::atomic<bool> fRunning{false};
    std::atomic<bool> fRecordRequested{false};
};
