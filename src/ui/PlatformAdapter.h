#pragma once
// 福瑞音乐 - 平台适配层
// 只暴露“平台是什么、目录在哪、能力是否具备”，页面不写平台条件编译。
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QString>
#include <QUrl>

class QQuickWindow;

namespace Furui {

class PlatformAdapter : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isMobile READ isMobile CONSTANT)
    Q_PROPERTY(bool isDesktop READ isDesktop CONSTANT)
    // 平板/手机区分：仅移动平台有意义，桌面恒为 false。用来决定手机横屏是否允许
    // 切到桌面壳——手机（非平板）一律强制移动壳，平板按窗口宽度判定。
    // 物理尺寸要等平台初始化后才可读，带 NOTIFY 才能让 QML 的绑定在启动后重算
    // （否则 QQmlExpression 会报 "depends on non-bindable properties"）。
    Q_PROPERTY(bool isTablet READ isTablet NOTIFY formFactorChanged)
    Q_PROPERTY(QString platformName READ platformName CONSTANT)
    Q_PROPERTY(QString dataDir READ dataDir CONSTANT)
    Q_PROPERTY(QString downloadsDir READ downloadsDir CONSTANT)
    Q_PROPERTY(bool supportsTray READ supportsTray CONSTANT)
    Q_PROPERTY(bool supportsNotification READ supportsNotification CONSTANT)
    Q_PROPERTY(int minimumWindowWidth READ minimumWindowWidth CONSTANT)
    Q_PROPERTY(int minimumWindowHeight READ minimumWindowHeight CONSTANT)
    // Qt 主版本：Qt5/Qt6 的 QtMultimedia QML API 有差异（seek↔setPosition、
    // Qt6 需要 AudioOutput），QML 据此加载对应的媒体变体文件
    Q_PROPERTY(int qtMajor READ qtMajor CONSTANT)
    // 视频解码后端：
    //   ffmpeg       Qt6 QtMultimedia 官方 FFmpeg 插件（Windows/Android/Linux/macOS 同一套）
    //   ffmpeg-qt5   Qt5 + 本工程自编译 FFmpeg（build_ffmpeg_desktop.sh），跨平台、不依赖 DirectShow
    //   directshow   Qt5 且未启用自编译 FFmpeg，MP4/H.264 需要系统额外装解码器
    // UI 据此给出准确的降级说明，不再笼统写「本机缺少 MP4 解码器」。
    Q_PROPERTY(QString videoBackend READ videoBackend CONSTANT)
    // QML 用它选择 MvMedia5Ffmpeg.qml（自编译后端）还是 MvMedia5.qml（DirectShow）
    Q_PROPERTY(bool ffmpegBackend READ ffmpegBackend CONSTANT)
    // 无边框窗口相关：UI 只读这些能力位，不写平台条件判断
    Q_PROPERTY(bool framelessSupported READ framelessSupported CONSTANT)
    Q_PROPERTY(QString titleBarStyle READ titleBarStyle CONSTANT)
    Q_PROPERTY(bool windowControlsOnLeft READ windowControlsOnLeft CONSTANT)
    // 系统栏/刘海占用的边距（设备无关像素）。车机和手机的状态栏、导航栏会盖住窗口，
    // 界面只能读这四位做安全区内缩，不能自己去猜屏幕尺寸。
    Q_PROPERTY(qreal insetLeft READ insetLeft NOTIFY insetsChanged)
    Q_PROPERTY(qreal insetTop READ insetTop NOTIFY insetsChanged)
    Q_PROPERTY(qreal insetRight READ insetRight NOTIFY insetsChanged)
    Q_PROPERTY(qreal insetBottom READ insetBottom NOTIFY insetsChanged)
    // Qt 自行对窗口内容区做的安全区内缩量（safeAreaMargins.top，钳制前原值）。
    // insetTop 是「还差多少要由 QML 补」的差额，窗口铺满整屏且 Qt 已内缩时为 0；
    // 但状态栏区域实际被 Qt 从内容区里切掉了这么多，QML 想让背景铺过去必须知道它。
    Q_PROPERTY(qreal osSafeTop READ osSafeTop NOTIFY insetsChanged)
    // 纯系统状态栏高度（来自 JNI，不受软键盘 IME 影响，恒为 24dp）。
    // osSafeTop 在部分 GSI 机型上键盘收起后会卡在 80dp（IME 高度被算进 safeArea），
    // QML 用 statusBarTop 算出被误推的偏移并反向补偿。
    Q_PROPERTY(qreal statusBarTop READ statusBarTop NOTIFY insetsChanged)
    // 同 osSafeTop，只是横屏挖孔在左侧时 Qt 对内容区做的横向内缩量
    // （safeAreaMargins.left）。窗口级背景要盖住挖孔那条必须知道它。
    Q_PROPERTY(qreal osSafeLeft READ osSafeLeft NOTIFY insetsChanged)
    // 触控优先：车机即使跑桌面壳也是手指操作，hover 才有的入口必须常驻可见。
    // 桌面端按系统上报的输入设备自动判断（有触摸屏即为真），不再提供手开关。
    Q_PROPERTY(bool touchPrimary READ touchPrimary NOTIFY touchPrimaryChanged)

