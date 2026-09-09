#include "ConfigEditor.hpp"
#include "../hw/Sis3316Daq.hpp"
#include "../hw/Sis3316Config.hpp"
#include "../util/Logger.hpp"
#include "../util/ConfigStore.hpp"
#include <QtWidgets>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#ifndef SIS_STUDIO_ROOT
#define SIS_STUDIO_ROOT ""
#endif

using namespace sis;

// --------------------------------------------------------------------------
//  BoardConfig <-> JSON
// --------------------------------------------------------------------------
static QJsonArray boolArr(const std::array<bool,16>& a){ QJsonArray j; for(bool b:a) j.append(b); return j; }
static QJsonArray intArr16(const std::array<int,16>& a){ QJsonArray j; for(int v:a) j.append(v); return j; }
static QJsonArray intArr4(const std::array<int,4>& a){ QJsonArray j; for(int v:a) j.append(v); return j; }

static QJsonObject toJson(const BoardConfig& c) {
    QJsonObject o;
    o["internalTrigger"] = boolArr(c.internalTrigger);
    o["sumTrigger"]      = boolArr(c.sumTrigger);
    o["invert"]          = boolArr(c.invert);
    o["threshold"]       = intArr16(c.threshold);
    o["cfd"]             = intArr16(c.cfd);
    o["heThreshold"]     = intArr16(c.heThreshold);
    o["peaking"]         = intArr16(c.peaking);
    o["gap"]             = intArr16(c.gap);
    o["dacOffset"]       = intArr16(c.dacOffset);
    o["termination"]     = boolArr(c.termination);
    o["gain"]            = intArr16(c.gain);
    o["sampleLength"] = c.sampleLength; o["sampleStart"] = c.sampleStart;
    o["preTrigger"] = c.preTrigger; o["gateWindow"] = c.gateWindow;
    o["addrThreshold"] = c.addrThreshold;
    o["accStart"] = intArr4(c.accStart); o["accLength"] = intArr4(c.accLength);
    o["saveAcc16"] = c.saveAcc16; o["saveAcc78"] = c.saveAcc78;
    o["saveEnergyMaw"] = c.saveEnergyMaw;
    o["enPeaking"] = c.enPeaking; o["enGap"] = c.enGap;
    return o;
}
static BoardConfig fromJson(const QJsonObject& o) {
    BoardConfig c;  // defaults, then override
    auto gb=[&](const char*k, std::array<bool,16>& a){ if(o.contains(k)){auto j=o[k].toArray(); for(int i=0;i<16&&i<j.size();++i)a[i]=j[i].toBool();} };
    auto gi=[&](const char*k, std::array<int,16>& a){ if(o.contains(k)){auto j=o[k].toArray(); for(int i=0;i<16&&i<j.size();++i)a[i]=j[i].toInt();} };
    auto g4=[&](const char*k, std::array<int,4>& a){ if(o.contains(k)){auto j=o[k].toArray(); for(int i=0;i<4&&i<j.size();++i)a[i]=j[i].toInt();} };
    gb("internalTrigger",c.internalTrigger); gb("sumTrigger",c.sumTrigger); gb("invert",c.invert);
    gi("threshold",c.threshold); gi("cfd",c.cfd); gi("heThreshold",c.heThreshold);
    gi("peaking",c.peaking); gi("gap",c.gap); gi("dacOffset",c.dacOffset);
    gb("termination",c.termination); gi("gain",c.gain);
    if(o.contains("sampleLength")) c.sampleLength=o["sampleLength"].toInt();
    if(o.contains("sampleStart")) c.sampleStart=o["sampleStart"].toInt();
    if(o.contains("preTrigger")) c.preTrigger=o["preTrigger"].toInt();
    if(o.contains("gateWindow")) c.gateWindow=o["gateWindow"].toInt();
    if(o.contains("addrThreshold")) c.addrThreshold=o["addrThreshold"].toInt();
    g4("accStart",c.accStart); g4("accLength",c.accLength);
    if(o.contains("saveAcc16")) c.saveAcc16=o["saveAcc16"].toBool();
    if(o.contains("saveAcc78")) c.saveAcc78=o["saveAcc78"].toBool();
    if(o.contains("saveEnergyMaw")) c.saveEnergyMaw=o["saveEnergyMaw"].toBool();
    if(o.contains("enPeaking")) c.enPeaking=o["enPeaking"].toInt();
    if(o.contains("enGap")) c.enGap=o["enGap"].toInt();
    return c;
}

