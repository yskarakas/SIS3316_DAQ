// ===========================================================================
//  LiveDaqWidget — the live-acquisition tab of SIS3316 Studio.
//  Hosts the acquisition thread and all live visualisations:
//   * 16-channel persistence oscilloscope (fading traces, per-channel invert,
//     adjustable persistence depth),
//   * PSD-vs-amplitude 2D map + 1-D peak-sum γ/n spectra + live FOM,
//   * per-channel trigger-rate bars (software + FPGA statistic counters) +
//     windowed-coincidence TOF spectrum,
//   * direct-to-ROOT recording control + health diagnostics + run summary.
// ===========================================================================
#pragma once
#include <QWidget>
#include <QThread>
#include <array>
#include "LiveWorker.hpp"
#include "../analysis/Analysis.hpp"

class PlotWidget;
class Heatmap2D;
class QLineEdit;
class QPushButton;
class QLabel;
class QDoubleSpinBox;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QSlider;
class QTableWidget;
class RateBars;

class LiveDaqWidget : public QWidget {
    Q_OBJECT
public:
    explicit LiveDaqWidget(QWidget* parent = nullptr);
    ~LiveDaqWidget() override;

private slots:
    void toggleRun();
    void toggleRecord();
    void clearAll();
    void onStarted();
    void onStopped();
    void onFailed(const QString& m);
    void onLog(const QString& m);
    void onRecordingChanged(bool on, const QString& path);
    void onRunSummary(const QString& text);
    void onWaveforms(const QVector<int>& channels, const QVector<QVector<double>>& wfs);
    void onHits(const QVector<LiveHit>& batch);
    void onCoincidences(const QVector<CoincPair>& pairs);
    void onStats(const LiveStats& s);

private:
    void buildUi();
    void refreshScope();
    void refreshPsd();
    void refreshSpectra();
    void refreshTof();
    void refreshCounters(const LiveStats& s);
    void updateCondStatus();          // refresh the conditional-trigger status label
    LiveConfig currentConfig() const;

    // worker
    QThread* fThread = nullptr;
    LiveWorker* fWorker = nullptr;
    bool fRunning = false;
    bool fRecording = false;

    // controls
    QLineEdit* fIp = nullptr;
    QLineEdit* fPort = nullptr;
    QPushButton* fBtnRun = nullptr;
    QPushButton* fBtnRec = nullptr;
    QDoubleSpinBox* fCoincWin = nullptr;
    QSpinBox* fCoincRecMult = nullptr;   // coincidence-trigger recording multiplicity
    QDoubleSpinBox* fPsdThresh = nullptr;
    QComboBox* fPsdMethod = nullptr;
    QSpinBox* fTailDelay = nullptr;
    QSpinBox* fTotalGate = nullptr;
    QCheckBox* fConfigureChk = nullptr;
    // hardware conditional (coincidence) trigger
    QComboBox* fCondMode = nullptr;                 // Off / AND / OR / Multiplicity
    QSpinBox*  fCondMult = nullptr;                 // multiplicity threshold
    std::array<QCheckBox*,16> fCondMember{};        // member channels (displayed 1-16)
    QLabel* fCondStatus = nullptr;                  // live status of the conditional trigger
    QSlider* fPersist = nullptr;
    std::array<QCheckBox*, 16> fInvertChk{};

    // stat labels
    QLabel *fLblState=nullptr, *fLblRate=nullptr, *fLblFpgaRate=nullptr, *fLblTotal=nullptr,
           *fLblTemp=nullptr, *fLblFom=nullptr, *fLblCoinc=nullptr,
           *fLblDrop=nullptr, *fLblDead=nullptr, *fLblRec=nullptr, *fLog=nullptr;

    // plots
    std::array<PlotWidget*, 16> fScope{};
    Heatmap2D* fPsdMap = nullptr;
    PlotWidget* fPeakSum = nullptr;
    PlotWidget* fTof = nullptr;
    RateBars* fRates = nullptr;
    QTableWidget* fCounters = nullptr;

    // accumulators
    std::array<QVector<QVector<double>>, 16> fScopeHist;
    int fPersistDepth = 10;
    sis::Hist1D fPsd1D{120, 0.0, 0.6};
    // Full 14-bit amplitude span: with the baseline parked low, positive pulses
    // (incl. high-energy muons) now reach ~14000 ADC, so a 4000 ceiling would
    // clip most of the spectrum. 200 bins over 16384 ≈ 82 ADC/bin.
    sis::Hist1D fPeakGamma{200, 0.0, 16384.0}, fPeakNeutron{200, 0.0, 16384.0};
    sis::Hist1D fTofHist{200, -200.0, 200.0};
    std::array<double,16> fRateHz{};
    std::array<bool,16> fActive{};
    std::array<bool,16> fInvert{};
    double fThreshold = 0.25;
    double fLastFom = 0; bool fFomValid = false;
};
