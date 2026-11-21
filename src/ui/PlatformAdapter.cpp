#include "PlatformAdapter.h"

#include "../core/Settings.h"
#include "core/QtCompat.h"

#include <QGuiApplication>
#include <QInputMethod>
#include <QMargins>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <cmath> // std::hypot：Qt 5.15 没有 qHypot（Qt 6 才加）
#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QCoreApplication>   // QNativeInterface::QAndroidApplication（Android 上随它引入）
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QInputDevice>
#else
#include <QTouchDevice>
#endif

#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
namespace {

// Android 安全区：直接问 DecorView 的 rootWindowInsets。
//
// 为什么不用 Qt 的 QWindow::safeAreaMargins()：Qt 6.11 在「刘海 + 状态栏」同时存在的
// 机型上会把两者相加（实测 141px 状态栏 + 141px 挖孔 = 282px），竖屏顶部因此多留了
// 整整一个状态栏的高度（80dp 而不是 40dp）；横屏旋转后又把同样的值同时塞进 left 和
// top，左侧轨道被推到屏幕中间 —— 就是「顶部/左侧空白太大」的直接原因。
// WindowInsets 是窗口坐标、随旋转实时更新，也自动处理分屏（窗口不在屏幕顶端时 top=0）。
struct AndroidInsets {
    qreal left = 0, top = 0, right = 0, bottom = 0;
    bool valid = false;
};

AndroidInsets queryAndroidInsets(qreal devicePixelRatio)
{
    AndroidInsets out;
    QJniObject activity = QNativeInterface::QAndroidApplication::context();
    if (!activity.isValid())
        return out;
    QJniObject window = activity.callObjectMethod("getWindow", "()Landroid/view/Window;");
    if (!window.isValid())
        return out;
    QJniObject decor = window.callObjectMethod("getDecorView", "()Landroid/view/View;");
    if (!decor.isValid())
        return out;
    // 窗口刚创建、还没 attach 到窗口管理器时返回 null，调用方会稍后重试
    QJniObject insets = decor.callObjectMethod("getRootWindowInsets",
                                               "()Landroid/view/WindowInsets;");
    if (!insets.isValid())
        return out;

    jint l = 0, t = 0, r = 0, b = 0;
    if (QNativeInterface::QAndroidApplication::sdkVersion() >= 30) {
        // Type.systemBars()(1) | Type.displayCutout()(128)：并集按「每边取最大」合并，
        // 正好是我们需要的「系统栏或挖孔压到的范围」，不会像相加那样翻倍
        QJniObject gi = insets.callObjectMethod("getInsets", "(I)Landroid/graphics/Insets;",
                                                jint(1 | 128));
        if (gi.isValid()) {
            l = gi.getField<jint>("left");
            t = gi.getField<jint>("top");
            r = gi.getField<jint>("right");
            b = gi.getField<jint>("bottom");
        }
    }
    if (l == 0 && t == 0 && r == 0 && b == 0) {
        // Android 10 及更早只有这组（已废弃但仍在）的逐边接口
        l = insets.callMethod<jint>("getSystemWindowInsetLeft");
        t = insets.callMethod<jint>("getSystemWindowInsetTop");
        r = insets.callMethod<jint>("getSystemWindowInsetRight");
        b = insets.callMethod<jint>("getSystemWindowInsetBottom");
    }
    if (l == 0 && t == 0 && r == 0 && b == 0)
        return out;

    const qreal dpr = devicePixelRatio > 0 ? devicePixelRatio : 1.0;
    out.left = l / dpr;
    out.top = t / dpr;
    out.right = r / dpr;
    out.bottom = b / dpr;
    out.valid = true;
    return out;
}

} // namespace
#endif

