#pragma once

#include <QWidget>
#include <QVector>
#include <QString>
#include <QColor>
#include <QPoint>

struct CurveData {
    QVector<double> x;
    QVector<double> y;
    QString label;
    bool pointsOnly = false;
    double alpha = 1.0;
    double lineWidth = 1.5;
};

struct MarkerData {
    double x = 0.0;
    QString label;
    bool enabled = false;
};

struct HistFitData {
    QVector<double> fitX;
    QVector<double> fitY;
    double fitXMin = 0.0;
    double fitXMax = 0.0;
    QString label;
    QColor color;
    bool hasFitInterval = false;
    bool hasFitCurve = false;
};

struct HistData {
    QVector<double> binEdges;
    QVector<double> counts;
    QVector<QVector<double>> channelCounts;
    QVector<QString> channelLabels;
    QString title;
    QString xLabel;
    QString yLabel;
    QString infoBox;
    double fitXMin = 0.0;
    double fitXMax = 0.0;
    bool hasFitInterval = false;
    QVector<double> fitX;
    QVector<double> fitY;
    bool hasFitCurve = false;
    QVector<HistFitData> extraFits;
};

class PlotWidget : public QWidget {
    Q_OBJECT
public:
    explicit PlotWidget(QWidget* parent=nullptr);

    void setCurves(const QVector<CurveData>& curves,
                   const QString& title,
                   const QString& xLabel,
                   const QString& yLabel,
                   bool logY=false,
                   const MarkerData& marker=MarkerData(),
                   const QString& infoBox=QString());

    void setHistogram(const HistData& hist, bool logY=false);
    void clearPlot(const QString& title="No data");
    bool savePng(const QString& filename);
    void resetView();
    void setInteractionMode(int mode); // 0 normal, 1 zoom-x, 2 pan-x, 3 select-interval

signals:
    void rangeSelected(double x0, double x1);   // emitted on drag-release in mode 3

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    enum class Mode { Empty, Curves, Histogram } mode = Mode::Empty;
    QVector<CurveData> curves;
    HistData hist;
    MarkerData marker;
    QString title = "No data";
    QString xLabel;
    QString yLabel;
    QString infoBox;
    bool logY = false;
    double lastXMin = 0.0, lastXMax = 1.0;
    double viewXMin = 0.0, viewXMax = 1.0;
    bool hasViewX = false;
    int interactionMode = 0;
    bool dragging = false;
    QPoint dragStart;
    QPoint dragCurrent;

    // Theme colours (dark on-screen to match the rest of the app; savePng()
    // temporarily swaps to a light/publication palette). Used everywhere instead
    // of hard-coded black/white so the canvas is consistent with the dark UI.
    QColor thBg{QColor("#0c0f15")};
    QColor thInk{QColor("#e6e9ef")};
    QColor thAxis{QColor("#9aa4b5")};
    QColor thGrid{QColor(42,49,64)};
    QColor thPanelBg{QColor(22,27,36,230)};
    QColor thPanelBorder{QColor(58,66,84)};

    QRectF plotArea(const QRect& r) const;
    double transformY(double y) const;
    void drawAxes(QPainter& p, const QRectF& area, double xmin, double xmax, double ymin, double ymax);
    void drawTextBox(QPainter& p, const QRectF& area, const QString& text);
    QRect drawLegend(QPainter& p, const QRectF& area, const QVector<QString>& labels, const QVector<QColor>& colors);
    QPointF legendPosNorm = QPointF(0.02, 0.02);
    QRect lastLegendRect;
    bool movingLegend = false;
    QPoint legendDragOffset;
};
