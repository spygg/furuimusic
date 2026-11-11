import QtQuick 2.15

// 轻量矢量图标。
// 图标现在由 C++ 的 GlyphImageProvider 用 QPainter 栅格化（image://glyph/...），
// QML 侧只负责按 name/color/size 拼 URL。相比原来的 Canvas 现画：
//   1) 同一个图标/颜色/尺寸只栅格化一次，之后走 Image 缓存，列表滚动不再跑 JS；
//   2) 不依赖运行时的 Canvas 协作式绘制，移动端与低端机明显更稳；
//   3) 高 DPI 下用 2 倍图再缩小，边缘比 Canvas 直接画更干净。
Item {
    id: root

    property string name: "music"
    property color color: theme.text
    property real size: 18

    implicitWidth: size
    implicitHeight: size

    // URL 同时编码图标名、逻辑尺寸和 ARGB，任何一项变化都会换一张缓存图。
    // color.toString() 返回 "#AARRGGBB"，去掉 # 以避免被当成 URL fragment。
    readonly property string _key: name + "/" + Math.round(size)
                                   + "/" + color.toString().substring(1)

    Image {
        anchors.fill: parent
        source: "image://glyph/" + root._key
        // 2 倍取样：1x 屏缩小更锐利，2x 屏接近物理像素
        sourceSize.width: Math.max(8, Math.round(root.size * 2))
        sourceSize.height: Math.max(8, Math.round(root.size * 2))
        fillMode: Image.PreserveAspectFit
        smooth: true
        // Android 上部分 GLES 驱动为图标生成 mipmap 时会丢掉 alpha 通道，
        // 图标变成黑色方块；图标本身不缩小到需要 mipmap 的程度，关掉即可。
        mipmap: false
        cache: true
        asynchronous: false
    }
}
