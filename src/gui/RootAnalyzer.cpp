#include "RootAnalyzer.hpp"
#include "Heatmap2D.hpp"
#include "PlotWidget.h"
#include "../analysis/Analysis.hpp"
#include "../util/Logger.hpp"

#include <QtWidgets>
#include <QElapsedTimer>
#include <QTimer>
#include <QDebug>
#include <QDir>
#include <QFileInfo>

#include <TFile.h>
#include <TTree.h>
#include <TLeaf.h>
#include <TKey.h>
#include <TH1D.h>
#include <TF1.h>
#include <TROOT.h>
#include <TSystem.h>
#include <TCanvas.h>
#include <TString.h>
#include <TInterpreter.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <vector>

struct RootAnalyzer::Impl {
    TFile* file = nullptr;
    TTree* events = nullptr;
    QString path;

    // ---- in-memory cache (the fix for GUI freezes) --------------------------
    // Every spectrum/PSD/coincidence view used to re-scan the whole TTree on the
    // GUI thread — reading ALL branches (incl. the raw waveform) per entry, on
    // every checkbox/spinbox change — which froze the UI on large files. Now the
    // per-event channel + timestamp are loaded once into RAM, and each numeric
    // branch is loaded lazily the first time it is used (reading ONLY that branch,
    // never the raw waveform) and memoised. Redraws then run from RAM = instant.
    long long n = 0;                 // cached event count (may be < file entries)
    bool truncated = false;          // true if the file was larger than the cap
    std::vector<int>    ch;          // channel per event
    std::vector<double> ts;          // timestamp per event (double is exact ≤2^52)
    std::unordered_map<std::string, std::vector<float>> colCache;

    TCanvas* inspCanvas = nullptr;   // batch canvas for the RootInspector's Draw output

    // Lazily load (once) and return a numeric branch's values for all cached
    // events. Reads only that branch (raw disabled) so even the first pass is fast.
    const std::vector<float>& col(const std::string& name) {
        auto it = colCache.find(name);
        if (it != colCache.end()) return it->second;
        std::vector<float>& v = colCache[name];
        v.assign((size_t)n, 0.f);
        if (events && n > 0) {
            events->SetBranchStatus("*", 0);
            events->SetBranchStatus(name.c_str(), 1);
            TLeaf* lf = events->GetLeaf(name.c_str());
            for (long long i = 0; i < n; ++i) { events->GetEntry(i); if (lf) v[(size_t)i] = (float)lf->GetValue(); }
            events->SetBranchStatus("*", 1);
        }
        return v;
    }
    ~Impl() { if (file) { file->Close(); delete file; } }
};

RootAnalyzer::RootAnalyzer(QWidget* parent) : QWidget(parent), d(new Impl) {
    buildUi();
    // Headless performance regression hook: SIS_ANALYZER_BENCH=<file.root> opens
    // the file and times many rapid view refreshes (the operation that used to
    // freeze the GUI). Prints timings and quits. Used to validate the cache.
    if (qEnvironmentVariableIsSet("SIS_ANALYZER_BENCH")) {
        QString f = qEnvironmentVariable("SIS_ANALYZER_BENCH");
        QTimer::singleShot(50, this, [this,f]{
            QElapsedTimer t; t.start(); openPath(f); qint64 tOpen=t.elapsed();
            t.restart(); for(int k=0;k<20;++k){ fBins->setValue(200+k); refreshSpectrum(); } qint64 tSpec=t.elapsed();
            t.restart(); for(int k=0;k<10;++k) refreshPsd2D(); qint64 tPsd=t.elapsed();
            t.restart(); fCoincMult->setValue(2); computeCoincidence(); qint64 tCoinc=t.elapsed();
            qInfo().noquote() << QString("ANALYZER_BENCH cached=%1 open=%2ms 20x_spectrum=%3ms 10x_psd2d=%4ms coinc=%5ms")
                .arg((qlonglong)d->n).arg(tOpen).arg(tSpec).arg(tPsd).arg(tCoinc);
            // --- Gaussian-fit sanity: μ must stay INSIDE the chosen interval ---
            fVar->setCurrentText("amplitudeSw"); fFitMin->setValue(0); fFitMax->setValue(0); refreshSpectrum();
            for (auto win : {qMakePair(2000.0,6000.0), qMakePair(500.0,2000.0), qMakePair(8000.0,12000.0)}) {
                fFitIntervals.clear(); fFitIntervals.push_back(win); doFit();
                qInfo().noquote() << QString("FIT_TEST [%1,%2]: %3").arg(win.first,0,'f',0).arg(win.second,0,'f',0).arg(fFitInfo->text());
            }
            if (qEnvironmentVariableIsSet("SIS_SHOT")) {   // render the analyzer to a PNG
                fFitIntervals.clear(); fFitIntervals.push_back(qMakePair(200.0,1500.0));
                fFitIntervals.push_back(qMakePair(3000.0,6000.0)); doFit();
                this->resize(1720,1000); this->grab().save(qEnvironmentVariable("SIS_SHOT"));
                qInfo().noquote() << "SHOT saved:" << qEnvironmentVariable("SIS_SHOT");
            }
            qApp->quit();
        });
    }
    // Headless RootInspector test: open a file, run a few ROOT commands, confirm
    // the interpreter output was captured and a Draw rendered to an image.
    if (qEnvironmentVariableIsSet("SIS_INSPECTOR_TEST")) {
        QString f = qEnvironmentVariable("SIS_INSPECTOR_TEST");
        QTimer::singleShot(50, this, [this,f]{
            openPath(f);
            execRoot("Events->GetEntries()");
            execRoot("Events->Draw(\"qTotalSw\")");
            execRoot("RunSummary->Scan(\"totalEvents:durationSec\")");
            QString png = QDir::tempPath()+"/sis_root_canvas.png";
            bool img = fInspImage && !fInspImage->pixmap(Qt::ReturnByValue).isNull();
            qInfo().noquote() << "==INSPECTOR_TEST output==\n" + fInspOut->toPlainText();
            qInfo().noquote() << QString("==INSPECTOR_TEST canvasPNG=%1 imageShown=%2==")
                .arg(QFileInfo(png).exists()?"yes":"no").arg(img?"yes":"no");
            qApp->quit();
        });
    }
}
RootAnalyzer::~RootAnalyzer() { delete d; }

// Hard cap on raw samples we will read from a file (matches the writer cap and
// protects against a foreign file whose raw[nraw] is larger than our buffer).
static constexpr int kMaxRawRead = 65536;

// helper: read one event's raw waveform + channel + timestamp. Safe against
// missing branches and oversized nraw (reads nraw first, clamps, then sizes the
// buffer before reading the array — no static/uninitialised buffers).
static bool readRaw(TTree* t, long long entry, std::vector<uint16_t>& out, int& ch, unsigned long long& ts) {
    out.clear(); ch = 0; ts = 0;
    if (!t || !t->GetBranch("nraw") || !t->GetBranch("raw")) return false;
    Int_t nraw = 0;
    TLeaf* chL = t->GetLeaf("channel"); TLeaf* tsL = t->GetLeaf("timestamp");
    t->SetBranchAddress("nraw", &nraw);
    t->GetEntry(entry);                       // load nraw (+ any leaf caches)
    ch = chL ? (int)chL->GetValue() : 0;
    ts = tsL ? (unsigned long long)tsL->GetValue() : 0;
    int n = std::max(0, std::min<int>(nraw, kMaxRawRead));
    std::vector<Short_t> raw(std::max(1, n));
    t->SetBranchAddress("raw", raw.data());
    t->GetEntry(entry);                       // now read the clamped array
    out.resize(n);
    for (int i=0;i<n;++i) out[i] = (uint16_t)raw[i];
    t->ResetBranchAddresses();
    return n > 0;
}

// count events in [x0,x1] for the selected channels (in-memory, instant) — defined below
static long long countInRange(const std::vector<int>& ch, const std::vector<float>& val,
                              long long n, const QVector<int>& chans,
                              double slope, double intercept, bool cal, double x0, double x1);

