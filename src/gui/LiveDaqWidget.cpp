#include "LiveDaqWidget.hpp"
#include "Heatmap2D.hpp"
#include "RateBars.hpp"
#include "../util/Logger.hpp"
#include "../util/ConfigStore.hpp"
#include "PlotWidget.h"

#include <QtWidgets>

#ifndef SIS_STUDIO_ROOT
#define SIS_STUDIO_ROOT ""
#endif

using namespace sis;

static HistData histToData(const Hist1D& h, const QString& title,
                           const QString& xl, const QString& yl) {
    HistData d; d.title = title; d.xLabel = xl; d.yLabel = yl;
    int n = h.n();
    d.binEdges.resize(n + 1);
    for (int i = 0; i <= n; ++i) d.binEdges[i] = h.lo + (h.hi - h.lo) * i / n;
    d.counts.resize(n);
    for (int i = 0; i < n; ++i) d.counts[i] = h.bins[i];
    return d;
}

// small helper for a contextual-help (?) button
static QPushButton* helpButton(QWidget* parent, const QString& title, const QString& body) {
    auto* b = new QPushButton("?", parent);
    b->setFixedSize(18, 18);
    b->setStyleSheet("QPushButton{border-radius:9px;background:#2b3340;color:#8ad;font-weight:700;}");
    QObject::connect(b, &QPushButton::clicked, parent, [parent, title, body]{
        QMessageBox::information(parent, title, body);
    });
    return b;
}

LiveDaqWidget::LiveDaqWidget(QWidget* parent) : QWidget(parent) {
    qRegisterMetaType<LiveHit>("LiveHit");
    qRegisterMetaType<QVector<LiveHit>>("QVector<LiveHit>");
    qRegisterMetaType<CoincPair>("CoincPair");
    qRegisterMetaType<QVector<CoincPair>>("QVector<CoincPair>");
    qRegisterMetaType<LiveStats>("LiveStats");
    qRegisterMetaType<QVector<QVector<double>>>("QVector<QVector<double>>");
    buildUi();

    if (qEnvironmentVariableIsSet("SIS_AUTOSTART")) {
        QTimer::singleShot(300, this, [this]{
            if (qEnvironmentVariableIsSet("SIS_COINC_CH")) {  // set the trigger, then start
                fCondMode->setCurrentIndex(1);  // AND
                for (const QString& s : qEnvironmentVariable("SIS_COINC_CH").split(',')) {
                    int d = s.trimmed().toInt(); if (d>=1 && d<=16) fCondMember[d-1]->setChecked(true);
                }
            }
            toggleRun();
        });
        QTimer::singleShot(4500, this, [this]{
            FomResult fom = computeFOM(fPsd1D);
            double tot = 0; for (double r : fRateHz) tot += r;
            qInfo().noquote() << QString("AUTOTEST rate=%1Hz active=%2 fom=%3 tofbins=%4 perch=%5")
                .arg(tot,0,'f',0)
                .arg([&]{ QStringList a; for(int c=0;c<16;++c) if(fActive[c]) a<<QString::number(c+1); return a.join(","); }())
                .arg(fom.valid?QString::number(fom.fom,'f',3):"single-band")
                .arg([&]{ double s=0; for(double b:fTofHist.bins) s+=b; return (int)s; }())
                .arg([&]{ QStringList a; for(int c=0;c<16;++c) if(fActive[c]) a<<QString("ch%1=%2Hz").arg(c+1).arg(fRateHz[c],0,'f',0); return a.join(" "); }());
            qApp->quit();
        });
    }
}

LiveDaqWidget::~LiveDaqWidget() {
    if (fRunning && fWorker) { fWorker->stop(); }
    if (fThread) { fThread->quit(); fThread->wait(2000); }
}

static QLabel* statRow(QGridLayout* g, int row, const QString& cap, const QString& val,
                       const QString& color = "#5cc8ff") {
    auto* c = new QLabel(cap); c->setStyleSheet("color:#8a94a6;");
    auto* v = new QLabel(val); v->setStyleSheet(QString("color:%1;font-size:15px;font-weight:700;").arg(color));
    g->addWidget(c, row, 0); g->addWidget(v, row, 1, Qt::AlignRight);
    return v;
}

