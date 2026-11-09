#include "Theme.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QFont>
#include <QApplication>
#include <QScreen>
#include <QtMath>
#include <cmath>

namespace Furui {

namespace {

int g_revision = 0;
const Tokens *g_current = nullptr;

} // namespace

namespace Theme {

static qreal g_scale = 1.0;

const Tokens &tokens(const QString &key)
{
    static Tokens light = [] {
        Tokens t;
        t.key = QStringLiteral("light");
        t.windowTop = QColor(0xf7f7fa);
        t.windowBottom = QColor(0xeef0f5);
        t.panel = QColor(0xffffff);
        t.panelAlt = QColor(0xfafafc);
        t.text = QColor(0x17171b);
        t.subtext = QColor(0x6b6b75);
        t.faint = QColor(0xa8a8b2);
        t.accent = QColor(0xec4141);
        t.accentText = QColor(0xffffff);
        t.accentSoft = QColor(0xff5a5f);
        t.border = QColor(0xe4e4ec);
        t.hover = QColor(0xf2f2f7);
        t.pressed = QColor(0xe7e7ef);
        t.track = QColor(0xdedee7);
        t.knob = QColor(0xec4141);
        t.mask = QColor(0xe9e9f0);
        return t;
    }();
    static Tokens dark = [] {
        Tokens t;
        t.key = QStringLiteral("dark");
        t.windowTop = QColor(0x17171c);
        t.windowBottom = QColor(0x101014);
        t.panel = QColor(0x212129);
        t.panelAlt = QColor(0x1d1d24);
        t.text = QColor(0xececf2);
        t.subtext = QColor(0x9a9aa6);
        t.faint = QColor(0x6a6a76);
        t.accent = QColor(0xec4141);
        t.accentText = QColor(0xffffff);
        t.accentSoft = QColor(0xff6b6f);
        t.border = QColor(0x30303a);
        t.hover = QColor(0x2b2b34);
        t.pressed = QColor(0x35353f);
        t.track = QColor(0x3a3a45);
        t.knob = QColor(0xff5a5f);
        t.mask = QColor(0x2a2a33);
        return t;
    }();
    return key == QStringLiteral("dark") ? dark : light;
}

const Tokens &apply(const QString &key)
{
    const Tokens &t = tokens(key);

    // 字体随系统 DPI：QSS 里的 px 在 Qt5 高 DPI 缩放下不会被缩放，
    // 所以按 logicalDpi/96 折算一次，保证 150%/200% 下文字与控件同步放大。
    qreal scale = 1.0;
    if (QGuiApplication::primaryScreen())
        scale = qMax(0.85, QGuiApplication::primaryScreen()->logicalDotsPerInch() / 96.0);
    g_scale = scale;

    qApp->setStyleSheet(styleSheet(t, scale));

    QPalette pal;
    pal.setColor(QPalette::Window, t.windowTop);
    pal.setColor(QPalette::Base, t.panel);
    pal.setColor(QPalette::AlternateBase, t.panelAlt);
    pal.setColor(QPalette::Text, t.text);
    pal.setColor(QPalette::WindowText, t.text);
    pal.setColor(QPalette::Button, t.panel);
    pal.setColor(QPalette::ButtonText, t.text);
    pal.setColor(QPalette::Highlight, t.accent);
    pal.setColor(QPalette::HighlightedText, t.accentText);
    pal.setColor(QPalette::ToolTipBase, t.panel);
    pal.setColor(QPalette::ToolTipText, t.text);
    pal.setColor(QPalette::PlaceholderText, t.faint);
    pal.setColor(QPalette::Link, t.accent);
    qApp->setPalette(pal);

    QFont f = qApp->font();
    f.setPointSizeF(10.5 * scale);
    f.setHintingPreference(QFont::PreferFullHinting);
    qApp->setFont(f);

    ++g_revision;
    g_current = &t;
    return t;
}

const Tokens &current()
{
    return g_current ? *g_current : tokens(QStringLiteral("light"));
}
int revision() { return g_revision; }

qreal pxScale() { return g_scale; }

} // namespace Theme

namespace Icons {

namespace {

// 所有图标在 24x24 单位坐标里描述，绘制前统一缩放到目标尺寸
void draw(Kind kind, QPainter &p, qreal w)
{
    const QColor col = p.pen().color();
    QPen pen(col);
    pen.setWidthF(w * 0.075);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    const qreal s = 24.0;
    switch (kind) {
    case Play: {
        QPainterPath path;
        path.moveTo(s * 0.32, s * 0.22);
        path.lineTo(s * 0.32, s * 0.78);
        path.lineTo(s * 0.80, s * 0.50);
        path.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawPath(path);
        break;
    }
    case Pause: {
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        const qreal bw = s * 0.13, gap = s * 0.16;
        p.drawRoundedRect(QRectF(s * 0.28, s * 0.22, bw, s * 0.56), bw * 0.35, bw * 0.35);
        p.drawRoundedRect(QRectF(s * 0.28 + bw + gap, s * 0.22, bw, s * 0.56), bw * 0.35, bw * 0.35);
        break;
    }
    case Prev:
    case Next: {
        const bool prev = kind == Prev;
        QPainterPath tri;
        if (prev) {
            tri.moveTo(s * 0.72, s * 0.26);
            tri.lineTo(s * 0.72, s * 0.74);
            tri.lineTo(s * 0.38, s * 0.50);
        } else {
            tri.moveTo(s * 0.28, s * 0.26);
            tri.lineTo(s * 0.28, s * 0.74);
            tri.lineTo(s * 0.62, s * 0.50);
        }
        tri.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawPath(tri);
        p.setPen(pen);
        const qreal bx = prev ? s * 0.76 : s * 0.21;
        p.drawLine(QPointF(bx, s * 0.28), QPointF(bx, s * 0.72));
        break;
    }
    case Search: {
        p.drawEllipse(QRectF(s * 0.24, s * 0.22, s * 0.40, s * 0.40));
        p.drawLine(QPointF(s * 0.60, s * 0.58), QPointF(s * 0.78, s * 0.76));
        break;
    }
    case Rank: {
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        const qreal bw = s * 0.13;
        p.drawRoundedRect(QRectF(s * 0.22, s * 0.48, bw, s * 0.30), bw * 0.3, bw * 0.3);
        p.drawRoundedRect(QRectF(s * 0.44, s * 0.26, bw, s * 0.52), bw * 0.3, bw * 0.3);
        p.drawRoundedRect(QRectF(s * 0.66, s * 0.58, bw, s * 0.20), bw * 0.3, bw * 0.3);
        break;
    }
    case Playlist:
    case Queue: {
        for (int i = 0; i < 3; ++i) {
            const qreal y = s * (0.32 + i * 0.18);
            p.drawLine(QPointF(s * 0.22, y), QPointF(s * (i == 2 ? 0.55 : 0.78), y));
        }
        if (kind == Queue) break;
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        QPainterPath note;
        note.addEllipse(QRectF(s * 0.62, s * 0.58, s * 0.14, s * 0.14));
        p.drawPath(note);
        p.setPen(pen);
        p.drawLine(QPointF(s * 0.755, s * 0.62), QPointF(s * 0.755, s * 0.34));
        break;
    }
    case Heart:
    case HeartFilled: {
        QPainterPath h;
        h.moveTo(s * 0.50, s * 0.76);
        h.cubicTo(s * 0.16, s * 0.54, s * 0.20, s * 0.24, s * 0.40, s * 0.26);
        h.cubicTo(s * 0.47, s * 0.27, s * 0.50, s * 0.33, s * 0.50, s * 0.33);
        h.cubicTo(s * 0.50, s * 0.33, s * 0.53, s * 0.27, s * 0.60, s * 0.26);
        h.cubicTo(s * 0.80, s * 0.24, s * 0.84, s * 0.54, s * 0.50, s * 0.76);
        h.closeSubpath();
        if (kind == HeartFilled) {
            p.setPen(Qt::NoPen);
            p.setBrush(col);
        }
        p.drawPath(h);
        break;
    }
    case History: {
        p.drawEllipse(QRectF(s * 0.22, s * 0.22, s * 0.50, s * 0.50));
        p.drawLine(QPointF(s * 0.47, s * 0.33), QPointF(s * 0.47, s * 0.48));
        p.drawLine(QPointF(s * 0.47, s * 0.48), QPointF(s * 0.58, s * 0.53));
        p.setPen(pen);
        p.drawLine(QPointF(s * 0.72, s * 0.40), QPointF(s * 0.80, s * 0.34));
        break;
    }
    case Account: {
        p.drawEllipse(QRectF(s * 0.36, s * 0.22, s * 0.28, s * 0.28));
        QPainterPath body;
        body.moveTo(s * 0.24, s * 0.78);
        body.cubicTo(s * 0.24, s * 0.58, s * 0.76, s * 0.58, s * 0.76, s * 0.78);
        p.drawPath(body);
        break;
    }
    case Settings: {
        p.drawEllipse(QRectF(s * 0.36, s * 0.36, s * 0.28, s * 0.28));
        for (int i = 0; i < 8; ++i) {
            const qreal a = i * M_PI / 4.0;
            const QPointF c(s * 0.50, s * 0.50);
            p.drawLine(c + QPointF(std::cos(a) * s * 0.30, std::sin(a) * s * 0.30),
                       c + QPointF(std::cos(a) * s * 0.40, std::sin(a) * s * 0.40));
        }
        break;
    }
    case Volume:
    case Mute: {
        QPainterPath sp;
        sp.moveTo(s * 0.26, s * 0.42);
        sp.lineTo(s * 0.40, s * 0.42);
        sp.lineTo(s * 0.54, s * 0.30);
        sp.lineTo(s * 0.54, s * 0.70);
        sp.lineTo(s * 0.40, s * 0.58);
        sp.lineTo(s * 0.26, s * 0.58);
        sp.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawPath(sp);
        p.setPen(pen);
        if (kind == Volume) {
            p.drawArc(QRectF(s * 0.54, s * 0.34, s * 0.22, s * 0.32), -60 * 16, 120 * 16);
            p.drawArc(QRectF(s * 0.54, s * 0.26, s * 0.36, s * 0.48), -50 * 16, 100 * 16);
        } else {
            p.drawLine(QPointF(s * 0.66, s * 0.42), QPointF(s * 0.78, s * 0.58));
            p.drawLine(QPointF(s * 0.78, s * 0.42), QPointF(s * 0.66, s * 0.58));
        }
        break;
    }
    case Lyric: {
        p.drawRoundedRect(QRectF(s * 0.22, s * 0.26, s * 0.56, s * 0.48), s * 0.08, s * 0.08);
        p.drawLine(QPointF(s * 0.32, s * 0.42), QPointF(s * 0.68, s * 0.42));
        p.drawLine(QPointF(s * 0.32, s * 0.54), QPointF(s * 0.56, s * 0.54));
        break;
    }
    case Repeat:
    case RepeatOne: {
        p.drawRoundedRect(QRectF(s * 0.22, s * 0.32, s * 0.56, s * 0.36), s * 0.18, s * 0.18);
        QPainterPath arrow;
        arrow.moveTo(s * 0.60, s * 0.26);
        arrow.lineTo(s * 0.74, s * 0.34);
        arrow.lineTo(s * 0.60, s * 0.42);
        arrow.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawPath(arrow);
        if (kind == RepeatOne) {
            p.setPen(pen);
            p.drawLine(QPointF(s * 0.48, s * 0.44), QPointF(s * 0.52, s * 0.40));
            p.drawLine(QPointF(s * 0.52, s * 0.40), QPointF(s * 0.52, s * 0.60));
        }
        break;
    }
    case Shuffle: {
        p.drawLine(QPointF(s * 0.24, s * 0.68), QPointF(s * 0.52, s * 0.36));
        p.drawLine(QPointF(s * 0.24, s * 0.36), QPointF(s * 0.52, s * 0.68));
        p.drawLine(QPointF(s * 0.60, s * 0.36), QPointF(s * 0.78, s * 0.36));
        p.drawLine(QPointF(s * 0.60, s * 0.68), QPointF(s * 0.78, s * 0.68));
        break;
    }
    case Close: {
        p.drawLine(QPointF(s * 0.30, s * 0.30), QPointF(s * 0.70, s * 0.70));
        p.drawLine(QPointF(s * 0.70, s * 0.30), QPointF(s * 0.30, s * 0.70));
        break;
    }
    case Download: {
        p.drawLine(QPointF(s * 0.50, s * 0.24), QPointF(s * 0.50, s * 0.60));
        p.drawLine(QPointF(s * 0.36, s * 0.48), QPointF(s * 0.50, s * 0.62));
        p.drawLine(QPointF(s * 0.64, s * 0.48), QPointF(s * 0.50, s * 0.62));
        p.drawLine(QPointF(s * 0.26, s * 0.76), QPointF(s * 0.74, s * 0.76));
        break;
    }
    case Plus: {
        p.drawLine(QPointF(s * 0.50, s * 0.26), QPointF(s * 0.50, s * 0.74));
        p.drawLine(QPointF(s * 0.26, s * 0.50), QPointF(s * 0.74, s * 0.50));
        break;
    }
    case Trash: {
        p.drawRoundedRect(QRectF(s * 0.30, s * 0.34, s * 0.40, s * 0.44), s * 0.06, s * 0.06);
        p.drawLine(QPointF(s * 0.22, s * 0.34), QPointF(s * 0.78, s * 0.34));
        p.drawLine(QPointF(s * 0.42, s * 0.26), QPointF(s * 0.58, s * 0.26));
        break;
    }
    case Check: {
        p.drawLine(QPointF(s * 0.26, s * 0.52), QPointF(s * 0.44, s * 0.70));
        p.drawLine(QPointF(s * 0.44, s * 0.70), QPointF(s * 0.76, s * 0.32));
        break;
    }
    case Refresh: {
        p.drawArc(QRectF(s * 0.26, s * 0.26, s * 0.48, s * 0.48), 40 * 16, 280 * 16);
        QPainterPath a;
        a.moveTo(s * 0.68, s * 0.22);
        a.lineTo(s * 0.80, s * 0.34);
        a.lineTo(s * 0.64, s * 0.38);
        a.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawPath(a);
        break;
    }
    case Login: {
        p.drawRoundedRect(QRectF(s * 0.26, s * 0.24, s * 0.36, s * 0.52), s * 0.08, s * 0.08);
        p.drawLine(QPointF(s * 0.56, s * 0.50), QPointF(s * 0.78, s * 0.50));
        p.drawLine(QPointF(s * 0.70, s * 0.42), QPointF(s * 0.78, s * 0.50));
        p.drawLine(QPointF(s * 0.70, s * 0.58), QPointF(s * 0.78, s * 0.50));
        break;
    }
    case Chevron: {
        p.drawLine(QPointF(s * 0.40, s * 0.34), QPointF(s * 0.58, s * 0.50));
        p.drawLine(QPointF(s * 0.58, s * 0.50), QPointF(s * 0.40, s * 0.66));
        break;
    }
    case Cover: {
        p.drawRoundedRect(QRectF(s * 0.22, s * 0.22, s * 0.56, s * 0.56), s * 0.10, s * 0.10);
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        QPainterPath note;
        note.addEllipse(QRectF(s * 0.38, s * 0.56, s * 0.13, s * 0.13));
        p.drawPath(note);
        p.setPen(pen);
        p.drawLine(QPointF(s * 0.505, s * 0.60), QPointF(s * 0.505, s * 0.36));
        p.drawLine(QPointF(s * 0.505, s * 0.36), QPointF(s * 0.64, s * 0.32));
        break;
    }
    }
}

} // namespace

QPixmap pixmap(Kind kind, const QColor &color, int size, qreal devicePixelRatio, qreal weight)
{
    Q_UNUSED(weight);
    const qreal dpr = qMax(1.0, devicePixelRatio);
    QPixmap pm(int(size * dpr), int(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    // 24 单位设计网格居中缩放进图标盒，笔画宽度随尺寸与 DPR 一起放大，高 DPI 下不再被裁
    p.translate(size / 2.0, size / 2.0);
    p.scale(size / 22.0, size / 22.0);
    p.translate(-12.0, -12.0);
    p.setPen(QPen(color));
    draw(kind, p, 24.0);
    p.end();
    return pm;
}

QIcon icon(Kind kind, const QColor &color, int size, qreal weight)
{
    QIcon ic;
    const qreal dpr = QGuiApplication::primaryScreen()
        ? QGuiApplication::primaryScreen()->devicePixelRatio() : 1.0;
    ic.addPixmap(pixmap(kind, color, size, dpr, weight));
    return ic;
}

QIcon toggle(Kind on, Kind off, const QColor &normal, const QColor &active, int size)
{
    const qreal dpr = QGuiApplication::primaryScreen()
        ? QGuiApplication::primaryScreen()->devicePixelRatio() : 1.0;
    QIcon ic;
    ic.addPixmap(pixmap(off, normal, size, dpr), QIcon::Normal, QIcon::Off);
    ic.addPixmap(pixmap(on, active, size, dpr), QIcon::Normal, QIcon::On);
    ic.addPixmap(pixmap(off, normal, size, dpr), QIcon::Active, QIcon::Off);
    ic.addPixmap(pixmap(on, active, size, dpr), QIcon::Active, QIcon::On);
    return ic;
}

} // namespace Icons

} // namespace Furui
