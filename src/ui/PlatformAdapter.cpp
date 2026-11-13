#include "PlatformAdapter.h"

#include "../core/Settings.h"
#include "core/QtCompat.h"

#include <QGuiApplication>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <cmath> // std::hypot：Qt 5.15 没有 qHypot（Qt 6 才加）
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QInputDevice>
#else
#include <QTouchDevice>
#endif

namespace Furui {

PlatformAdapter::PlatformAdapter(QObject *parent) : QObject(parent)
{
    // 输入设备列表要等平台插件初始化完才可靠（车机触摸屏常晚于窗口创建被枚举），
    // 启动后补一次广播，让 QML 里对 touchPrimary 的绑定重新求值。
    QTimer::singleShot(1200, this, [this] { emit touchPrimaryChanged(); });
}

bool PlatformAdapter::isMobile() const
{
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    return true;
#else
    return false;
#endif
}

bool PlatformAdapter::isTablet() const
{
    // 仅移动平台区分平板/手机；桌面一律视为非平板（走窗口宽度判定）。
    if (!isMobile())
        return false;
    // 外接屏/部分机型可能拿不到有效物理尺寸，此时按手机处理——强制移动壳，
    // 避免手机横屏宽度越过 760 阈值窜到桌面壳，在窄高屏上布局极不合理。
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen)
        return false;
    const QSizeF phys = screen->physicalSize(); // 单位毫米
    if (phys.width() <= 0 || phys.height() <= 0)
        return false;
    // 手机普遍 < 7 英寸，平板 >= 7 英寸（含车机矮宽屏 1920x720 这类 > 7 英寸的仍走桌面壳）。
    const qreal diagIn = std::hypot(phys.width(), phys.height()) / 25.4;
    return diagIn >= 7.0;
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
    // Qt < 5.15 没有该 API，furuiStartSystemMove 返回 false，QML 侧回退手算拖动
    return furuiStartSystemMove(window);
}

bool PlatformAdapter::startSystemResize(QQuickWindow *window, int edges)
{
    if (!window || isMobile() || edges == 0)
        return false;
    // Qt 5.15 起支持原生边缘拉伸；不支持的窗口管理器返回 false，由 QML 侧回退手算
    return furuiStartSystemResize(window, Qt::Edges(edges));
}

bool PlatformAdapter::isMaximized(QQuickWindow *window) const
{
    if (!window)
        return false;
    const Qt::WindowStates states = furuiWindowStates(window);
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

void PlatformAdapter::refreshInsets(QQuickWindow *window)
{
    qreal l = 0, t = 0, r = 0, b = 0;
    // QWindow::geometry() 和 QScreen 的几何都是设备无关像素，直接同单位相减。
    // 早先在这里把窗口乘了 dpr（以为 QScreen 是物理像素），dpr=2 的手机上右/下安全区
    // 被算成屏幕的一半，Loader 内缩后整个界面只剩左上 1/4。
    if (window && window->screen()) {
        // 只在「窗口铺屏」时计算：桌面浮窗被拖到任务栏上时也确实压到了可用区外，
        // 但那种情况该让用户移动窗口，而不是界面凭空缩进去一条
        const bool fillsScreen = isMobile() || window->visibility() == QWindow::Maximized
                                 || window->visibility() == QWindow::FullScreen;
        if (fillsScreen) {
            const QRect avail = window->screen()->availableGeometry();
            const QRect win(window->x(), window->y(), window->width(), window->height());
            // 只补「窗口确实压到系统栏上」的那部分：Qt 在多数平台上已经把窗口放在可用区内，
            // 此时四项都是 0，不会二次留白；车机/手机全屏时才会真的算出边距
            l = qMax(0.0, static_cast<qreal>(avail.left() - win.left()));
            t = qMax(0.0, static_cast<qreal>(avail.top() - win.top()));
            r = qMax(0.0, static_cast<qreal>(win.right() - avail.right()));
            b = qMax(0.0, static_cast<qreal>(win.bottom() - avail.bottom()));
        }
    }

    if (qFuzzyCompare(l, m_insetLeft) && qFuzzyCompare(t, m_insetTop)
        && qFuzzyCompare(r, m_insetRight) && qFuzzyCompare(b, m_insetBottom))
        return;
    m_insetLeft = l;
    m_insetTop = t;
    m_insetRight = r;
    m_insetBottom = b;
    emit insetsChanged();
}

bool PlatformAdapter::touchPrimary() const
{
    if (isMobile())
        return true;
    // 桌面/车机桌面外壳：直接问系统有没有触摸屏。以前是设置页里的手开关，容易忘了开；
    // 现在跟随系统自动判断——Windows 触屏一体机、车机主机自动为真，纯鼠标桌面为假。
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QList<const QInputDevice *> devices = QInputDevice::devices();
    for (const QInputDevice *dev : devices) {
        if (dev && dev->type() == QInputDevice::DeviceType::TouchScreen)
            return true;
    }
#else
    const QList<const QTouchDevice *> devices = QTouchDevice::devices();
    for (const QTouchDevice *dev : devices) {
        if (dev && dev->type() == QTouchDevice::TouchScreen)
            return true;
    }
#endif
    return false;
}

} // namespace Furui