void LiveDaqWidget::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8,8,8,8); root->setSpacing(8);

    // ---- control bar ----
    auto* bar = new QHBoxLayout();
    fIp = new QLineEdit("192.168.1.10"); fIp->setFixedWidth(120);
    fPort = new QLineEdit("1234"); fPort->setFixedWidth(54);
    fBtnRun = new QPushButton("▶  START"); fBtnRun->setObjectName("run");
    fBtnRun->setMinimumWidth(110);
    fBtnRec = new QPushButton("●  REC"); fBtnRec->setObjectName("rec");
    fBtnRec->setMinimumWidth(96); fBtnRec->setEnabled(false);
    auto* btnClear = new QPushButton("Clear");
    fCoincWin = new QDoubleSpinBox(); fCoincWin->setRange(0,10000); fCoincWin->setValue(100);
    fCoincWin->setSuffix(" ns"); fCoincWin->setDecimals(0);
    fPsdThresh = new QDoubleSpinBox(); fPsdThresh->setRange(0,1); fPsdThresh->setSingleStep(0.01);
    fPsdThresh->setValue(0.25); fPsdThresh->setDecimals(2);
    fPsdMethod = new QComboBox(); fPsdMethod->addItems({"PSD M1","PSD M2"});
    fPsdMethod->setMinimumWidth(90);
    fTailDelay = new QSpinBox(); fTailDelay->setRange(0,200); fTailDelay->setValue(8);
    fTotalGate = new QSpinBox(); fTotalGate->setRange(4,290); fTotalGate->setValue(80);

    auto addLbl=[&](const char* t){ auto*l=new QLabel(t); l->setStyleSheet("color:#8a94a6;"); bar->addWidget(l); };
    addLbl("IP"); bar->addWidget(fIp);
    addLbl("Port"); bar->addWidget(fPort);
    bar->addWidget(fBtnRun); bar->addWidget(fBtnRec); bar->addWidget(btnClear);
    bar->addSpacing(10);
    addLbl("Coinc"); bar->addWidget(fCoincWin);
    fCoincRecMult = new QSpinBox(); fCoincRecMult->setRange(0,16); fCoincRecMult->setValue(0);
    fCoincRecMult->setSpecialValueText("off");
    addLbl("Coinc REC ≥"); bar->addWidget(fCoincRecMult);
    bar->addWidget(helpButton(this, "Coincidence trigger recording",
        "0 / off = record every triggered event.\n\n"
        ">= 2 = coincidence TRIGGER mode: only events that fall in a timestamp\n"
        "cluster spanning at least this many DIFFERENT channels (within the Coinc\n"
        "window) are written to the ROOT file. Use 2 for a two-detector muon\n"
        "telescope — it records only the coincident (muon) events and drops the\n"
        "singles, shrinking the file to the interesting sample. The live displays\n"
        "still show ALL events; only what is RECORDED is gated."));
    addLbl("PSD thr"); bar->addWidget(fPsdThresh);
    bar->addWidget(fPsdMethod);
    bar->addWidget(helpButton(this, "PSD Method",
        "FPGA PSD method (manual pp.18-19):\n\n"
        "Method 1:  PSD = (Acc3-Acc1)·0x10000 / (Acc2-Acc4)\n"
        "Method 2:  PSD = (Acc3-Acc1)\n\n"
        "Both use the on-board accumulator integrals, so the displayed PSD "
        "matches what the board itself histograms. The dimensionless tail "
        "fraction (tail/peakSum) is what is plotted and recorded."));
    addLbl("Tail Δ"); bar->addWidget(fTailDelay);
    addLbl("Gate"); bar->addWidget(fTotalGate);
    fConfigureChk = new QCheckBox("Configure on Start"); fConfigureChk->setChecked(true);
    fConfigureChk->setStyleSheet("color:#9aa4b5;");
    bar->addWidget(fConfigureChk);
    bar->addWidget(helpButton(this, "Configure on Start",
        "When checked, pressing START writes a full working configuration to the\n"
        "board before arming: internal trigger on all channels, threshold 1200,\n"
        "sample length 300, PSD accumulator gates, etc. (built-in defaults).\n"
        "For a specific detector, save/apply your own preset in the Config tab.\n\n"
        "A freshly powered/reset SIS3316 has all trigger bits and thresholds = 0,\n"
        "so it looks connected but produces NO events. Leave this checked unless\n"
        "another program already configured the board. Fine-tune parameters in\n"
        "the Configuration tab."));
    bar->addStretch();
    fLblState = new QLabel(" IDLE "); fLblState->setStyleSheet("color:#ffcc55;font-weight:700;");
    bar->addWidget(fLblState);
    root->addLayout(bar);

    connect(fBtnRun, &QPushButton::clicked, this, &LiveDaqWidget::toggleRun);
    connect(fBtnRec, &QPushButton::clicked, this, &LiveDaqWidget::toggleRecord);
    connect(btnClear, &QPushButton::clicked, this, &LiveDaqWidget::clearAll);

    // ---- conditional (hardware coincidence) trigger row --------------------
    auto* condBar = new QHBoxLayout();
    auto* condLbl = new QLabel("Conditional trigger:"); condLbl->setStyleSheet("color:#9ad17a;font-weight:700;");
    condBar->addWidget(condLbl);
    fCondMode = new QComboBox(); fCondMode->addItems({"Off","AND","OR","Multiplicity ≥"});
    fCondMode->setMinimumWidth(120); condBar->addWidget(fCondMode);
    fCondMult = new QSpinBox(); fCondMult->setRange(2,16); fCondMult->setValue(2);
    fCondMult->setEnabled(false); condBar->addWidget(fCondMult);
    condBar->addWidget(new QLabel("  channels:"));
    // Group the 16 member checkboxes in their own sub-layout: a TIGHT box→number
    // gap (spacing:2 + compact styled indicator) with GENEROUS space BETWEEN
    // channels (16 px). Because the gap within a channel is much smaller than the
    // gap between channels, every number binds unambiguously to its own box — and
    // this is pure layout, so it renders the same offscreen and on the live macOS
    // compositor (the native checkbox's wide indicator padding no longer applies).
    auto* memLay = new QHBoxLayout(); memLay->setSpacing(16); memLay->setContentsMargins(0,0,0,0);
    for (int c=0;c<16;++c){
        fCondMember[c] = new QCheckBox(QString::number(c+1));
        fCondMember[c]->setStyleSheet(
            "QCheckBox{color:#9aa4b5;spacing:2px;}"
            "QCheckBox::indicator{width:13px;height:13px;}");
        memLay->addWidget(fCondMember[c]);
    }
    condBar->addLayout(memLay);
    condBar->addWidget(helpButton(this, "Conditional (coincidence) trigger — hardware",
        "Makes the board record an event ONLY when a trigger condition on the\n"
        "selected channels is met, within the coincidence window. Implemented in\n"
        "the FPGA (Trigger Coincidence Lookup Table + internal feedback), so the\n"
        "singles are dropped at the source — small files, clean sample.\n\n"
        "MODES:\n"
        "  AND  — every selected channel must fire together. Use for a MUON\n"
        "         TELESCOPE: tick the two stacked detectors → only muons that\n"
        "         cross BOTH are recorded (verified: 100 % time-coincident).\n"
        "  OR   — any selected channel fires.\n"
        "  Multiplicity ≥ N — at least N of the selected channels fire.\n\n"
        "The selected 'member' channels then save only on the coincidence; other\n"
        "channels keep their normal self-trigger. Applied on START (needs\n"
        "'Configure on Start'). Set the channels above to your detectors."));
    fCondStatus = new QLabel("off"); fCondStatus->setStyleSheet("color:#8a94a6;");
    condBar->addWidget(fCondStatus);
    condBar->addStretch();
    root->addLayout(condBar);

    // Keep the status label in sync, and remind the user to restart to apply a
    // change made while running. (The trigger is a board config applied on START.)
    auto condChanged = [this](bool live){
        updateCondStatus();
        if (live && fRunning) onLog("Conditional trigger changed — press STOP then START to apply.");
    };
    connect(fCondMode, &QComboBox::currentIndexChanged, this,
            [this,condChanged](int i){ fCondMult->setEnabled(i==3); condChanged(true); });
    connect(fCondMult, QOverload<int>::of(&QSpinBox::valueChanged), this, [condChanged](int){ condChanged(true); });
    for (auto* cb : fCondMember) connect(cb, &QCheckBox::toggled, this, [condChanged](bool){ condChanged(true); });
    updateCondStatus();

    // ---- body: stats panel + tabs ----
    auto* body = new QHBoxLayout();
    auto* panel = new QWidget(); panel->setFixedWidth(232);
    auto* pg = new QGridLayout(panel); pg->setVerticalSpacing(5);
    fLblRate     = statRow(pg,0,"Rate sw [Hz]","0");
    fLblFpgaRate = statRow(pg,1,"Pile-up","0","#d2855b");
    fLblTotal    = statRow(pg,2,"Events","0");
    fLblTemp     = statRow(pg,3,"Temp [°C]","—","#ffcc55");
    fLblFom      = statRow(pg,4,"FOM","—","#9ad17a");
    fLblCoinc    = statRow(pg,5,"Coinc.","0");
    fLblDrop     = statRow(pg,6,"Pkt drop","0","#d2855b");
    fLblDead     = statRow(pg,7,"Dead-time","0 %","#d2855b");
    fLblRec      = statRow(pg,8,"Recording","off","#8a94a6");
    fLog = new QLabel(); fLog->setWordWrap(true); fLog->setAlignment(Qt::AlignTop|Qt::AlignLeft);
    fLog->setStyleSheet("color:#8a94a6;font-size:11px;");
    auto* logScroll = new QScrollArea(); logScroll->setWidget(fLog); logScroll->setWidgetResizable(true);
    logScroll->setStyleSheet("border:1px solid #2a3140;border-radius:6px;background:#0c0f15;");
    pg->addWidget(logScroll, 9, 0, 1, 2);
    pg->setRowStretch(9, 1);
    body->addWidget(panel);

    auto* tabs = new QTabWidget();

    // ---- scope tab (with per-channel invert + persistence depth) ----
    auto* scopeTab = new QWidget(); auto* sv = new QVBoxLayout(scopeTab);
    auto* scopeCtl = new QHBoxLayout();
    scopeCtl->addWidget(new QLabel("Invert:"));
    auto* invLay = new QHBoxLayout(); invLay->setSpacing(16); invLay->setContentsMargins(0,0,0,0);
    for (int c=0;c<16;++c){
        fInvertChk[c] = new QCheckBox(QString::number(c+1));
        fInvertChk[c]->setStyleSheet(
            "QCheckBox{color:#9aa4b5;spacing:2px;}"
            "QCheckBox::indicator{width:13px;height:13px;}");
        connect(fInvertChk[c], &QCheckBox::toggled, this, [this,c](bool on){ fInvert[c]=on; });
        invLay->addWidget(fInvertChk[c]);
    }
    scopeCtl->addLayout(invLay);
    scopeCtl->addSpacing(12);
    scopeCtl->addWidget(new QLabel("Persistence:"));
    fPersist = new QSlider(Qt::Horizontal); fPersist->setRange(1,60); fPersist->setValue(10);
    fPersist->setFixedWidth(120);
    connect(fPersist, &QSlider::valueChanged, this, [this](int v){ fPersistDepth=v; });
    scopeCtl->addWidget(fPersist);
    scopeCtl->addStretch();
    sv->addLayout(scopeCtl);
    auto* scopeGrid = new QWidget(); auto* sg = new QGridLayout(scopeGrid); sg->setSpacing(3);
    for (int c=0;c<16;++c){
        fScope[c] = new PlotWidget();
        fScope[c]->setMinimumSize(150,90);
        sg->addWidget(fScope[c], c/4, c%4);
    }
    sv->addWidget(scopeGrid, 1);
    tabs->addTab(scopeTab, "16-Channel Persistence Scope");

    // ---- psd tab ----
    // Use a resizable splitter so neither plot can overflow the window; give
    // each a sensible minimum and let the user drag the divider.
    auto* psdTab = new QWidget(); auto* ph = new QHBoxLayout(psdTab);
    ph->setContentsMargins(0,0,0,0);
    auto* psdSplit = new QSplitter(Qt::Horizontal);
    fPsdMap = new Heatmap2D();  fPsdMap->setMinimumWidth(320);
    fPeakSum = new PlotWidget(); fPeakSum->setMinimumWidth(280);
    psdSplit->addWidget(fPsdMap); psdSplit->addWidget(fPeakSum);
    psdSplit->setStretchFactor(0, 3); psdSplit->setStretchFactor(1, 2);
    psdSplit->setChildrenCollapsible(false);
    ph->addWidget(psdSplit);
    tabs->addTab(psdTab, "PSD vs Amplitude  /  Peak-Sum γ·n");

    // ---- rates + counters + tof tab ----
    auto* rtTab = new QWidget(); auto* rv = new QVBoxLayout(rtTab);
    fRates = new RateBars();
    fCounters = new QTableWidget(16, 5);
    fCounters->setHorizontalHeaderLabels({"Ch","Counts","Rate [Hz]","Pile-up","Pileup %"});
    fCounters->verticalHeader()->setVisible(false);
    fCounters->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    fCounters->setEditTriggers(QAbstractItemView::NoEditTriggers);
    fCounters->setMaximumHeight(360);
    for (int c=0;c<16;++c){
        fCounters->setItem(c,0,new QTableWidgetItem(QString::number(c+1)));
        for (int k=1;k<5;++k) fCounters->setItem(c,k,new QTableWidgetItem("0"));
    }
    fTof = new PlotWidget();
    auto* topRow = new QHBoxLayout();
    topRow->addWidget(fRates, 3); topRow->addWidget(fCounters, 2);
    rv->addLayout(topRow, 1); rv->addWidget(fTof, 1);
    tabs->addTab(rtTab, "Trigger Rates  /  Counts  /  TOF");

    body->addWidget(tabs, 1);
    root->addLayout(body, 1);

    fPsdMap->setRange(0, 16384, 0.0, 0.6);
    fPsdMap->setLabels("PSD vs Amplitude (live)", "Amplitude [ADC]", "PSD (tail/total)");
    fPeakSum->clearPlot("Peak-Sum (γ / n)");
    fTof->clearPlot("Coincidence TOF");
}