// Front-panel connector (what the table row / Live view / analyzer call
// "Channel N") ↔ ADC readout index used by BoardConfig/applyConfig. On this
// board they are swapped within each pair (connector = readout XOR 1), same
// mapping LiveWorker::connectorOf uses. Without this, editing "Channel 1" here
// configured the WRONG ADC channel and the live channel kept its old threshold.
// (If a unit is not wired this way, set swapChannelPairs=false in LiveWorker.hpp
//  and change this to `return row;`.)
static inline int adcIndexForRow(int row) { return row ^ 1; }

static QPushButton* help(QWidget* parent, const QString& title, const QString& body) {
    auto* b = new QPushButton("?", parent);
    b->setFixedSize(18,18);
    b->setStyleSheet("QPushButton{border-radius:9px;background:#2b3340;color:#8ad;font-weight:700;}");
    QObject::connect(b, &QPushButton::clicked, parent, [parent,title,body]{
        QMessageBox::information(parent, title, body);
    });
    return b;
}

// --------------------------------------------------------------------------
ConfigEditor::ConfigEditor(QWidget* parent) : QWidget(parent) {
    buildUi();
    populateFromConfig(ConfigStore::instance().get());   // shared active config
    refreshConfigList();
    // Keep the shared ConfigStore in sync with every edit so the Live DAQ tab's
    // "Configure on Start" always applies exactly what is shown here.
    for (QSpinBox* w : findChildren<QSpinBox*>())
        connect(w, &QSpinBox::valueChanged, this, &ConfigEditor::syncStore);
    for (QCheckBox* w : findChildren<QCheckBox*>())
        connect(w, &QCheckBox::toggled, this, &ConfigEditor::syncStore);
    for (QComboBox* w : findChildren<QComboBox*>())
        if (w != fConfigList)
            connect(w, &QComboBox::currentIndexChanged, this, &ConfigEditor::syncStore);
    syncStore();
}

void ConfigEditor::syncStore() {
    ConfigStore::instance().set(buildConfig());
}

QString ConfigEditor::host() const { return fIp->text().trimmed(); }
quint16 ConfigEditor::port() const { return quint16(fPort->text().toUInt()); }

QString ConfigEditor::configDir() const {
    QString root = QString::fromUtf8(SIS_STUDIO_ROOT);
    if (root.isEmpty()) root = QCoreApplication::applicationDirPath();
    QDir().mkpath(root + "/configs");
    return root + "/configs";
}

void ConfigEditor::setStatus(const QString& msg, bool error) {
    fStatus->setText(msg);
    fStatus->setStyleSheet(error ? "color:#ff6b6b;font-weight:600;" : "color:#9ad17a;font-weight:600;");
}

void ConfigEditor::refreshConfigList(const QString& select) {
    fConfigList->blockSignals(true);
    fConfigList->clear();
    QDir d(configDir());
    for (const QFileInfo& fi : d.entryInfoList({"*.json"}, QDir::Files, QDir::Name))
        fConfigList->addItem(fi.completeBaseName());
    if (!select.isEmpty()) { int i=fConfigList->findText(select); if(i>=0) fConfigList->setCurrentIndex(i); }
    fConfigList->blockSignals(false);
}

