#include "PlotWidget.h"
#include <QPainter>
#include <QPainterPath>
#include <QImage>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QWheelEvent>
#include <cmath>
#include <algorithm>
#include <limits>

PlotWidget::PlotWidget(QWidget* parent): QWidget(parent) {
    setMinimumSize(540, 330);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void PlotWidget::resetView() { hasViewX = false; update(); }

void PlotWidget::setInteractionMode(int mode) { interactionMode = mode; }

void PlotWidget::setCurves(const QVector<CurveData>& c,
                           const QString& t,
                           const QString& xl,
                           const QString& yl,
                           bool ly,
                           const MarkerData& mk,
                           const QString& ib) {
    mode = Mode::Curves;
    curves = c;
    title = t;
    xLabel = xl;
    yLabel = yl;
    logY = ly;
    marker = mk;
    infoBox = ib;
    update();
}

void PlotWidget::setHistogram(const HistData& h, bool ly) {
    mode = Mode::Histogram;
    hist = h;
    title = h.title;
    xLabel = h.xLabel;
    yLabel = h.yLabel;
    logY = ly;
    marker = MarkerData();
    infoBox.clear();
    update();
}

void PlotWidget::clearPlot(const QString& t) {
    mode = Mode::Empty;
    curves.clear();
    hist = HistData();
    marker = MarkerData();
    title = t;
    xLabel.clear();
    yLabel.clear();
    infoBox.clear();
    logY = false;
    update();
}

bool PlotWidget::savePng(const QString& filename) {
    // Export on a clean white background with dark ink (publication-friendly),
    // then restore the on-screen dark theme.
    QColor bg=thBg, ink=thInk, ax=thAxis, gr=thGrid, pb=thPanelBg, pbd=thPanelBorder;
    thBg=Qt::white; thInk=QColor(20,20,20); thAxis=QColor(70,70,70); thGrid=QColor(200,200,200);
    thPanelBg=QColor(255,255,255,235); thPanelBorder=QColor(120,120,120);
    QImage img(size()*devicePixelRatioF(), QImage::Format_ARGB32);
    img.setDevicePixelRatio(devicePixelRatioF());
    img.fill(thBg);
    QPainter p(&img);
    render(&p);
    p.end();
    thBg=bg; thInk=ink; thAxis=ax; thGrid=gr; thPanelBg=pb; thPanelBorder=pbd;
    return img.save(filename);
}

QRectF PlotWidget::plotArea(const QRect& r) const {
    return QRectF(r.left()+82, r.top()+58, r.width()-120, r.height()-112);
}

double PlotWidget::transformY(double y) const {
    if (!logY) return y;
    return std::log10(std::max(y, 1e-12));
}

void PlotWidget::drawAxes(QPainter& p, const QRectF& a, double xmin, double xmax, double ymin, double ymax) {
    lastXMin = xmin; lastXMax = xmax;
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(thAxis, 1));
    p.drawRect(a);

    QFont tickFont = p.font();
    tickFont.setPointSize(9);
    p.setFont(tickFont);

    for (int i=0; i<=5; ++i) {
        double tx = a.left() + i*a.width()/5.0;
        p.setPen(QPen(thGrid, 1));
        p.drawLine(QPointF(tx, a.top()), QPointF(tx, a.bottom()));
        double xv = xmin + i*(xmax-xmin)/5.0;
        p.setPen(thInk);
        p.drawText(QRectF(tx-48, a.bottom()+4, 96, 20), Qt::AlignCenter, QString::number(xv, 'g', 5));

        double ty = a.bottom() - i*a.height()/5.0;
        p.setPen(QPen(thGrid, 1));
        p.drawLine(QPointF(a.left(), ty), QPointF(a.right(), ty));
        double yv = ymin + i*(ymax-ymin)/5.0;
        if (logY) yv = std::pow(10.0, yv);
        p.setPen(thInk);
        p.drawText(QRectF(a.left()-78, ty-10, 72, 20), Qt::AlignRight|Qt::AlignVCenter, QString::number(yv, 'g', 5));
    }

    p.setPen(thAxis);
    p.drawText(QRectF(a.left(), a.bottom()+32, a.width(), 24), Qt::AlignCenter, xLabel);
    p.save();
    p.translate(20, a.center().y());
    p.rotate(-90);
    p.drawText(QRectF(-a.height()/2, -10, a.height(), 22), Qt::AlignCenter, yLabel);
    p.restore();

    QFont titleFont = p.font();
    titleFont.setPointSize(13);
    titleFont.setBold(true);
    p.setFont(titleFont);
    p.setPen(thInk);
    p.drawText(QRectF(a.left(), 12, a.width(), 32), Qt::AlignCenter, title);
}