LiveConfig LiveDaqWidget::currentConfig() const {
    LiveConfig c;
    c.ip = fIp->text().trimmed();
    c.port = quint16(fPort->text().toUInt());
    c.coincWindowNs = fCoincWin->value();
    c.coincRecordMult = fCoincRecMult->value();
    c.psdMethod = fPsdMethod->currentIndex() == 1 ? 2 : 1;
    c.analysis.tailDelay = fTailDelay->value();
    c.analysis.totalGate = fTotalGate->value();
    c.dataRoot = QString::fromUtf8(SIS_STUDIO_ROOT);
    if (c.dataRoot.isEmpty()) c.dataRoot = QCoreApplication::applicationDirPath();
    for (int i=0;i<16;++i) c.invert[i] = fInvert[i];
    c.configureOnStart = fConfigureChk->isChecked();
    // Use the SHARED active configuration edited in the Config tab, so the
    // thresholds/gates the user set there actually take effect here.
    c.board = sis::ConfigStore::instance().get();
    // hardware conditional (coincidence) trigger from the Live DAQ controls
    c.board.coincMode = fCondMode->currentIndex();     // 0=off,1=AND,2=OR,3=mult
    c.board.coincMinMult = fCondMult->value();
    c.board.coincMembers.fill(false);
    for (int disp=0; disp<16; ++disp)
        if (fCondMember[disp]->isChecked()) {
            int readout = c.connectorOf(disp);   // displayed connector → ADC readout (swap-aware)
            if (readout>=0 && readout<16) c.board.coincMembers[readout] = true;
        }
    return c;
}

