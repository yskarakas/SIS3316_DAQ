// RateBars — per-channel trigger-rate bar monitor (16 channels).
#pragma once
#include <QWidget>
#include <QPainter>
#include <array>
#include <algorithm>
#include <cmath>

class RateBars : public QWidget {
    Q_OBJECT
public:
    explicit RateBars(QWidget* parent = nullptr) : QWidget(parent) {
        fRates.fill(0); setMinimumHeight(180);
    }
    void setRates(const std::array<double,16>& r) { fRates = r; update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#0c0f15"));
        double mx = 1.0;
        for (double v : fRates) mx = std::max(mx, v);
        const int L = 40, Bm = 24, T = 24, Rm = 12;
        QRectF area(L, T, width()-L-Rm, height()-T-Bm);
        p.setPen(QColor("#e6e9ef")); QFont tf=p.font(); tf.setBold(true); p.setFont(tf);
        p.drawText(QRectF(L,2,area.width(),18), Qt::AlignLeft, "Per-channel trigger rate [Hz]");
        tf.setBold(false); tf.setPointSizeF(8); p.setFont(tf);
        double bw = area.width()/16.0;
        for (int c=0;c<16;++c){
            double h = area.height() * (fRates[c]/mx);
            QRectF bar(area.left()+c*bw+2, area.bottom()-h, bw-4, h);
            QColor col = fRates[c]>0 ? QColor("#5cc8ff") : QColor("#2a3140");
            p.fillRect(bar, col);
            p.setPen(QColor("#8a94a6"));
            p.drawText(QRectF(area.left()+c*bw, area.bottom()+2, bw, 16), Qt::AlignHCenter,
                       QString::number(c+1));
            if (fRates[c]>0){
                p.setPen(QColor("#cdd4df"));
                p.drawText(QRectF(area.left()+c*bw-6, bar.top()-14, bw+12, 14),
                           Qt::AlignHCenter, QString::number(fRates[c],'f',0));
            }
        }
        p.setPen(QColor("#9aa4b5"));
        p.drawText(QRectF(2,T-2,L-6,14), Qt::AlignRight, QString::number(mx,'f',0));
    }
private:
    std::array<double,16> fRates{};
};