// --------------------------------------------------------------------------
void RootAnalyzer::buildUi() {
    auto* root = new QVBoxLayout(this);
    auto* bar = new QHBoxLayout();
    auto* btnOpen = new QPushButton("📂  Open ROOT File…"); btnOpen->setObjectName("run");
    fPath = new QLabel("No file open"); fPath->setStyleSheet("color:#8a94a6;");
    bar->addWidget(btnOpen); bar->addWidget(fPath); bar->addStretch();
    root->addLayout(bar);
    connect(btnOpen, &QPushButton::clicked, this, &RootAnalyzer::openFile);

    // debounce timer: coalesce rapid spinbox edits into one spectrum refresh
    fSpecTimer = new QTimer(this); fSpecTimer->setSingleShot(true); fSpecTimer->setInterval(150);
    connect(fSpecTimer, &QTimer::timeout, this, &RootAnalyzer::refreshSpectrum);

    auto* split = new QSplitter(Qt::Horizontal);

    // ---- left: explorer + channel filter ----
    auto* leftW = new QWidget(); auto* lv = new QVBoxLayout(leftW);
    lv->setContentsMargins(0,0,0,0);
    fTree = new QTreeWidget(); fTree->setHeaderLabels({"Tree / Branch","Type"});
    connect(fTree, &QTreeWidget::itemActivated, this, &RootAnalyzer::onTreeActivated);
    lv->addWidget(fTree, 1);
    auto* chanBox = new QGroupBox("Channels (filter + coincidence set)"); auto* cg = new QGridLayout(chanBox);
    cg->setSpacing(2);
    for (int c=0;c<16;++c){ fChanChk[c]=new QCheckBox(QString::number(c+1));
        fChanChk[c]->setChecked(true); cg->addWidget(fChanChk[c], c/4, c%4);
        connect(fChanChk[c], &QCheckBox::toggled, this, [this]{ refreshSpectrum(); refreshPsd2D(); }); }
    auto* allBtn = new QPushButton("all"); auto* noneBtn = new QPushButton("none");
    // Bulk-toggle without firing 16× the per-box refresh (which each redraw the
    // spectrum + PSD map); block signals, set all, then refresh once.
    auto setAll=[this](bool on){ for(auto*c:fChanChk){ c->blockSignals(true); c->setChecked(on); c->blockSignals(false); }
                                 refreshSpectrum(); refreshPsd2D(); };
    connect(allBtn,&QPushButton::clicked,this,[setAll]{ setAll(true); });
    connect(noneBtn,&QPushButton::clicked,this,[setAll]{ setAll(false); });
    cg->addWidget(allBtn,4,0,1,2); cg->addWidget(noneBtn,4,2,1,2);
    lv->addWidget(chanBox);
    leftW->setMinimumWidth(230); leftW->setMaximumWidth(300);
    split->addWidget(leftW);

    auto* tabs = new QTabWidget();
    const int kCombo = 150;   // readable minimum combo width

    // ================= Spectrum + Fit + Calibration =================
    auto* specTab = new QWidget(); auto* sv = new QVBoxLayout(specTab);
    auto* sc = new QHBoxLayout();
    fVar = new QComboBox(); fVar->setMinimumWidth(kCombo);
    fBins = new QSpinBox(); fBins->setRange(10,4000); fBins->setValue(256);
    fXmin = new QDoubleSpinBox(); fXmin->setRange(-1e9,1e9); fXmin->setDecimals(2);
    fXmax = new QDoubleSpinBox(); fXmax->setRange(-1e9,1e9); fXmax->setDecimals(2);
    fLogY = new QCheckBox("log Y"); fNorm = new QCheckBox("normalize");
    fUseCalib = new QCheckBox("energy calib"); fColorByChan = new QCheckBox("colours by channel");
    auto* autoBtn = new QPushButton("auto range");
    sc->addWidget(new QLabel("Variable")); sc->addWidget(fVar);
    sc->addWidget(new QLabel("Bins")); sc->addWidget(fBins);
    sc->addWidget(new QLabel("min")); sc->addWidget(fXmin);
    sc->addWidget(new QLabel("max")); sc->addWidget(fXmax);
    sc->addWidget(autoBtn); sc->addWidget(fLogY); sc->addWidget(fNorm);
    sc->addWidget(fUseCalib); sc->addWidget(fColorByChan);
    sc->addStretch();
    sv->addLayout(sc);
    // spectrum plot (created early so the toolbar can wire to it)
    fSpectrum = new PlotWidget(); fSpectrum->setMinimumWidth(360);
    fSpectrum->setInteractionMode(3);   // default: drag = select fit interval

    auto* fc = new QHBoxLayout();
    fFitMin = new QDoubleSpinBox(); fFitMin->setRange(-1e9,1e9); fFitMin->setDecimals(2);
    fFitMax = new QDoubleSpinBox(); fFitMax->setRange(-1e9,1e9); fFitMax->setDecimals(2);
    auto* fitBtn = new QPushButton("Gaussian fit"); fitBtn->setObjectName("run");
    auto* addIvBtn = new QPushButton("＋ add interval");
    auto* clrIvBtn = new QPushButton("clear intervals");
    auto* csvBtn = new QPushButton("Export CSV"); auto* pngBtn = new QPushButton("Save PNG");
    fFitInfo = new QLabel("—"); fFitInfo->setStyleSheet("color:#9ad17a;");
    fFitInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    fc->addWidget(new QLabel("Fit range")); fc->addWidget(fFitMin); fc->addWidget(fFitMax);
    fc->addWidget(addIvBtn); fc->addWidget(fitBtn); fc->addWidget(clrIvBtn);
    fc->addWidget(fFitInfo,1); fc->addWidget(csvBtn); fc->addWidget(pngBtn);
    sv->addLayout(fc);

    // plot-interaction toolbar: drag = select fit interval (default) or zoom X.
    auto* tb = new QHBoxLayout();
    fSelBtn  = new QPushButton("▭ Select interval (drag)"); fSelBtn->setCheckable(true); fSelBtn->setChecked(true);
    fZoomBtn = new QPushButton("🔍 Zoom (drag)");           fZoomBtn->setCheckable(true);
    auto* resetBtn = new QPushButton("Reset view");
    fNInfo = new QLabel(""); fNInfo->setStyleSheet("color:#5cc8ff;font-weight:700;");
    tb->addWidget(new QLabel("Tools:")); tb->addWidget(fSelBtn); tb->addWidget(fZoomBtn);
    tb->addWidget(resetBtn); tb->addSpacing(14); tb->addWidget(fNInfo,1); tb->addStretch();
    sv->addLayout(tb);

    connect(fitBtn,  &QPushButton::clicked, this, &RootAnalyzer::doFit);
    connect(addIvBtn,&QPushButton::clicked, this, &RootAnalyzer::addInterval);
    connect(clrIvBtn,&QPushButton::clicked, this, &RootAnalyzer::clearIntervals);
    connect(resetBtn,&QPushButton::clicked, this, &RootAnalyzer::resetSpecView);
    connect(fSpectrum, &PlotWidget::rangeSelected, this, &RootAnalyzer::onRangeSelected);
    connect(fSelBtn, &QPushButton::clicked, this, [this]{ fSelBtn->setChecked(true); fZoomBtn->setChecked(false);
                                                          fSpectrum->setInteractionMode(3); });
    connect(fZoomBtn,&QPushButton::clicked, this, [this]{ fZoomBtn->setChecked(true); fSelBtn->setChecked(false);
                                                          fSpectrum->setInteractionMode(1); });
    // live N in the current fit range whenever the range spinboxes change
    auto showN=[this]{ if(!d->events||d->n==0){ fNInfo->setText(""); return; }
        double a=fFitMin->value(), b=fFitMax->value(); if(!(b>a)){ fNInfo->setText(""); return; }
        double sl=1,ic=0; bool cal=fUseCalib->isChecked()&&calibration(sl,ic);
        const std::vector<float>& v=d->col(fVar->currentText().toStdString());
        long long N=countInRange(d->ch, v, d->n, selectedChannels(), sl, ic, cal, a, b);
        fNInfo->setText(QString("N in [%1, %2] = %3 events").arg(a,0,'f',1).arg(b,0,'f',1).arg(N)); };
    connect(fFitMin, &QDoubleSpinBox::valueChanged, this, [showN](double){ showN(); });
    connect(fFitMax, &QDoubleSpinBox::valueChanged, this, [showN](double){ showN(); });

    auto* specSplit = new QSplitter(Qt::Horizontal);
    specSplit->addWidget(fSpectrum);
    auto* calibW = new QGroupBox("ADC → Energy calibration"); calibW->setMaximumWidth(260);
    auto* cvl = new QVBoxLayout(calibW);
    fCalibTable = new QTableWidget(0,2); fCalibTable->setHorizontalHeaderLabels({"ADC","Energy"});
    fCalibTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    fCalibTable->verticalHeader()->setVisible(false);
    cvl->addWidget(fCalibTable);
    auto* cadd = new QHBoxLayout();
    fCalibAdc = new QDoubleSpinBox(); fCalibAdc->setRange(0,1e9); fCalibAdc->setDecimals(1);
    fCalibEnergy = new QDoubleSpinBox(); fCalibEnergy->setRange(0,1e9); fCalibEnergy->setDecimals(3);
    auto* addBtn = new QPushButton("+"); auto* clrBtn = new QPushButton("clear");
    cadd->addWidget(fCalibAdc); cadd->addWidget(fCalibEnergy); cadd->addWidget(addBtn); cadd->addWidget(clrBtn);
    cvl->addLayout(cadd);
    fCalibInfo = new QLabel("Add ≥2 points (ADC, keVee).\nFit μ auto-fills ADC.");
    fCalibInfo->setStyleSheet("color:#8a94a6;font-size:11px;"); fCalibInfo->setWordWrap(true);
    cvl->addWidget(fCalibInfo);
    specSplit->addWidget(calibW);
    specSplit->setStretchFactor(0,1); specSplit->setChildrenCollapsible(false);
    sv->addWidget(specSplit, 1);
    tabs->addTab(specTab, "Spectrum + Fit");
    auto sched=[this]{ fSpecTimer->start(); };   // debounced refresh
    // Switching variable must RESET the x-range (auto-range for the new branch) and
    // clear fit intervals — otherwise the old branch's (possibly huge) range makes
    // the histogram bins far too wide for the new branch.
    connect(fVar, &QComboBox::currentIndexChanged, this, [this]{
        fFitIntervals.clear(); fFitInfo->setText("—"); if(fNInfo) fNInfo->setText("");
        fXmin->blockSignals(true); fXmax->blockSignals(true);
        fXmin->setValue(0); fXmax->setValue(0);
        fXmin->blockSignals(false); fXmax->blockSignals(false);
        refreshSpectrum();
    });
    connect(fBins, &QSpinBox::valueChanged, this, sched);
    connect(fXmin, &QDoubleSpinBox::valueChanged, this, sched);
    connect(fXmax, &QDoubleSpinBox::valueChanged, this, sched);
    connect(fLogY, &QCheckBox::toggled, this, &RootAnalyzer::refreshSpectrum);
    connect(fNorm, &QCheckBox::toggled, this, &RootAnalyzer::refreshSpectrum);
    connect(fUseCalib, &QCheckBox::toggled, this, &RootAnalyzer::refreshSpectrum);
    connect(fColorByChan, &QCheckBox::toggled, this, &RootAnalyzer::refreshSpectrum);
    connect(autoBtn, &QPushButton::clicked, this, [this]{ fXmin->setValue(0); fXmax->setValue(0); refreshSpectrum(); });
    connect(csvBtn, &QPushButton::clicked, this, &RootAnalyzer::exportCsv);
    connect(pngBtn, &QPushButton::clicked, this, &RootAnalyzer::savePng);
    connect(addBtn, &QPushButton::clicked, this, &RootAnalyzer::addCalibPoint);
    connect(clrBtn, &QPushButton::clicked, this, &RootAnalyzer::clearCalib);

    // ================= PSD 2D + FOM =================
    auto* psdTab = new QWidget(); auto* pv = new QVBoxLayout(psdTab);
    auto* pc = new QHBoxLayout();
    fPsdEnergyVar = new QComboBox(); fPsdEnergyVar->setMinimumWidth(kCombo);
    fPsdVar = new QComboBox(); fPsdVar->setMinimumWidth(kCombo);
    fFomLabel = new QLabel("FOM: —"); fFomLabel->setStyleSheet("color:#9ad17a;font-weight:700;");
    auto* psdBtn = new QPushButton("Recompute");
    pc->addWidget(new QLabel("Energy X")); pc->addWidget(fPsdEnergyVar);
    pc->addWidget(new QLabel("PSD Y")); pc->addWidget(fPsdVar);
    pc->addWidget(psdBtn); pc->addWidget(fFomLabel,1); pc->addStretch();
    pv->addLayout(pc);
    fPsdMap = new Heatmap2D(); fPsdMap->setLabels("PSD vs Energy", "Energy", "PSD");
    pv->addWidget(fPsdMap, 1);
    tabs->addTab(psdTab, "PSD 2D + FOM");
    connect(psdBtn, &QPushButton::clicked, this, &RootAnalyzer::refreshPsd2D);
    connect(fPsdEnergyVar, &QComboBox::currentIndexChanged, this, &RootAnalyzer::refreshPsd2D);
    connect(fPsdVar, &QComboBox::currentIndexChanged, this, &RootAnalyzer::refreshPsd2D);

    // ================= Waveforms + metrics =================
    auto* wfTab = new QWidget(); auto* wv = new QHBoxLayout(wfTab);
    fEventTable = new QTableWidget(0, 4);
    fEventTable->setHorizontalHeaderLabels({"#","Ch","Energy","PSD"});
    fEventTable->verticalHeader()->setVisible(false);
    fEventTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    fEventTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    fEventTable->setMaximumWidth(300);
    connect(fEventTable, &QTableWidget::itemSelectionChanged, this, &RootAnalyzer::onEventRowChanged);
    auto* wfRight = new QVBoxLayout();
    auto* navBar = new QHBoxLayout();
    auto* prevBtn = new QPushButton("◀ prev"); auto* nextBtn = new QPushButton("next ▶");
    fBaselineSub = new QCheckBox("baseline subtract"); fInvert = new QCheckBox("invert");
    fEventInfo = new QLabel("—"); fEventInfo->setStyleSheet("color:#8a94a6;");
    navBar->addWidget(prevBtn); navBar->addWidget(nextBtn);
    navBar->addWidget(fBaselineSub); navBar->addWidget(fInvert); navBar->addWidget(fEventInfo,1);
    wfRight->addLayout(navBar);
    auto* wfSplit = new QSplitter(Qt::Horizontal);
    fWave = new PlotWidget(); fWave->setMinimumWidth(360);
    fMetrics = new QTextEdit(); fMetrics->setReadOnly(true); fMetrics->setMaximumWidth(230);
    wfSplit->addWidget(fWave); wfSplit->addWidget(fMetrics);
    wfSplit->setStretchFactor(0,1); wfSplit->setChildrenCollapsible(false);
    wfRight->addWidget(wfSplit, 1);
    wv->addWidget(fEventTable); wv->addLayout(wfRight, 1);
    tabs->addTab(wfTab, "Waveforms");
    connect(prevBtn, &QPushButton::clicked, this, &RootAnalyzer::prevEvent);
    connect(nextBtn, &QPushButton::clicked, this, &RootAnalyzer::nextEvent);
    connect(fBaselineSub, &QCheckBox::toggled, this, &RootAnalyzer::onEventRowChanged);
    connect(fInvert, &QCheckBox::toggled, this, &RootAnalyzer::onEventRowChanged);

    // ================= Overlay / Average =================
    auto* ovTab = new QWidget(); auto* ov = new QVBoxLayout(ovTab);
    auto* oc = new QHBoxLayout();
    fOverlayChan = new QComboBox(); fOverlayChan->setMinimumWidth(kCombo);
    for(int c=0;c<16;++c) fOverlayChan->addItem(QString("Channel %1").arg(c+1));
    fOverlayN = new QSpinBox(); fOverlayN->setRange(1,500); fOverlayN->setValue(50);
    fAverage = new QCheckBox("show average");
    fOverlayBaseline = new QCheckBox("baseline subtract"); fOverlayBaseline->setChecked(true);
    auto* ovBtn = new QPushButton("Update");
    oc->addWidget(new QLabel("Channel")); oc->addWidget(fOverlayChan);
    oc->addWidget(new QLabel("Count")); oc->addWidget(fOverlayN);
    oc->addWidget(fAverage); oc->addWidget(fOverlayBaseline); oc->addWidget(ovBtn);
    oc->addStretch();
    ov->addLayout(oc);
    fOverlay = new PlotWidget(); ov->addWidget(fOverlay, 1);
    tabs->addTab(ovTab, "Overlay / Average");
    connect(ovBtn, &QPushButton::clicked, this, &RootAnalyzer::refreshOverlay);
    connect(fAverage, &QCheckBox::toggled, this, &RootAnalyzer::refreshOverlay);

    // ================= Coincidence (muon / multi-detector) =================
    auto* coTab = new QWidget(); auto* cov = new QVBoxLayout(coTab);
    auto* coc = new QHBoxLayout();
    fCoincWin = new QDoubleSpinBox(); fCoincWin->setRange(0,100000); fCoincWin->setValue(40); fCoincWin->setSuffix(" ns");
    fCoincMult = new QSpinBox(); fCoincMult->setRange(2,16); fCoincMult->setValue(2);
    fCoincVar = new QComboBox(); fCoincVar->setMinimumWidth(kCombo);
    auto* coBtn = new QPushButton("Compute coincidences"); coBtn->setObjectName("run");
    fCoincInfo = new QLabel("Select the coincidence channel set on the left, then Compute.");
    fCoincInfo->setStyleSheet("color:#9ad17a;");
    coc->addWidget(new QLabel("Window ±")); coc->addWidget(fCoincWin);
    coc->addWidget(new QLabel("Min multiplicity")); coc->addWidget(fCoincMult);
    coc->addWidget(new QLabel("Energy")); coc->addWidget(fCoincVar);
    coc->addWidget(coBtn); coc->addStretch();
    cov->addLayout(coc);
    cov->addWidget(fCoincInfo);
    auto* coTabs = new QTabWidget();
    fCoincEnergy = new PlotWidget(); coTabs->addTab(fCoincEnergy, "Coincidence energy (per channel)");
    fCoincTof = new PlotWidget();    coTabs->addTab(fCoincTof, "TOF (Δt)");
    fCoincMatrix = new Heatmap2D();  fCoincMatrix->setBins(16,16); fCoincMatrix->setRange(0,16,0,16);
    fCoincMatrix->setLogZ(false);    fCoincMatrix->setLabels("Coincidence matrix", "Channel A", "Channel B");
    coTabs->addTab(fCoincMatrix, "Coincidence matrix");
    fCoincMulti = new PlotWidget();  coTabs->addTab(fCoincMulti, "Multiplicity distribution");
    cov->addWidget(coTabs, 1);
    tabs->addTab(coTab, "⚡ Coincidence");
    connect(coBtn, &QPushButton::clicked, this, &RootAnalyzer::computeCoincidence);

    // ================= 🔎 ROOT Inspector =================
    buildInspectorTab(tabs);

    // ================= Run / Config =================
    fInfo = new QTextEdit(); fInfo->setReadOnly(true);
    tabs->addTab(fInfo, "Run / Config");

    split->addWidget(tabs);
    split->setStretchFactor(1, 1);
    root->addWidget(split, 1);
}