void LiveDaqWidget::updateCondStatus() {
    if (!fCondStatus) return;
    const int mode = fCondMode->currentIndex();
    QStringList mem; for (int c=0;c<16;++c) if (fCondMember[c]->isChecked()) mem << QString::number(c+1);
    static const char* names[] = {"off","AND","OR","Mult≥"};
    if (mode == 0) {
        fCondStatus->setText("off"); fCondStatus->setStyleSheet("color:#8a94a6;");
    } else if (mem.isEmpty()) {
        fCondStatus->setText(QString("⚠ %1 — tick member channels!").arg(names[mode]));
        fCondStatus->setStyleSheet("color:#ffcc55;font-weight:700;");
    } else {
        QString m = names[mode]; if (mode==3) m += QString::number(fCondMult->value());
        fCondStatus->setText(QString("✓ %1 [ch %2]").arg(m, mem.join(",")));
        fCondStatus->setStyleSheet("color:#9ad17a;font-weight:700;");
    }
}

void LiveDaqWidget::toggleRun() {
    if (fRunning) {
        if (fWorker) fWorker->stop();
        fBtnRun->setEnabled(false);
        // Watchdog: if the worker wedges (e.g. a stuck UDP read) and onStopped
        // never fires, force-recover so the Run button can't brick.
        QTimer::singleShot(5000, this, [this]{
            if (!fRunning) return;              // stopped normally
            onLog("Worker did not stop in time — forcing recovery.");
            if (fThread) { fThread->quit(); if(!fThread->wait(1500)) fThread->terminate();
                           fThread->wait(); fThread->deleteLater(); fThread=nullptr; }
            if (fWorker) { fWorker->deleteLater(); fWorker=nullptr; }
            sis::ConfigStore::instance().setLiveActive(false);
            fRunning=false; fRecording=false;
            fBtnRun->setEnabled(true); fBtnRun->setText("▶  START"); fBtnRun->setObjectName("run");
            fBtnRec->setEnabled(false); fBtnRec->setText("●  REC"); fBtnRec->setObjectName("rec");
            fLblRec->setText("off"); fLblRec->setStyleSheet("color:#8a94a6;");
            fLblState->setText(" IDLE "); fLblState->setStyleSheet("color:#ffcc55;font-weight:700;");
        });
        return;
    }
    if (fCondMode->currentIndex() != 0) {
        bool any=false; for (auto* cb : fCondMember) if (cb->isChecked()) any=true;
        if (!any)
            onLog("⚠ Conditional trigger is set but NO member channels are ticked — it will "
                  "have no effect. Tick your detector channels (e.g. the two stacked ones for AND).");
        else if (!fConfigureChk->isChecked())
            onLog("⚠ Conditional trigger needs 'Configure on Start' checked to be applied.");
    }
    fThreshold = fPsdThresh->value();
    fThread = new QThread(this);
    fWorker = new LiveWorker(currentConfig());
    fWorker->moveToThread(fThread);
    connect(fThread, &QThread::started, fWorker, &LiveWorker::run);
    connect(fWorker, &LiveWorker::started, this, &LiveDaqWidget::onStarted);
    connect(fWorker, &LiveWorker::stopped, this, &LiveDaqWidget::onStopped);
    connect(fWorker, &LiveWorker::failed, this, &LiveDaqWidget::onFailed);
    connect(fWorker, &LiveWorker::logMsg, this, &LiveDaqWidget::onLog);
    connect(fWorker, &LiveWorker::recordingChanged, this, &LiveDaqWidget::onRecordingChanged);
    connect(fWorker, &LiveWorker::runSummaryReady, this, &LiveDaqWidget::onRunSummary);
    connect(fWorker, &LiveWorker::waveforms, this, &LiveDaqWidget::onWaveforms);
    connect(fWorker, &LiveWorker::hits, this, &LiveDaqWidget::onHits);
    connect(fWorker, &LiveWorker::coincidences, this, &LiveDaqWidget::onCoincidences);
    connect(fWorker, &LiveWorker::stats, this, &LiveDaqWidget::onStats);
    fThread->start();
    fRunning = true;
    sis::ConfigStore::instance().setLiveActive(true);   // block Config-tab board access
    fBtnRun->setText("■  STOP"); fBtnRun->setObjectName("stop"); fBtnRun->setStyleSheet("");
    fBtnRec->setEnabled(true);
    fLblState->setText(" RUNNING "); fLblState->setStyleSheet("color:#66ff66;font-weight:700;");
}

