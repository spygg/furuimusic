// 福瑞音乐 - 桌面与移动共用的 QML 入口
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QtGlobal>

#include "../core/AppController.h"
#include "../core/AuthManager.h"
#include "PlatformAdapter.h"
#include "ThemeManager.h"

int main(int argc, char *argv[])
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("福瑞音乐"));
    QGuiApplication::setOrganizationName(QStringLiteral("Furui"));

    // 自绘无边框窗口不会自动继承 exe 图标：显式设置窗口图标，任务栏与 Alt+Tab 才能显示
    QIcon appIcon;
    static const int kIconSizes[] = {16, 24, 32, 48, 64, 128, 256, 512};
    for (int size : kIconSizes)
        appIcon.addFile(QStringLiteral(":/icons/app-%1.png").arg(size));
    QGuiApplication::setWindowIcon(appIcon);
    // Linux Wayland 用 desktop file 名匹配应用图标（与 CI AppImage 的 FuruiMusic.desktop 一致）
    QGuiApplication::setDesktopFileName(QStringLiteral("FuruiMusic"));

    // 应用名与数据目录确定后再创建门面单例
    Furui::PlatformAdapter platform;
    Furui::ThemeManager theme;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("app"),
                                             Furui::AppController::instance());
    engine.rootContext()->setContextProperty(QStringLiteral("platform"), &platform);
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));

    if (engine.rootObjects().isEmpty())
        return -1;

    // 无人值守截图校验：设置 FURUI_CAPTURE=<png 路径> 时抓取窗口内容后退出
    if (!qEnvironmentVariableIsEmpty("FURUI_CAPTURE")) {
        const QString path = qEnvironmentVariable("FURUI_CAPTURE");
        const QString size = qEnvironmentVariable("FURUI_WINDOW_SIZE");
        QTimer::singleShot(1200, &engine, [&engine, size] {
            if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0))) {
                if (!size.isEmpty()) {
                    const QStringList wh = size.split(QLatin1Char('x'));
                    if (wh.size() == 2)
                        window->resize(wh.at(0).toInt(), wh.at(1).toInt());
                }
            }
        });
        QTimer::singleShot(1900, &engine, [&engine, path] {
            if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0)))
                window->grabWindow().save(path);
            QCoreApplication::quit();
        });
    }

    // 无边框与主题自检：设置 FURUI_SELFTEST=<日志路径> 时切换窗口状态并记录几何，结束后退出
    if (!qEnvironmentVariableIsEmpty("FURUI_SELFTEST")) {
        const QString logPath = qEnvironmentVariable("FURUI_SELFTEST");
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
        const auto report = [window, logPath](const char *tag) {
            const QRect g = window->geometry();
            const QRect a = window->screen()->availableGeometry();
            QFile f(logPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
                f.write(QStringLiteral("%1 geometry=%2,%3 %4x%5 available=%6,%7 %8x%9 state=%10\n")
                            .arg(QString::fromLatin1(tag))
                            .arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height())
                            .arg(a.x()).arg(a.y()).arg(a.width()).arg(a.height())
                            .arg(int(window->windowStates()))
                            .toUtf8());
                f.close();
            }
        };
        QTimer::singleShot(900, window, [report] { report("normal"); });
        QTimer::singleShot(1100, window, [window, &platform] { platform.toggleMaximize(window); });
        QTimer::singleShot(1700, window, [report] { report("maximized"); });
        QTimer::singleShot(1900, window, [window, &platform] { platform.toggleMaximize(window); });
        QTimer::singleShot(2400, window, [report] { report("restored"); });
        QTimer::singleShot(2600, window, [&theme] { theme.setMode(QStringLiteral("dark")); });
        QTimer::singleShot(3000, window, [&theme, logPath] {
            QFile f(logPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
                f.write(QStringLiteral("theme mode=%1 dark=%2\n")
                            .arg(theme.mode()).arg(int(theme.isDark())).toUtf8());
                f.close();
            }
        });
        QTimer::singleShot(3200, &engine, [] { QCoreApplication::quit(); });
    }

    // 扫码链路自检：FURUI_QR_DIAG=<日志路径> 时自动发起网易云扫码，逐次记录状态后退出
    if (!qEnvironmentVariableIsEmpty("FURUI_QR_DIAG")) {
        const QString logPath = qEnvironmentVariable("FURUI_QR_DIAG");
        auto *auth = Furui::AuthManager::instance();
        auto *ctl = Furui::AppController::instance();
        QObject::connect(auth, &Furui::AuthManager::qrChanged, &engine, [auth, logPath] {
            QFile f(logPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
                f.write(QStringLiteral("status=%1 active=%2 image=%3 hint=%4\n")
                            .arg(auth->qrStatus()).arg(int(auth->qrActive()))
                            .arg(auth->qrImagePath()).arg(auth->qrHint()).toUtf8());
                f.close();
            }
        });
        QObject::connect(auth, &Furui::AuthManager::loginStateChanged, &engine,
                         [logPath](const QString &id, bool ok, const QString &msg) {
            QFile f(logPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
                f.write(QStringLiteral("loginState %1 ok=%2 msg=%3\n").arg(id).arg(int(ok)).arg(msg).toUtf8());
                f.close();
            }
        });
        QObject::connect(auth, &Furui::AuthManager::librarySynced, &engine,
                         [logPath](const QString &id, bool ok, const QString &msg) {
            QFile f(logPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
                f.write(QStringLiteral("librarySynced %1 ok=%2 msg=%3\n").arg(id).arg(int(ok)).arg(msg).toUtf8());
                f.close();
            }
        });
        QTimer::singleShot(600, ctl, [ctl] { ctl->startQrLogin(QStringLiteral("netease")); });
        QTimer::singleShot(qEnvironmentVariableIntValue("FURUI_QR_DIAG_MS") > 0
                               ? qEnvironmentVariableIntValue("FURUI_QR_DIAG_MS") : 45000,
                           &engine, [] { QCoreApplication::quit(); });
    }

    // Cookie 登录自检：FURUI_COOKIE_DIAG=<日志路径> + FURUI_COOKIE_VALUE=<cookie> 时
    // 自动走一次网易云 Cookie 登录并记录校验/落库/歌单同步结果后退出
    if (!qEnvironmentVariableIsEmpty("FURUI_COOKIE_DIAG")) {
        const QString logPath = qEnvironmentVariable("FURUI_COOKIE_DIAG");
        auto *auth = Furui::AuthManager::instance();
        auto *ctl = Furui::AppController::instance();
        const auto writeLine = [logPath](const QString &line) {
            QFile f(logPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
                f.write(line.toUtf8());
                f.close();
            }
        };
        QObject::connect(auth, &Furui::AuthManager::loginStateChanged, &engine,
                         [writeLine](const QString &id, bool ok, const QString &msg) {
            writeLine(QStringLiteral("loginStateChanged %1 ok=%2 msg=%3\n").arg(id).arg(int(ok)).arg(msg));
        });
        QObject::connect(auth, &Furui::AuthManager::librarySynced, &engine,
                         [writeLine](const QString &id, bool ok, const QString &msg) {
            writeLine(QStringLiteral("librarySynced %1 ok=%2 msg=%3\n").arg(id).arg(int(ok)).arg(msg));
        });
        QTimer::singleShot(600, ctl, [ctl] {
            ctl->loginByCookie(QStringLiteral("netease"),
                               qEnvironmentVariable("FURUI_COOKIE_VALUE"));
        });
        QTimer::singleShot(15000, &engine, [] { QCoreApplication::quit(); });
    }

    return app.exec();
}