// --------------------------------------------------------------------------
void RootAnalyzer::openFile() {
    QString fn = QFileDialog::getOpenFileName(this, "Open ROOT file", QString(), "ROOT files (*.root)");
    if (fn.isEmpty()) return;
    openPath(fn);
}

void RootAnalyzer::openPath(const QString& fn) {
    if (fn.isEmpty()) return;
    if (d->file) { d->file->Close(); delete d->file; d->file=nullptr; d->events=nullptr; }
    d->file = TFile::Open(fn.toUtf8().constData(), "READ");
    if (!d->file || d->file->IsZombie()) {
        QMessageBox::warning(this, "Open failed", "Could not open " + fn);
        if (d->file) { delete d->file; d->file=nullptr; } return;
    }
    d->path = fn; fPath->setText(fn);
    d->events = dynamic_cast<TTree*>(d->file->Get("Events"));
    SIS_LOG_INFO("ROOT", "opened " + fn.toStdString());
    loadCache();               // pull channel+timestamp into RAM up-front (no freeze)
    populateTree();
    QStringList vars = numericBranches();
    for (QComboBox* cb : {fVar, fPsdEnergyVar, fPsdVar, fCoincVar}) {
        cb->blockSignals(true); cb->clear(); cb->addItems(vars); cb->blockSignals(false);
    }
    // Default to the SOFTWARE (baseline-subtracted) quantities — these are the
    // validated, physically-correct energy/PSD. The raw FPGA accumulators
    // (peakSumFpga, psdFpga, acc1-4) are baseline-dominated and remain selectable
    // for reference but are not the default (see the physics validation).
    auto pick=[&](QComboBox* cb, const QString& p, const QString& fb){
        int i=cb->findText(p); if(i<0) i=cb->findText(fb); if(i>=0) cb->setCurrentIndex(i); };
    pick(fVar, "qTotalSw", "amplitudeSw");
    pick(fPsdEnergyVar, "qTotalSw", "amplitudeSw");
    pick(fPsdVar, "psdSw", "psdFpga");
    pick(fCoincVar, "qTotalSw", "amplitudeSw");
    loadEventTable();
    refreshSpectrum();
    refreshPsd2D();

    // ---- RootInspector: browse structure + bind trees into the interpreter ----
    populateInspector();
    if (d->file) {
        d->file->cd();
        gROOT->ProcessLine(TString::Format("sisFile=(TFile*)%p;", (void*)d->file));
        gROOT->ProcessLine("Events=(TTree*)(sisFile?sisFile->Get(\"Events\"):nullptr);");
        gROOT->ProcessLine("RunSummary=(TTree*)(sisFile?sisFile->Get(\"RunSummary\"):nullptr);");
        gROOT->ProcessLine("HardwareConfig=(TTree*)(sisFile?sisFile->Get(\"HardwareConfig\"):nullptr);");
        if (fInspOut)
            fInspOut->append(QString("<b style=\"color:#9ad17a\">Loaded %1  (Events = %2 entries)</b>")
                             .arg(QFileInfo(fn).fileName()).arg(d->events? (long long)d->events->GetEntries():0));
    }
}

