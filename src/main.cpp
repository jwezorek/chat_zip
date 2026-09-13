#include "ui/main_window.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QIcon>
#include <QWebEngineProfile>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QCoreApplication::setOrganizationName(QStringLiteral("ChatZip"));
    QCoreApplication::setApplicationName(QStringLiteral("ChatZip"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/ChatZip.ico")));

    QWebEngineProfile web_profile(QStringLiteral("ChatZip"));
    web_profile.setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);

    ui::MainWindow window(web_profile);
    window.show();

    return app.exec();
}
