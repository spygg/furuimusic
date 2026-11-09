#pragma once
// 福瑞音乐 - 主题与设计令牌
// 所有颜色、圆角、字号、间距只在这里定义，页面只引用 Theme 令牌，不写死色值。
#include <QColor>
#include <QObject>
#include <QTimer>

namespace Furui {

class ThemeManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(bool dark READ isDark NOTIFY paletteChanged)
    Q_PROPERTY(bool systemDark READ systemDark NOTIFY systemDarkChanged)
    Q_PROPERTY(bool systemThemeSupported READ systemThemeSupported CONSTANT)

    // 基础色
    Q_PROPERTY(QColor bg READ bg NOTIFY paletteChanged)
    Q_PROPERTY(QColor surface READ surface NOTIFY paletteChanged)
    Q_PROPERTY(QColor card READ card NOTIFY paletteChanged)
    Q_PROPERTY(QColor cardHover READ cardHover NOTIFY paletteChanged)
    Q_PROPERTY(QColor text READ text NOTIFY paletteChanged)
    Q_PROPERTY(QColor subText READ subText NOTIFY paletteChanged)
    Q_PROPERTY(QColor mutedText READ mutedText NOTIFY paletteChanged)
    Q_PROPERTY(QColor line READ line NOTIFY paletteChanged)
    Q_PROPERTY(QColor accent READ accent NOTIFY paletteChanged)
    Q_PROPERTY(QColor accentSoft READ accentSoft NOTIFY paletteChanged)
    Q_PROPERTY(QColor accentAlt READ accentAlt NOTIFY paletteChanged)
    Q_PROPERTY(QColor heroStart READ heroStart NOTIFY paletteChanged)
    Q_PROPERTY(QColor heroEnd READ heroEnd NOTIFY paletteChanged)
    Q_PROPERTY(QColor vip READ vip NOTIFY paletteChanged)
    Q_PROPERTY(QColor success READ success NOTIFY paletteChanged)
    Q_PROPERTY(QColor danger READ danger NOTIFY paletteChanged)

    // 形状与排版令牌
    Q_PROPERTY(int radiusXs READ radiusXs CONSTANT)
    Q_PROPERTY(int radiusSm READ radiusSm CONSTANT)
    Q_PROPERTY(int radiusMd READ radiusMd CONSTANT)
    Q_PROPERTY(int radiusLg READ radiusLg CONSTANT)
    Q_PROPERTY(int spaceXs READ spaceXs CONSTANT)
    Q_PROPERTY(int spaceSm READ spaceSm CONSTANT)
    Q_PROPERTY(int spaceMd READ spaceMd CONSTANT)
    Q_PROPERTY(int spaceLg READ spaceLg CONSTANT)
    Q_PROPERTY(int fontXs READ fontXs CONSTANT)
    Q_PROPERTY(int fontSm READ fontSm CONSTANT)
    Q_PROPERTY(int fontMd READ fontMd CONSTANT)
    Q_PROPERTY(int fontLg READ fontLg CONSTANT)
    Q_PROPERTY(int fontXl READ fontXl CONSTANT)
    Q_PROPERTY(int titleBarHeight READ titleBarHeight CONSTANT)

public:
    explicit ThemeManager(QObject *parent = nullptr);

    QString mode() const { return m_mode; }
    void setMode(const QString &mode);

    bool isDark() const { return m_mode == QLatin1String("dark")
                              || (m_mode == QLatin1String("system") && m_systemDark); }
    bool systemDark() const { return m_systemDark; }
    bool systemThemeSupported() const;

    QColor bg() const { return isDark() ? QColor("#121216") : QColor("#F4F6FA"); }
    QColor surface() const { return isDark() ? QColor("#1A1A20") : QColor("#FFFFFF"); }
    QColor card() const { return isDark() ? QColor("#212128") : QColor("#FFFFFF"); }
    QColor cardHover() const { return isDark() ? QColor("#2A2A33") : QColor("#F1F4F9"); }
    QColor text() const { return isDark() ? QColor("#ECECF1") : QColor("#1B2029"); }
    QColor subText() const { return isDark() ? QColor("#9A9AA6") : QColor("#6B7280"); }
    QColor mutedText() const { return isDark() ? QColor("#6E6E7A") : QColor("#9AA1AC"); }
    QColor line() const { return isDark() ? QColor("#2E2E38") : QColor("#E4E8EF"); }
    QColor accent() const { return isDark() ? QColor("#FF5B6E") : QColor("#EC4141"); }
    QColor accentSoft() const { return isDark() ? QColor("#3A2229") : QColor("#FDECEF"); }
    QColor accentAlt() const { return isDark() ? QColor("#FF8A5B") : QColor("#FF7A59"); }
    QColor heroStart() const { return isDark() ? QColor("#C22B44") : QColor("#E63B52"); }
    QColor heroEnd() const { return isDark() ? QColor("#E2603F") : QColor("#FF7A59"); }
    QColor vip() const { return isDark() ? QColor("#F0A23C") : QColor("#E8912A"); }
    QColor success() const { return isDark() ? QColor("#35D07F") : QColor("#2ECC71"); }
    QColor danger() const { return isDark() ? QColor("#FF6B6B") : QColor("#E5484D"); }

    int radiusXs() const { return 6; }
    int radiusSm() const { return 8; }
    int radiusMd() const { return 12; }
    int radiusLg() const { return 16; }
    int spaceXs() const { return 6; }
    int spaceSm() const { return 8; }
    int spaceMd() const { return 12; }
    int spaceLg() const { return 16; }
    int fontXs() const { return 10; }
    int fontSm() const { return 11; }
    int fontMd() const { return 13; }
    int fontLg() const { return 16; }
    int fontXl() const { return 20; }
    int titleBarHeight() const { return 44; }

signals:
    void modeChanged();
    void paletteChanged();
    void systemDarkChanged();

private:
    void refreshSystemDark();
    static bool detectSystemDark();

    QString m_mode;
    bool m_systemDark = false;
    QTimer m_watch;
};

} // namespace Furui