void RootAnalyzer::populateTree() {
    fTree->clear();
    if (!d->file) return;
    QString infoText;
    TIter next(d->file->GetListOfKeys()); TKey* key;
    while ((key = (TKey*)next())) {
        auto* tree = dynamic_cast<TTree*>(key->ReadObj());
        if (!tree) continue;
        auto* top = new QTreeWidgetItem(fTree);
        top->setText(0, QString("%1 (%2)").arg(tree->GetName()).arg((long long)tree->GetEntries()));
        top->setData(0, Qt::UserRole, "TREE");
        TIter lnext(tree->GetListOfLeaves()); TLeaf* leaf;
        while ((leaf = (TLeaf*)lnext())) {
            auto* child = new QTreeWidgetItem(top);
            child->setText(0, leaf->GetName()); child->setText(1, leaf->GetTypeName());
            child->setData(0, Qt::UserRole, tree->GetName());
        }
        QString tn = tree->GetName();
        if (tn == "RunSummary" || tn == "HardwareConfig") {
            infoText += QString("=== %1 ===\n").arg(tn);
            if (tree->GetEntries()>0){ tree->GetEntry(0);
                TIter l2(tree->GetListOfLeaves()); TLeaf* lf;
                while ((lf=(TLeaf*)l2())){ infoText += QString("  %1 = ").arg(lf->GetName());
                    int n=std::min(lf->GetLen(),16); for(int i=0;i<n;++i) infoText+=QString::number(lf->GetValue(i))+" ";
                    infoText += "\n"; } }
            infoText += "\n";
        }
    }
    fTree->expandAll();
    fInfo->setPlainText(infoText);
}

QStringList RootAnalyzer::numericBranches() const {
    QStringList out;
    if (!d->events) return out;
    TIter l(d->events->GetListOfLeaves()); TLeaf* leaf;
    while ((leaf=(TLeaf*)l())){ QString n=leaf->GetName(); if(n=="raw") continue; out<<n; }
    return out;
}

// Load channel + timestamp for every event into RAM once, capped so a pathological
// file cannot exhaust memory. Numeric branches are loaded lazily on first use via
// Impl::col(). This is what makes the analyzer views instant (no per-view rescan).
void RootAnalyzer::loadCache() {
    d->ch.clear(); d->ts.clear(); d->colCache.clear(); d->n = 0; d->truncated = false;
    if (!d->events) return;
    long long N = d->events->GetEntries();
    const long long kCap = 5000000;              // ≈ ballpark 1 h at ~800 Hz
    d->n = std::min(N, kCap);
    d->truncated = d->n < N;
    d->ch.resize((size_t)d->n);
    d->ts.resize((size_t)d->n);
    d->events->SetBranchStatus("*", 0);
    d->events->SetBranchStatus("channel", 1);
    d->events->SetBranchStatus("timestamp", 1);
    TLeaf* chL = d->events->GetLeaf("channel");
    TLeaf* tsL = d->events->GetLeaf("timestamp");
    for (long long i = 0; i < d->n; ++i) {
        d->events->GetEntry(i);
        d->ch[(size_t)i] = chL ? (int)chL->GetValue() : 0;
        d->ts[(size_t)i] = tsL ? (double)tsL->GetValue() : 0.0;
    }
    d->events->SetBranchStatus("*", 1);
    if (d->truncated)
        SIS_LOG_WARN("ROOT", "file has " + std::to_string(N) + " events; analyzer caches first "
                     + std::to_string(d->n));
}

QVector<int> RootAnalyzer::selectedChannels() const {
    QVector<int> v; for(int c=0;c<16;++c) if(fChanChk[c]->isChecked()) v<<c; return v;
}

void RootAnalyzer::onTreeActivated(QTreeWidgetItem* item, int) {
    if (!item) return;
    QString role = item->data(0, Qt::UserRole).toString();
    if (role=="TREE" || role.isEmpty()) return;
    if (role=="Events"){ int i=fVar->findText(item->text(0)); if(i>=0) fVar->setCurrentIndex(i); }
}

bool RootAnalyzer::calibration(double& slope, double& intercept) const {
    int n = fCalibTable->rowCount();
    if (n < 2) return false;
    double sx=0,sy=0,sxx=0,sxy=0;
    for (int i=0;i<n;++i){ double x=fCalibTable->item(i,0)->text().toDouble();
        double y=fCalibTable->item(i,1)->text().toDouble();
        sx+=x; sy+=y; sxx+=x*x; sxy+=x*y; }
    double det = n*sxx - sx*sx; if (std::fabs(det)<1e-12) return false;
    slope = (n*sxy - sx*sy)/det; intercept = (sy - slope*sx)/n; return true;
}

void RootAnalyzer::addCalibPoint() {
    double adc = fCalibAdc->value();
    int r=fCalibTable->rowCount(); fCalibTable->insertRow(r);
    fCalibTable->setItem(r,0,new QTableWidgetItem(QString::number(adc,'f',1)));
    fCalibTable->setItem(r,1,new QTableWidgetItem(QString::number(fCalibEnergy->value(),'f',3)));
    double s,b; if(calibration(s,b)) fCalibInfo->setText(QString("E = %1·ADC + %2").arg(s,0,'g',4).arg(b,0,'g',4));
    if (fUseCalib->isChecked()) refreshSpectrum();
}
void RootAnalyzer::clearCalib(){ fCalibTable->setRowCount(0); fCalibInfo->setText("Add ≥2 points."); if(fUseCalib->isChecked()) refreshSpectrum(); }

// ---- coloured multi-channel histogram helper ------------------------------
void RootAnalyzer::fillColoredHist(PlotWidget* plot, const QVector<QVector<double>>& perChan,
                                   double lo, double hi, const QString& title,
                                   const QString& xl, bool logY) {
    int nb = perChan.isEmpty()? 0 : perChan[0].size();
    HistData hd; hd.title=title; hd.xLabel=xl; hd.yLabel="Counts";
    hd.binEdges.resize(nb+1); for(int i=0;i<=nb;++i) hd.binEdges[i]=lo+(hi-lo)*i/std::max(1,nb);
    for (int c=0;c<16;++c){
        double s=0; for(double v:perChan[c]) s+=v;
        if (s<=0) continue;
        hd.channelCounts.push_back(perChan[c]);
        hd.channelLabels.push_back(QString("Ch %1").arg(c+1));
    }
    // `counts` MUST always be populated: PlotWidget uses it both as the render
    // guard (an empty counts vector suppresses the whole plot) and to derive the
    // y-axis maximum. Here it holds the per-bin sum over all channels (the total
    // spectrum) so the coloured per-channel bars are correctly scaled underneath.
    hd.counts = QVector<double>(nb, 0.0);
    for (int c=0;c<16;++c){ int m=std::min(nb,(int)perChan[c].size());
        for (int i=0;i<m;++i) hd.counts[i]+=perChan[c][i]; }
    plot->setHistogram(hd, logY);
}

