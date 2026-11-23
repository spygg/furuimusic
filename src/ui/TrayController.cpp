// 福瑞音乐 - 桌面端系统托盘（见 TrayController.h）
#include "TrayController.h"

#include <QAction>
#include <QCoreApplication>
#include <QIcon>
#include <QMenu>
#include <QQuickWindow>
#include <QSystemTrayIcon>

#include "../core/Logger.h"
#include "../core/Settings.h"

namespace Furui {

TrayController *TrayController::instance()
{
    static TrayController s;
    return &s;
}

TrayController::TrayController(QObject *parent)
    : QObject(parent)
{
}

bool TrayController::available() const
{
    return m_tray != nullptr && m_tray->isVisible()
           && QSystemTrayIcon::isSystemTrayAvailable();
}

void TrayController::ensureTray()
{
    if (m_tray) return;
    // 双保险：QSystemTrayIcon 本身要求 QApplication，且系统必须有托盘区
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;

    QIcon icon;
    icon.addFile(QStringLiteral(":/icons/app-64.png"));
    icon.addFile(QStringLiteral(":/icons/app-32.png"));

    m_menu = new QMenu();
    QAction *show = m_menu->addAction(tr("显示主界面"));
    QAction *quit = m_menu->addAction(tr("退出"));
    connect(show, &QAction::triggered, this, &TrayController::showWindow);
    connect(quit, &QAction::triggered, this, &TrayController::quitApp);

    m_tray = new QSystemTrayIcon(icon, this);
    m_tray->setToolTip(QCoreApplication::applicationName());
    m_tray->setContextMenu(m_menu);
    // 左键单击托盘图标 = 显示主界面（与主流播放器一致）
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
            showWindow();
    });
    m_tray->show();
    emit availableChanged();
    Logger::write(QStringLiteral("I"), QStringLiteral("系统托盘已就绪"));
}

void TrayController::attach(QObject *window)
{
    m_window = qobject_cast<QQuickWindow *>(window);
    ensureTray();
}

void TrayController::hideWindow()
{
    if (!m_window) return;
    m_window->hide();
}

void TrayController::showWindow()
{
    if (!m_window) return;
    // 从托盘恢复：最小化状态直接 restore，再抢前台焦点
    if (m_window->visibility() == QWindow::Minimized)
        m_window->showNormal();
    else
        m_window->show();
    m_window->raise();
    m_window->requestActivate();
}

void TrayController::quitApp()
{
    // 托盘「退出」绕过 QML 的 close 拦截，直接结束事件循环；
    // 设置落盘由 aboutToQuit 的统一 flush 负责
    QCoreApplication::quit();
}

} // namespace Furui
