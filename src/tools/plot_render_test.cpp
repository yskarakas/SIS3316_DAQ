// ===========================================================================
//  plot_render_test.cpp — headless render regression test for PlotWidget.
//
//  Renders (a) a plain histogram and (b) a per-channel COLOURED histogram to
//  PNGs off-screen and checks that pixels are actually drawn and that multiple
//  distinct series colours appear. Guards against the class of bug where a
//  coloured histogram (channelCounts set, counts left empty) silently renders
//  blank because the paint guard bails on an empty `counts` vector.
//
//  Run:  QT_QPA_PLATFORM=offscreen ./build/plot_render_test
//  Exit 0 = both render with content; 1 = a plot came out blank.
// ===========================================================================
#include "../gui/PlotWidget.h"
#include <QApplication>
#include <QImage>
#include <cstdio>
#include <set>

// Count saturated DATA-colour pixels (bars/curves) and how many distinct such
// colours appear. Theme-robust: a blank plot draws only the background + grey
// axes/text (near-zero saturation), so `content` stays tiny — which is exactly
// the bug we guard against (channelCounts set but counts empty → nothing drawn).
static void analyze(PlotWidget& w, const char* png, long& content, int& distinctColors) {
    w.resize(700, 420);
    w.savePng(png);                       // renders the widget (bars/curves) to PNG
    QImage im(png);
    content = 0; std::set<unsigned> cols;
    for (int y=0;y<im.height();++y) for (int x=0;x<im.width();++x){
        QRgb c = im.pixel(x,y);
        int r=qRed(c),g=qGreen(c),b=qBlue(c);
        int mx=std::max({r,g,b}), mn=std::min({r,g,b});
        if (mx-mn > 40 && mx > 80) { ++content; cols.insert((r/32<<10)|(g/32<<5)|(b/32)); }
    }
    distinctColors = (int)cols.size();
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    int fail = 0;

    // (a) plain histogram
    {
        PlotWidget w; HistData h;
        int nb=64; h.title="plain"; h.xLabel="x"; h.yLabel="Counts";
        h.binEdges.resize(nb+1); for(int i=0;i<=nb;++i) h.binEdges[i]=i;
        h.counts.resize(nb); for(int i=0;i<nb;++i) h.counts[i]= 100.0*std::exp(-0.5*std::pow((i-32)/6.0,2));
        w.setHistogram(h,false);
        long ct; int dc; analyze(w,"/tmp/pt_plain.png",ct,dc);
        printf("plain histogram    : content=%ld distinctColors=%d  %s\n", ct,dc, (ct>500&&dc>=1)?"ok":"FAIL(blank)");
        if (!(ct>500 && dc>=1)) ++fail;
    }
    // (b) coloured per-channel histogram (the regression case)
    {
        PlotWidget w; HistData h;
        int nb=64; h.title="coloured by channel"; h.xLabel="x"; h.yLabel="Counts";
        h.binEdges.resize(nb+1); for(int i=0;i<=nb;++i) h.binEdges[i]=i;
        // 3 channels, peaks at different positions
        for (int ch=0; ch<3; ++ch){
            QVector<double> cc(nb,0.0);
            for(int i=0;i<nb;++i) cc[i]= 80.0*std::exp(-0.5*std::pow((i-(16+ch*16))/5.0,2));
            h.channelCounts.push_back(cc);
            h.channelLabels.push_back(QString("Ch %1").arg(ch+1));
        }
        // per-bin total (this is what the fix populates)
        h.counts = QVector<double>(nb,0.0);
        for (auto& cc : h.channelCounts) for(int i=0;i<nb;++i) h.counts[i]+=cc[i];
        w.setHistogram(h,false);
        long ct; int dc; analyze(w,"/tmp/pt_colored.png",ct,dc);
        printf("coloured histogram : content=%ld distinctColors=%d  %s\n", ct,dc,
               (ct>500 && dc>=3)?"ok":"FAIL(blank or single colour)");
        if (!(ct>500 && dc>=3)) ++fail;
    }

    printf("\n== %s ==\n", fail==0 ? "PLOT RENDER OK" : "PLOT RENDER FAILURES");
    return fail==0?0:1;
}
