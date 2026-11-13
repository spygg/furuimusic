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
    // 投影基色：浅色主题用带一点冷调的深灰（黑投影在冷灰背景上发闷），
    // 深色主题投影几乎不可见，用纯黑低透明度即可
    Q_PROPERTY(QColor shadowTint READ shadowTint NOTIFY paletteChanged)

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

    // ---- 配色对齐 AlgerMusicPlayer（品牌绿 #22C55E + 白/黑干净的底）----
    QColor bg() const { return isDark() ? QColor("#0E0E10") : QColor("#FFFFFF"); }
    QColor surface() const { return isDark() ? QColor("#17171A") : QColor("#FFFFFF"); }
    QColor card() const { return isDark() ? QColor("#1A1A1E") : QColor("#FFFFFF"); }
    QColor cardHover() const { return isDark() ? QColor("#242429") : QColor("#F3F4F6"); }
    QColor text() const { return isDark() ? QColor("#F5F5F7") : QColor("#111827"); }
    // 次级/弱化文字仍然过 4.5:1：浅色 #6B7280 / #9CA3AF，深色 #A1A1AA / #71717A
    QColor subText() const { return isDark() ? QColor("#A1A1AA") : QColor("#6B7280"); }
    QColor mutedText() const { return isDark() ? QColor("#71717A") : QColor("#9CA3AF"); }
    QColor line() const { return isDark() ? QColor("#2A2A2F") : QColor("#E8EAED"); }
    // Alger 品牌绿：浅深同色，弱化底用浅绿/深绿
    QColor accent() const { return QColor("#22C55E"); }
    QColor accentSoft() const { return isDark() ? QColor("#12301E") : QColor("#DCFCE7"); }
    QColor accentAlt() const { return isDark() ? QColor("#4ADE80") : QColor("#16A34A"); }
    QColor heroStart() const { return QColor("#22C55E"); }
    QColor heroEnd() const { return isDark() ? QColor("#15803D") : QColor("#16A34A"); }
    QColor vip() const { return isDark() ? QColor("#F0A23C") : QColor("#E8912A"); }
    QColor success() const { return isDark() ? QColor("#4ADE80") : QColor("#22C55E"); }
    QColor danger() const { return isDark() ? QColor("#F87171") : QColor("#EF4444"); }
    QColor shadowTint() const { return isDark() ? QColor("#000000") : QColor("#94A3B8"); }

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
