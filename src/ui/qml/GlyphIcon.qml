import QtQuick 2.15

// 轻量矢量图标：用 Canvas 绘制，避免依赖系统字体里的符号字形
// （Android 上 ☾ / ⌕ 等字符会变成方框或彩色 emoji，桌面端显示也不一致）
Item {
    id: root

    property string name: "music"
    property color color: theme.text
    property real size: 18

    implicitWidth: size
    implicitHeight: size

    onNameChanged: canvas.requestPaint()
    onColorChanged: canvas.requestPaint()
    onSizeChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent
        antialiasing: true
        renderStrategy: Canvas.Cooperative

        onPaint: {
            var ctx = getContext("2d")
            var w = width
            var h = height
            ctx.reset()
            ctx.strokeStyle = root.color
            ctx.fillStyle = root.color
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            ctx.lineWidth = Math.max(1.2, w * 0.085)

            function circle(cx, cy, r, fill) {
                ctx.beginPath()
                ctx.arc(cx * w, cy * h, r * w, 0, Math.PI * 2)
                fill ? ctx.fill() : ctx.stroke()
            }
            function line(x1, y1, x2, y2) {
                ctx.beginPath()
                ctx.moveTo(x1 * w, y1 * h)
                ctx.lineTo(x2 * w, y2 * h)
                ctx.stroke()
            }

            if (root.name === "search") {
                circle(0.42, 0.42, 0.24, false)
                line(0.61, 0.61, 0.84, 0.84)
            } else if (root.name === "music") {
                // 八分音符：符头 + 符干 + 符尾
                ctx.beginPath()
                ctx.ellipse(0.30 * w, 0.76 * h, 0.12 * w, 0.09 * h, -0.35, 0, Math.PI * 2)
                ctx.fill()
                line(0.42, 0.74, 0.42, 0.24)
                ctx.beginPath()
                ctx.moveTo(0.42 * w, 0.24 * h)
                ctx.quadraticCurveTo(0.66 * w, 0.24 * h, 0.74 * w, 0.44 * h)
                ctx.stroke()
            } else if (root.name === "user") {
                circle(0.5, 0.33, 0.16, false)
                ctx.beginPath()
                ctx.arc(0.5 * w, 0.88 * h, 0.28 * w, Math.PI, Math.PI * 2)
                ctx.stroke()
            } else if (root.name === "play") {
                ctx.beginPath()
                ctx.moveTo(0.32 * w, 0.22 * h)
                ctx.lineTo(0.32 * w, 0.78 * h)
                ctx.lineTo(0.78 * w, 0.5 * h)
                ctx.closePath()
                ctx.fill()
            } else if (root.name === "prev" || root.name === "next") {
                // 上一首/下一首：竖条 + 实心三角，next 为 prev 的水平镜像
                var next = root.name === "next"
                var barX = next ? 0.76 : 0.18
                ctx.beginPath()
                ctx.rect(barX * w, 0.2 * h, 0.07 * w, 0.6 * h)
                ctx.fill()
                ctx.beginPath()
                if (next) {
                    ctx.moveTo(0.32 * w, 0.2 * h)
                    ctx.lineTo(0.32 * w, 0.8 * h)
                    ctx.lineTo(0.70 * w, 0.5 * h)
                } else {
                    ctx.moveTo(0.68 * w, 0.2 * h)
                    ctx.lineTo(0.68 * w, 0.8 * h)
                    ctx.lineTo(0.30 * w, 0.5 * h)
                }
                ctx.closePath()
                ctx.fill()
            } else if (root.name === "pause") {
                ctx.beginPath()
                ctx.rect(0.32 * w, 0.24 * h, 0.11 * w, 0.52 * h)
                ctx.rect(0.57 * w, 0.24 * h, 0.11 * w, 0.52 * h)
                ctx.fill()
            } else if (root.name === "settings") {
                circle(0.5, 0.5, 0.13, false)
                for (var i = 0; i < 8; ++i) {
                    var a = i * Math.PI / 4
                    line(0.5 + Math.cos(a) * 0.24, 0.5 + Math.sin(a) * 0.24,
                         0.5 + Math.cos(a) * 0.36, 0.5 + Math.sin(a) * 0.36)
                }
            } else if (root.name === "moon") {
                // 用两次绘制叠加出月牙：先画圆，再挖掉偏移的圆
                circle(0.5, 0.5, 0.34, true)
                ctx.globalCompositeOperation = "destination-out"
                circle(0.66, 0.36, 0.30, true)
                ctx.globalCompositeOperation = "source-over"
            } else if (root.name === "sun") {
                circle(0.5, 0.5, 0.15, true)
                for (var j = 0; j < 8; ++j) {
                    var b = j * Math.PI / 4
                    line(0.5 + Math.cos(b) * 0.25, 0.5 + Math.sin(b) * 0.25,
                         0.5 + Math.cos(b) * 0.38, 0.5 + Math.sin(b) * 0.38)
                }
            }
        }
    }
}
