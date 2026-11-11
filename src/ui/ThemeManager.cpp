#include "ThemeManager.h"

#include "../core/Settings.h"

#include <QGuiApplication>
#include <QtMath>
#include <QProcess>
#include <QStandardPaths>
#include <QStyleHints>

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

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    // Qt 6.5+ 的 QStyleHints 直接给出系统配色（Windows / macOS / Linux / Android / iOS），
    // 有它就不必再轮询，系统切换深色时这里会立即收到通知。
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
            this, &ThemeManager::refreshSystemDark);
#endif

    // 跟随系统时周期性复核系统外观（仅在拿不到 colorScheme 的平台上需要）
    m_watch.setInterval(4000);
    connect(&m_watch, &QTimer::timeout, this, &ThemeManager::refreshSystemDark);
    updateWatch();
}

void ThemeManager::updateWatch()
{
    const bool follow = m_mode == QLatin1String("system");
    bool poll = follow;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    // colorScheme 已知时由 colorSchemeChanged 驱动，不再每 4 秒做一次系统调用
    poll = follow && QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Unknown;
#endif
    if (poll) m_watch.start();
    else m_watch.stop();
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
    updateWatch();
    emit modeChanged();
    if (wasDark != isDark())
        emit paletteChanged();
}

bool ThemeManager::systemThemeSupported() const
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (QGuiApplication::styleHints()->colorScheme() != Qt::ColorScheme::Unknown)
        return true;
#endif
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    return false;
#elif defined(Q_OS_WIN) || defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    return true;
#else
    // Linux 桌面环境差异大：只有找到 gsettings 才认为可以跟随系统
    return !QStandardPaths::findExecutable(QStringLiteral("gsettings")).isEmpty();
#endif
}

void ThemeManager::previewMode(const QString &mode)
{
    const bool wasDark = isDark();
    if (mode == QLatin1String("light") || mode == QLatin1String("dark")) {
        m_mode = mode;
    } else {
        m_mode = QStringLiteral("system");
        refreshSystemDark();
    }
    updateWatch();
    emit modeChanged();
    if (wasDark != isDark())
        emit paletteChanged();
}

// 车机/平板/高分屏的视口缩放。
// 缩放只按「逻辑像素（dp）对角线」算，不用物理分辨率：Qt 已经把 density 折算进
// 窗口尺寸，再乘一次会让高分屏车机字号失控。
// 四条硬约束：
//   1) 只放大不缩小，最小 1.0 —— 手机与既有截图基线不受影响；
//   2) 1700dp 以下不放大 —— 常规 1280x800 及以下的窗口必须与改造前逐像素一致；
//   3) 上限 1.15 —— 再大就要动写死的行高与胶囊宽度，会溢出；
//   4) 分屏时不放大 —— 横屏分屏高度常只有 400dp，字撑大就把内容挤没了。
void ThemeManager::setViewport(qreal width, qreal height, bool adaptive)
{
    qreal target = 1.0;
    if (adaptive && width > 1.0 && height > 1.0) {
        const qreal diag = qSqrt(width * width + height * height);
        if (height >= 520.0 && width >= 560.0)
            target = qBound(1.0, 1.0 + (diag - 1700.0) / 3000.0, 1.15);
    }
    if (qAbs(target - m_uiScale) < 0.01)
        return;
    m_uiScale = target;
    emit metricsChanged();
}

void ThemeManager::refreshSystemDark()
{
    const bool dark = detectSystemDark();
    if (dark == m_systemDark)
        return;    m_systemDark = dark;
    emit systemDarkChanged();
    if (m_mode == QLatin1String("system"))
        emit paletteChanged();
}

bool ThemeManager::detectSystemDark()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const Qt::ColorScheme scheme = QGuiApplication::styleHints()->colorScheme();
    if (scheme == Qt::ColorScheme::Dark) return true;
    if (scheme == Qt::ColorScheme::Light) return false;
#endif
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
    // Android/iOS 在 Qt 5.15 没有跨端系统外观接口，跟随系统时按浅色处理
    return false;
#endif
}

} // namespace Furui
