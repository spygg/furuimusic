#pragma once
// 福瑞音乐 - 主题与设计令牌
// 所有颜色、圆角、字号、间距只在这里定义，页面只引用 Theme 令牌，不写死色值。
#include <QColor>
#include <QObject>
#include <QString>
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
    // 强边界色：分隔线用 line 就够淡，但窗口外框、区块边界在纯白浅色底上看不见，
    // 这一档专门给「必须看得见的边界」用（比 line 深一档，仍不抢内容）
    Q_PROPERTY(QColor lineStrong READ lineStrong NOTIFY paletteChanged)
    Q_PROPERTY(QColor accent READ accent NOTIFY paletteChanged)
    Q_PROPERTY(QColor accentSoft READ accentSoft NOTIFY paletteChanged)
    Q_PROPERTY(QColor accentAlt READ accentAlt NOTIFY paletteChanged)
    // 歌词逐字高亮：红绿色弱对品牌绿 #22C55E 与灰色未唱部分分辨困难，
    // 单独用蓝轴色（浅色深蓝 / 深色亮蓝），红绿色盲也能靠蓝黄轴区分。
    Q_PROPERTY(QColor lyricHighlight READ lyricHighlight NOTIFY paletteChanged)
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
    // 统一动画时长：140ms 用于轻反馈（颜色/缩放），220ms 用于整块淡入淡出
    Q_PROPERTY(int durationFast READ durationFast CONSTANT)
    Q_PROPERTY(int durationBase READ durationBase CONSTANT)

    // 歌词字号缩放（百分比，75~175，100=跟随视口基准）。设置页可改并持久化；
    // 用独立信号，改字号不会连带重算所有与歌词无关的间距/圆角绑定。
    Q_PROPERTY(int lyricFontScale READ lyricFontScale WRITE setLyricFontScale
               NOTIFY lyricFontScaleChanged)

    // 播放页/歌词外观（设置页可改并持久化）
    // 关掉封面背景后播放页与移动端外壳统一用纯主题底色
    Q_PROPERTY(bool coverLyricBackground READ coverLyricBackground WRITE setCoverLyricBackground
               NOTIFY coverLyricBackgroundChanged)
    // 歌词对齐：auto=宽屏左/竖屏居中，left/center/right 强制对齐
    Q_PROPERTY(QString lyricAlign READ lyricAlign WRITE setLyricAlign NOTIFY lyricStyleChanged)
    // 译文相对整体字号再缩放一档（100=与原来一致）
    Q_PROPERTY(int lyricTransScale READ lyricTransScale WRITE setLyricTransScale NOTIFY lyricStyleChanged)
    // 歌词行间距（px）
    Q_PROPERTY(int lyricLineGap READ lyricLineGap WRITE setLyricLineGap NOTIFY lyricStyleChanged)

