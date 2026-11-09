#pragma once

#include <QColor>
#include <QIcon>
#include <QString>
#include <QPixmap>

class QPainter;

namespace Furui {

// 主题令牌：所有颜色/圆角/字号只在这里定义一次，QSS 由令牌拼装，
// 避免样式散落在各控件的 setStyleSheet 里改一处漏一处。
struct Tokens {
    QString key;
    QColor windowTop, windowBottom;
    QColor panel;        // 卡片/列表底
    QColor panelAlt;     // 交替行
    QColor text, subtext, faint;
    QColor accent, accentText, accentSoft;
    QColor border, hover, pressed;
    QColor track, knob;
    QColor mask;         // 封面占位遮罩
    int radius = 12;
    // 字号按逻辑像素给，缩放系数变化时由 apply() 统一换算
    int fontBase = 13;
    int fontSmall = 12;
    int fontTitle = 15;
};

namespace Theme {

const Tokens &tokens(const QString &key);
QString styleSheet(const Tokens &t, qreal fontScale = 1.0);
// 应用主题：设置全局 QSS + QPalette（QPalette 影响原生绘制的菜单/提示框），
// 返回值供其它控件取当前令牌画自定义内容。
const Tokens &apply(const QString &key);
const Tokens &current();
// 主题切换时递增，自定义绘制控件据此重取颜色
int revision();

// QSS 里 px 的放大系数，C++ 侧写死的尺寸要用它对齐，否则圆角会超过半边长而失效
qreal pxScale();

} // namespace Theme

// 矢量图标：全部用 QPainterPath 在 24x24 单位坐标里画，按请求尺寸与 DPR 生成 pixmap，
// 这样高分辨率与任意缩放都不会糊（项目里没有位图图标资源，也不打算引入）。
namespace Icons {

enum Kind {
    Search, Rank, Playlist, Heart, HeartFilled, History, Account, Settings,
    Play, Pause, Prev, Next, Volume, Mute, Lyric,
    Repeat, RepeatOne, Shuffle, Queue, Close, Download, Plus, Trash, Check,
    Refresh, Login, Chevron, Cover
};

QPixmap pixmap(Kind kind, const QColor &color, int size, qreal devicePixelRatio = 1.0,
               qreal weight = 1.7);
QIcon icon(Kind kind, const QColor &color, int size, qreal weight = 1.7);
// 同一图标的一组状态（常态/强调态），给可切换按钮用
QIcon toggle(Kind on, Kind off, const QColor &normal, const QColor &active, int size);

} // namespace Icons

} // namespace Furui
