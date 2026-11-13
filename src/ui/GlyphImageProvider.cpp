#include "GlyphImageProvider.h"

#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPointF>
#include <QRectF>
#include <QStringList>
#include <QtGlobal>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <cmath>

namespace Furui {

namespace {

constexpr qreal kPi = 3.14159265358979323846;

} // namespace

GlyphImageProvider::GlyphImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image)
{
}

QImage GlyphImageProvider::requestImage(const QString &id, QSize *size,
                                        const QSize &requestedSize)
{
    const QStringList parts = id.split(QLatin1Char('/'));
    const QString name = parts.value(0, QStringLiteral("music"));

    int logical = parts.value(1).toInt();
    if (logical <= 0)
        logical = 18;

    QColor color(QStringLiteral("#") + parts.value(2));
    if (!color.isValid())
        color = QColor(QStringLiteral("#1B2029"));

    // QML 侧传 size*2 的 sourceSize：1x 屏上缩小显示更锐利，2x 屏上正好是物理像素。
    int pixels = logical * 2;
    if (requestedSize.isValid() && requestedSize.width() > 0)
        pixels = qMax(requestedSize.width(), requestedSize.height());
    pixels = qBound(8, pixels, 512);

    // 同一个图标在列表里会被反复请求（滚动、hover 变色、主题切换），
    // 栅格化要走 QPainterPathStroker，是这条路径上唯一的重活，所以按参数缓存。
    const QString key = QStringLiteral("%1|%2|%3")
                            .arg(name).arg(color.rgba()).arg(pixels);
    {
        QMutexLocker lock(&cacheMutex());
        auto it = cache().constFind(key);
        if (it != cache().constEnd()) {
            if (size) *size = it->size();
            return *it;
        }
    }

    QImage image = render(name, color, pixels);
    {
        QMutexLocker lock(&cacheMutex());
        QHash<QString, QImage> &c = cache();
        // 图标种类 × 尺寸 × 配色本来是有限的，但颜色动画会不断产生新键，必须有上限
        if (c.size() >= 4096)
            c.clear();
        c.insert(key, image);
    }
    if (size)
        *size = image.size();
    return image;
}

QHash<QString, QImage> &GlyphImageProvider::cache()
{
    static QHash<QString, QImage> c;
    return c;
}

QMutex &GlyphImageProvider::cacheMutex()
{
    static QMutex m;
    return m;
}

