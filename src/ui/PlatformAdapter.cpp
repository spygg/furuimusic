#include "PlatformAdapter.h"

#include "../core/Settings.h"

#include <QQuickWindow>

namespace Furui {

PlatformAdapter::PlatformAdapter(QObject *parent) : QObject(parent) {}

bool PlatformAdapter::isMobile() const
{
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    return true;
#else
    return false;
#endif
}

QString PlatformAdapter::platformName() const
{
    // Android 也会定义 Q_OS_LINUX，所以这两个分支必须 Android 在前，
    // 否则手机上报的是「Linux」，所有按 Android 判断的分支都是死代码
#if defined(Q_OS_WIN)
    return QStringLiteral("Windows");
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    return QStringLiteral("macOS");
#elif defined(Q_OS_ANDROID)
    return QStringLiteral("Android");
#elif defined(Q_OS_IOS)
    return QStringLiteral("iOS");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("Linux");
#else
    return QStringLiteral("Unknown");
#endif
}

QString PlatformAdapter::dataDir() const { return Settings::dataDir(); }

QString PlatformAdapter::downloadsDir() const
{
    const QString configured = Settings::string(QStringLiteral("downloadDir"));
    return configured.isEmpty() ? Settings::filePath(QStringLiteral("downloads")) : configured;
}

QUrl PlatformAdapter::toFileUrl(const QString &path) const
{
    if (path.isEmpty()) return QUrl();
    if (path.startsWith(QStringLiteral("file:")) || path.startsWith(QStringLiteral("http")))
        return QUrl(path);
    return QUrl::fromLocalFile(path);
}

QString PlatformAdapter::titleBarStyle() const
{
#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    return QStringLiteral("mac");
#else
    return QStringLiteral("win");
#endif
}

bool PlatformAdapter::windowControlsOnLeft() const
{
    return titleBarStyle() == QLatin1String("mac");
}

bool PlatformAdapter::startSystemMove(QQuickWindow *window)
{
    if (!window || isMobile())
        return false;
    // 交给窗口管理器 / 系统做原生拖动，双击最大化等行为也随之保留
    return window->startSystemMove();
}

bool PlatformAdapter::startSystemResize(QQuickWindow *window, int edges)
{
    if (!window || isMobile() || edges == 0)
        return false;
    // Qt 5.15 起支持原生边缘拉伸；不支持的窗口管理器返回 false，由 QML 侧回退手算
    return window->startSystemResize(Qt::Edges(edges));
}

bool PlatformAdapter::isMaximized(QQuickWindow *window) const
{
    if (!window)
        return false;
    const Qt::WindowStates states = window->windowStates();
    return states.testFlag(Qt::WindowMaximized) || states.testFlag(Qt::WindowFullScreen);
}

void PlatformAdapter::toggleMaximize(QQuickWindow *window)
{
    if (!window || isMobile())
        return;
    if (isMaximized(window))
        window->showNormal();
    else
        window->showMaximized();
}

void PlatformAdapter::minimizeWindow(QQuickWindow *window)
{
    if (!window || isMobile())
        return;
    window->showMinimized();
}

void PlatformAdapter::closeWindow(QQuickWindow *window)
{
    if (!window)
        return;
    window->close();
}

} // namespace Furui
