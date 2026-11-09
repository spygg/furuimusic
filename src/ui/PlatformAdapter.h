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

public:
    explicit PlatformAdapter(QObject *parent = nullptr);

    bool isMobile() const;
    bool isDesktop() const { return !isMobile(); }
    QString platformName() const;
    QString dataDir() const;
    QString downloadsDir() const;
    bool supportsTray() const { return isDesktop(); }
    bool supportsNotification() const { return isDesktop(); }
    int minimumWindowWidth() const { return isMobile() ? 300 : 360; }
    int minimumWindowHeight() const { return isMobile() ? 480 : 520; }

    // 桌面端才有无边框窗口；Android/iOS 用系统窗口
    bool framelessSupported() const { return isDesktop(); }
    // macOS 用红绿灯式控件放左侧，Windows/Linux 用右侧方形按钮
    QString titleBarStyle() const;
    bool windowControlsOnLeft() const;

    // 本地路径转 QML 可用的 file:// URL，避免页面自己拼字符串
    Q_INVOKABLE QUrl toFileUrl(const QString &path) const;

    // 系统级窗口操作：由 QQmlWindow/QQuickWindow 原生接管，比在 QML 里模拟更稳
    Q_INVOKABLE bool startSystemMove(QQuickWindow *window);
    Q_INVOKABLE bool startSystemResize(QQuickWindow *window, int edges);
    Q_INVOKABLE bool isMaximized(QQuickWindow *window) const;
    Q_INVOKABLE void toggleMaximize(QQuickWindow *window);
    Q_INVOKABLE void minimizeWindow(QQuickWindow *window);
    Q_INVOKABLE void closeWindow(QQuickWindow *window);
};

} // namespace Furui
