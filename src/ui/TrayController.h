#pragma once
// 福瑞音乐 - 桌面端系统托盘
// 点标题栏 ✕ 时的行为由用户决定：最小化到托盘（可恢复）/ 直接退出 / 每次询问，
// 选择可记住（Settings 的 closeAction 键）。托盘菜单提供「显示主界面 / 退出」。
// Android / iOS 不编译：移动端没有系统托盘，关闭走各自的返回键逻辑。

#include <QObject>
#include <QPointer>

class QQuickWindow;
class QSystemTrayIcon;
class QMenu;

namespace Furui {

class TrayController : public QObject {
    Q_OBJECT
    // 系统托盘是否可用（桌面端创建成功即 true）；QML 据此决定拦不拦关闭事件
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
public:
    static TrayController *instance();

    bool available() const;

    // QML 关闭对话框 / 托盘菜单共用
    Q_INVOKABLE void attach(QObject *window);
    Q_INVOKABLE void hideWindow();
    Q_INVOKABLE void showWindow();
    Q_INVOKABLE void quitApp();

signals:
    void availableChanged();

private:
    explicit TrayController(QObject *parent = nullptr);
    void ensureTray();

    QPointer<QQuickWindow> m_window;
    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_menu = nullptr;
};

} // namespace Furui
