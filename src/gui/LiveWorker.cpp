#include "LiveWorker.hpp"
#include "../util/Logger.hpp"
#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <array>
#include <vector>

using namespace sis;

LiveWorker::LiveWorker(const LiveConfig& cfg) : fCfg(cfg) {}

void LiveWorker::run() {
    Sis3316Daq daq(fCfg.ip.toStdString(), fCfg.port);
    EventFormat fmt[16];
    ConfigSnapshot snapshot;
    try {
        daq.open();
        emit logMsg(QString("Link OK — id 0x%1, %2 °C")
                    .arg(daq.moduleId(), 8, 16, QChar('0'))
                    .arg(daq.temperatureC(), 0, 'f', 1));
        SIS_LOG_INFO("DAQ", "link opened, temperature " +
                     std::to_string(daq.temperatureC()) + " C");
        // Configure the board before arming. A blank/reset SIS3316 has all
        // trigger-enable bits and thresholds = 0, so it looks connected but
        // produces NO events — this is the usual cause of "no signals".
        if (fCfg.configureOnStart) {
            emit logMsg("Configuring board (trigger/threshold/gates/raw window)…");
            applyConfig(daq, fCfg.board);
            SIS_LOG_INFO("CONFIG", "board configured with default settings before arming");
        }
        // Read firmware AFTER configuring: applyConfig re-initialises the ADC
        // FPGAs (DCM reset + tap-delay), so the version register is only
        // guaranteed valid once that has run. Retry briefly if still settling.
        FirmwareInfo fw = daq.firmwareInfo(0);
        for (int r = 0; r < 6 && fw.type == 0; ++r) { QThread::msleep(50); fw = daq.firmwareInfo(0); }
        emit logMsg(QString("ADC firmware: type 0x%1 v0x%2 r0x%3")
                    .arg(fw.type, 4, 16, QChar('0')).arg(fw.version, 2, 16, QChar('0'))
                    .arg(fw.revision, 2, 16, QChar('0')));
        for (int c = 0; c < 16; ++c) fmt[c] = daq.channelEventFormat(c);
        snapshot = daq.readConfigSnapshot();
        if (fCfg.clearTimestamp) daq.timestampClear();
        daq.disarm(); daq.armBank1(); daq.memToggle();
    } catch (const std::exception& e) {
        SIS_LOG_ERROR("DAQ", std::string("connect/arm failed: ") + e.what());
        emit failed(QString("Connect/arm failed: %1").arg(e.what()));
        return;
    }

    emit started();
    fRunning = true;

    QElapsedTimer total; total.start();
    QElapsedTimer emitTimer; emitTimer.start();
    uint64_t rid = 0;

    std::array<quint64, 16> chCounts{};
    std::array<quint64, 16> chWin{};
    std::array<quint64, 16> pileupCount{};   // software pile-up flag per connector
    quint64 totalEvents = 0, coincTotal = 0;
    double tempAccum = 0; int tempSamples = 0; double lastTemp = 0;

    // recording state (worker-local; controlled via atomic request flag)
    RootWriter writer;
    bool recording = false;

    QVector<LiveHit> hitBatch;
    QVector<CoincPair> coincBatch;
    std::vector<LiveHit> coincBuffer;

    // one event queued for recording (raw kept by value so coincidence gating can
    // run across the whole bank readout before deciding what to write to ROOT).
    struct RecHit { int conn; uint64_t ts; RootEvent re; std::vector<uint16_t> raw; };

    // dead-time estimate: fraction of wall time the board reported BUSY/veto.
    double busyAccumMs = 0, loopAccumMs = 0;

    while (fRunning) {
        QElapsedTimer iterTimer; iterTimer.start();
        // -------- recording start/stop transitions (worker thread only) -----
        bool wantRecord = fRecordRequested.load();
        if (wantRecord && !recording) {
            std::string path = writer.open(fCfg.dataRoot.toStdString());
            if (!path.empty()) {
                writer.writeConfig(snapshot);
                recording = true;
                SIS_LOG_INFO("ROOT", "recording started: " + path);
                emit recordingChanged(true, QString::fromStdString(path));
            } else {
                SIS_LOG_ERROR("ROOT", "failed to open file: " + writer.lastError());
                emit logMsg("Recording failed: " + QString::fromStdString(writer.lastError()));
                fRecordRequested = false;
            }
        } else if (!wantRecord && recording) {
            // run summary written below in writeRunSummary on close
            RootRunSummary sum;
            sum.totalEvents = totalEvents;
            sum.durationSec = total.elapsed() / 1000.0;
            sum.avgTempC = tempSamples ? tempAccum / tempSamples : 0;
            sum.udpPacketsDropped = daq.droppedPackets();
            sum.deadTimeFrac = loopAccumMs > 0 ? busyAccumMs / loopAccumMs : 0;
            for (int c = 0; c < 16; ++c) {
                sum.rateHz[c] = chCounts[c] / std::max(sum.durationSec, 1e-6);
                sum.counts[c] = (uint32_t)chCounts[c]; sum.pileup[c] = (uint32_t)pileupCount[c];
            }
            writer.writeRunSummary(sum);
            writer.close();
            recording = false;
            SIS_LOG_INFO("ROOT", "recording stopped: " + writer.path());
            emit recordingChanged(false, QString::fromStdString(writer.path()));
        }

        try {
            // crude dead-time proxy: threshold-overrun/veto means the board is
            // discarding triggers because a bank is (almost) full.
            if (daq.thresholdOverrun()) busyAccumMs += iterTimer.elapsed();

            daq.memToggle();
            int prevBank = daq.currentBank() ^ 1;

            QVector<int> wfChannels;
            QVector<QVector<double>> wfData;
            std::vector<RecHit> recBatch;   // events queued for (gated) recording

            for (int c = 0; c < 16; ++c) {
                if (!fmt[c].valid()) continue;
                // front-panel connector this ADC readout index corresponds to
                const int conn = fCfg.connectorOf(c);
                std::vector<uint32_t> buf;
                if (daq.readChannelPreviousBank(c, prevBank, buf) == 0) continue;
                auto events = parseBuffer(buf.data(), buf.size(), fmt[c], c, rid);
                if (events.empty()) continue;

                const auto& last = events.back();
                if (!last.raw.empty()) {
                    QVector<double> w(last.raw.size());
                    for (size_t i = 0; i < last.raw.size(); ++i) w[i] = last.raw[i];
                    wfChannels.push_back(conn);
                    wfData.push_back(std::move(w));
                }

                for (auto& e : events) {
                    // software waveform metrics (only if raw samples present)
                    PulseMetrics m = e.raw.empty()
                        ? PulseMetrics{} : analyzeWaveform(e.raw, fCfg.analysis);
                    // FPGA-accumulator PSD (always available, matches on-board)
                    uint32_t g[4] = {e.gate[0], e.gate[1], e.gate[2], e.gate[3]};
                    FpgaPsd fp = computeFpgaPsd(g, fCfg.psdMethod);

                    chCounts[conn]++; chWin[conn]++; totalEvents++;
                    if (e.pileup) pileupCount[conn]++;

                    LiveHit h;
                    h.ch = conn; h.ts = e.timestamp;
                    h.fpgaPsdValid = fp.valid;
                    h.peakSum = fp.peakSum;
                    h.psdSw = m.valid ? m.psd : 0.0;
                    h.amplitude = m.valid ? m.amplitude : fp.peakSum;
                    // Display/histogram PSD: prefer the physical software charge-
                    // comparison PSD (always valid from the waveform). The FPGA
                    // accumulator ratio is only meaningful once the four PSD gates
                    // are configured (Gate1=pre-baseline, 2=total, 3=tail, 4=pre-
                    // peak) via the Config editor; on a board whose gates are not
                    // PSD-configured the ratio can fall outside [0,1], so we only
                    // fall back to it when no raw samples were saved. Both values
                    // are recorded to ROOT regardless.
                    bool fpgaPhysical = fp.valid && fp.psdRatio >= 0.0 && fp.psdRatio <= 1.0;
                    h.psd = m.valid ? m.psd : (fpgaPhysical ? fp.psdRatio : 0.0);
                    h.qTotal = m.valid ? m.qTotal : 0.0;
                    h.pileup = e.pileup;
                    hitBatch.push_back(h);
                    coincBuffer.push_back(h);

                    if (recording) {
                        RecHit rh; rh.conn = conn; rh.ts = e.timestamp;
                        rh.re.channel = conn; rh.re.det = e.det;
                        rh.re.timestamp = e.timestamp; rh.re.timestampSec = e.timestampSeconds();
                        rh.re.adcMax = e.adcMax; rh.re.adcArgmax = e.adcArgmax;
                        rh.re.acc1 = e.gate[0]; rh.re.acc2 = e.gate[1];
                        rh.re.acc3 = e.gate[2]; rh.re.acc4 = e.gate[3];
                        rh.re.psdFpga = fp.psdRatio; rh.re.peakSumFpga = fp.peakSum;
                        rh.re.psdSw = h.psdSw; rh.re.qTotalSw = m.valid ? m.qTotal : 0;
                        rh.re.qTailSw = m.valid ? m.qTail : 0;
                        rh.re.baselineSw = m.valid ? m.baseline : 0;
                        rh.re.amplitudeSw = m.valid ? m.amplitude : 0;
                        rh.re.pileup = e.pileup;
                        rh.raw = e.raw;   // copy; re.raw is set at write time below
                        recBatch.push_back(std::move(rh));
                    }
                }
            }
            if (!wfChannels.isEmpty())
                emit waveforms(wfChannels, wfData);

            // ---- (gated) recording: write the batch to ROOT ------------------
            // Default: write everything. Coincidence TRIGGER mode
            // (coincRecordMult>=2): only write hits in a timestamp cluster that
            // spans >= that many distinct channels (e.g. a muon telescope). A
            // coincidence window is far shorter than a bank period, so coincident
            // hits share this batch.
            if (recording && !recBatch.empty()) {
                const int mult = fCfg.coincRecordMult;
                std::vector<char> keep(recBatch.size(), mult >= 2 ? 0 : 1);
                if (mult >= 2) {
                    const double winTicks = fCfg.coincWindowNs / SAMPLE_NS;
                    std::vector<size_t> idx(recBatch.size());
                    for (size_t k=0;k<idx.size();++k) idx[k]=k;
                    std::sort(idx.begin(), idx.end(),
                              [&](size_t a, size_t b){ return recBatch[a].ts < recBatch[b].ts; });
                    size_t i=0;
                    while (i<idx.size()) {
                        size_t j=i+1;
                        while (j<idx.size() &&
                               double(recBatch[idx[j]].ts) - double(recBatch[idx[i]].ts) <= winTicks) ++j;
                        std::array<bool,16> seen{}; int m2=0;
                        for (size_t k=i;k<j;++k){ int ch=recBatch[idx[k]].conn;
                            if (ch>=0&&ch<16&&!seen[ch]){ seen[ch]=true; ++m2; } }
                        if (m2 >= mult) for (size_t k=i;k<j;++k) keep[idx[k]]=1;
                        i=j;
                    }
                }
                for (size_t k=0;k<recBatch.size();++k) if (keep[k]) {
                    recBatch[k].re.raw = recBatch[k].raw.empty() ? nullptr : &recBatch[k].raw;
                    writer.fillEvent(recBatch[k].re);
                }
            }

            // windowed time-coincidence (cluster by timestamp, distinct channels)
            if (coincBuffer.size() > 1) {
                std::sort(coincBuffer.begin(), coincBuffer.end(),
                          [](const LiveHit& a, const LiveHit& b){ return a.ts < b.ts; });
                const double winTicks = fCfg.coincWindowNs / SAMPLE_NS;
                for (size_t i = 0; i + 1 < coincBuffer.size(); ++i) {
                    for (size_t j = i + 1; j < coincBuffer.size(); ++j) {
                        double dt = double(coincBuffer[j].ts) - double(coincBuffer[i].ts);
                        if (dt > winTicks) break;
                        if (coincBuffer[i].ch == coincBuffer[j].ch) continue;
                        CoincPair p;
                        p.chA = coincBuffer[i].ch; p.chB = coincBuffer[j].ch;
                        p.tofNs = dt * SAMPLE_NS;
                        p.ampA = coincBuffer[i].amplitude; p.ampB = coincBuffer[j].amplitude;
                        p.psdA = coincBuffer[i].psd; p.psdB = coincBuffer[j].psd;
                        coincBatch.push_back(p);
                        coincTotal++;
                    }
                }
                coincBuffer.clear();
            }
        } catch (const std::exception& e) {
            SIS_LOG_WARN("DAQ", std::string("read error: ") + e.what());
            emit logMsg(QString("read error: %1").arg(e.what()));
        }
        loopAccumMs += iterTimer.elapsed();

        if (emitTimer.elapsed() >= 50) {     // ~20 Hz UI refresh
            double dt = emitTimer.elapsed() / 1000.0;
            LiveStats s;
            s.elapsed = total.elapsed() / 1000.0;
            s.totalEvents = totalEvents;
            s.coincTotal = coincTotal;
            for (int c = 0; c < 16; ++c) {
                s.rateHz[c] = chWin[c] / std::max(dt, 1e-6);
                s.chCounts[c] = chCounts[c];
                s.pileup[c] = pileupCount[c];
                s.active[c] = chCounts[c] > 0;
            }
            try { lastTemp = daq.temperatureC(); s.temp = lastTemp;
                  tempAccum += lastTemp; ++tempSamples; } catch (...) {}
            s.packetsDropped = daq.droppedPackets();
            s.deadTimeFrac = loopAccumMs > 0 ? busyAccumMs / loopAccumMs : 0;
            s.recording = recording;
            s.recordPath = QString::fromStdString(writer.path());
            s.recordEntries = recording ? (quint64)writer.entries() : 0;
            emit stats(s);
            if (!hitBatch.isEmpty()) { emit hits(hitBatch); hitBatch.clear(); }
            if (!coincBatch.isEmpty()) { emit coincidences(coincBatch); coincBatch.clear(); }
            chWin.fill(0);
            emitTimer.restart();
        }
        QThread::msleep(6);
    }

    // ---- clean shutdown: finalize any open recording + run summary --------
    double dur = total.elapsed() / 1000.0;
    if (recording) {
        RootRunSummary sum;
        sum.totalEvents = totalEvents;
        sum.durationSec = dur;
        sum.avgTempC = tempSamples ? tempAccum / tempSamples : 0;
        sum.udpPacketsDropped = daq.droppedPackets();
        sum.deadTimeFrac = loopAccumMs > 0 ? busyAccumMs / loopAccumMs : 0;
        for (int c = 0; c < 16; ++c) {
            sum.rateHz[c] = chCounts[c] / std::max(dur, 1e-6);
            sum.counts[c] = (uint32_t)chCounts[c]; sum.pileup[c] = (uint32_t)pileupCount[c];
        }
        writer.writeRunSummary(sum);
        writer.close();
        emit recordingChanged(false, QString::fromStdString(writer.path()));
    }

    // textual run summary (always emitted, even without recording)
    QString summary;
    summary += QString("Run duration: %1 s\n").arg(dur, 0, 'f', 1);
    summary += QString("Total events: %1\n").arg(totalEvents);
    summary += QString("Average rate: %1 Hz\n").arg(dur>0 ? totalEvents/dur : 0, 0, 'f', 1);
    summary += QString("Coincidences: %1\n").arg(coincTotal);
    summary += QString("Avg temperature: %1 °C\n").arg(tempSamples?tempAccum/tempSamples:0, 0, 'f', 1);
    summary += QString("UDP packets dropped: %1\n").arg(daq.droppedPackets());
    summary += QString("Dead-time fraction: %1 %\n").arg((loopAccumMs>0?busyAccumMs/loopAccumMs:0)*100, 0, 'f', 2);
    emit runSummaryReady(summary);
    SIS_LOG_INFO("DAQ", "run stopped, " + std::to_string(totalEvents) + " events");

    try { daq.disarm(); daq.close(); } catch (...) {}
    emit stopped();
}