// ---- spectrum -------------------------------------------------------------
void RootAnalyzer::refreshSpectrum() {
    if (!d->events || fVar->currentText().isEmpty() || d->n == 0) return;
    const std::vector<float>& val = d->col(fVar->currentText().toStdString());
    QVector<int> chans = selectedChannels();
    auto chanOk=[&](int ch){ for(int c:chans) if(c==ch) return true; return chans.isEmpty(); };
    double slope=1,intercept=0; bool cal = fUseCalib->isChecked() && calibration(slope,intercept);
    auto xform=[&](double v){ return cal ? slope*v+intercept : v; };

    double lo=fXmin->value(), hi=fXmax->value();
    if (!(hi>lo)) {
        lo=std::numeric_limits<double>::max(); hi=-lo;
        for (long long i=0;i<d->n;++i){ if(!chanOk(d->ch[i])) continue;
            double v=xform(val[i]); lo=std::min(lo,v); hi=std::max(hi,v); }
        if(!(hi>lo)) hi=lo+1;
        fXmin->blockSignals(true); fXmax->blockSignals(true);
        fXmin->setValue(lo); fXmax->setValue(hi); fFitMin->setValue(lo); fFitMax->setValue(hi);
        fXmin->blockSignals(false); fXmax->blockSignals(false);
    }
    int nb=fBins->value();
    QString xl = cal? "Energy" : fVar->currentText();

    if (fColorByChan->isChecked()) {
        QVector<QVector<double>> perChan(16, QVector<double>(nb, 0.0));
        for (long long i=0;i<d->n;++i){ int ch=d->ch[i];
            if (!chanOk(ch) || ch<0 || ch>15) continue;
            double v=xform(val[i]);
            int bin = int((v-lo)/(hi-lo)*nb); if(bin>=0 && bin<nb) perChan[ch][bin]+=1.0;
        }
        fillColoredHist(fSpectrum, perChan, lo, hi, "Spectrum by channel: "+fVar->currentText(), xl, fLogY->isChecked());
        return;
    }

    sis::Hist1D h(nb, lo, hi + (hi-lo)*1e-9);
    for (long long i=0;i<d->n;++i){ if(!chanOk(d->ch[i])) continue; h.fill(xform(val[i])); }
    double norm=1.0; if(fNorm->isChecked()){ double s=0; for(double b:h.bins)s+=b; if(s>0)norm=1.0/s; }
    HistData hd; hd.title="Spectrum: "+fVar->currentText()+(cal?" (calibrated)":"");
    hd.xLabel = xl; hd.yLabel = fNorm->isChecked()?"Normalized":"Counts";
    hd.binEdges.resize(nb+1); for(int i=0;i<=nb;++i) hd.binEdges[i]=lo+(hi-lo)*i/nb;
    hd.counts.resize(nb); for(int i=0;i<nb;++i) hd.counts[i]=h.bins[i]*norm;
    fSpectrum->setHistogram(hd, fLogY->isChecked());
}

void RootAnalyzer::doFit() {
    if (!d->events || fVar->currentText().isEmpty() || d->n == 0) return;
    const std::vector<float>& val = d->col(fVar->currentText().toStdString());
    double slope=1,intercept=0; bool cal=fUseCalib->isChecked()&&calibration(slope,intercept);
    auto xform=[&](double v){ return cal?slope*v+intercept:v; };
    double lo=fXmin->value(), hi=fXmax->value();
    if(!(hi>lo)){ refreshSpectrum(); lo=fXmin->value(); hi=fXmax->value(); }
    int nb=fBins->value();
    TH1D hist("h_fit","",nb,lo,hi);
    QVector<int> chans=selectedChannels();
    auto chanOk=[&](int ch){ for(int c:chans) if(c==ch) return true; return chans.isEmpty(); };
    for (long long i=0;i<d->n;++i){ if(!chanOk(d->ch[i])) continue; hist.Fill(xform(val[i])); }

    // Which interval(s) to fit: the multi-interval set if any, else the current
    // [FitMin,FitMax], else the whole range.
    QVector<QPair<double,double>> ivs = fFitIntervals;
    if (ivs.isEmpty()) {
        double a=fFitMin->value(), b=fFitMax->value();
        ivs.push_back((b>a) ? qMakePair(a,b) : qMakePair(lo,hi));
    }

    HistData hd; hd.title = "Spectrum: "+fVar->currentText()+"  (Gaussian fit)";
    hd.xLabel = cal?"Energy":fVar->currentText(); hd.yLabel = "Counts";
    hd.binEdges.resize(nb+1); for(int i=0;i<=nb;++i) hd.binEdges[i]=lo+(hi-lo)*i/nb;
    hd.counts.resize(nb); for(int i=0;i<nb;++i) hd.counts[i]=hist.GetBinContent(i+1);

    static const QColor pal[5] = {QColor(220,20,60),QColor(44,160,44),QColor(148,103,189),
                                  QColor(255,140,0),QColor(31,119,180)};
    QStringList report; int idx=0; double firstMean=0;
    for (const auto& iv : ivs) {
        double fmin=iv.first, fmax=iv.second; if(!(fmax>fmin)) continue;
        int b1=hist.FindBin(fmin), b2=hist.FindBin(fmax);
        double N = hist.Integral(b1,b2);                 // events inside the interval
        // seed the Gaussian from the PEAK inside the interval (this is the fix:
        // an unseeded gaus over a falling spectrum converges to a negative mean).
        int mb=b1; double mc=0; for(int b=b1;b<=b2;++b){ double c=hist.GetBinContent(b); if(c>mc){mc=c;mb=b;} }
        double seedMean=hist.GetBinCenter(mb), bw=hist.GetBinWidth(1);
        // σ must fit inside the interval (a real peak); guarantee sigMin < sigMax
        // even when the bin width is large relative to a narrow window.
        double sigMax = std::max((fmax-fmin)/2.0, bw);
        double sigMin = std::min(bw*0.5, sigMax*0.25);
        double seedSig = std::min(std::max((fmax-fmin)/6.0, sigMin), sigMax);
        // Custom Gaussian (NOT the predefined "gaus"): with a user TF1, SetParLimits
        // is always honoured — the predefined gaus + "B" option ignored the limits,
        // letting σ/μ diverge over a non-peak (falling) spectrum.
        TF1 g(Form("gfit%d",idx), "[0]*TMath::Exp(-0.5*((x-[1])/[2])*((x-[1])/[2]))", fmin, fmax);
        g.SetParameters(std::max(1.0,mc), seedMean, seedSig);
        g.SetParLimits(0, 0, mc*10+1);
        g.SetParLimits(1, fmin, fmax);                   // μ must lie inside the interval
        g.SetParLimits(2, sigMin, sigMax);               // σ must fit inside the interval
        hist.Fit(&g,"RQN");                              // R=range, Q=quiet, N=no draw
        double mean=g.GetParameter(1), sigma=std::fabs(g.GetParameter(2));
        double fwhm=2.3548200*sigma, resn=mean!=0?100.0*fwhm/mean:0;
        report << QString("μ=%1  σ=%2  FWHM=%3  R=%4%  N=%5")
            .arg(mean,0,'f',1).arg(sigma,0,'f',1).arg(fwhm,0,'f',1).arg(resn,0,'f',2).arg((long long)N);
        HistFitData ef; ef.hasFitCurve=true; ef.hasFitInterval=true;
        ef.fitXMin=fmin; ef.fitXMax=fmax; ef.color=pal[idx%5];
        ef.label = QString("μ=%1  N=%2").arg(mean,0,'f',0).arg((long long)N);
        int np=200; ef.fitX.resize(np); ef.fitY.resize(np);
        for(int i=0;i<np;++i){ double x=fmin+(fmax-fmin)*i/(np-1); ef.fitX[i]=x; ef.fitY[i]=g.Eval(x); }
        hd.extraFits.push_back(ef);
        if(idx==0){ firstMean=mean; }
        ++idx;
    }
    fFitInfo->setText(report.join("    |    "));
    if(idx>0) fCalibAdc->setValue(firstMean);
    fSpectrum->setHistogram(hd, fLogY->isChecked());
    SIS_LOG_INFO("ROOT", "gaussian fit: "+std::to_string(idx)+" interval(s)");
}

// count events in [x0,x1] for the selected channels (in-memory, instant)
static long long countInRange(const std::vector<int>& ch, const std::vector<float>& val,
                              long long n, const QVector<int>& chans,
                              double slope, double intercept, bool cal, double x0, double x1) {
    long long N=0;
    auto ok=[&](int c){ for(int s:chans) if(s==c) return true; return chans.isEmpty(); };
    for (long long i=0;i<n;++i){ if(!ok(ch[(size_t)i])) continue;
        double v = cal ? slope*val[(size_t)i]+intercept : val[(size_t)i];
        if (v>=x0 && v<=x1) ++N; }
    return N;
}

void RootAnalyzer::onRangeSelected(double x0, double x1) {
    if (!d->events || d->n==0 || !(x1>x0)) return;
    fFitMin->blockSignals(true); fFitMax->blockSignals(true);
    fFitMin->setValue(x0); fFitMax->setValue(x1);
    fFitMin->blockSignals(false); fFitMax->blockSignals(false);
    fFitIntervals.push_back(qMakePair(x0,x1));   // each drag adds a fit region
    doFit();
}

