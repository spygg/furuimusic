import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15

// 统一入口：同一套 QML 资源，按窗口宽度切换移动/桌面外壳
ApplicationWindow {
    id: win
    visible: true
    width: platform.isMobile ? 420 : 1280
    height: platform.isMobile ? 780 : 820
    minimumWidth: platform.minimumWindowWidth
    minimumHeight: platform.minimumWindowHeight
    title: app.appName + (platform.isMobile ? "" : " - " + platform.platformName)

    // 桌面端去掉系统边框，标题栏与拉伸热区由 QML 自绘；移动端保持系统窗口
    flags: platform.framelessSupported
           ? (Qt.Window | Qt.FramelessWindowHint)
           : Qt.Window

    // 窗口底色与主题同步，避免切换深色时闪白
    color: theme.bg

    readonly property bool compact: width < 760
    // 用 visibility 而不是平台查询，最大化状态变化时才能自动刷新
    readonly property bool maximized: win.visibility === Window.Maximized

    Loader {
        id: shell
        anchors.fill: parent
        source: win.compact ? "MainMobile.qml" : "MainDesktop.qml"
    }

    // 边缘拉伸热区：仅桌面端、非最大化时启用
    Item {
        id: resizeLayer
        anchors.fill: parent
        visible: platform.framelessSupported && !win.maximized
        z: 9999

        readonly property int edge: 5
        readonly property int corner: 14

        ResizeHandle {
            targetWindow: win
            edges: Qt.LeftEdge
            x: 0; y: corner
            width: resizeLayer.edge
            height: parent.height - corner * 2
        }
        ResizeHandle {
            targetWindow: win
            edges: Qt.RightEdge
            x: parent.width - resizeLayer.edge; y: corner
            width: resizeLayer.edge
            height: parent.height - corner * 2
        }
        ResizeHandle {
            targetWindow: win
            edges: Qt.TopEdge
            x: corner; y: 0
            width: parent.width - corner * 2
            height: resizeLayer.edge
        }
        ResizeHandle {
            targetWindow: win
            edges: Qt.BottomEdge
            x: corner; y: parent.height - resizeLayer.edge
            width: parent.width - corner * 2
            height: resizeLayer.edge
        }
        ResizeHandle {
            targetWindow: win
            edges: Qt.LeftEdge | Qt.TopEdge
            x: 0; y: 0
            width: resizeLayer.corner
            height: resizeLayer.corner
        }
        ResizeHandle {
            targetWindow: win
            edges: Qt.RightEdge | Qt.TopEdge
            x: parent.width - resizeLayer.corner; y: 0
            width: resizeLayer.corner
            height: resizeLayer.corner
        }
        ResizeHandle {
            targetWindow: win
            edges: Qt.LeftEdge | Qt.BottomEdge
            x: 0; y: parent.height - resizeLayer.corner
            width: resizeLayer.corner
            height: resizeLayer.corner
        }
        ResizeHandle {
            targetWindow: win
            edges: Qt.RightEdge | Qt.BottomEdge
            x: parent.width - resizeLayer.corner; y: parent.height - resizeLayer.corner
            width: resizeLayer.corner
            height: resizeLayer.corner
        }
    }

    // 全局轻提示：所有页面通过 app 信号统一在这里展示
    Popup {
        id: toastPopup
        property string message: ""
        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: parent.height - height - 88
        width: Math.min(parent.width - 48, 380)
        height: 44
        modal: false
        focus: false
        closePolicy: Popup.NoAutoClose
        background: Rectangle {
            color: theme.dark ? "#2C2C36" : "#FFFFFF"
            radius: theme.radiusSm
            border.color: theme.line
            border.width: 1
        }
        contentItem: Text {
            text: toastPopup.message
            color: theme.text
            font.pixelSize: theme.fontSm
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            leftPadding: 12
            rightPadding: 12
        }
        Timer {
            id: toastTimer
            interval: 2400
            repeat: false
            onTriggered: toastPopup.close()
        }
        function showMessage(msg) {
            message = msg
            open()
            toastTimer.restart()
        }
    }

    Connections {
        target: app
        onToast: toastPopup.showMessage(message)
        onSearchFinished: toastPopup.showMessage("搜索完成，共 " + count + " 首结果")
        onLoginStateChanged: toastPopup.showMessage((ok ? "登录成功：" : "登录失败：") + message)
        onSubstituteInfo: toastPopup.showMessage(text)
        onDownloadFinished: toastPopup.showMessage("下载完成：" + path)
    }
}