void PlotWidget::drawTextBox(QPainter& p, const QRectF& a, const QString& text) {
    if (text.trimmed().isEmpty()) return;
    QFont f = p.font();
    f.setPointSize(10);
    f.setBold(true);
    p.setFont(f);
    QFontMetrics fm(f);
    QRect br = fm.boundingRect(QRect(0, 0, 240, 180), Qt::TextWordWrap, text).adjusted(-10, -8, 10, 8);
    br.moveTopRight(QPoint(int(a.right()-10), int(a.top()+14)));
    p.setPen(QPen(thPanelBorder, 1));
    p.setBrush(thPanelBg);
    p.drawRoundedRect(br, 5, 5);
    p.setPen(thInk);
    p.drawText(br.adjusted(7,5,-7,-5), Qt::TextWordWrap, text);
}

QRect PlotWidget::drawLegend(QPainter& p, const QRectF& a, const QVector<QString>& labels, const QVector<QColor>& colors) {
    if (labels.isEmpty()) { lastLegendRect = QRect(); return QRect(); }
    QFont f = p.font();
    f.setPointSize(9);
    p.setFont(f);
    QFontMetrics fm(f);
    int w = 0;
    for (const auto& s : labels) w = std::max(w, fm.horizontalAdvance(s));
    QRect box(0, 0, w+38, labels.size()*20+10);
    int bx = int(a.left() + legendPosNorm.x() * std::max(1.0, a.width() - box.width()));
    int by = int(a.top() + legendPosNorm.y() * std::max(1.0, a.height() - box.height()));
    box.moveTopLeft(QPoint(bx, by));
    lastLegendRect = box;
    p.setPen(QPen(thPanelBorder, 1));
    p.setBrush(thPanelBg);
    p.drawRoundedRect(box, 4, 4);
    for (int i=0; i<labels.size(); ++i) {
        int y = box.top()+9+i*20;
        p.setPen(QPen(colors[i % colors.size()], 2));
        p.drawLine(box.left()+8, y+7, box.left()+25, y+7);
        p.setPen(thInk);
        p.drawText(box.left()+31, y, w, 16, Qt::AlignLeft|Qt::AlignVCenter, labels[i]);
    }
    return box;
}


void PlotWidget::mousePressEvent(QMouseEvent* event) {
    if (lastLegendRect.contains(event->pos())) {
        movingLegend = true;
        legendDragOffset = event->pos() - lastLegendRect.topLeft();
        return;
    }
    if (interactionMode == 0) return;
    QRectF a = plotArea(rect());
    if (!a.contains(event->pos())) return;
    dragging = true;
    dragStart = dragCurrent = event->pos();
}

void PlotWidget::mouseMoveEvent(QMouseEvent* event) {
    if (movingLegend) {
        QRectF a = plotArea(rect());
        QPoint tl = event->pos() - legendDragOffset;
        double nx = (tl.x() - a.left()) / std::max(1.0, a.width() - lastLegendRect.width());
        double ny = (tl.y() - a.top()) / std::max(1.0, a.height() - lastLegendRect.height());
        legendPosNorm = QPointF(std::max(0.0, std::min(1.0, nx)), std::max(0.0, std::min(1.0, ny)));
        update();
        return;
    }
    if (!dragging) return;
    dragCurrent = event->pos();
    update();
}

void PlotWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (movingLegend) { movingLegend = false; update(); return; }
    if (!dragging) return;
    dragging = false;
    QRectF a = plotArea(rect());
    double xmin = hasViewX ? viewXMin : lastXMin;
    double xmax = hasViewX ? viewXMax : lastXMax;
    auto toX = [&](int px){
        double t = (px - a.left()) / a.width();
        t = std::max(0.0, std::min(1.0, t));
        return xmin + t*(xmax-xmin);
    };
    if (interactionMode == 1) {
        if (std::abs(event->pos().x() - dragStart.x()) > 8) {
            double x0 = toX(dragStart.x());
            double x1 = toX(event->pos().x());
            if (x1 < x0) std::swap(x0, x1);
            if (x1 > x0) { viewXMin = x0; viewXMax = x1; hasViewX = true; }
        }
    } else if (interactionMode == 2) {
        double dx0 = toX(dragStart.x());
        double dx1 = toX(event->pos().x());
        double shift = dx0 - dx1;
        viewXMin = xmin + shift; viewXMax = xmax + shift; hasViewX = true;
    } else if (interactionMode == 3) {          // select an interval (ROI / fit range)
        if (std::abs(event->pos().x() - dragStart.x()) > 4) {
            double x0 = toX(dragStart.x()), x1 = toX(event->pos().x());
            if (x1 < x0) std::swap(x0, x1);
            if (x1 > x0) emit rangeSelected(x0, x1);
        }
    }
    update();
}

void PlotWidget::wheelEvent(QWheelEvent* event) {
    QRectF a = plotArea(rect());
    if (!a.contains(event->position().toPoint())) return;
    double xmin = hasViewX ? viewXMin : lastXMin;
    double xmax = hasViewX ? viewXMax : lastXMax;
    double span = xmax - xmin;
    if (span <= 0) return;
    double frac = (event->position().x() - a.left()) / a.width();
    frac = std::max(0.0, std::min(1.0, frac));
    double center = xmin + frac*span;
    double factor = event->angleDelta().y() > 0 ? 0.8 : 1.25;
    double newSpan = span * factor;
    viewXMin = center - frac*newSpan;
    viewXMax = center + (1.0-frac)*newSpan;
    hasViewX = true;
    update();
}

void PlotWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), thBg);
    QRectF a = plotArea(rect());

    if (mode == Mode::Empty) {
        drawAxes(p, a, 0, 1, 0, 1);
        return;
    }

    double xmin=0, xmax=1, ymin=0, ymax=1;
    QVector<QColor> colors = {QColor(31,119,180), QColor(255,127,14), QColor(44,160,44), QColor(214,39,40), QColor(148,103,189), QColor(140,86,75), QColor(227,119,194), QColor(127,127,127)};

    if (mode == Mode::Curves) {
        bool init = false;
        for (const auto& c : curves) {
            int n = std::min(c.x.size(), c.y.size());
            for (int i=0; i<n; ++i) {
                double x = c.x[i];
                double y = transformY(c.y[i]);
                if (!std::isfinite(x) || !std::isfinite(y)) continue;
                if (!init) { xmin=xmax=x; ymin=ymax=y; init=true; }
                xmin = std::min(xmin, x); xmax = std::max(xmax, x);
                ymin = std::min(ymin, y); ymax = std::max(ymax, y);
            }
        }
        if (!init) { drawAxes(p, a, 0, 1, 0, 1); return; }
        if (xmin == xmax) { xmin -= 1; xmax += 1; }
        if (hasViewX) { xmin = viewXMin; xmax = viewXMax; }
        if (ymin == ymax) { ymin -= 1; ymax += 1; }
        double yPad = 0.06*(ymax-ymin);
        ymin -= yPad; ymax += yPad;
        drawAxes(p, a, xmin, xmax, ymin, ymax);

        QVector<QString> labels;
        QVector<QColor> usedColors;
        for (int ci=0; ci<curves.size(); ++ci) {
            const auto& c = curves[ci];
            QColor col = colors[ci % colors.size()];
            col.setAlphaF(std::max(0.05, std::min(1.0, c.alpha)));
            p.setPen(QPen(col, c.lineWidth));
            if (c.pointsOnly) {
                p.setBrush(col);
                int n = std::min(c.x.size(), c.y.size());
                QVector<QPointF> pts;
                pts.reserve(n);
                for (int i=0; i<n; ++i) {
                    double xx = a.left() + (c.x[i]-xmin)/(xmax-xmin)*a.width();
                    double yy = a.bottom() - (transformY(c.y[i])-ymin)/(ymax-ymin)*a.height();
                    if (std::isfinite(xx) && std::isfinite(yy)) pts.append(QPointF(xx,yy));
                }
                p.drawPoints(pts.constData(), pts.size());
            } else {
                QPainterPath path;
                bool first=true;
                int n = std::min(c.x.size(), c.y.size());
                for (int i=0; i<n; ++i) {
                    double xx = a.left() + (c.x[i]-xmin)/(xmax-xmin)*a.width();
                    double yy = a.bottom() - (transformY(c.y[i])-ymin)/(ymax-ymin)*a.height();
                    if (!std::isfinite(xx) || !std::isfinite(yy)) continue;
                    if (first) { path.moveTo(xx, yy); first=false; }
                    else path.lineTo(xx, yy);
                }
                p.drawPath(path);
            }
            if (!c.label.isEmpty()) { labels.append(c.label); usedColors.append(colors[ci % colors.size()]); }
        }

        if (marker.enabled) {
            double mx = a.left() + (marker.x-xmin)/(xmax-xmin)*a.width();
            p.setPen(QPen(QColor(31,119,180), 2, Qt::DashLine));
            p.drawLine(QPointF(mx,a.top()), QPointF(mx,a.bottom()));
        }
        if (labels.size() > 1) drawLegend(p, a, labels, usedColors);
        drawTextBox(p, a, infoBox);
        if (dragging && (interactionMode == 1 || interactionMode == 3)) {
            QColor dc = interactionMode == 3 ? QColor(90,200,120) : QColor(255,140,0);  // green=select, orange=zoom
            p.setPen(QPen(dc, 1, Qt::DashLine));
            p.setBrush(QColor(dc.red(), dc.green(), dc.blue(), 40));
            QRect r(QPoint(dragStart.x(), int(a.top())), QPoint(dragCurrent.x(), int(a.bottom())));
            p.drawRect(r.normalized());
        }
        return;
    }

    if (hist.binEdges.size() < 2 || hist.counts.isEmpty()) {
        drawAxes(p, a, 0, 1, 0, 1);
        return;
    }
    xmin = hist.binEdges.first(); xmax = hist.binEdges.last();
    ymin = 0;
    ymax = *std::max_element(hist.counts.begin(), hist.counts.end());
    if (hist.hasFitCurve && !hist.fitY.isEmpty()) ymax = std::max(ymax, *std::max_element(hist.fitY.begin(), hist.fitY.end()));
    if (logY) { ymin = 0; ymax = transformY(std::max(10.0, ymax)); }
    if (xmax == xmin) { xmin -= 1; xmax += 1; }
    if (hasViewX) { xmin = viewXMin; xmax = viewXMax; }
    if (ymax <= ymin) ymax = ymin + 1;
    drawAxes(p, a, xmin, xmax, ymin, ymax);

    QVector<QString> histLabels;
    QVector<QColor> histColors;
    int nBars = std::min(hist.counts.size(), hist.binEdges.size()-1);
    if (!hist.channelCounts.isEmpty()) {
        for (int ch=0; ch<hist.channelCounts.size(); ++ch) {
            QColor col = colors[ch % colors.size()];
            col.setAlpha(130);
            p.setPen(Qt::NoPen);
            p.setBrush(col);
            int nLocal = std::min(nBars, int(hist.channelCounts[ch].size()));
            for (int i=0; i<nLocal; ++i) {
                double x0 = a.left() + (hist.binEdges[i]-xmin)/(xmax-xmin)*a.width();
                double x1 = a.left() + (hist.binEdges[i+1]-xmin)/(xmax-xmin)*a.width();
                double cy = transformY(hist.channelCounts[ch][i]);
                double yy = a.bottom() - (cy-ymin)/(ymax-ymin)*a.height();
                p.drawRect(QRectF(x0, yy, std::max(1.0, x1-x0), a.bottom()-yy));
            }
            histLabels << (ch < hist.channelLabels.size() ? hist.channelLabels[ch] : QString("Channel %1").arg(ch+1));
            histColors << colors[ch % colors.size()];
        }
    } else {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(31,119,180,205));
        for (int i=0; i<nBars; ++i) {
            double x0 = a.left() + (hist.binEdges[i]-xmin)/(xmax-xmin)*a.width();
            double x1 = a.left() + (hist.binEdges[i+1]-xmin)/(xmax-xmin)*a.width();
            double cy = transformY(hist.counts[i]);
            double yy = a.bottom() - (cy-ymin)/(ymax-ymin)*a.height();
            p.drawRect(QRectF(x0, yy, std::max(1.0, x1-x0), a.bottom()-yy));
        }
    }

    if (hist.hasFitInterval) {
        double x0 = a.left() + (hist.fitXMin-xmin)/(xmax-xmin)*a.width();
        double x1 = a.left() + (hist.fitXMax-xmin)/(xmax-xmin)*a.width();
        p.setPen(QPen(QColor(255,140,0), 2, Qt::DashLine));
        p.drawLine(QPointF(x0, a.top()), QPointF(x0, a.bottom()));
        p.drawLine(QPointF(x1, a.top()), QPointF(x1, a.bottom()));
    }

    if (hist.hasFitCurve) {
        QPainterPath path;
        bool first=true;
        int n = std::min(int(hist.fitX.size()), int(hist.fitY.size()));
        for (int i=0; i<n; ++i) {
            double xx = a.left() + (hist.fitX[i]-xmin)/(xmax-xmin)*a.width();
            double yy = a.bottom() - (transformY(hist.fitY[i])-ymin)/(ymax-ymin)*a.height();
            if (!std::isfinite(xx) || !std::isfinite(yy)) continue;
            if (first) { path.moveTo(xx, yy); first=false; }
            else path.lineTo(xx, yy);
        }
        p.setPen(QPen(QColor(220,20,60), 2, Qt::DashLine));
        p.drawPath(path);
    }

    for (const auto& ef : hist.extraFits) {
        QColor c = ef.color.isValid() ? ef.color : QColor(220,20,60);
        if (ef.hasFitInterval) {
            double x0 = a.left() + (ef.fitXMin-xmin)/(xmax-xmin)*a.width();
            double x1 = a.left() + (ef.fitXMax-xmin)/(xmax-xmin)*a.width();
            p.setPen(QPen(c, 2, Qt::DashLine));
            p.drawLine(QPointF(x0, a.top()), QPointF(x0, a.bottom()));
            p.drawLine(QPointF(x1, a.top()), QPointF(x1, a.bottom()));
        }
        if (ef.hasFitCurve) {
            QPainterPath path;
            bool first=true;
            int n = std::min(int(ef.fitX.size()), int(ef.fitY.size()));
            for (int i=0; i<n; ++i) {
                double xx = a.left() + (ef.fitX[i]-xmin)/(xmax-xmin)*a.width();
                double yy = a.bottom() - (transformY(ef.fitY[i])-ymin)/(ymax-ymin)*a.height();
                if (!std::isfinite(xx) || !std::isfinite(yy)) continue;
                if (first) { path.moveTo(xx, yy); first=false; }
                else path.lineTo(xx, yy);
            }
            p.setPen(QPen(c, 2, Qt::DashLine));
            p.drawPath(path);
        }
    }

    QVector<QString> labels = histLabels;
    QVector<QColor> legColors = histColors;
    if (hist.hasFitCurve) { labels.prepend("Fit interval"); legColors.prepend(QColor(255,140,0)); labels.prepend("Gaussian peak fit"); legColors.prepend(QColor(220,20,60)); }
    for (const auto& ef : hist.extraFits) {
        if (!ef.label.isEmpty()) { labels << ef.label; legColors << (ef.color.isValid() ? ef.color : QColor(220,20,60)); }
    }
    drawLegend(p, a, labels, legColors);

    drawTextBox(p, a, hist.infoBox);
    if (dragging && interactionMode == 1) {
        p.setPen(QPen(QColor(255,140,0), 1, Qt::DashLine));
        p.setBrush(QColor(255,140,0,35));
        QRect r(QPoint(dragStart.x(), int(a.top())), QPoint(dragCurrent.x(), int(a.bottom())));
        p.drawRect(r.normalized());
    }
}
