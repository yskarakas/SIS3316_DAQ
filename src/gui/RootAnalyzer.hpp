// ===========================================================================
//  RootAnalyzer — full native-ROOT offline analysis workspace.
//
//  All offline analysis runs directly on `.root` files written by this software
//  (no intermediate .bin). Feature set:
//    * TBrowser-like tree/branch explorer + run/config inspector,
//    * per-channel spectra of any branch (incl. FPGA charge peakSumFpga) with
//      per-channel colours, Gaussian fit, resolution, log-Y, normalisation,
//      ADC→energy calibration, CSV/PNG export,
//    * waveform navigator with full pulse metrics, baseline subtract, invert,
//    * waveform overlay + average per channel,
//    * PSD-vs-energy 2-D map with automatic n/γ Figure-of-Merit,
//    * COINCIDENCE analysis (for muon / multi-detector studies): timestamp
//      clustering with configurable window + multiplicity + channel set, giving
//      a coincidence-gated energy histogram (per-channel colours), a TOF
//      spectrum, a channel-vs-channel coincidence matrix and a multiplicity
//      distribution.
//
//  ROOT (TFile/TTree/TF1) is the analysis backend; the GUI is exclusively Qt6.
// ===========================================================================
#pragma once
#include <QWidget>
#include <QVector>
#include <array>

class QTreeWidget;
class QTreeWidgetItem;
class QTextEdit;
class QLabel;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QTableWidget;
class QLineEdit;
class QTimer;
class QTabWidget;
class QTreeWidgetItem;
class QPushButton;
class PlotWidget;
class Heatmap2D;

class RootAnalyzer : public QWidget {
    Q_OBJECT
public:
    explicit RootAnalyzer(QWidget* parent = nullptr);
    ~RootAnalyzer() override;

protected:
    bool eventFilter(QObject* obj, QEvent* ev) override;   // ↑/↓ history in terminal

private slots:
    void openFile();
    void onTreeActivated(QTreeWidgetItem* item, int col);
    void refreshSpectrum();
    void refreshPsd2D();
    void onEventRowChanged();
    void prevEvent();
    void nextEvent();
    void doFit();
    void exportCsv();
    void savePng();
    void refreshOverlay();
    void computeCoincidence();
    void addCalibPoint();
    void clearCalib();
    void runRootCommand();              // execute the ROOT-terminal command line
    void onInspectorActivated(QTreeWidgetItem* item, int col);
    void onRangeSelected(double x0, double x1);  // drag-selected interval on the plot
    void addInterval();                 // add current fit range to the multi-interval set
    void clearIntervals();              // clear the multi-interval set
    void resetSpecView();               // reset spectrum zoom

private:
    void buildUi();
    void buildInspectorTab(QTabWidget* tabs);   // the RootInspector tab
    void populateInspector();                   // fill the inspector structure tree
    void execRoot(const QString& cmd);          // ProcessLine + capture output + canvas
    void openPath(const QString& fn);   // open a .root file by path (no dialog)
    void populateTree();
    void loadCache();          // load channel+timestamp into RAM (once, on open)
    void loadEventTable();
    void showWaveform(long long entry);
    QVector<int> selectedChannels() const;
    QStringList numericBranches() const;
    bool calibration(double& slope, double& intercept) const;
    void fillColoredHist(PlotWidget* plot, const QVector<QVector<double>>& perChan,
                         double lo, double hi, const QString& title, const QString& xl,
                         bool logY);

    struct Impl;
    Impl* d = nullptr;

    QTreeWidget* fTree = nullptr;
    QTextEdit* fInfo = nullptr;
    QLabel* fPath = nullptr;
    QTimer* fSpecTimer = nullptr;      // debounce spectrum refresh on large files
    std::array<QCheckBox*,16> fChanChk{};

    // spectrum
    QComboBox* fVar = nullptr;
    QSpinBox* fBins = nullptr;
    QDoubleSpinBox* fXmin = nullptr;
    QDoubleSpinBox* fXmax = nullptr;
    QCheckBox* fLogY = nullptr;
    QCheckBox* fNorm = nullptr;
    QCheckBox* fUseCalib = nullptr;
    QCheckBox* fColorByChan = nullptr;
    QLabel* fFitInfo = nullptr;
    QDoubleSpinBox* fFitMin = nullptr;
    QDoubleSpinBox* fFitMax = nullptr;
    PlotWidget* fSpectrum = nullptr;
    QLabel* fNInfo = nullptr;                       // live N (counts) in the current interval
    QVector<QPair<double,double>> fFitIntervals;    // multiple fit/ROI intervals
    QPushButton* fZoomBtn = nullptr;                // toggle: drag = zoom
    QPushButton* fSelBtn = nullptr;                 // toggle: drag = select interval

    // calibration
    QTableWidget* fCalibTable = nullptr;
    QDoubleSpinBox* fCalibAdc = nullptr;
    QDoubleSpinBox* fCalibEnergy = nullptr;
    QLabel* fCalibInfo = nullptr;

    // psd 2d
    QComboBox* fPsdEnergyVar = nullptr;
    QComboBox* fPsdVar = nullptr;
    QLabel* fFomLabel = nullptr;
    Heatmap2D* fPsdMap = nullptr;

    // waveform navigator
    QTableWidget* fEventTable = nullptr;
    PlotWidget* fWave = nullptr;
    QLabel* fEventInfo = nullptr;
    QTextEdit* fMetrics = nullptr;
    QCheckBox* fBaselineSub = nullptr;
    QCheckBox* fInvert = nullptr;

    // overlay / average
    QComboBox* fOverlayChan = nullptr;
    QSpinBox* fOverlayN = nullptr;
    QCheckBox* fAverage = nullptr;
    QCheckBox* fOverlayBaseline = nullptr;
    PlotWidget* fOverlay = nullptr;

    // coincidence
    QDoubleSpinBox* fCoincWin = nullptr;
    QSpinBox* fCoincMult = nullptr;
    QComboBox* fCoincVar = nullptr;
    QLabel* fCoincInfo = nullptr;
    PlotWidget* fCoincEnergy = nullptr;
    PlotWidget* fCoincTof = nullptr;
    PlotWidget* fCoincMulti = nullptr;
    Heatmap2D* fCoincMatrix = nullptr;

    // RootInspector: browse + run ROOT commands on the file without launching ROOT
    QTreeWidget* fInspTree = nullptr;   // left: full file structure browser
    QLabel*      fInspImage = nullptr;  // left: rendered canvas image (Draw output)
    QTextEdit*   fInspOut = nullptr;    // right: ROOT terminal output log
    QLineEdit*   fInspCmd = nullptr;    // right: ROOT command input
    QStringList  fInspHistory;          // command history
    int          fInspHistPos = 0;
};