void RootAnalyzer::addInterval() {
    double a=fFitMin->value(), b=fFitMax->value();
    if (b>a) { fFitIntervals.push_back(qMakePair(a,b)); doFit(); }
}

void RootAnalyzer::clearIntervals() {
    fFitIntervals.clear();
    fFitInfo->setText("—");
    if (fNInfo) fNInfo->setText("");
    refreshSpectrum();
}

void RootAnalyzer::resetSpecView() { if (fSpectrum) fSpectrum->resetView(); }

// ---- psd 2d ---------------------------------------------------------------
void RootAnalyzer::refreshPsd2D() {
    if (!d->events || d->n == 0) return;
    if (fPsdEnergyVar->currentText().isEmpty() || fPsdVar->currentText().isEmpty()) return;
    const std::vector<float>& ev = d->col(fPsdEnergyVar->currentText().toStdString());
    const std::vector<float>& pv = d->col(fPsdVar->currentText().toStdString());
    QVector<int> chans=selectedChannels();
    auto chanOk=[&](int ch){ for(int c:chans) if(c==ch) return true; return chans.isEmpty(); };
    double emax=1,pmin=1e30,pmax=-1e30;
    for(long long i=0;i<d->n;++i){ if(!chanOk(d->ch[i])) continue;
        emax=std::max(emax,(double)ev[i]); double p=pv[i]; pmin=std::min(pmin,p); pmax=std::max(pmax,p); }
    if(!(pmax>pmin)){ pmin=0; pmax=1; }
    fPsdMap->setBins(256,256);
    fPsdMap->clear(); fPsdMap->setRange(0,emax*1.05,pmin,pmax);
    fPsdMap->setLabels(QString("PSD (%1) vs Energy (%2)").arg(fPsdVar->currentText(),fPsdEnergyVar->currentText()),
                       fPsdEnergyVar->currentText(), fPsdVar->currentText());
    sis::Hist1D psdHist(120,pmin,pmax);
    for(long long i=0;i<d->n;++i){ if(!chanOk(d->ch[i])) continue;
        fPsdMap->fill(ev[i],pv[i]); psdHist.fill(pv[i]); }
    fPsdMap->update();
    sis::FomResult fom=sis::computeFOM(psdHist);
    if(fom.valid) fFomLabel->setText(QString("FOM = %1   (γ μ=%2  n μ=%3)")
        .arg(fom.fom,0,'f',3).arg(fom.gamma.mean,0,'f',3).arg(fom.neutron.mean,0,'f',3));
    else fFomLabel->setText("FOM: single band / not resolved");
}

// ---- waveform navigator + metrics ----------------------------------------
void RootAnalyzer::loadEventTable() {
    fEventTable->setRowCount(0);
    if(!d->events || d->n == 0) return;
    QStringList vars = numericBranches();
    QString eName = vars.contains("qTotalSw") ? "qTotalSw" : (vars.contains("peakSumFpga")?"peakSumFpga":"");
    QString pName = vars.contains("psdSw")    ? "psdSw"    : (vars.contains("psdFpga")?"psdFpga":"");
    const std::vector<float>* eCol = eName.isEmpty()? nullptr : &d->col(eName.toStdString());
    const std::vector<float>* pCol = pName.isEmpty()? nullptr : &d->col(pName.toStdString());
    long long cap=std::min<long long>(d->n,20000);
    fEventTable->setRowCount((int)cap);
    for(long long i=0;i<cap;++i){
        auto set=[&](int col,const QString& s){ auto* it=new QTableWidgetItem(s);
            it->setData(Qt::UserRole,(qlonglong)i); fEventTable->setItem((int)i,col,it); };
        set(0,QString::number(i));
        set(1,QString::number(d->ch[(size_t)i]+1));
        set(2,eCol?QString::number((*eCol)[(size_t)i],'f',0):"-");
        set(3,pCol?QString::number((*pCol)[(size_t)i],'f',3):"-"); }
    if(cap>0) fEventTable->selectRow(0);
    long long total = d->truncated ? d->events->GetEntries() : d->n;
    fEventInfo->setText(QString("%1 events (showing first %2)").arg(total).arg(cap));
}

void RootAnalyzer::onEventRowChanged() {
    auto sel=fEventTable->selectionModel()->selectedRows();
    if(sel.isEmpty()) return;
    auto* it=fEventTable->item(sel.first().row(),0);
    if(it) showWaveform(it->data(Qt::UserRole).toLongLong());
}

void RootAnalyzer::showWaveform(long long entry) {
    if(!d->events) return;
    std::vector<uint16_t> raw; int ch; unsigned long long ts;
    if(!readRaw(d->events, entry, raw, ch, ts)){ fWave->clearPlot("no waveform"); return; }
    sis::AnalysisConfig acfg;
    sis::PulseMetrics m = sis::analyzeWaveform(raw, acfg);
    double sign = fInvert->isChecked()? -1.0 : 1.0;
    double base = (fBaselineSub->isChecked() && m.valid) ? m.baseline : 0.0;
    CurveData cd; int nn=(int)raw.size(); cd.x.resize(nn); cd.y.resize(nn);
    for(int i=0;i<nn;++i){ cd.x[i]=i*4.0; cd.y[i]=sign*(double(raw[i])-base); }
    fWave->setCurves({cd}, QString("Event %1 · Ch %2 · %3 samples").arg(entry).arg(ch+1).arg(nn),
                     "t [ns]", fBaselineSub->isChecked()?"ADC - baseline":"ADC", false);
    fEventInfo->setText(QString("Event %1  Ch %2  ts=%3").arg(entry).arg(ch+1).arg(ts));
    if (m.valid) {
        fMetrics->setPlainText(QString(
            "Channel     %1\nTimestamp   %2\n\nBaseline    %3\nAmplitude   %4\nPeak index  %5\n"
            "Peak time   %6 ns\nCharge Qtot %7\nQ tail      %8\nPSD (tail)  %9\nFWHM        %10 ns\n"
            "Noise RMS   %11\nSNR         %12\nMin ADC     %13\nMax ADC     %14\nMean ADC    %15\n"
            "Std dev     %16\nPeak-Peak   %17")
            .arg(ch+1).arg(ts).arg(m.baseline,0,'f',1).arg(m.amplitude,0,'f',1).arg(m.peakIndex)
            .arg(m.peakTimeNs,0,'f',0).arg(m.qTotal,0,'f',0).arg(m.qTail,0,'f',0).arg(m.psd,0,'f',4)
            .arg(m.fwhmNs,0,'f',1).arg(m.noiseRMS,0,'f',2).arg(m.snr,0,'f',1).arg(m.minADC,0,'f',0)
            .arg(m.maxADC,0,'f',0).arg(m.meanADC,0,'f',1).arg(m.stddev,0,'f',1).arg(m.p2p,0,'f',0));
    } else fMetrics->setPlainText("waveform too short for metrics");
}

void RootAnalyzer::prevEvent(){ auto s=fEventTable->selectionModel()->selectedRows();
    int r=s.isEmpty()?0:s.first().row(); if(r>0) fEventTable->selectRow(r-1); }
void RootAnalyzer::nextEvent(){ auto s=fEventTable->selectionModel()->selectedRows();
    int r=s.isEmpty()?0:s.first().row(); if(r<fEventTable->rowCount()-1) fEventTable->selectRow(r+1); }

// ---- overlay / average ----------------------------------------------------
void RootAnalyzer::refreshOverlay() {
    if(!d->events) return;
    int wantCh=fOverlayChan->currentIndex(); int wantN=fOverlayN->value();
    TLeaf* chLeaf=d->events->GetLeaf("channel");
    Long64_t n=d->events->GetEntries();
    QVector<CurveData> curves;
    std::vector<double> avg; int avgLen=0, avgCount=0, collected=0;
    for(Long64_t i=0;i<n && collected<wantN;++i){
        d->events->GetEntry(i);
        if(chLeaf && (int)chLeaf->GetValue()!=wantCh) continue;
        std::vector<uint16_t> raw; int ch; unsigned long long ts;
        if(!readRaw(d->events,i,raw,ch,ts)) continue;
        double base=0;
        if(fOverlayBaseline->isChecked()){ int nb=std::min<int>(40,(int)raw.size());
            for(int k=0;k<nb;++k) base+=raw[k]; if(nb) base/=nb; }
        int nn=(int)raw.size();
        CurveData cd; cd.x.resize(nn); cd.y.resize(nn); cd.alpha=0.35; cd.lineWidth=1.0;
        for(int k=0;k<nn;++k){ cd.x[k]=k*4.0; cd.y[k]=double(raw[k])-base; }
        if(!fAverage->isChecked()) curves.push_back(cd);
        if((int)avg.size()<nn) avg.resize(nn,0.0);
        avgLen=std::max(avgLen,nn);
        for(int k=0;k<nn;++k) avg[k]+=cd.y[k];
        ++avgCount; ++collected;
    }
    if(fAverage->isChecked() && avgCount>0){
        CurveData cd; cd.x.resize(avgLen); cd.y.resize(avgLen); cd.lineWidth=2.0; cd.alpha=1.0;
        for(int k=0;k<avgLen;++k){ cd.x[k]=k*4.0; cd.y[k]=avg[k]/avgCount; }
        curves.push_back(cd);
    }
    fOverlay->setCurves(curves, QString("%1 Ch %2 — %3 waveforms")
        .arg(fAverage->isChecked()?"Average of":"Overlay of").arg(wantCh+1).arg(avgCount),
        "t [ns]", "ADC (baseline sub)", false);
}