public:
    explicit ThemeManager(QObject *parent = nullptr);

    QString mode() const { return m_mode; }
    void setMode(const QString &mode);
    // 只改本次运行的外观、不写回设置：给截图/自检用（FURUI_THEME），
    // MV 全屏时也用它临时切深色让系统画深色状态栏。
    Q_INVOKABLE void previewMode(const QString &mode);

    bool isDark() const { return m_mode == QLatin1String("dark")
                              || (m_mode == QLatin1String("system") && m_systemDark); }
    bool systemDark() const { return m_systemDark; }
    bool systemThemeSupported() const;

    // 深色页面（MV 全屏）打开时强制状态栏用浅色图标，关闭时恢复跟随主题。
    // -1 = 跟随主题（默认）；0 = 强制深色图标；1 = 强制浅色图标。
    Q_PROPERTY(int statusBarOverride READ statusBarOverride WRITE setStatusBarOverride
               NOTIFY statusBarOverrideChanged)
    int statusBarOverride() const { return m_statusBarOverride; }
    void setStatusBarOverride(int override);

    // ---- 配色对齐 AlgerMusicPlayer（品牌绿 #22C55E + 白/黑干净的底）----
    QColor bg() const { return isDark() ? QColor("#0E0E10") : QColor("#FFFFFF"); }
    QColor surface() const { return isDark() ? QColor("#1B1B20") : QColor("#F7F8FA"); }
    QColor card() const { return isDark() ? QColor("#1A1A1E") : QColor("#FFFFFF"); }
    QColor cardHover() const { return isDark() ? QColor("#242429") : QColor("#F3F4F6"); }
    QColor text() const { return isDark() ? QColor("#F5F5F7") : QColor("#111827"); }
    // 次级/弱化文字都过 4.5:1：浅色 #6B7280，深色 #A1A1AA。
    // 旧值浅 #9CA3AF / 深 #71717A 在各自底色上只有约 2.8:1，小字几乎看不清。
    QColor subText() const { return isDark() ? QColor("#A1A1AA") : QColor("#6B7280"); }
    QColor mutedText() const { return isDark() ? QColor("#A1A1AA") : QColor("#6B7280"); }
    QColor line() const { return isDark() ? QColor("#2A2A2F") : QColor("#E8EAED"); }
    // 浅色下 #E8EAED 与纯白底几乎同色，无边框窗口/白底区块的边界会完全消失；
    // 这一档取 gray-300 / zinc-700，肉眼可见又不至于像分割线那么抢眼
    QColor lineStrong() const { return isDark() ? QColor("#3F3F46") : QColor("#D1D5DB"); }
    // Alger 品牌绿：浅深同色，弱化底用浅绿/深绿
    QColor accent() const { return QColor("#22C55E"); }
    QColor accentSoft() const { return isDark() ? QColor("#12301E") : QColor("#DCFCE7"); }
    QColor accentAlt() const { return isDark() ? QColor("#4ADE80") : QColor("#16A34A"); }
    // 蓝轴高亮：浅色 #1D4ED8 对白底约 6.7:1，深色 #93C5FD 对近黑底约 10:1
    QColor lyricHighlight() const { return isDark() ? QColor("#93C5FD") : QColor("#1D4ED8"); }
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
    int fontXs() const { return scaled(11); }
    int fontSm() const { return scaled(12); }
    int fontMd() const { return scaled(13); }
    int fontLg() const { return scaled(16); }
    int fontXl() const { return scaled(20); }
    int titleBarHeight() const { return scaled(44); }
    int durationFast() const { return 140; }
    int durationBase() const { return 220; }

    int lyricFontScale() const { return m_lyricFontScale; }
    void setLyricFontScale(int scale);

    bool coverLyricBackground() const { return m_coverLyricBackground; }
    void setCoverLyricBackground(bool on);
    QString lyricAlign() const { return m_lyricAlign; }
    void setLyricAlign(const QString &align);
    int lyricTransScale() const { return m_lyricTransScale; }
    void setLyricTransScale(int scale);
    int lyricLineGap() const { return m_lyricLineGap; }
    void setLyricLineGap(int gap);

signals:
    void modeChanged();
    void paletteChanged();
    void systemDarkChanged();
    void statusBarOverrideChanged();
    void metricsChanged();
    void lyricFontScaleChanged();
    void coverLyricBackgroundChanged();
    void lyricStyleChanged();

private:
    void refreshSystemDark();
    void updateWatch();
    static bool detectSystemDark();
    // Android：按当前明暗主题设置状态栏图标颜色（浅色主题用深色图标，深色主题用浅色图标），
    // 避免全面屏让出状态栏后白底白字看不见时间/信号。非 Android 平台空实现。
    void syncStatusBarIcons() const;
    int scaled(int base) const { return qRound(base * m_uiScale); }

    QString m_mode;
    bool m_systemDark = false;
    int m_statusBarOverride = -1;   // -1 跟随主题；深色页面（MV）打开时置 1
    QTimer m_watch;
    qreal m_uiScale = 1.0;
    int m_lyricFontScale = 100;
    bool m_coverLyricBackground = true;
    QString m_lyricAlign = QStringLiteral("auto");
    int m_lyricTransScale = 100;
    int m_lyricLineGap = 2;
};

} // namespace Furui