// --------------------------------------------------------------------------
void ConfigEditor::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setSpacing(8);

    // ---- connection + preset manager bar -------------------------------
    auto* bar = new QHBoxLayout();
    fIp = new QLineEdit("192.168.1.10"); fIp->setFixedWidth(120);
    fPort = new QLineEdit("1234"); fPort->setFixedWidth(54);
    bar->addWidget(new QLabel("IP")); bar->addWidget(fIp);
    bar->addWidget(new QLabel("Port")); bar->addWidget(fPort);
    bar->addSpacing(16);
    bar->addWidget(new QLabel("Preset:"));
    fConfigList = new QComboBox(); fConfigList->setMinimumWidth(150);
    bar->addWidget(fConfigList);
    auto* btnLoad = new QPushButton("Load");
    auto* btnSave = new QPushButton("Save As…");
    auto* btnDel = new QPushButton("Delete");
    bar->addWidget(btnLoad); bar->addWidget(btnSave); bar->addWidget(btnDel);
    bar->addStretch();
    root->addLayout(bar);
    connect(btnLoad, &QPushButton::clicked, this, &ConfigEditor::loadSelected);
    connect(btnSave, &QPushButton::clicked, this, &ConfigEditor::saveConfigAs);
    connect(btnDel, &QPushButton::clicked, this, &ConfigEditor::deleteSelected);

    // ---- board action bar ----------------------------------------------
    auto* abar = new QHBoxLayout();
    auto* btnApply = new QPushButton("✓  Apply to Board"); btnApply->setObjectName("run");
    auto* btnRead = new QPushButton("⟳  Read from Board");
    auto* btnClear = new QPushButton("Clear Histograms");
    abar->addWidget(btnApply); abar->addWidget(btnRead); abar->addWidget(btnClear);
    abar->addWidget(help(this, "Apply to Board",
        "Writes the COMPLETE configuration shown below to the board (ADC init,\n"
        "triggers, thresholds, raw window, PSD gates). This is what makes a\n"
        "freshly powered/reset board start producing events. Stop Live DAQ first."));
    abar->addStretch();
    fStatus = new QLabel("Stop Live DAQ before applying (single link grant).");
    fStatus->setStyleSheet("color:#8a94a6;");
    abar->addWidget(fStatus);
    root->addLayout(abar);
    connect(btnApply, &QPushButton::clicked, this, &ConfigEditor::applyFullConfig);
    connect(btnRead, &QPushButton::clicked, this, &ConfigEditor::readFromBoard);
    connect(btnClear, &QPushButton::clicked, this, &ConfigEditor::clearHistograms);

    auto* scroll = new QScrollArea(); scroll->setWidgetResizable(true);
    auto* content = new QWidget(); auto* cv = new QVBoxLayout(content);
    cv->setSpacing(10);

    // ---- global acquisition settings -----------------------------------
    auto* globBox = new QGroupBox("Global acquisition settings");
    auto* gg = new QHBoxLayout(globBox);
    gg->addWidget(new QLabel("Sample length")); fSampleLen = new QSpinBox();
    fSampleLen->setRange(2,65534); fSampleLen->setValue(300); fSampleLen->setSuffix(" smp"); gg->addWidget(fSampleLen);
    gg->addWidget(new QLabel("Pre-trigger")); fPreTrig = new QSpinBox();
    fPreTrig->setRange(0,2042); fPreTrig->setValue(100); fPreTrig->setSuffix(" smp"); gg->addWidget(fPreTrig);
    gg->addWidget(new QLabel("Gate window")); fGateWin = new QSpinBox();
    fGateWin->setRange(2,65534); fGateWin->setValue(100); fGateWin->setSuffix(" smp"); gg->addWidget(fGateWin);
    gg->addWidget(new QLabel("Baseline DAC")); fDacOffset = new QSpinBox();
    fDacOffset->setRange(0,65535); fDacOffset->setSingleStep(500); fDacOffset->setValue(10000);
    gg->addWidget(fDacOffset);
    gg->addWidget(new QLabel("Input range")); fInputRange = new QComboBox();
    fInputRange->addItems({"5 V (full)","2 V (fine)","1.9 V"});
    gg->addWidget(fInputRange);
    gg->addWidget(help(this, "Global settings",
        "Sample length: raw waveform samples per event (300 = 1.2 µs at 250 MHz).\n"
        "Pre-trigger: samples kept before the trigger. Gate window: active trigger\n"
        "gate length. 1 sample = 4 ns.\n\n"
        "Baseline DAC — WHERE the flat baseline sits in the ADC range\n"
        "(baseline_ADC ≈ DAC/4, measured). For POSITIVE-going pulses use a LOW\n"
        "value (10000 → baseline ≈2100 ADC) so large/high-energy pulses have the\n"
        "full upward range and do not clip at 16383. For NEGATIVE pulses enable\n"
        "Invert and use a HIGH value.\n\n"
        "Input range (dynamic range) — the full-scale input voltage the 14-bit\n"
        "ADC (16384 codes) maps to, for ALL channels:\n"
        "   • 5 V  → 1 ADC code ≈ 305 µV. Big range, coarser steps. Use for LARGE\n"
        "            signals / high energy / muons (less chance of clipping).\n"
        "   • 2 V  → 1 ADC code ≈ 122 µV. Smaller range, ~2.5× finer resolution.\n"
        "            Use for SMALL signals where you want the best resolution.\n"
        "   • 1.9 V→ slightly finer still.\n"
        "Rule of thumb: pick the smallest range your biggest pulse still fits in."));
    gg->addStretch();
    cv->addWidget(globBox);

    // ---- per-channel trigger table -------------------------------------
    auto* chBox = new QGroupBox("Per-channel trigger configuration");
    auto* chl = new QVBoxLayout(chBox);
    auto* chHint = new QHBoxLayout();
    auto* hint = new QLabel("Channel numbers match the front-panel connectors and the Live view. "
                            "Threshold is ABOVE baseline. CFD improves timing for PSD/TOF. "
                            "FIR peaking/gap shape the trigger filter.");
    hint->setStyleSheet("color:#8a94a6;"); chHint->addWidget(hint);
    chHint->addWidget(help(this, "Trigger threshold — explained simply",
        "WHAT IT DOES: each channel only records an event when the pulse is taller\n"
        "than the Threshold. Small threshold = record small pulses too (and noise);\n"
        "big threshold = only record big pulses. Set it just ABOVE the noise.\n\n"
        "WHAT THE NUMBER MEANS (important): the board does not compare the raw pulse\n"
        "height directly. It compares a FIR trapezoid FILTER SUM, which adds up the\n"
        "pulse over 'FIR peaking' samples. So roughly:\n"
        "     Threshold ≈ (pulse height in ADC counts) × (FIR peaking)\n"
        "EXAMPLE: peaking = 10 and you want to trigger on pulses ≥ ~120 ADC counts\n"
        "tall → set Threshold ≈ 120 × 10 = 1200 (this is the default).\n"
        "         Want to catch smaller ~50-count pulses? → 50 × 10 = 500.\n"
        "         Only big ~500-count pulses? → 500 × 10 = 5000.\n"
        "So if you DOUBLE the threshold you roughly double the minimum pulse height.\n"
        "TIP: if you change 'FIR peaking', the effective height threshold changes by\n"
        "the same factor — re-scale the Threshold to keep the same sensitivity.\n"
        "(1 ADC count ≈ 305 µV on the 5 V range, ≈ 122 µV on the 2 V range.)\n\n"
        "OTHER COLUMNS:\n"
        "  Invert   — tick for NEGATIVE-going pulses (most PMT anodes).\n"
        "  Int Trig — this channel makes its own (self) trigger. Usually ON.\n"
        "  Ext Trig — trigger this channel from the external/coincidence trigger.\n"
        "  CFD      — constant-fraction 50 % timing; better for PSD / TOF.\n"
        "  HE supp  — veto events ABOVE the high-energy threshold (needs CFD).\n"
        "  FIR Peak / Gap — trapezoid rise length / flat-top. Fast scintillators:\n"
        "                   keep Peak short (≈10)."));
    chHint->addStretch(); chl->addLayout(chHint);
    fChanTable = new QTableWidget(16, 8);
    fChanTable->setHorizontalHeaderLabels(
        {"Ch","Invert","Int Trig","Ext Trig","Threshold","CFD","HE supp","FIR Peak / Gap"});
    fChanTable->verticalHeader()->setVisible(false);
    fChanTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    for (int c=0;c<16;++c){
        auto* chItem = new QTableWidgetItem(QString::number(c+1));
        chItem->setFlags(Qt::ItemIsEnabled);
        fChanTable->setItem(c,0,chItem);
        auto mkChk=[&](int col){ auto* w=new QWidget(); auto* l=new QHBoxLayout(w);
            l->setContentsMargins(0,0,0,0); l->setAlignment(Qt::AlignCenter);
            auto* cb=new QCheckBox(); l->addWidget(cb); fChanTable->setCellWidget(c,col,w); return cb; };
        mkChk(1); mkChk(2); mkChk(3);
        auto* thr=new QSpinBox(); thr->setRange(0,0x0FFFFFFF); thr->setSingleStep(10);
        fChanTable->setCellWidget(c,4,thr);
        auto* cfd=new QComboBox(); cfd->addItems({"off","CFD"});
        fChanTable->setCellWidget(c,5,cfd);
        mkChk(6);
        auto* pg=new QWidget(); auto* pgl=new QHBoxLayout(pg); pgl->setContentsMargins(2,0,2,0);
        auto* pk=new QSpinBox(); pk->setRange(2,510); pk->setValue(10);
        auto* gp=new QSpinBox(); gp->setRange(2,510); gp->setValue(4);
        pgl->addWidget(pk); pgl->addWidget(gp);
        fChanTable->setCellWidget(c,7,pg);
    }
    fChanTable->setMinimumHeight(430);
    fChanTable->verticalHeader()->setDefaultSectionSize(26);
    chl->addWidget(fChanTable);
    cv->addWidget(chBox);

    // ---- accumulator PSD gates (roomy, clearly explained) --------------
    auto* gateBox = new QGroupBox("PSD accumulator gates");
    auto* gv = new QVBoxLayout(gateBox);
    auto* gHint = new QHBoxLayout();
    auto* gl = new QLabel("Gate 1 = prompt/short integral · Gate 2 = longer total integral · "
                          "PSD = (Gate2 − Gate1) / Gate2.  Start & length in samples (4 ns).");
    gl->setStyleSheet("color:#8a94a6;"); gl->setWordWrap(true); gHint->addWidget(gl,1);
    gHint->addWidget(help(this, "Accumulator gates (PSD)",
        "Four integration gates per group define the on-board charge integrals.\n\n"
        "For neutron/gamma PSD with organic scintillators:\n"
        "  Gate 1 = prompt/short window over the pulse peak (e.g. len 9, start 0)\n"
        "  Gate 2 = long window capturing the slow tail (e.g. len 29, start 15)\n"
        "  PSD = (Gate2 − Gate1)/Gate2 → larger for neutrons (more slow light).\n\n"
        "Gate placement relative to the pulse peak is the single most important\n"
        "PSD tuning parameter. Widen Gate 2 to capture more tail if bands overlap."));
    gv->addLayout(gHint);
    fGateTable = new QTableWidget(4, 9);
    fGateTable->setHorizontalHeaderLabels(
        {"Group","G1 start","G1 len","G2 start","G2 len","G3 start","G3 len","G4 start","G4 len"});
    fGateTable->verticalHeader()->setVisible(false);
    fGateTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    fGateTable->verticalHeader()->setDefaultSectionSize(30);
    for (int g=0;g<4;++g){
        auto* it=new QTableWidgetItem(QString("%1  (ch %2-%3)").arg(g+1).arg(g*4+1).arg(g*4+4));
        it->setFlags(Qt::ItemIsEnabled);
        fGateTable->setItem(g,0,it);
        for (int k=0;k<4;++k){
            auto* st=new QSpinBox(); st->setRange(0,65535); fGateTable->setCellWidget(g,1+2*k,st);
            auto* ln=new QSpinBox(); ln->setRange(0,512);   fGateTable->setCellWidget(g,2+2*k,ln);
        }
    }
    fGateTable->setMinimumHeight(170);
    gv->addWidget(fGateTable);
    cv->addWidget(gateBox);

    cv->addStretch();
    scroll->setWidget(content);
    root->addWidget(scroll, 1);
}

