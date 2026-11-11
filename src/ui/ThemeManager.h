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
    // 这些令牌不再是常量：车机/平板的屏幕形态（全屏、分屏、旋转）会改变窗口尺寸，
    // 字号与间距必须跟着视口走，否则 12.9 寸车机上就是手机字号放大版的小字。
    Q_PROPERTY(qreal uiScale READ uiScale NOTIFY metricsChanged)
    Q_PROPERTY(int touchMin READ touchMin NOTIFY metricsChanged)
    Q_PROPERTY(int radiusXs READ radiusXs NOTIFY metricsChanged)
    Q_PROPERTY(int radiusSm READ radiusSm NOTIFY metricsChanged)
    Q_PROPERTY(int radiusMd READ radiusMd NOTIFY metricsChanged)
    Q_PROPERTY(int radiusLg READ radiusLg NOTIFY metricsChanged)
    Q_PROPERTY(int spaceXs READ spaceXs NOTIFY metricsChanged)
    Q_PROPERTY(int spaceSm READ spaceSm NOTIFY metricsChanged)
    Q_PROPERTY(int spaceMd READ spaceMd NOTIFY metricsChanged)
    Q_PROPERTY(int spaceLg READ spaceLg NOTIFY metricsChanged)
    Q_PROPERTY(int fontXs READ fontXs NOTIFY metricsChanged)
    Q_PROPERTY(int fontSm READ fontSm NOTIFY metricsChanged)
    Q_PROPERTY(int fontMd READ fontMd NOTIFY metricsChanged)
    Q_PROPERTY(int fontLg READ fontLg NOTIFY metricsChanged)
    Q_PROPERTY(int fontXl READ fontXl NOTIFY metricsChanged)
    Q_PROPERTY(int titleBarHeight READ titleBarHeight NOTIFY metricsChanged)

public:
    explicit ThemeManager(QObject *parent = nullptr);

    QString mode() const { return m_mode; }
    void setMode(const QString &mode);
    // 只改本次运行的外观、不写回设置：给截图/自检用（FURUI_THEME）
    void previewMode(const QString &mode);

    bool isDark() const { return m_mode == QLatin1String("dark")
                              || (m_mode == QLatin1String("system") && m_systemDark); }
    bool systemDark() const { return m_systemDark; }
    bool systemThemeSupported() const;

    QColor bg() const { return isDark() ? QColor("#121216") : QColor("#F4F6FA"); }
    QColor surface() const { return isDark() ? QColor("#1A1A20") : QColor("#FFFFFF"); }
    QColor card() const { return isDark() ? QColor("#212128") : QColor("#FFFFFF"); }
    QColor cardHover() const { return isDark() ? QColor("#2A2A33") : QColor("#F1F4F9"); }
    QColor text() const { return isDark() ? QColor("#ECECF1") : QColor("#1B2029"); }
    // 次级/弱化文字要同时过 4.5:1：浅色主题原先 mutedText(#9AA1AC) 在卡片上只有
    // 2.60、深色 3.18，占位符几乎看不清；subText 也仅 4.83，压不出层次。
    // 整体下移一档，实测浅色 5.98/5.06、深色 5.75/4.56（卡片/背景都达标）。
    QColor subText() const { return isDark() ? QColor("#9A9AA6") : QColor("#5C6470"); }
    QColor mutedText() const { return isDark() ? QColor("#888892") : QColor("#686F7C"); }
    QColor line() const { return isDark() ? QColor("#2E2E38") : QColor("#E4E8EF"); }
    QColor accent() const { return isDark() ? QColor("#FF5B6E") : QColor("#EC4141"); }
    QColor accentSoft() const { return isDark() ? QColor("#3A2229") : QColor("#FDECEF"); }
    QColor accentAlt() const { return isDark() ? QColor("#FF8A5B") : QColor("#FF7A59"); }
    QColor heroStart() const { return isDark() ? QColor("#C22B44") : QColor("#E63B52"); }
    QColor heroEnd() const { return isDark() ? QColor("#E2603F") : QColor("#FF7A59"); }
    QColor vip() const { return isDark() ? QColor("#F0A23C") : QColor("#E8912A"); }
    QColor success() const { return isDark() ? QColor("#35D07F") : QColor("#2ECC71"); }
    QColor danger() const { return isDark() ? QColor("#FF6B6B") : QColor("#E5484D"); }

    // 视口驱动的缩放：width/height 用 QML 的逻辑像素（Android 上约等于 dp）。
    // adaptive=false（桌面）时恒为 1.0 —— 桌面缩放交给 Qt 的高 DPI 机制，
    // 再叠一层会把 1280x820 的普通窗口字撑到 1.24 倍。
    Q_INVOKABLE void setViewport(qreal width, qreal height, bool adaptive);
    qreal uiScale() const { return m_uiScale; }
    int touchMin() const { return qRound(48.0 * m_uiScale); }

    int radiusXs() const { return scaled(6); }
    int radiusSm() const { return scaled(8); }
    int radiusMd() const { return scaled(12); }
    int radiusLg() const { return scaled(16); }
    int spaceXs() const { return scaled(6); }
    int spaceSm() const { return scaled(8); }
    int spaceMd() const { return scaled(12); }
    int spaceLg() const { return scaled(16); }
    int fontXs() const { return scaled(10); }
    int fontSm() const { return scaled(11); }
    int fontMd() const { return scaled(13); }
    int fontLg() const { return scaled(16); }
    int fontXl() const { return scaled(20); }
    int titleBarHeight() const { return scaled(44); }

signals:
    void modeChanged();
    void paletteChanged();
    void systemDarkChanged();
    void metricsChanged();

private:
    void refreshSystemDark();
    void updateWatch();
    static bool detectSystemDark();
    int scaled(int base) const { return qRound(base * m_uiScale); }

    QString m_mode;
    bool m_systemDark = false;
    QTimer m_watch;
    qreal m_uiScale = 1.0;
};

} // namespace Furui
