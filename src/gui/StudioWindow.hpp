// StudioWindow — top-level window: Live DAQ + Configuration + ROOT Analyzer +
// structured Log, in a compact modern tabbed layout. All offline analysis is
// ROOT-based; there is no .bin path.
#pragma once
#include <QMainWindow>

class QPlainTextEdit;

class StudioWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit StudioWindow(QWidget* parent = nullptr);

signals:
    void logArrived(const QString& line);   // thread-safe bridge from Logger

private:
    QPlainTextEdit* fLogView = nullptr;
};