// --------------------------------------------------------------------------
sis::BoardConfig ConfigEditor::buildConfig() const {
    // Start from the shared active config so fields WITHOUT a UI widget
    // (sumTrigger, dacOffset, gain, termination, sampleStart, addrThreshold,
    // saveAcc*, energy filter, …) are preserved instead of reset to defaults.
    BoardConfig cfg = ConfigStore::instance().get();
    for (int row=0; row<16; ++row) {
        const int ch = adcIndexForRow(row);   // table row = connector → ADC index
        auto chk=[&](int col){ return fChanTable->cellWidget(row,col)->findChild<QCheckBox*>()->isChecked(); };
        cfg.invert[ch]          = chk(1);
        cfg.internalTrigger[ch] = chk(2);
        cfg.threshold[ch]  = qobject_cast<QSpinBox*>(fChanTable->cellWidget(row,4))->value();
        cfg.cfd[ch]        = qobject_cast<QComboBox*>(fChanTable->cellWidget(row,5))->currentIndex() ? 3 : 0;
        cfg.heThreshold[ch] = chk(6) ? 1500 : 0;
        auto sboxes = fChanTable->cellWidget(row,7)->findChildren<QSpinBox*>();
        if (sboxes.size()==2){ cfg.peaking[ch]=sboxes[0]->value(); cfg.gap[ch]=sboxes[1]->value(); }
    }
    for (int k=0;k<4;++k){
        cfg.accStart[k]  = qobject_cast<QSpinBox*>(fGateTable->cellWidget(0,1+2*k))->value();
        cfg.accLength[k] = qobject_cast<QSpinBox*>(fGateTable->cellWidget(0,2+2*k))->value();
    }
    cfg.sampleLength = fSampleLen->value();
    cfg.preTrigger   = fPreTrig->value();
    cfg.gateWindow   = fGateWin->value();
    for (int ch=0; ch<16; ++ch) cfg.dacOffset[ch] = fDacOffset->value();
    for (int ch=0; ch<16; ++ch) cfg.gain[ch] = fInputRange->currentIndex();  // 0=5V,1=2V,2=1.9V
    return cfg;
}

