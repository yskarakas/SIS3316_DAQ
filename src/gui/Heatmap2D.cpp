#include "Heatmap2D.hpp"
#include <QPainter>
#include <QtMath>
#include <algorithm>

Heatmap2D::Heatmap2D(QWidget* parent) : QWidget(parent) {
    fBins.fill(0.0, fNx * fNy);
    setMinimumSize(360, 280);
}

void Heatmap2D::setRange(double xlo, double xhi, double ylo, double yhi) {
    fXlo = xlo; fXhi = xhi; fYlo = ylo; fYhi = yhi; clear();
}
void Heatmap2D::setBins(int nx, int ny) { fNx = nx; fNy = ny; clear(); }
void Heatmap2D::setLabels(const QString& t, const QString& x, const QString& y) {
    fTitle = t; fXlabel = x; fYlabel = y; update();
}

void Heatmap2D::clear() {
    fBins.fill(0.0, fNx * fNy);
    fMax = 0;
    update();
}

void Heatmap2D::fill(double x, double y, double w) {
    if (x < fXlo || x >= fXhi || y < fYlo || y >= fYhi) return;
    int ix = int((x - fXlo) / (fXhi - fXlo) * fNx);
    int iy = int((y - fYlo) / (fYhi - fYlo) * fNy);
    if (ix < 0 || ix >= fNx || iy < 0 || iy >= fNy) return;
    double v = (fBins[iy * fNx + ix] += w);
    if (v > fMax) fMax = v;
}

QColor Heatmap2D::palette(double t) const {
    // ROOT "bird"-like: dark blue -> cyan -> green -> yellow -> red
    t = std::clamp(t, 0.0, 1.0);
    static const double r[] = {0.20,0.06,0.00,0.87,0.99};
    static const double g[] = {0.08,0.69,0.81,0.93,0.20};
    static const double b[] = {0.46,0.86,0.36,0.00,0.10};
    double f = t * 4.0; int i = std::min(3, int(f)); double u = f - i;
    auto mix = [&](const double* c){ return c[i] + (c[i+1]-c[i]) * u; };
    return QColor::fromRgbF(mix(r), mix(g), mix(b));
}

void Heatmap2D::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#0c0f15"));
    const int L = 60, R = 70, T = 28, B = 42;
    QRectF area(L, T, width() - L - R, height() - T - B);
    if (area.width() < 10 || area.height() < 10) return;

    // cells
    const double zmax = fMax > 0 ? fMax : 1.0;
    const double cw = area.width() / fNx, chh = area.height() / fNy;
    for (int iy = 0; iy < fNy; ++iy) {
        for (int ix = 0; ix < fNx; ++ix) {
            double v = fBins[iy * fNx + ix];
            if (v <= 0) continue;
            double t = fLogZ ? std::log1p(v) / std::log1p(zmax) : v / zmax;
            double x = area.left() + ix * cw;
            double y = area.bottom() - (iy + 1) * chh;
            p.fillRect(QRectF(x, y, cw + 1.0, chh + 1.0), palette(t));
        }
    }

    // frame + ticks
    p.setPen(QColor("#3a4254"));
    p.drawRect(area);
    p.setPen(QColor("#9aa4b5"));
    QFont f = p.font(); f.setPointSizeF(8); p.setFont(f);
    for (int k = 0; k <= 5; ++k) {
        double fx = k / 5.0;
        double xv = fXlo + fx * (fXhi - fXlo);
        double px = area.left() + fx * area.width();
        p.drawLine(QPointF(px, area.bottom()), QPointF(px, area.bottom() + 4));
        p.drawText(QRectF(px - 30, area.bottom() + 6, 60, 14), Qt::AlignHCenter,
                   QString::number(xv, 'g', 3));
        double yv = fYlo + fx * (fYhi - fYlo);
        double py = area.bottom() - fx * area.height();
        p.drawLine(QPointF(area.left() - 4, py), QPointF(area.left(), py));
        p.drawText(QRectF(area.left() - 56, py - 8, 50, 16), Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(yv, 'f', 2));
    }

    // color bar
    QRectF bar(area.right() + 14, area.top(), 14, area.height());
    for (int i = 0; i < int(bar.height()); ++i) {
        double t = 1.0 - i / bar.height();
        p.fillRect(QRectF(bar.left(), bar.top() + i, bar.width(), 1.2), palette(t));
    }
    p.setPen(QColor("#3a4254")); p.drawRect(bar);

    // labels
    p.setPen(QColor("#e6e9ef"));
    QFont tf = p.font(); tf.setPointSizeF(10); tf.setBold(true); p.setFont(tf);
    p.drawText(QRectF(area.left(), 4, area.width(), 20), Qt::AlignHCenter, fTitle);
    tf.setBold(false); tf.setPointSizeF(8.5); p.setFont(tf);
    p.setPen(QColor("#9aa4b5"));
    p.drawText(QRectF(area.left(), height() - 18, area.width(), 16), Qt::AlignHCenter, fXlabel);
    p.save();
    p.translate(14, area.center().y());
    p.rotate(-90);
    p.drawText(QRectF(-area.height() / 2, 0, area.height(), 14), Qt::AlignHCenter, fYlabel);
    p.restore();
}
