// Heatmap2D — lightweight 2D color-map (COLZ-style) widget for PSD-vs-amplitude.
#pragma once
#include <QWidget>
#include <QVector>
#include <QString>

class Heatmap2D : public QWidget {
    Q_OBJECT
public:
    explicit Heatmap2D(QWidget* parent = nullptr);

    void setRange(double xlo, double xhi, double ylo, double yhi);
    void setBins(int nx, int ny);
    void setLabels(const QString& title, const QString& xlabel, const QString& ylabel);
    void setLogZ(bool on) { fLogZ = on; update(); }
    void fill(double x, double y, double w = 1.0);
    void clear();

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QColor palette(double t) const;     // t in [0,1] -> ROOT-bird-like color
    int fNx = 256, fNy = 256;
    double fXlo = 0, fXhi = 4000, fYlo = 0, fYhi = 0.6;
    QVector<double> fBins;              // fNx*fNy, row-major (iy*fNx+ix)
    double fMax = 0;
    bool fLogZ = true;
    QString fTitle = "PSD vs Amplitude", fXlabel = "Amplitude [ADC]", fYlabel = "PSD (tail/total)";
};