void ConfigEditor::populateFromConfig(const sis::BoardConfig& cfg) {
    for (int row=0; row<16; ++row) {
        const int ch = adcIndexForRow(row);   // table row = connector → ADC index
        fChanTable->cellWidget(row,1)->findChild<QCheckBox*>()->setChecked(cfg.invert[ch]);
        fChanTable->cellWidget(row,2)->findChild<QCheckBox*>()->setChecked(cfg.internalTrigger[ch]);
        fChanTable->cellWidget(row,3)->findChild<QCheckBox*>()->setChecked(false);
        qobject_cast<QSpinBox*>(fChanTable->cellWidget(row,4))->setValue(cfg.threshold[ch]);
        qobject_cast<QComboBox*>(fChanTable->cellWidget(row,5))->setCurrentIndex(cfg.cfd[ch]?1:0);
        fChanTable->cellWidget(row,6)->findChild<QCheckBox*>()->setChecked(cfg.heThreshold[ch]>0);
        auto sboxes = fChanTable->cellWidget(row,7)->findChildren<QSpinBox*>();
        if (sboxes.size()==2){ sboxes[0]->setValue(cfg.peaking[ch]); sboxes[1]->setValue(cfg.gap[ch]); }
    }
    for (int g=0; g<4; ++g) for (int k=0;k<4;++k){
        qobject_cast<QSpinBox*>(fGateTable->cellWidget(g,1+2*k))->setValue(cfg.accStart[k]);
        qobject_cast<QSpinBox*>(fGateTable->cellWidget(g,2+2*k))->setValue(cfg.accLength[k]);
    }
    fSampleLen->setValue(cfg.sampleLength); fPreTrig->setValue(cfg.preTrigger); fGateWin->setValue(cfg.gateWindow);
    fDacOffset->setValue(cfg.dacOffset[0]);
    fInputRange->setCurrentIndex(std::min(2, std::max(0, cfg.gain[0])));
}

