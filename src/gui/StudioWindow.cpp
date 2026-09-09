#include "StudioWindow.hpp"
#include "LiveDaqWidget.hpp"
#include "ConfigEditor.hpp"
#include "RootAnalyzer.hpp"
#include "../util/Logger.hpp"

#include <QTabWidget>
#include <QPlainTextEdit>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QComboBox>
#include <QLabel>
#include <QFileDialog>
#include <QDateTime>

static const char* kStyle = R"(
* { font-family:'Helvetica Neue','Arial'; font-size:12px; color:#e6e9ef; }
QMainWindow, QWidget { background:#11141b; }
QTabWidget::pane { border:1px solid #2a3140; border-radius:9px; }
QTabBar::tab { background:#1b212c; padding:8px 18px; margin-right:3px;
               border-top-left-radius:8px; border-top-right-radius:8px; color:#9aa4b5; }
QTabBar::tab:selected { background:#232a36; color:#ffffff; }
QTabBar::tab:hover:!selected { background:#20262f; color:#c5cbd6; }
QGroupBox { border:1px solid #2a3140; border-radius:9px; margin-top:9px; padding-top:7px; }
QGroupBox::title { subcontrol-origin:margin; left:11px; padding:0 4px; color:#9aa4b5; font-weight:600; }
QPushButton { background:#232a36; border:1px solid #313a4a; border-radius:8px; padding:6px 13px; }
QPushButton:hover { background:#2b3340; border-color:#3a4658; }
QPushButton:pressed { background:#1b212c; }
QPushButton:checked { background:#2e6bd6; border:none; color:white; font-weight:700; }
QPushButton#run  { background:#17a06a; border:none; color:white; font-weight:700; }
QPushButton#run:hover { background:#1cb27a; }
QPushButton#stop { background:#d2453b; border:none; color:white; font-weight:700; }
QPushButton#rec  { background:#7a2230; border:none; color:white; font-weight:700; }
QPushButton#recOn{ background:#d2453b; border:none; color:white; font-weight:700; }
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
    background:#1b212c; border:1px solid #2a3140; border-radius:7px; padding:4px 6px;
    selection-background-color:#2e6bd6; }
QComboBox { min-width:70px; }
QComboBox:hover, QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover { border-color:#3a4658; }
QComboBox::drop-down { border:0; width:18px; }
QTableWidget, QTextEdit, QPlainTextEdit, QTreeWidget { background:#0c0f15; border:1px solid #2a3140; border-radius:8px; }
QHeaderView::section { background:#161b24; color:#9aa4b5; border:0; padding:5px; }
QScrollArea { border:0; }
QCheckBox { color:#c5cbd6; spacing:5px; }
QScrollBar:vertical { background:transparent; width:12px; margin:2px; }
QScrollBar::handle:vertical { background:#2a3140; border-radius:5px; min-height:26px; }
QScrollBar::handle:vertical:hover { background:#3a4658; }
QScrollBar:horizontal { background:transparent; height:12px; margin:2px; }
QScrollBar::handle:horizontal { background:#2a3140; border-radius:5px; min-width:26px; }
QScrollBar::handle:horizontal:hover { background:#3a4658; }
QScrollBar::add-line, QScrollBar::sub-line { width:0; height:0; }
QScrollBar::add-page, QScrollBar::sub-page { background:transparent; }
QSplitter::handle { background:#1b212c; }
QTabWidget QTabBar::tab { min-width:60px; }
)";

// SIS_WINDOW_SHOT=<png> [SIS_WINDOW_TAB=<idx>]: screenshot a tab and quit (visual QA).
#include <QTimer>
#include <QApplication>

StudioWindow::StudioWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("SIS3316 DAQ & Analysis Software");
    resize(1660, 980);

    auto* tabs = new QTabWidget();
    tabs->addTab(new LiveDaqWidget(), "●  Live DAQ");
    tabs->addTab(new ConfigEditor(),  "⚙  Configuration");
    tabs->addTab(new RootAnalyzer(),  "📊  ROOT Analyzer");

    // ---- structured log tab ----
    auto* logTab = new QWidget(); auto* lv = new QVBoxLayout(logTab);
    auto* logBar = new QHBoxLayout();
    auto* level = new QComboBox(); level->addItems({"Debug","Info","Warning","Error"});
    level->setCurrentIndex(0);
    auto* btnExport = new QPushButton("Export log…");
    auto* btnClear = new QPushButton("Clear view");
    logBar->addWidget(new QLabel("Min level:")); logBar->addWidget(level);
    logBar->addWidget(btnExport); logBar->addWidget(btnClear); logBar->addStretch();
    lv->addLayout(logBar);
    fLogView = new QPlainTextEdit(); fLogView->setReadOnly(true);
    fLogView->setMaximumBlockCount(5000);
    lv->addWidget(fLogView, 1);
    tabs->addTab(logTab, "📝  Logs");

    setCentralWidget(tabs);
    setStyleSheet(kStyle);

    connect(this, &StudioWindow::logArrived, fLogView, &QPlainTextEdit::appendPlainText,
            Qt::QueuedConnection);
    sis::Logger::instance().setSink([this](const sis::LogRecord& r){
        QString line = QString("%1  %2  [%3] %4")
            .arg(QDateTime::fromSecsSinceEpoch((qint64)r.tSec).toString("HH:mm:ss"))
            .arg(sis::logLevelName(r.level)).arg(QString::fromStdString(r.category))
            .arg(QString::fromStdString(r.message));
        emit logArrived(line);
    });
    connect(level, &QComboBox::currentIndexChanged, this, [](int idx){
        sis::Logger::instance().setMinLevel(static_cast<sis::LogLevel>(idx));
    });
    connect(btnExport, &QPushButton::clicked, this, [this]{
        QString fn = QFileDialog::getSaveFileName(this, "Export log", "sis3316.log", "Log (*.log *.txt)");
        if (!fn.isEmpty()) sis::Logger::instance().exportTo(fn.toStdString());
    });
    connect(btnClear, &QPushButton::clicked, fLogView, &QPlainTextEdit::clear);

    sis::Logger::instance().info("APP", "SIS3316 DAQ & Analysis Software started");

    if (qEnvironmentVariableIsSet("SIS_WINDOW_SHOT")) {
        int tab = qEnvironmentVariableIsSet("SIS_WINDOW_TAB") ? qEnvironmentVariable("SIS_WINDOW_TAB").toInt() : 1;
        QTimer::singleShot(700, this, [this,tabs,tab]{
            tabs->setCurrentIndex(tab);
            QTimer::singleShot(350, this, [this]{
                this->grab().save(qEnvironmentVariable("SIS_WINDOW_SHOT"));
                qInfo().noquote() << "WINDOW_SHOT saved";
                qApp->quit();
            });
        });
    }
}
