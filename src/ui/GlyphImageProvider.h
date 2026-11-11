#pragma once
// 福瑞音乐 - 矢量图标图像提供器
// 原来 GlyphIcon.qml 用 Canvas 现画图标：每个图标一个协作式 Canvas，
// 主题切换 / 尺寸变化 / 列表滚动时都会重新执行一遍 JS 绘制脚本，是 QML 侧
// 最明显的一处 CPU 开销。这里改由 C++ 用 QPainter 一次性栅格化，
// 通过 image://glyph/<name>/<size>/<argb> 交给 Image 缓存复用。
#include <QColor>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>
#include <QString>

namespace Furui {

class GlyphImageProvider : public QQuickImageProvider {
public:
    GlyphImageProvider();

    QImage requestImage(const QString &id, QSize *size,
                        const QSize &requestedSize) override;

    // 供单元自检 / 调试使用：把某个图标渲染成给定像素尺寸
    static QImage render(const QString &name, const QColor &color, int pixels);

private:
    static QHash<QString, QImage> &cache();
    static QMutex &cacheMutex();
};

} // namespace Furui