// --------------------------------------------------------------------------
void ConfigEditor::saveConfigAs() {
    bool ok=false;
    QString name = QInputDialog::getText(this, "Save configuration",
        "Preset name (e.g. plastic, EJ309, run42):", QLineEdit::Normal,
        fConfigList->currentText(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    name = name.trimmed().replace(QRegularExpression("[^A-Za-z0-9_\\- ]"), "_");
    QString path = configDir() + "/" + name + ".json";
    QJsonDocument doc(toJson(buildConfig()));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { setStatus("Could not write "+path, true); return; }
    f.write(doc.toJson(QJsonDocument::Indented)); f.close();
    refreshConfigList(name);
    setStatus("Saved preset '" + name + "'.");
    SIS_LOG_INFO("CONFIG", "saved preset " + name.toStdString());
}

void ConfigEditor::loadSelected() {
    QString name = fConfigList->currentText();
    if (name.isEmpty()) { setStatus("No preset selected.", true); return; }
    QFile f(configDir() + "/" + name + ".json");
    if (!f.open(QIODevice::ReadOnly)) { setStatus("Could not read preset.", true); return; }
    auto doc = QJsonDocument::fromJson(f.readAll()); f.close();
    populateFromConfig(fromJson(doc.object()));
    syncStore();
    setStatus("Loaded preset '" + name + "' (not yet on board — press Apply).");
}

void ConfigEditor::deleteSelected() {
    QString name = fConfigList->currentText();
    if (name.isEmpty()) return;
    if (QMessageBox::question(this, "Delete preset", "Delete preset '"+name+"'?") != QMessageBox::Yes) return;
    QFile::remove(configDir() + "/" + name + ".json");
    refreshConfigList();
    setStatus("Deleted preset '" + name + "'.");
}

// --------------------------------------------------------------------------
void ConfigEditor::applyFullConfig() {
    if (ConfigStore::instance().liveActive()) {
        setStatus("Stop Live DAQ first — it holds the board's single link grant.", true);
        QMessageBox::warning(this, "Live DAQ is running",
            "Live DAQ currently holds the board's single link grant.\n"
            "Stop it (Live DAQ tab → STOP) before applying a configuration, or the\n"
            "grant conflict will flood the log with 'lost grant' errors.");
        return;
    }
    if (QMessageBox::question(this, "Apply configuration",
        "Write the complete configuration to the board? Live acquisition must be stopped.")
        != QMessageBox::Yes) return;
    try {
        BoardConfig cfg = buildConfig();
        Sis3316Daq daq(host().toStdString(), port());
        daq.open();
        applyConfig(daq, cfg);
        daq.disarm();
        daq.close();
        setStatus("Configuration applied — board is ready to acquire.");
        SIS_LOG_INFO("CONFIG", "full configuration applied from editor");
    } catch (const std::exception& e) {
        setStatus(QString("Apply failed: %1").arg(e.what()), true);
        SIS_LOG_ERROR("CONFIG", std::string("apply failed: ") + e.what());
    }
}

void ConfigEditor::readFromBoard() {
    if (ConfigStore::instance().liveActive()) {
        setStatus("Stop Live DAQ first — it holds the board's single link grant.", true);
        return;
    }
    try {
        Sis3316Daq daq(host().toStdString(), port());
        daq.open();
        ConfigSnapshot s = daq.readConfigSnapshot();
        BoardConfig cfg;
        for (int ch=0; ch<16; ++ch) {
            int g=ch/4, cid=ch%4;
            uint32_t f = (s.eventConfig[g] >> reg::eventCfgShift(cid));
            cfg.invert[ch]          = f & reg::EVCFG_INPUT_INVERT;
            cfg.internalTrigger[ch] = f & reg::EVCFG_INTERNAL_TRIG;
            uint32_t thr = s.trigThreshold[ch];
            int tv = int(thr & 0x0FFFFFFF) - int(reg::THRESHOLD_OFFSET);
            cfg.threshold[ch] = tv > 0 ? tv : 0;
            cfg.cfd[ch] = ((thr >> reg::THRESHOLD_CFD_SHIFT) & 0x3) ? 3 : 0;
            cfg.heThreshold[ch] = (thr & reg::THRESHOLD_HE_SUPPRESS) ? 1500 : 0;
            uint32_t fir = s.firTrigSetup[ch];
            cfg.peaking[ch] = int((fir >> reg::FIRSETUP_PEAKING_SHIFT) & reg::FIRSETUP_PEAKING_MASK);
            cfg.gap[ch]     = int((fir >> reg::FIRSETUP_GAP_SHIFT) & reg::FIRSETUP_GAP_MASK);
        }
        for (int k=0;k<4;++k){
            uint32_t ac = s.accGateConfig[0][k];
            cfg.accStart[k]  = int(ac & 0xFFFF);
            cfg.accLength[k] = int((ac >> 16) & 0x1FF);
        }
        cfg.sampleLength = int((s.rawDataConfig[0] >> 16) & 0xFFFE);
        cfg.preTrigger   = int(s.preTrigDelay[0] & 0x3FE);
        cfg.gateWindow   = int(s.activeTrigGateLen[0] & 0xFFFF) + 2;
        populateFromConfig(cfg);
        daq.close();
        setStatus("Read configuration from board.");
        SIS_LOG_INFO("CONFIG", "read configuration snapshot from board");
    } catch (const std::exception& e) {
        setStatus(QString("Read failed: %1").arg(e.what()), true);
    }
}

void ConfigEditor::clearHistograms() {
    if (ConfigStore::instance().liveActive()) {
        setStatus("Stop Live DAQ first — it holds the board's single link grant.", true);
        return;
    }
    try {
        Sis3316Daq daq(host().toStdString(), port());
        daq.open(); daq.histogramsClear(); daq.close();
        setStatus("On-board histograms cleared (~35 ms).");
    } catch (const std::exception& e) {
        setStatus(QString("Clear failed: %1").arg(e.what()), true);
    }
}
