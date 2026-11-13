#include <QApplication>
#include <QtGlobal>

#include "MainWindow.h"
#include "../core/AppController.h"

int main(int argc, char *argv[])
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("福瑞音乐"));
    QApplication::setOrganizationName(QStringLiteral("Furui"));

    // 触发 UI 门面单例创建，后续所有页面都通过 instance() 访问。
    Furui::AppController::instance();

    MainWindow w;
    w.show();
    return app.exec();
}
