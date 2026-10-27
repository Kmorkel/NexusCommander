#include <QApplication>
#include <QIcon>
#include <QLocale>
#include <QTranslator>
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QTranslator translator;
    const QStringList uiLanguages = QLocale::system().uiLanguages();

    for (const QString &locale : uiLanguages) {
        const QString baseName = "NexusCommander_" + QLocale(locale).name();
        if (translator.load(":/i18n/" + baseName)) {
            app.installTranslator(&translator);
            break;
        }
    }

    app.setApplicationName("NexusCommander");
    app.setApplicationVersion(QStringLiteral(APP_VERSION));
    // PNG, not the SVG: the SVG would need the QtSvg icon plugin
    app.setWindowIcon(QIcon(":/icons/nexuscommander_256.png"));

    MainWindow w;
    w.show();

    return app.exec();
}