void LiveDaqWidget::toggleRecord() {
    if (!fRunning || !fWorker) return;
    if (fRecording) fWorker->stopRecording();
    else            fWorker->startRecording();
}

void LiveDaqWidget::onRecordingChanged(bool on, const QString& path) {
    fRecording = on;
    if (on) {
        fBtnRec->setText("■  STOP REC"); fBtnRec->setObjectName("recOn");
        fLblRec->setText("ON"); fLblRec->setStyleSheet("color:#ff5555;font-weight:700;");
        onLog("Recording → " + path);
    } else {
        fBtnRec->setText("●  REC"); fBtnRec->setObjectName("rec");
        fLblRec->setText("off"); fLblRec->setStyleSheet("color:#8a94a6;");
        if (!path.isEmpty()) onLog("Saved " + path);
    }
    fBtnRec->style()->unpolish(fBtnRec); fBtnRec->style()->polish(fBtnRec);
}

void LiveDaqWidget::onRunSummary(const QString& text) {
    onLog("---- Run summary ----\n" + text);
    QMessageBox::information(this, "Run Summary", text);
}

void LiveDaqWidget::onStarted() {}
void LiveDaqWidget::onStopped() {
    if (fThread) { fThread->quit(); fThread->wait(2000); fThread->deleteLater(); fThread=nullptr; }
    if (fWorker) { fWorker->deleteLater(); fWorker=nullptr; }
    sis::ConfigStore::instance().setLiveActive(false);
    fRunning = false; fRecording = false; fBtnRun->setEnabled(true);
    fBtnRun->setText("▶  START"); fBtnRun->setObjectName("run");
    fBtnRec->setEnabled(false); fBtnRec->setText("●  REC"); fBtnRec->setObjectName("rec");
    fLblRec->setText("off"); fLblRec->setStyleSheet("color:#8a94a6;");
    fLblState->setText(" IDLE "); fLblState->setStyleSheet("color:#ffcc55;font-weight:700;");
}
void LiveDaqWidget::onFailed(const QString& m) { onLog("ERROR: "+m); onStopped(); }
void LiveDaqWidget::onLog(const QString& m) {
    if (qEnvironmentVariableIsSet("SIS_AUTOSTART")) qInfo().noquote() << "[worker]" << m;
    QString t = fLog->text(); t += (t.isEmpty()?"":"\n") + m;
    auto lines = t.split('\n'); if (lines.size()>120) lines = lines.mid(lines.size()-120);
    fLog->setText(lines.join('\n'));
}