namespace Furui {

PlatformAdapter::PlatformAdapter(QObject *parent) : QObject(parent)
{
    // 输入设备列表要等平台插件初始化完才可靠（车机触摸屏常晚于窗口创建被枚举），
    // 启动后补一次广播，让 QML 里对 touchPrimary 的绑定重新求值。
    // isTablet 依赖屏幕物理尺寸，同样要等平台就绪，一并补发。
    QTimer::singleShot(1200, this, [this] {
        emit touchPrimaryChanged();
        emit formFactorChanged();
    });

    // 软键盘显隐会改变「底部安全区是否要计入」（见 refreshInsets 的去重逻辑），
    // 而输入法状态变化不一定伴随窗口尺寸变化（部分机型的 adjustResize 时机更晚），
    // 这里直接跟着输入法信号重算一次，键盘一弹出就把正确边距交给 QML。
    if (QInputMethod *ime = QGuiApplication::inputMethod()) {
        connect(ime, &QInputMethod::visibleChanged, this, [this] {
            if (m_safeAreaWindow)
                refreshInsets(m_safeAreaWindow);
        });
        connect(ime, &QInputMethod::keyboardRectangleChanged, this, [this] {
            if (m_safeAreaWindow)
                refreshInsets(m_safeAreaWindow);
        });
    }
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

bool PlatformAdapter::isMultiWindow(QQuickWindow *window) const
{
    bool multi = false;
#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // API 24+：Activity.isInMultiWindowMode() 是判断分屏/自由窗口最准的接口；
    // 车机 OEM 有时不按标准上报，所以再叠加一个「窗口明显窄于屏幕」的兜底。
    const QJniObject activity = QNativeInterface::QAndroidApplication::context();
    if (activity.isValid() && QNativeInterface::QAndroidApplication::sdkVersion() >= 24)
        multi = activity.callMethod<jboolean>("isInMultiWindowMode", "()Z");
#endif
    if (!multi && isMobile() && window && window->screen()) {
        const int screenWidth = window->screen()->geometry().width();
        if (screenWidth > 0 && window->width() > 0 && screenWidth >= window->width() * 1.2)
            multi = true;
    }
    return multi;
}

void PlatformAdapter::refreshInsets(QQuickWindow *window)
{
    qreal l = 0, t = 0, r = 0, b = 0;
    bool fromPlatform = false;
#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // ---- Android：安全区完全交给 Qt Quick，这里不下发任何边距（l/t/r/b 保持 0）----
    // Qt 6.11 的 Qt Quick 已把窗口摆到状态栏/挖孔下沿（实测竖屏窗口 360x734、屏幕
    // 360x806，QML 原点即在状态栏下沿），内容本就不会被系统栏盖住。最初误以为
    // 「搜索框与状态栏重合、点击无效」，真正原因是状态栏图标白底白字看不见（已由
    // ThemeManager::syncStatusBarIcons 修复），并非位置偏移。
    // 因此这里不再补 insetTop：补多少都会在状态栏下沿之上多出一块白色空白。
    // JNI 仅取一次系统 insets 记日志（现场核对机型用）；取到值即屏蔽下面的
    // availableGeometry 兜底，避免兜底在 Android 上算出左右/底部边距、改动横屏布局。
    if (window) {
        const AndroidInsets ai = queryAndroidInsets(window->devicePixelRatio());
        if (ai.valid) {
            m_lastOsLeft = ai.left;
            m_lastOsTop = ai.top;
            m_insetRetries = 0;
            fromPlatform = true;   // 屏蔽 availableGeometry 兜底，边距全部保持 0
        } else if (m_insetRetries < 6) {
            ++m_insetRetries;
            QPointer<QQuickWindow> guard(window);
            QTimer::singleShot(220, this, [this, guard] {
                if (guard)
                    refreshInsets(guard);
            });
        }
    }
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0) && !defined(Q_OS_ANDROID)
    // 非 Android 平台：safeAreaMargins 四边都可用（桌面全屏/刘海屏），照常应用。
    if (!fromPlatform && window) {
        const QMargins safe = window->safeAreaMargins();
        if (!safe.isNull()) {
            l = qMax(0, safe.left());
            t = qMax(0, safe.top());
            r = qMax(0, safe.right());
            b = qMax(0, safe.bottom());
            fromPlatform = !safe.isNull();
        }
    }
#endif
    // QWindow::geometry() 和 QScreen 的几何都是设备无关像素，直接同单位相减。
    // 早先在这里把窗口乘了 dpr（以为 QScreen 是物理像素），dpr=2 的手机上右/下安全区
    // 被算成屏幕的一半，Loader 内缩后整个界面只剩左上 1/4。
    if (!fromPlatform && l == 0 && t == 0 && r == 0 && b == 0 && window && window->screen()) {
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

    // ---- 软键盘与安全区去重（仅 Android）----
    // adjustResize 已经把键盘占掉的高度从窗口里扣掉了，此时窗口只剩键盘上方的可见区。
    // 但 Qt 6.9+ 的 safeAreaMargins() 在部分机型上还会把 IME 一并算进 bottom，
    // 两边叠加等于「扣两次」：窗口 527dp 再减掉 265dp 键盘，内容区只剩一百多 dp，
    // 表现就是曲库页一点搜索框，页头和底部标签栏被挤成顶部薄薄一条、下面一片空白。
    // 键盘可见时底部安全区没有意义（那块空间已被键盘盖住），直接清零；收起后按系统栏重算。
    const QInputMethod *ime = QGuiApplication::inputMethod();
    const bool imeVisible = ime && ime->isVisible();
#if defined(Q_OS_ANDROID)
    if (imeVisible)
        b = 0;
#endif

    // ---- 兜底：安全区不可能占掉那么多 ----
    // Android 上「adjustResize 已压缩窗口」与「平台上报 IME/系统栏安全区」叠加时，实测会
    // 算出把窗口大半都算成边距的离谱值（曲库页一点搜索框，页头与底部标签栏被压成顶部
    // 扁扁一条，下面一片空白）。真实系统栏最厚也就 60dp 上下，上下加起来超过窗口高度
    // 40% 一定是有误 —— 此时整组丢弃，让内容直接铺满窗口（键盘本身已经把窗口压小了）。
    if (window && window->height() > 0 && t + b > window->height() * 0.4) {
        qWarning("insets: 上下边距异常（%.0f+%.0f / 窗口高 %.0f dp），丢弃安全区",
                 t, b, window->height());
        l = t = r = b = 0;
    }
    if (window && window->width() > 0 && l + r > window->width() * 0.4) {
        qWarning("insets: 左右边距异常（%.0f+%.0f / 窗口宽 %.0f dp），丢弃安全区",
                 l, r, window->width());
        l = t = r = b = 0;
    }

    // 窗口几何变化（旋转、分屏、键盘）单独留一行，和安全区值分开看，
    // 便于判断「留白是系统栏占的」还是「Qt 自己把窗口挪下去了」
    if (window && window->geometry() != m_lastGeom) {
        m_lastGeom = window->geometry();
        const QRect scr = window->screen() ? window->screen()->geometry() : QRect();
        QQuickItem *c = window->contentItem();
        qInfo("geom: win=%d,%d %dx%d screen=%dx%d dpr=%.2f content=%d,%d %dx%d",
              window->x(), window->y(), window->width(), window->height(),
              scr.width(), scr.height(), window->devicePixelRatio(),
              c ? int(c->x()) : -1, c ? int(c->y()) : -1,
              c ? int(c->width()) : -1, c ? int(c->height()) : -1);
    }
    if (qFuzzyCompare(l, m_insetLeft) && qFuzzyCompare(t, m_insetTop)
        && qFuzzyCompare(r, m_insetRight) && qFuzzyCompare(b, m_insetBottom))
        return;
    // 每次安全区变化都留一行：现场排查「界面被挤到顶部 / 留白太大」时，
    // 一眼能看出是窗口被压扁、系统栏占位，还是两套算法给出了不同答案
    qInfo("insets: win=%dx%d at %d,%d os=%d,%d ime=%d src=%s -> %d,%d,%d,%d",
          window ? int(window->width()) : -1, window ? int(window->height()) : -1,
          window ? window->x() : -1, window ? int(window->y()) : -1,
          int(m_lastOsLeft), int(m_lastOsTop),
          int(imeVisible),
          fromPlatform ? "os" : "fallback",
          int(l), int(t), int(r), int(b));
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
