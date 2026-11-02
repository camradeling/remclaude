#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include "RemoteSession.h"

int main(int argc, char* argv[])
{
#ifndef Q_OS_ANDROID
    // Qt6 has no fcitx5 IM plugin; use IBus plugin which fcitx5 exposes via
    // its built-in IBus frontend (libibusfrontend.so).
    if (qgetenv("QT_IM_MODULE").isEmpty())
        qputenv("QT_IM_MODULE", "ibus");
#endif

    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");

    QGuiApplication app(argc, argv);
    app.setOrganizationName("remclaude");
    app.setApplicationName("remclaude");
    app.setApplicationVersion("0.1.0");

    RemoteSession remote;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("remote", &remote);

    engine.load(QUrl(QStringLiteral("qrc:/qml/ChatScreen.qml")));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
