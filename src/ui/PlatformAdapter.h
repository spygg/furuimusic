#pragma once
// 福瑞音乐 - 平台适配层
// 只暴露“平台是什么、目录在哪、能力是否具备”，页面不写平台条件编译。
#include <QObject>
#include <QString>
#include <QUrl>

class QQuickWindow;

namespace Furui {

class PlatformAdapter : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isMobile READ isMobile CONSTANT)
    Q_PROPERTY(bool isDesktop READ isDesktop CONSTANT)
    Q_PROPERTY(QString platformName READ platformName CONSTANT)
    Q_PROPERTY(QString dataDir READ dataDir CONSTANT)
    Q_PROPERTY(QString downloadsDir READ downloadsDir CONSTANT)
    Q_PROPERTY(bool supportsTray READ supportsTray CONSTANT)
    Q_PROPERTY(bool supportsNotification READ supportsNotification CONSTANT)
    Q_PROPERTY(int minimumWindowWidth READ minimumWindowWidth CONSTANT)
    Q_PROPERTY(int minimumWindowHeight READ minimumWindowHeight CONSTANT)
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
    // 触控优先：车机即使跑桌面壳也是手指操作，hover 才有的入口必须常驻可见。
    // 只按 isMobile 判断会在「Windows 车机主机 + 桌面外壳」下把播放浮钮藏掉。
    Q_PROPERTY(bool touchPrimary READ touchPrimary NOTIFY touchPrimaryChanged)

public:
    explicit PlatformAdapter(QObject *parent = nullptr);

    bool isMobile() const;
    bool isDesktop() const { return !isMobile(); }
    QString platformName() const;
    QString dataDir() const;
    QString downloadsDir() const;
    bool supportsTray() const { return isDesktop(); }
    bool supportsNotification() const { return isDesktop(); }
    // 车机横分屏常把窗口压到 300~400 高（1920x1080 半屏 @density1.5 只有 360dp），
    // 旧的最小值 480/520 会让 App 直接进不去分屏，这里必须留出余量
    int minimumWindowWidth() const { return isMobile() ? 240 : 320; }
    int minimumWindowHeight() const { return isMobile() ? 300 : 360; }

    // 桌面端才有无边框窗口；Android/iOS 用系统窗口
    bool framelessSupported() const { return isDesktop(); }
    // macOS 用红绿灯式控件放左侧，Windows/Linux 用右侧方形按钮
    QString titleBarStyle() const;
    bool windowControlsOnLeft() const;

    bool touchPrimary() const;
    qreal insetLeft() const { return m_insetLeft; }
    qreal insetTop() const { return m_insetTop; }
    qreal insetRight() const { return m_insetRight; }
    qreal insetBottom() const { return m_insetBottom; }

    // 本地路径转 QML 可用的 file:// URL，避免页面自己拼字符串
    Q_INVOKABLE QUrl toFileUrl(const QString &path) const;

    // 重新计算安全区边距：窗口尺寸、可见状态、屏幕或 DPR 变化时由 QML 调用
    Q_INVOKABLE void refreshInsets(QQuickWindow *window);
    // 车机/触屏桌面可显式打开触控优先（手机不需要，恒为开）
    Q_INVOKABLE void setTouchPrimary(bool on);

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

private:
    qreal m_insetLeft = 0;
    qreal m_insetTop = 0;
    qreal m_insetRight = 0;
    qreal m_insetBottom = 0;
};

} // namespace Furui