public:
    explicit PlatformAdapter(QObject *parent = nullptr);

    bool isMobile() const;
    bool isDesktop() const { return !isMobile(); }
    // 物理对角线 >= 7 英寸判为平板；无法探测尺寸时按手机处理（强制移动壳）
    bool isTablet() const;
    QString platformName() const;
    QString dataDir() const;
    QString downloadsDir() const;
    bool supportsTray() const { return isDesktop(); }
    bool supportsNotification() const { return isDesktop(); }
    // 车机横分屏常把窗口压到 300~400 高（1920x1080 半屏 @density1.5 只有 360dp），
    // 旧的最小值 480/520 会让 App 直接进不去分屏，这里必须留出余量
    int minimumWindowWidth() const { return isMobile() ? 240 : 320; }
    int minimumWindowHeight() const { return isMobile() ? 300 : 360; }
    int qtMajor() const { return QT_VERSION_MAJOR; }
    QString videoBackend() const {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return QStringLiteral("ffmpeg");
#elif defined(FURUI_HAVE_FFMPEG)
        return QStringLiteral("ffmpeg-qt5");
#else
        return QStringLiteral("directshow");
#endif
    }
    bool ffmpegBackend() const {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0) && defined(FURUI_HAVE_FFMPEG)
        return true;
#else
        return false;
#endif
    }

    // 桌面端才有无边框窗口；Android/iOS 用系统窗口
    bool framelessSupported() const { return isDesktop(); }
    // macOS 用红绿灯式控件放左侧，Windows/Linux 用右侧方形按钮
    QString titleBarStyle() const;
    bool windowControlsOnLeft() const;

    bool touchPrimary() const;
    qreal insetLeft() const { return m_insetLeft; }
    qreal insetTop() const { return m_insetTop; }
    qreal osSafeTop() const { return m_lastSafeTop; }
    qreal statusBarTop() const { return m_lastOsTop; }
    qreal osSafeLeft() const { return m_lastSafeLeft; }
    qreal insetRight() const { return m_insetRight; }
    qreal insetBottom() const { return m_insetBottom; }

    // 本地路径转 QML 可用的 file:// URL，避免页面自己拼字符串
    Q_INVOKABLE QUrl toFileUrl(const QString &path) const;

    // 重新计算安全区边距：窗口尺寸、可见状态、屏幕或 DPR 变化时由 QML 调用
    Q_INVOKABLE void refreshInsets(QQuickWindow *window);

    // 是否处于分屏/多窗口（Android 原生 isInMultiWindowMode + 窗口/屏幕宽度兜底）。
    // 车机上分屏后的窗口仍可能很宽（半屏 960dp），单靠宽度阈值会误选桌面壳。
    Q_INVOKABLE bool isMultiWindow(QQuickWindow *window) const;

    // 系统级窗口操作：由 QQmlWindow/QQuickWindow 原生接管，比在 QML 里模拟更稳
    Q_INVOKABLE bool startSystemMove(QQuickWindow *window);
    Q_INVOKABLE bool startSystemResize(QQuickWindow *window, int edges);
    Q_INVOKABLE bool isMaximized(QQuickWindow *window) const;
    Q_INVOKABLE void toggleMaximize(QQuickWindow *window);
    Q_INVOKABLE void minimizeWindow(QQuickWindow *window);
    Q_INVOKABLE void closeWindow(QQuickWindow *window);

signals:
    void insetsChanged();
    void touchPrimaryChanged();
    // isTablet 依赖屏幕物理尺寸，启动后（或切换主屏时）补发一次，驱动 QML 重新判定外壳
    void formFactorChanged();

private:
    qreal m_insetLeft = 0;
    qreal m_insetTop = 0;
    qreal m_insetRight = 0;
    qreal m_insetBottom = 0;
    // 已接上 safeAreaMarginsChanged 的窗口：Qt 的 UniqueConnection 不支持 lambda，
    // 这里自己记一个，避免每次 refreshInsets 重复 connect
    QPointer<QQuickWindow> m_safeAreaWindow;
    // Android 上 getRootWindowInsets() 在窗口 attach 之前返回 null，做有限次短延迟重试
    int m_insetRetries = 0;
    // 最近一次系统上报的原始 insets（扣掉 Qt 已让出的部分之前），只用于日志排查
    qreal m_lastOsLeft = 0;
    qreal m_lastOsTop = 0;
    // Qt safeAreaMargins().top()/left() 的原始值（钳制前），供窗口背景反向延伸覆盖状态栏/挖孔
    qreal m_lastSafeTop = 0;
    qreal m_lastSafeLeft = 0;
    // 上次 emit insetsChanged 时的 safeArea 快照：横竖屏 l/t/r/b 都是 0 时
    // 靠它判断原始安全区是否已变，避免 osSafeLeft/osSafeTop 绑定刷新不到。
    qreal m_emittedSafeTop = 0;
    qreal m_emittedSafeLeft = 0;
    // 窗口矩形变化时打一行几何日志（旋转/分屏/上下分屏切换时定位问题用）
    QRect m_lastGeom;
};

} // namespace Furui
