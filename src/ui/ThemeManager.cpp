#include "ThemeManager.h"

#include "../core/Settings.h"

#include <QProcess>
#include <QStandardPaths>

namespace Furui {

ThemeManager::ThemeManager(QObject *parent) : QObject(parent)
{
    // 旧版本的 theme=light/dark 直接迁移为新的主题模式，避免升级后主题丢失
    m_mode = Settings::string(QStringLiteral("themeMode"));
    if (m_mode.isEmpty()) {
        const QString legacy = Settings::string(QStringLiteral("theme"));
        m_mode = legacy == QLatin1String("dark") ? QStringLiteral("dark")
               : legacy == QLatin1String("light") ? QStringLiteral("light")
                                                  : QStringLiteral("system");
        Settings::set(QStringLiteral("themeMode"), m_mode);
    }
    if (m_mode != QLatin1String("light") && m_mode != QLatin1String("dark")
        && m_mode != QLatin1String("system"))
        m_mode = QStringLiteral("system");

    m_systemDark = detectSystemDark();

    // 跟随系统时周期性复核系统外观；手动指定时不产生任何系统调用
    m_watch.setInterval(4000);
    connect(&m_watch, &QTimer::timeout, this, &ThemeManager::refreshSystemDark);
    if (m_mode == QLatin1String("system"))
        m_watch.start();
}

void ThemeManager::setMode(const QString &mode)
{
    const QString normalized = (mode == QLatin1String("light") || mode == QLatin1String("dark"))
                                   ? mode : QStringLiteral("system");
    if (normalized == m_mode) {
        // 同名模式再次点击时仍然刷新一次系统外观，避免手动触发无效
        if (normalized == QLatin1String("system"))
            refreshSystemDark();
        return;
    }

    const bool wasDark = isDark();
    m_mode = normalized;
    Settings::set(QStringLiteral("themeMode"), m_mode);
    Settings::set(QStringLiteral("theme"), isDark() ? QStringLiteral("dark") : QStringLiteral("light"));
    if (m_mode == QLatin1String("system"))
        refreshSystemDark();
    else
        m_watch.stop();
    emit modeChanged();
    if (wasDark != isDark())
        emit paletteChanged();
}

bool ThemeManager::systemThemeSupported() const
{
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    return false;
#elif defined(Q_OS_WIN) || defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    return true;
#else
    // Linux 桌面环境差异大：只有找到 gsettings 才认为可以跟随系统
    return !QStandardPaths::findExecutable(QStringLiteral("gsettings")).isEmpty();
#endif
}

void ThemeManager::refreshSystemDark()
{
    const bool dark = detectSystemDark();
    if (dark == m_systemDark)
        return;
    m_systemDark = dark;
    emit systemDarkChanged();
    if (m_mode == QLatin1String("system"))
        emit paletteChanged();
}

bool ThemeManager::detectSystemDark()
{
#if defined(Q_OS_WIN)
    QSettings personalize(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
        QSettings::NativeFormat);
    // AppsUseLightTheme: 0=深色 1=浅色；读不到时按浅色处理
    return personalize.value(QStringLiteral("AppsUseLightTheme"), 1).toInt() == 0;
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    QProcess p;
    p.start(QStringLiteral("defaults"), {QStringLiteral("read"), QStringLiteral("-g"),
                                        QStringLiteral("AppleInterfaceStyle")});
    if (!p.waitForFinished(400))
        return false;
    return QString::fromUtf8(p.readAllStandardOutput()).contains(QStringLiteral("Dark"));
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    // GNOME 优先读 color-scheme，其它桌面回退到 GTK 主题名
    QProcess gs;
    gs.start(QStringLiteral("gsettings"),
             {QStringLiteral("get"), QStringLiteral("org.gnome.desktop.interface"),
              QStringLiteral("color-scheme")});
    if (gs.waitForFinished(400)) {
        const QString scheme = QString::fromUtf8(gs.readAllStandardOutput()).trimmed();
        if (scheme.contains(QStringLiteral("dark")))
            return true;
        if (scheme.contains(QStringLiteral("light")))
            return false;
    }
    const QString gtkTheme = qEnvironmentVariable("GTK_THEME").toLower();
    if (!gtkTheme.isEmpty())
        return gtkTheme.contains(QStringLiteral("dark"));

    QProcess gt;
    gt.start(QStringLiteral("gsettings"),
             {QStringLiteral("get"), QStringLiteral("org.gnome.desktop.interface"),
              QStringLiteral("gtk-theme")});
    if (gt.waitForFinished(400))
        return QString::fromUtf8(gt.readAllStandardOutput()).toLower().contains(QStringLiteral("dark"));
    return false;
#else
    // Android/iOS（Qt 5.15）没有跨端系统外观接口，跟随系统时按浅色处理
    return false;
#endif
}

} // namespace Furui
