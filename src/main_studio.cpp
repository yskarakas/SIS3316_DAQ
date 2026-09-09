#include <QApplication>
#include "gui/StudioWindow.hpp"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationName("SIS3316 Studio");
    StudioWindow w;
    w.show();
    return app.exec();
}