void LiveDaqWidget::onWaveforms(const QVector<int>& channels, const QVector<QVector<double>>& wfs) {
    for (int i=0;i<channels.size();++i){
        int c = channels[i];
        auto& ring = fScopeHist[c];
        ring.push_back(wfs[i]);
        while (ring.size() > fPersistDepth) ring.pop_front();
    }
}

void LiveDaqWidget::onHits(const QVector<LiveHit>& batch) {
    for (const auto& h : batch) {
        fPsd1D.fill(h.psd);
        fPsdMap->fill(h.amplitude, h.psd);
        if (h.psd >= fThreshold) fPeakNeutron.fill(h.amplitude);
        else                     fPeakGamma.fill(h.amplitude);
    }
}

void LiveDaqWidget::onCoincidences(const QVector<CoincPair>& pairs) {
    for (const auto& p : pairs) fTofHist.fill(p.tofNs);
}

void LiveDaqWidget::onStats(const LiveStats& s) {
    fLblRate->setText(QString::number(std::accumulate(s.rateHz.begin(), s.rateHz.end(), 0.0), 'f', 0));
    quint64 pileupSum = std::accumulate(s.pileup.begin(), s.pileup.end(), quint64(0));
    fLblFpgaRate->setText(QString::number(pileupSum));
    fLblTotal->setText(QString::number(s.totalEvents));
    fLblTemp->setText(s.temp>0?QString::number(s.temp,'f',1):"—");
    fLblCoinc->setText(QString::number(s.coincTotal));
    fLblDrop->setText(QString::number(s.packetsDropped));
    fLblDead->setText(QString::number(s.deadTimeFrac*100,'f',2)+" %");
    fRateHz = s.rateHz; fActive = s.active;
    fRates->setRates(s.rateHz);            // <-- the missing call that left the bars empty
    refreshCounters(s);
    refreshScope(); refreshPsd(); refreshSpectra(); refreshTof();
}

