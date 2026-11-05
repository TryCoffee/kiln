// main.cpp — kiln-gui.
//
//   kiln-gui [image]                 open with an image chosen
//   kiln-gui --screenshot FILE [image]   render the window to FILE and quit
//                                        (with QT_QPA_PLATFORM=offscreen: no display needed)
//   kiln-gui [--screenshot FILE] --ventoy  open on "Make a Ventoy disk"
#include "mainwindow.hpp"

#include <QApplication>
#include <QIcon>
#include <QTimer>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("kiln"));
    QApplication::setApplicationName(QStringLiteral("kiln"));
    QApplication::setApplicationVersion(QStringLiteral(KILN_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("kiln"));
    // The installed theme icon when there is one, else the copy built in (Windows, macOS, a build tree).
    QIcon builtIn;
    for (int size : {16, 24, 32, 48, 64, 128, 256, 512})
        builtIn.addFile(QStringLiteral(":/icons/kiln-%1.png").arg(size), QSize(size, size));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("io.github.trycoffee.kiln"), builtIn));

    QStringList args = QApplication::arguments().mid(1);
    QString shot;
    if (args.size() >= 2 && args[0] == QLatin1String("--screenshot")) {
        shot = args[1];
        args = args.mid(2);
    }

    kiln::MainWindow w;
    w.show();
    if (!args.isEmpty() && args[0] == QLatin1String("--ventoy")) w.chooseVentoy();
    else if (!args.isEmpty()) w.openImage(args[0]);
    if (shot.isEmpty())
        w.checkForUpdates();  // not for screenshots: those run in tests and must not use the network
    else
        QTimer::singleShot(4000, &w, [&w, shot] {  // time for the image to be inspected
            w.grab().save(shot);
            QApplication::quit();
        });
    return QApplication::exec();
}