QImage GlyphImageProvider::render(const QString &name, const QColor &color, int pixels)
{
    // Qt 6 Android 的 QQuickTextureFactory 会把 QImage 直接当 RGBA 纹理上传：预乘格式
    // 配合 QPen 描边时，部分 GLES 驱动只上传 brush 填充区域、丢掉描边，图标就退化成
    // 一堆黑色方块（桌面端正常，Android 端乱码）。这里统一改成：
    //   1) 非预乘画布，最后转 RGBA8888（Qt Quick 最标准的纹理格式）；
    //   2) 全程 NoPen，描边先用 QPainterPathStroker 转成轮廓再填充；
    //   3) 不再依赖运行时的 pen/brush 状态切换，各平台像素结果一致。
    QImage image(qMax(8, pixels), qMax(8, pixels), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    if (color.alpha() == 0)
        return image.convertToFormat(QImage::Format_RGBA8888);

    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::NoBrush);

    const qreal w = image.width();
    const qreal h = image.height();
    const qreal strokeW = qMax(1.2, w * 0.085);

    auto fill = [&](const QPainterPath &path) {
        p.setBrush(color);
        p.drawPath(path);
        p.setBrush(Qt::NoBrush);
    };
    auto strokeWith = [&](const QPainterPath &path, qreal width) {
        QPainterPathStroker stroker;
        stroker.setWidth(width);
        stroker.setCapStyle(Qt::RoundCap);
        stroker.setJoinStyle(Qt::RoundJoin);
        fill(stroker.createStroke(path));
    };
    auto stroke = [&](const QPainterPath &path) { strokeWith(path, strokeW); };

    auto circle = [&](qreal cx, qreal cy, qreal r, bool filled) {
        const QRectF rc((cx - r) * w, (cy - r) * w, 2 * r * w, 2 * r * w);
        QPainterPath path;
        path.addEllipse(rc);
        if (filled) fill(path); else stroke(path);
    };
    auto line = [&](qreal x1, qreal y1, qreal x2, qreal y2) {
        QPainterPath path;
        path.moveTo(x1 * w, y1 * h);
        path.lineTo(x2 * w, y2 * h);
        stroke(path);
    };
    auto lineWith = [&](qreal x1, qreal y1, qreal x2, qreal y2, qreal width) {
        QPainterPath path;
        path.moveTo(x1 * w, y1 * h);
        path.lineTo(x2 * w, y2 * h);
        strokeWith(path, width);
    };
    auto triangle = [&](qreal x1, qreal y1, qreal x2, qreal y2, qreal x3, qreal y3) {
        QPainterPath path;
        path.moveTo(x1 * w, y1 * h);
        path.lineTo(x2 * w, y2 * h);
        path.lineTo(x3 * w, y3 * h);
        path.closeSubpath();
        fill(path);
    };
    auto rect = [&](qreal x, qreal y, qreal rw, qreal rh, bool filled) {
        const QRectF rc(x * w, y * h, rw * w, rh * h);
        QPainterPath path;
        path.addRect(rc);
        if (filled) fill(path); else stroke(path);
    };
    // Canvas 的角度以「屏幕顺时针」为正，Qt 的 arcTo 以数学逆时针为正。
    // 这里统一按 canvas 习惯传参，内部换算，避免逐个图标记两套角度。
    auto arc = [&](qreal cx, qreal cy, qreal r, qreal startDeg, qreal sweepDeg) {
        const QRectF rc((cx - r) * w, (cy - r) * w, 2 * r * w, 2 * r * w);
        QPainterPath path;
        path.arcMoveTo(rc, -startDeg);
        path.arcTo(rc, -startDeg, -sweepDeg);
        stroke(path);
    };

    if (name == QLatin1String("search")) {
        circle(0.42, 0.42, 0.24, false);
        line(0.61, 0.61, 0.84, 0.84);
    } else if (name == QLatin1String("music")) {
        // 八分音符：符头 + 符干 + 符尾
        p.save();
        p.translate(0.30 * w, 0.76 * h);
        p.rotate(-0.35 * 180.0 / kPi);
        p.setBrush(color);
        p.drawEllipse(QPointF(0, 0), 0.12 * w, 0.09 * w);
        p.restore();
        p.setBrush(Qt::NoBrush);
        line(0.42, 0.74, 0.42, 0.24);
        QPainterPath path;
        path.moveTo(0.42 * w, 0.24 * h);
        path.quadTo(0.66 * w, 0.24 * h, 0.74 * w, 0.44 * h);
        stroke(path);
    } else if (name == QLatin1String("user")) {
        circle(0.5, 0.33, 0.16, false);
        arc(0.5, 0.88, 0.28, 180.0, 180.0);
    } else if (name == QLatin1String("play")) {
        triangle(0.32, 0.22, 0.32, 0.78, 0.78, 0.5);
    } else if (name == QLatin1String("prev") || name == QLatin1String("next")) {
        const bool next = name == QLatin1String("next");
        const qreal barX = next ? 0.76 : 0.18;
        rect(barX, 0.2, 0.07, 0.6, true);
        if (next)
            triangle(0.32, 0.2, 0.32, 0.8, 0.70, 0.5);
        else
            triangle(0.68, 0.2, 0.68, 0.8, 0.30, 0.5);
    } else if (name == QLatin1String("pause")) {
        rect(0.32, 0.24, 0.11, 0.52, true);
        rect(0.57, 0.24, 0.11, 0.52, true);
    } else if (name == QLatin1String("volume")) {
        QPainterPath body;
        body.moveTo(0.12 * w, 0.38 * h);
        body.lineTo(0.28 * w, 0.38 * h);
        body.lineTo(0.46 * w, 0.22 * h);
        body.lineTo(0.46 * w, 0.78 * h);
        body.lineTo(0.28 * w, 0.62 * h);
        body.lineTo(0.12 * w, 0.62 * h);
        body.closeSubpath();
        fill(body);
        arc(0.48, 0.5, 0.20, -60.0, 120.0);
        arc(0.48, 0.5, 0.34, -56.25, 112.5);
    } else if (name == QLatin1String("volumeMute")) {
        QPainterPath body;
        body.moveTo(0.12 * w, 0.38 * h);
        body.lineTo(0.28 * w, 0.38 * h);
        body.lineTo(0.46 * w, 0.22 * h);
        body.lineTo(0.46 * w, 0.78 * h);
        body.lineTo(0.28 * w, 0.62 * h);
        body.lineTo(0.12 * w, 0.62 * h);
        body.closeSubpath();
        fill(body);
        line(0.60, 0.38, 0.88, 0.62);
        line(0.88, 0.38, 0.60, 0.62);
    } else if (name == QLatin1String("settings")) {
        // 两条滑杆（和网页控制台同一套线性图标）。原来的「中心圆 + 八根放射线」在
        // 18px 下会被看成太阳，跟主题图标 moon/sun 也撞脸。
        const qreal r = 0.10;
        const qreal gap = r + 0.015;
        line(0.17, 0.34, 0.62 - gap, 0.34);
        line(0.62 + gap, 0.34, 0.84, 0.34);
        circle(0.62, 0.34, r, false);
        line(0.17, 0.66, 0.42 - gap, 0.66);
        line(0.42 + gap, 0.66, 0.84, 0.66);
        circle(0.42, 0.66, r, false);
    } else if (name == QLatin1String("moon")) {
        circle(0.5, 0.5, 0.34, true);
        p.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        circle(0.66, 0.36, 0.30, true);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    } else if (name == QLatin1String("sun")) {
        circle(0.5, 0.5, 0.15, true);
        for (int i = 0; i < 8; ++i) {
            const qreal a = i * kPi / 4.0;
            line(0.5 + std::cos(a) * 0.25, 0.5 + std::sin(a) * 0.25,
                 0.5 + std::cos(a) * 0.38, 0.5 + std::sin(a) * 0.38);
        }
    } else if (name == QLatin1String("refresh")) {
        // 原实现从 -0.30π 顺时针画到 1.45π；这里按 canvas 角度直接换算
        arc(0.5, 0.5, 0.30, -0.30 * 180.0, 1.75 * 180.0);
        triangle(0.50, 0.10, 0.74, 0.20, 0.56, 0.36);
    } else if (name == QLatin1String("chevron")) {
        line(0.40, 0.24, 0.66, 0.50);
        line(0.66, 0.50, 0.40, 0.76);
    } else if (name == QLatin1String("check")) {
        line(0.20, 0.53, 0.42, 0.75);
        line(0.42, 0.75, 0.81, 0.27);
    } else if (name == QLatin1String("close")) {
        line(0.24, 0.24, 0.76, 0.76);
        line(0.76, 0.24, 0.24, 0.76);
    } else if (name == QLatin1String("sync")) {
        arc(0.5, 0.5, 0.30, 0.85 * 180.0, 1.10 * 180.0);
        arc(0.5, 0.5, 0.30, 1.85 * 180.0, 1.10 * 180.0);
        triangle(0.84, 0.30, 0.84, 0.56, 0.62, 0.46);
        triangle(0.16, 0.70, 0.16, 0.44, 0.38, 0.54);
    } else if (name == QLatin1String("trash")) {
        line(0.18, 0.28, 0.82, 0.28);
        rect(0.26, 0.28, 0.48, 0.56, false);
        line(0.38, 0.20, 0.42, 0.28);
        line(0.62, 0.20, 0.58, 0.28);
        line(0.42, 0.42, 0.42, 0.72);
        line(0.58, 0.42, 0.58, 0.72);
    } else if (name == QLatin1String("heart") || name == QLatin1String("heartFill")) {
        QPainterPath path;
        path.moveTo(0.50 * w, 0.84 * h);
        path.cubicTo(0.14 * w, 0.60 * h, 0.10 * w, 0.34 * h, 0.30 * w, 0.22 * h);
        path.cubicTo(0.41 * w, 0.16 * h, 0.48 * w, 0.23 * h, 0.50 * w, 0.32 * h);
        path.cubicTo(0.52 * w, 0.23 * h, 0.59 * w, 0.16 * h, 0.70 * w, 0.22 * h);
        path.cubicTo(0.90 * w, 0.34 * h, 0.86 * w, 0.60 * h, 0.50 * w, 0.84 * h);
        path.closeSubpath();
        if (name == QLatin1String("heartFill")) {
            strokeWith(path, qMax(1.6, w * 0.10));
            fill(path);
        } else {
            stroke(path);
        }
    } else if (name == QLatin1String("repeat") || name == QLatin1String("repeatOne")) {
        QPainterPath top;
        top.moveTo(0.20 * w, 0.62 * h);
        top.lineTo(0.20 * w, 0.44 * h);
        top.quadTo(0.20 * w, 0.30 * h, 0.34 * w, 0.30 * h);
        top.lineTo(0.64 * w, 0.30 * h);
        stroke(top);
        triangle(0.60, 0.21, 0.60, 0.39, 0.76, 0.30);

        QPainterPath bottom;
        bottom.moveTo(0.80 * w, 0.38 * h);
        bottom.lineTo(0.80 * w, 0.56 * h);
        bottom.quadTo(0.80 * w, 0.70 * h, 0.66 * w, 0.70 * h);
        bottom.lineTo(0.36 * w, 0.70 * h);
        stroke(bottom);
        triangle(0.40, 0.61, 0.40, 0.79, 0.24, 0.70);

        if (name == QLatin1String("repeatOne")) {
            const qreal thin = qMax(1.0, w * 0.065);
            lineWith(0.455, 0.485, 0.515, 0.445, thin);
            lineWith(0.515, 0.445, 0.515, 0.575, thin);
            lineWith(0.455, 0.575, 0.575, 0.575, thin);
        }
    } else if (name == QLatin1String("shuffle")) {
        QPainterPath upper;
        upper.moveTo(0.14 * w, 0.30 * h);
        upper.lineTo(0.26 * w, 0.30 * h);
        upper.quadTo(0.46 * w, 0.30 * h, 0.58 * w, 0.50 * h);
        upper.quadTo(0.66 * w, 0.64 * h, 0.76 * w, 0.64 * h);
        stroke(upper);

        QPainterPath lower;
        lower.moveTo(0.14 * w, 0.70 * h);
        lower.lineTo(0.26 * w, 0.70 * h);
        lower.quadTo(0.46 * w, 0.70 * h, 0.58 * w, 0.50 * h);
        lower.quadTo(0.66 * w, 0.36 * h, 0.76 * w, 0.36 * h);
        stroke(lower);

        triangle(0.72, 0.26, 0.88, 0.36, 0.72, 0.46);
        triangle(0.72, 0.54, 0.88, 0.64, 0.72, 0.74);
    } else if (name == QLatin1String("sequential")) {
        line(0.16, 0.30, 0.54, 0.30);
        line(0.16, 0.50, 0.54, 0.50);
        line(0.16, 0.70, 0.54, 0.70);
        line(0.62, 0.50, 0.84, 0.50);
        triangle(0.80, 0.40, 0.92, 0.50, 0.80, 0.60);
    } else if (name == QLatin1String("list")) {
        const qreal ys[3] = {0.28, 0.50, 0.72};
        for (int i = 0; i < 3; ++i) {
            circle(0.20, ys[i], 0.055, true);
            line(0.34, ys[i], 0.84, ys[i]);
        }
    } else if (name == QLatin1String("grid")) {
        // 2x2 圆角方块：与“列表”图标配对，供音乐库/搜索结果切换视图
        const qreal xs[2] = {0.20, 0.54};
        const qreal ys[2] = {0.20, 0.54};
        const qreal side = 0.26;
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 2; ++col) {
                const qreal x = xs[col];
                const qreal y = ys[row];
                line(x, y, x + side, y);
                line(x + side, y, x + side, y + side);
                line(x + side, y + side, x, y + side);
                line(x, y + side, x, y);
            }
        }
    } else if (name == QLatin1String("chevronDown")) {
        line(0.26, 0.40, 0.50, 0.64);
        line(0.50, 0.64, 0.74, 0.40);
    } else if (name == QLatin1String("expand")) {
        line(0.14, 0.34, 0.14, 0.14);
        line(0.14, 0.14, 0.34, 0.14);
        line(0.66, 0.14, 0.86, 0.14);
        line(0.86, 0.14, 0.86, 0.34);
        line(0.14, 0.66, 0.14, 0.86);
        line(0.14, 0.86, 0.34, 0.86);
        line(0.66, 0.86, 0.86, 0.86);
        line(0.86, 0.86, 0.86, 0.66);
    } else if (name == QLatin1String("download")) {
        line(0.50, 0.10, 0.50, 0.62);
        line(0.30, 0.44, 0.50, 0.62);
        line(0.70, 0.44, 0.50, 0.62);
        line(0.14, 0.82, 0.86, 0.82);
    } else if (name == QLatin1String("more")) {
        // 三个圆点：触屏端的「更多操作」入口
        circle(0.18, 0.5, 0.075, true);
        circle(0.50, 0.5, 0.075, true);
        circle(0.82, 0.5, 0.075, true);
    } else if (name == QLatin1String("filter")) {
        // 漏斗：首行最长、逐行收窄（平台/筛选入口）
        line(0.16, 0.28, 0.84, 0.28);
        line(0.27, 0.50, 0.73, 0.50);
        line(0.38, 0.72, 0.62, 0.72);
    } else if (name == QLatin1String("video")) {
        // 摄像机造型：机身 + 内嵌播放三角 + 右侧两格胶片条
        rect(0.06, 0.30, 0.62, 0.40, false);
        triangle(0.26, 0.40, 0.26, 0.60, 0.46, 0.50);
        rect(0.72, 0.24, 0.20, 0.16, true);
        rect(0.72, 0.60, 0.20, 0.16, true);
        line(0.82, 0.40, 0.82, 0.60);
    } else if (name == QLatin1String("history")) {
        // 播放历史：表盘 + 时针/分针。用矢量绘制，避免「◷」这类时钟符号
        // 在部分系统字体里缺字形，空状态显示成方框/乱码。
        circle(0.52, 0.54, 0.32, false);
        line(0.52, 0.54, 0.52, 0.35);
        line(0.52, 0.54, 0.68, 0.61);
    } else {
        // 未知图标：画一个音符，界面不会出现空白
        circle(0.36, 0.72, 0.12, true);
        line(0.48, 0.72, 0.48, 0.28);
        line(0.48, 0.28, 0.72, 0.36);
    }

    p.end();

    return image.convertToFormat(QImage::Format_RGBA8888);
}

} // namespace Furui