void LiveDaqWidget::refreshCounters(const LiveStats& s) {
    for (int c=0;c<16;++c){
        double pct = s.chCounts[c] > 0 ? 100.0*double(s.pileup[c])/double(s.chCounts[c]) : 0.0;
        fCounters->item(c,1)->setText(QString::number(s.chCounts[c]));
        fCounters->item(c,2)->setText(QString::number(s.rateHz[c],'f',0));
        fCounters->item(c,3)->setText(QString::number(s.pileup[c]));
        fCounters->item(c,4)->setText(QString::number(pct,'f',1));
    }
}

void LiveDaqWidget::refreshScope() {
    for (int c=0;c<16;++c){
        auto& ring = fScopeHist[c];
        if (ring.isEmpty()){
            if (fActive[c]) fScope[c]->clearPlot(QString("Ch %1").arg(c+1));
            continue;
        }
        QVector<CurveData> curves;
        int N = ring.size();
        for (int k=0;k<N;++k){
            CurveData cd; const auto& w = ring[k];
            cd.x.resize(w.size()); cd.y.resize(w.size());
            double sign = fInvert[c] ? -1.0 : 1.0;
            for (int i=0;i<w.size();++i){ cd.x[i] = i*4.0; cd.y[i] = sign*w[i]; }
            cd.alpha = 0.15 + 0.85*double(k+1)/N;
            cd.lineWidth = (k==N-1)?1.6:1.0;
            curves.push_back(cd);
        }
        fScope[c]->setCurves(curves, QString("Ch %1  ·  %2 Hz").arg(c+1).arg(fRateHz[c],0,'f',0),
                             "t [ns]", "ADC", false);
    }
}