// ---- coincidence engine (clustering) --------------------------------------
void RootAnalyzer::computeCoincidence() {
    if(!d->events || d->n == 0) return;
    if(fCoincVar->currentText().isEmpty()) return;
    const std::vector<float>& eCol = d->col(fCoincVar->currentText().toStdString());
    QVector<int> set = selectedChannels();
    auto inSet=[&](int ch){ for(int c:set) if(c==ch) return true; return false; };
    const double win = fCoincWin->value();
    const double winTicks = win / sis::SAMPLE_NS;
    const int minMult = fCoincMult->value();

    // read (ts, ch, energy) for the selected channel set, sorted by timestamp — all
    // from the in-memory cache (no TTree rescan → no GUI freeze on large files).
    struct Hit{ double ts; int ch; double e; };
    std::vector<Hit> hits; hits.reserve((size_t)d->n);
    std::array<long,16> chCount{}; double tmin=1e300, tmax=-1e300;
    for(long long i=0;i<d->n;++i){
        int ch=d->ch[(size_t)i];
        if(!inSet(ch)) continue;
        double t=d->ts[(size_t)i];
        hits.push_back({t, ch, (double)eCol[(size_t)i]});
        if(ch>=0&&ch<16) chCount[ch]++; tmin=std::min(tmin,t); tmax=std::max(tmax,t); }
    std::sort(hits.begin(),hits.end(),[](const Hit&a,const Hit&b){return a.ts<b.ts;});
    double durSec = hits.size()>1 ? (tmax-tmin)*sis::SAMPLE_NS*1e-9 : 0.0;

    // energy range
    double emax=1; for(auto&h:hits) emax=std::max(emax,h.e);
    int nb=fBins->value(); if(nb<10) nb=256;
    QVector<QVector<double>> perChan(16, QVector<double>(nb, 0.0));
    sis::Hist1D tof(200,-win,win);
    std::vector<std::vector<double>> matrix(16, std::vector<double>(16,0.0));
    std::vector<long> multCount(17,0);
    long clusters=0, coincClusters=0, coincEvents=0;

    // greedy leading-edge clustering
    size_t i=0;
    while (i<hits.size()){
        size_t j=i+1;
        while (j<hits.size() && (hits[j].ts - hits[i].ts) <= winTicks) ++j;
        // cluster = [i, j) ; count distinct channels present
        std::array<bool,16> present{}; int mult=0;
        for (size_t k=i;k<j;++k) if(!present[hits[k].ch]){ present[hits[k].ch]=true; ++mult; }
        ++clusters;
        if (mult>=1 && mult<=16) multCount[mult]++;
        if (mult>=minMult){
            ++coincClusters;
            // coincidence-gated energy per channel (one entry per hit)
            for (size_t k=i;k<j;++k){ int ch=hits[k].ch; double e=hits[k].e;
                int bin=int(e/emax*nb); if(bin>=0&&bin<nb){ perChan[ch][bin]+=1.0; } ++coincEvents; }
            // TOF + matrix over distinct-channel pairs, referenced to earliest hit.
            // Cap pair enumeration so a pathological huge cluster (very high rate
            // + wide window) cannot make this O(N²) and freeze the GUI.
            size_t clusterSize = j - i;
            if (clusterSize <= 512) {
                for (size_t a=i;a<j;++a) for (size_t b=a+1;b<j;++b){
                    if (hits[a].ch==hits[b].ch) continue;
                    double dtns=(hits[b].ts-hits[a].ts)*sis::SAMPLE_NS;
                    tof.fill(dtns);
                    matrix[hits[a].ch][hits[b].ch]+=1.0;
                    matrix[hits[b].ch][hits[a].ch]+=1.0;
                }
            }
        }
        i=j;
    }

    // energy histogram (per-channel colours)
    fillColoredHist(fCoincEnergy, perChan, 0, emax, "Coincidence energy (per channel)",
                    fCoincVar->currentText(), false);
    // TOF
    HistData th; th.title="Coincidence TOF"; th.xLabel="Δt [ns]"; th.yLabel="Pairs";
    th.binEdges.resize(tof.n()+1); for(int k=0;k<=tof.n();++k) th.binEdges[k]=tof.lo+(tof.hi-tof.lo)*k/tof.n();
    th.counts.resize(tof.n()); for(int k=0;k<tof.n();++k) th.counts[k]=tof.bins[k];
    fCoincTof->setHistogram(th,false);
    // matrix heatmap
    fCoincMatrix->setBins(16,16); fCoincMatrix->clear(); fCoincMatrix->setRange(0,16,0,16);
    for(int a=0;a<16;++a) for(int b=0;b<16;++b) if(matrix[a][b]>0)
        fCoincMatrix->fill(a+0.5,b+0.5,matrix[a][b]);
    fCoincMatrix->update();
    // multiplicity distribution
    HistData mh; mh.title="Multiplicity distribution"; mh.xLabel="Multiplicity"; mh.yLabel="Clusters";
    mh.binEdges.resize(17); for(int k=0;k<=16;++k) mh.binEdges[k]=k+0.5;
    mh.counts.resize(16); for(int k=0;k<16;++k) mh.counts[k]=multCount[k+1];
    fCoincMulti->setHistogram(mh,true);

    // physics context: real vs accidental. Accidental pair rate ≈ rA·rB·2τ.
    double accidentalTotal = 0;
    for (int a=0;a<16;++a) for (int b=a+1;b<16;++b) if(inSet(a)&&inSet(b) && durSec>0){
        double rA=chCount[a]/durSec, rB=chCount[b]/durSec;
        accidentalTotal += rA*rB*2.0*(win*1e-9)*durSec;   // expected random pairs
    }
    double coincRate = durSec>0 ? coincClusters/durSec : 0;
    fCoincInfo->setText(QString("%1 clusters, %2 met multiplicity ≥ %3  →  %4 coincidence hits.  "
        "Coinc. rate %5 Hz (over %6 s).  Expected random pairs ≈ %7  "
        "(window ±%8 ns, channels: %9)")
        .arg(clusters).arg(coincClusters).arg(minMult).arg(coincEvents)
        .arg(coincRate,0,'f',1).arg(durSec,0,'f',1).arg(accidentalTotal,0,'f',1).arg(win)
        .arg([&]{ QStringList s; for(int c:set) s<<QString::number(c+1); return s.join(","); }()));
    SIS_LOG_INFO("ROOT", "coincidence: "+std::to_string(coincClusters)+" clusters ≥ mult "+std::to_string(minMult));
}

// ---- export ---------------------------------------------------------------
void RootAnalyzer::exportCsv() {
    if(!d->events) return;
    QString fn=QFileDialog::getSaveFileName(this,"Export spectrum CSV","spectrum.csv","CSV (*.csv)");
    if(fn.isEmpty()) return;
    QFile f(fn); if(!f.open(QIODevice::WriteOnly|QIODevice::Text)) return;
    QTextStream ts(&f);
    ts<<"bin_center,"<<fVar->currentText()<<"_counts\n";
    double lo=fXmin->value(),hi=fXmax->value(); int nb=fBins->value();
    TLeaf* leaf=d->events->GetLeaf(fVar->currentText().toUtf8().constData());
    TLeaf* chLeaf=d->events->GetLeaf("channel");
    QVector<int> chans=selectedChannels();
    auto chanOk=[&](int ch){ for(int c:chans) if(c==ch) return true; return chans.isEmpty(); };
    double slope=1,intercept=0; bool cal=fUseCalib->isChecked()&&calibration(slope,intercept);
    sis::Hist1D h(nb,lo,hi+(hi-lo)*1e-9);
    Long64_t n=d->events->GetEntries();
    for(Long64_t i=0;i<n;++i){ d->events->GetEntry(i);
        if(chLeaf && !chanOk((int)chLeaf->GetValue())) continue;
        double v=leaf->GetValue(); if(cal) v=slope*v+intercept; h.fill(v); }
    for(int i=0;i<nb;++i) ts<<h.binCenter(i)<<","<<h.bins[i]<<"\n";
    SIS_LOG_INFO("ROOT","exported spectrum CSV "+fn.toStdString());
}

void RootAnalyzer::savePng() {
    QString fn=QFileDialog::getSaveFileName(this,"Save spectrum PNG","spectrum.png","PNG (*.png)");
    if(!fn.isEmpty()) fSpectrum->savePng(fn);
}

// ============================ RootInspector ================================
// A full ROOT session inside the app: a structure browser (left) + a Cling
// interpreter terminal (right). Commands run via gROOT->ProcessLine(); their
// output is captured with gSystem->RedirectOutput(); Draw commands render to an
// inline PNG. No separate ROOT process and no external windows (batch mode).

