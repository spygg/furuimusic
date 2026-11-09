import QtQuick 2.15

// 无边框窗口的边缘/圆角拉伸热区
// 优先交给系统原生拉伸；窗口管理器不支持时，用鼠标增量手算窗口几何作为回退
MouseArea {
    id: root

    property var targetWindow: null
    property int edges: 0

    // 手算回退状态
    property bool manual: false
    property point startScreen
    property rect startGeom

    hoverEnabled: true
    acceptedButtons: Qt.LeftButton
    cursorShape: {
        var l = edges & Qt.LeftEdge, r = edges & Qt.RightEdge
        var t = edges & Qt.TopEdge, b = edges & Qt.BottomEdge
        if ((l && t) || (r && b)) return Qt.SizeFDiagCursor
        if ((r && t) || (l && b)) return Qt.SizeBDiagCursor
        if (l || r) return Qt.SizeHorCursor
        if (t || b) return Qt.SizeVerCursor
        return Qt.ArrowCursor
    }

    function screenPoint(mx, my) {
        var p = root.mapToItem(null, mx, my)
        return Qt.point(root.targetWindow.x + p.x, root.targetWindow.y + p.y)
    }

    function applyResize(dx, dy) {
        var w = root.targetWindow
        var g = root.startGeom
        var minW = w.minimumWidth > 0 ? w.minimumWidth : 360
        var minH = w.minimumHeight > 0 ? w.minimumHeight : 480

        if (edges & Qt.LeftEdge) {
            var nx = Math.min(g.x + dx, g.x + g.width - minW)
            nx = Math.max(nx, g.x + g.width - Math.max(g.width, minW))
            w.x = nx
            w.width = g.width - (nx - g.x)
        } else if (edges & Qt.RightEdge) {
            w.width = Math.max(minW, g.width + dx)
        }

        if (edges & Qt.TopEdge) {
            var ny = Math.min(g.y + dy, g.y + g.height - minH)
            ny = Math.max(ny, g.y + g.height - Math.max(g.height, minH))
            w.y = ny
            w.height = g.height - (ny - g.y)
        } else if (edges & Qt.BottomEdge) {
            w.height = Math.max(minH, g.height + dy)
        }
    }

    onPressed: {
        if (!targetWindow)
            return
        var p = screenPoint(mouse.x, mouse.y)
        startScreen = p
        startGeom = Qt.rect(targetWindow.x, targetWindow.y, targetWindow.width, targetWindow.height)
        if (platform.isMaximized(targetWindow))
            return
        if (!platform.startSystemResize(targetWindow, edges))
            manual = true
    }

    onReleased: manual = false
    onCanceled: manual = false

    onPositionChanged: {
        if (!manual || !targetWindow || !pressed)
            return
        var p = screenPoint(mouse.x, mouse.y)
        applyResize(p.x - startScreen.x, p.y - startScreen.y)
    }
}
