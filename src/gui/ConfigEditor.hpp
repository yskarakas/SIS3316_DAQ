// ===========================================================================
//  ConfigEditor — full hardware configuration editor with named, saved presets.
//
//  Exposes every important SIS3316 parameter in the GUI (per-channel trigger
//  enable/invert, threshold, CFD, high-energy suppress, FIR peaking/gap;
//  per-group accumulator PSD gates and histogram options; global sample length
//  / pre-trigger / gate window). Each non-obvious control has a (?) help popup.
//
//  Configurations are NOT tied to any one scintillator: the user saves and
//  recalls their own named presets (stored as JSON under <root>/configs/), so
//  plastic / EJ-309 / other detectors each get their own configuration.
//
//  Use while live acquisition is STOPPED (single link grant).
// ===========================================================================
#pragma once
#include <QWidget>
#include <array>
#include "../hw/Sis3316Config.hpp"

class QTableWidget;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QLabel;

class ConfigEditor : public QWidget {
    Q_OBJECT
public:
    explicit ConfigEditor(QWidget* parent = nullptr);

private slots:
    void readFromBoard();
    void applyFullConfig();
    void clearHistograms();
    void saveConfigAs();
    void loadSelected();
    void deleteSelected();
    void syncStore();          // push current editor state to the shared ConfigStore

private:
    void buildUi();
    void populateFromConfig(const sis::BoardConfig& cfg);
    sis::BoardConfig buildConfig() const;
    QString host() const;
    quint16 port() const;
    QString configDir() const;
    void refreshConfigList(const QString& select = QString());
    void setStatus(const QString& msg, bool error=false);

    QLineEdit* fIp = nullptr;
    QLineEdit* fPort = nullptr;
    QLabel* fStatus = nullptr;
    QComboBox* fConfigList = nullptr;

    QTableWidget* fChanTable = nullptr;
    QTableWidget* fGateTable = nullptr;

    QSpinBox* fSampleLen = nullptr;
    QSpinBox* fPreTrig = nullptr;
    QSpinBox* fGateWin = nullptr;
    QSpinBox* fDacOffset = nullptr;   // global baseline DAC offset (all channels)
    QComboBox* fInputRange = nullptr; // global input range / gain (5V/2V/1.9V)
};