void RootAnalyzer::buildInspectorTab(QTabWidget* tabs) {
    auto* w = new QWidget(); auto* h = new QHBoxLayout(w);
    auto* split = new QSplitter(Qt::Horizontal);

    // ---- LEFT: structure browser + rendered canvas ----
    auto* leftW = new QWidget(); auto* lv = new QVBoxLayout(leftW); lv->setContentsMargins(0,0,0,0);
    auto* lbl = new QLabel("File structure — double-click a branch to plot it, a tree to Print()");
    lbl->setStyleSheet("color:#9aa4b5;"); lv->addWidget(lbl);
    fInspTree = new QTreeWidget(); fInspTree->setHeaderLabels({"Name","Type / value","Entries"});
    fInspTree->setColumnWidth(0,190);
    connect(fInspTree,&QTreeWidget::itemActivated,this,&RootAnalyzer::onInspectorActivated);
    lv->addWidget(fInspTree, 3);
    auto* imgScroll = new QScrollArea(); imgScroll->setWidgetResizable(true);
    imgScroll->setStyleSheet("background:#0c0f15;");
    fInspImage = new QLabel("Draw output appears here"); fInspImage->setAlignment(Qt::AlignCenter);
    fInspImage->setMinimumHeight(240); fInspImage->setStyleSheet("color:#8a94a6;");
    imgScroll->setWidget(fInspImage);
    lv->addWidget(imgScroll, 4);
    split->addWidget(leftW);

    // ---- RIGHT: ROOT terminal ----
    auto* rightW = new QWidget(); auto* rv = new QVBoxLayout(rightW); rv->setContentsMargins(0,0,0,0);
    auto* lbl2 = new QLabel("ROOT terminal — type any ROOT / C++ command (Enter to run, ↑/↓ history)");
    lbl2->setStyleSheet("color:#9aa4b5;"); rv->addWidget(lbl2);
    fInspOut = new QTextEdit(); fInspOut->setReadOnly(true);
    QFont mono("Menlo"); mono.setStyleHint(QFont::Monospace); mono.setPointSize(11);
    fInspOut->setFont(mono);
    fInspOut->setStyleSheet("background:#0c0f15;color:#d6dae2;");
    rv->addWidget(fInspOut, 1);
    auto* cmdBar = new QHBoxLayout();
    auto* prompt = new QLabel("root []"); prompt->setStyleSheet("color:#9ad17a;font-weight:700;");
    fInspCmd = new QLineEdit(); fInspCmd->setFont(mono);
    fInspCmd->setPlaceholderText("Events->Draw(\"qTotalSw\")   |   Events->GetEntries()   |   Events->Scan(\"channel:qTotalSw\",\"\",\"\",5)");
    fInspCmd->installEventFilter(this);
    auto* runBtn = new QPushButton("Run"); runBtn->setObjectName("run");
    cmdBar->addWidget(prompt); cmdBar->addWidget(fInspCmd,1); cmdBar->addWidget(runBtn);
    rv->addLayout(cmdBar);
    connect(fInspCmd,&QLineEdit::returnPressed,this,&RootAnalyzer::runRootCommand);
    connect(runBtn,&QPushButton::clicked,this,&RootAnalyzer::runRootCommand);
    split->addWidget(rightW);

    split->setStretchFactor(0,1); split->setStretchFactor(1,1); split->setChildrenCollapsible(false);
    h->addWidget(split);
    tabs->addTab(w, "🔎 ROOT Inspector");

    fInspOut->setPlainText(QString(
        "ROOT %1  —  interactive session inside SIS3316 Studio\n"
        "Open a .root file with 📂 above. The trees are pre-bound as globals:\n"
        "    Events, RunSummary, HardwareConfig   (and the file as sisFile)\n"
        "Examples:\n"
        "    Events->Draw(\"qTotalSw\")\n"
        "    Events->Draw(\"psdSw:qTotalSw\",\"channel==0\",\"colz\")\n"
        "    Events->GetEntries()\n"
        "    RunSummary->Scan(\"*\")\n"
        "------------------------------------------------------------------\n")
        .arg(gROOT->GetVersion()));

    // Cling globals: declare once here, assign on each file open (below).
    gROOT->SetBatch(kTRUE);   // never pop up external ROOT windows
    gROOT->ProcessLine("TFile* sisFile=nullptr; TTree* Events=nullptr;"
                       " TTree* RunSummary=nullptr; TTree* HardwareConfig=nullptr;");
}

void RootAnalyzer::populateInspector() {
    if (!fInspTree) return;
    fInspTree->clear();
    if (!d->file) return;
    TIter next(d->file->GetListOfKeys()); TKey* key;
    while ((key=(TKey*)next())) {
        auto* top = new QTreeWidgetItem(fInspTree);
        top->setText(0, key->GetName());
        top->setText(1, key->GetClassName());
        auto* tree = dynamic_cast<TTree*>(key->ReadObj());
        if (tree) {
            long long ne = (long long)tree->GetEntries();
            top->setText(2, QString::number(ne));
            top->setData(0, Qt::UserRole, QString("TREE:%1").arg(key->GetName()));
            if (ne==1) tree->GetEntry(0);          // single-row trees: show values
            TIter l(tree->GetListOfLeaves()); TLeaf* leaf;
            while ((leaf=(TLeaf*)l())) {
                auto* c = new QTreeWidgetItem(top);
                c->setText(0, leaf->GetName());
                if (ne==1) {
                    QString vals; int m=std::min(leaf->GetLen(),12);
                    for(int i=0;i<m;++i) vals += QString::number(leaf->GetValue(i))+" ";
                    c->setText(1, QString("%1  = %2").arg(leaf->GetTypeName(), vals.trimmed()));
                } else c->setText(1, leaf->GetTypeName());
                c->setData(0, Qt::UserRole, QString("BRANCH:%1:%2").arg(key->GetName(), leaf->GetName()));
            }
            top->setExpanded(true);
        }
    }
}

void RootAnalyzer::execRoot(const QString& cmd) {
    if (!fInspOut || cmd.trimmed().isEmpty()) return;
    fInspOut->append(QString("<span style=\"color:#5cc8ff\">root [%1] %2</span>")
                     .arg(fInspHistory.size()).arg(cmd.toHtmlEscaped()));
    if (d->file) d->file->cd();                    // make tree names resolve
    if (!d->inspCanvas) d->inspCanvas = new TCanvas("sisInspCanvas","SIS3316 Inspector",720,500);
    d->inspCanvas->cd(); d->inspCanvas->Clear();

    QString tmp = QDir::tempPath()+"/sis_root_out.txt";
    gSystem->RedirectOutput(tmp.toUtf8().constData(), "w");
    Int_t err = 0;
    Longptr_t ret = gROOT->ProcessLine(cmd.toUtf8().constData(), &err);
    gSystem->RedirectOutput(nullptr);

    QString out;
    QFile f(tmp);
    if (f.open(QIODevice::ReadOnly|QIODevice::Text)) {
        out = QString::fromUtf8(f.readAll());
        if (!out.trimmed().isEmpty())
            fInspOut->append("<span style=\"color:#d6dae2\">"+out.toHtmlEscaped().replace("\n","<br>")+"</span>");
    }
    if (err != 0)
        fInspOut->append(QString("<span style=\"color:#ff6b6b\">(interpreter error %1)</span>").arg((long long)err));

    d->inspCanvas->Update();
    int nprim = (d->inspCanvas->GetListOfPrimitives()) ? d->inspCanvas->GetListOfPrimitives()->GetSize() : 0;
    // Echo the return value for a bare expression (like the real ROOT prompt) —
    // only when nothing was printed/drawn, so statements stay quiet.
    if (out.trimmed().isEmpty() && err==0 && nprim==0 && !cmd.contains(';'))
        fInspOut->append(QString("<span style=\"color:#9aa4b5\">⟶ %1</span>").arg((long long)ret));

    if (nprim > 0) {
        QString png = QDir::tempPath()+"/sis_root_canvas.png";
        d->inspCanvas->SaveAs(png.toUtf8().constData());
        QPixmap pm(png);
        if (!pm.isNull()) {
            int w = std::max(680, fInspImage->width()-8);
            fInspImage->setPixmap(pm.scaled(w, 480, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    }
    fInspOut->moveCursor(QTextCursor::End);
}

bool RootAnalyzer::eventFilter(QObject* obj, QEvent* ev) {
    if (obj==fInspCmd && ev->type()==QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(ev);
        if (ke->key()==Qt::Key_Up) {
            if (fInspHistPos>0){ fInspHistPos--; fInspCmd->setText(fInspHistory.value(fInspHistPos)); }
            return true;
        }
        if (ke->key()==Qt::Key_Down) {
            if (fInspHistPos < fInspHistory.size()-1){ fInspHistPos++; fInspCmd->setText(fInspHistory.value(fInspHistPos)); }
            else { fInspHistPos=fInspHistory.size(); fInspCmd->clear(); }
            return true;
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void RootAnalyzer::runRootCommand() {
    QString cmd = fInspCmd->text();
    if (cmd.trimmed().isEmpty()) return;
    fInspHistory << cmd; fInspHistPos = fInspHistory.size();
    execRoot(cmd);
    fInspCmd->clear();
}

void RootAnalyzer::onInspectorActivated(QTreeWidgetItem* item, int) {
    if (!item) return;
    QString role = item->data(0, Qt::UserRole).toString();
    if (role.startsWith("BRANCH:")) {
        QStringList p = role.split(':');
        if (p.size()>=3 && p[2]!="raw") execRoot(QString("%1->Draw(\"%2\")").arg(p[1], p[2]));
        else if (p.size()>=3) execRoot(QString("%1->Draw(\"raw\",\"\",\"\",1,0)").arg(p[1]));
    } else if (role.startsWith("TREE:")) {
        execRoot(QString("%1->Print()").arg(role.mid(5)));
    }
}