void LiveDaqWidget::refreshPsd() {
    fPsdMap->update();
    FomResult fom = computeFOM(fPsd1D);
    fFomValid = fom.valid; fLastFom = fom.valid ? fom.fom : 0;
    if (fom.valid) {
        // Phase 3.4: warn if discrimination quality degrades below a useful FOM
        QString col = fom.fom < 0.7 ? "#d2855b" : "#9ad17a";
        fLblFom->setText(QString::number(fom.fom,'f',3));
        fLblFom->setStyleSheet(QString("color:%1;font-size:15px;font-weight:700;").arg(col));
    } else {
        fLblFom->setText("—");
    }
}

void LiveDaqWidget::refreshSpectra() {
    HistData d = histToData(fPeakGamma, "Peak-Sum: γ (blue) / n (orange)", "Amplitude [ADC]", "Counts");
    QVector<double> g(fPeakGamma.n()), n(fPeakNeutron.n());
    for (int i=0;i<fPeakGamma.n();++i){ g[i]=fPeakGamma.bins[i]; n[i]=fPeakNeutron.bins[i]; }
    d.channelCounts.clear();
    d.channelCounts.push_back(g); d.channelCounts.push_back(n);
    d.channelLabels = {"Gamma","Neutron"};
    // y-axis must accommodate BOTH overlaid series: scale to the per-bin total so
    // the neutron (orange) histogram is never clipped above the gamma maximum.
    for (int i=0;i<d.counts.size() && i<n.size();++i) d.counts[i] = g[i] + n[i];
    fPeakSum->setHistogram(d, false);
}

void LiveDaqWidget::refreshTof() {
    HistData d = histToData(fTofHist, "Coincidence TOF (windowed)", "TOF [ns]", "Counts");
    fTof->setHistogram(d, false);
}

void LiveDaqWidget::clearAll() {
    fPsd1D = Hist1D{120,0.0,0.6};
    fPeakGamma = Hist1D{200,0.0,16384.0};
    fPeakNeutron = Hist1D{200,0.0,16384.0};
    fTofHist = Hist1D{200,-200.0,200.0};
    fPsdMap->clear();
    for (auto& r : fScopeHist) r.clear();
    refreshPsd(); refreshSpectra(); refreshTof();
}
