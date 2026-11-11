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

    // 返回键 / Esc 兜底：焦点可能落在页面内任意子控件（输入框、按钮）上，
    // 外壳自己挂的 Keys 收不到时事件会冒泡到窗口这一层，这里再转给外壳处理，
    // 顺序仍是「队列面板 → 播放页 → 首页」逐级回退
    Keys.onReleased: function(event) {
        if (event.key === Qt.Key_Back || event.key === Qt.Key_Escape) {
            const item = shell.item
            if (item && item.handleBack && item.handleBack())
                event.accepted = true
        }
    }

    // 桌面端去掉系统边框，标题栏与拉伸热区由 QML 自绘；移动端保持系统窗口
    flags: platform.framelessSupported
           ? (Qt.Window | Qt.FramelessWindowHint)
           : Qt.Window

    // 窗口底色与主题同步，避免切换深色时闪白
    color: theme.bg

    // ---- 形态判定：车机四种形态（横屏/竖屏/横分屏/竖分屏）靠这里统一决策 ----
    // 单一阈值（width<760）在分屏拖动时会在阈值附近来回换外壳，每换一次整棵页面树
    // 重建、滚动位置与在播高亮全丢。改为「进入窄阈值 + 离开宽阈值」的滞后带 + 去抖。
    readonly property int compactEnterWidth: 760
    readonly property int compactLeaveWidth: 830
    property bool compact: false
    // 用 visibility 而不是平台查询，最大化状态变化时才能自动刷新
    readonly property bool maximized: win.visibility === Window.Maximized
                      || win.visibility === Window.FullScreen

    // 外壳（移动/桌面）切换会销毁重建子树，状态必须常驻在窗口层，否则切回来就回首页
    property int pageIndex: 0
    property bool queueOpen: false
    property int lastContentPage: 0
    // 反向同步时压制回写，避免 win <-> shell.item 互相触发形成循环
    property bool shellSyncing: false

    onCompactChanged: applyQueuedFormFactor()

    // 尺寸变化先去抖，稳定后再定形态：拖动分屏滑杆过程中不做重建
    Timer {
        id: formFactorTimer
        interval: 170
        repeat: false
        onTriggered: win.evaluateFormFactor()
    }

    function applyQueuedFormFactor()
    {
        if (win.compact)
            win.shellSource = "MainMobile.qml"
        else
            win.shellSource = "MainDesktop.qml"
    }

    function evaluateFormFactor()
    {
        // 只量窗口，不看平台：车机横屏 1920x720 这类「矮宽屏」应当用桌面壳
        // 滞后带：已处于移动壳时，只有宽过「离开阈值」才换回桌面壳，避免临界宽度抖动
        var nextCompact = win.compact
                ? win.width <= win.compactLeaveWidth
                : win.width < win.compactEnterWidth
        if (nextCompact !== win.compact)
            win.compact = nextCompact
        else
            win.applyQueuedFormFactor()
        // 令牌缩放与安全区每次尺寸变化都重新上报（与外壳无关）
        theme.setViewport(win.width, win.height, true)
        platform.refreshInsets(win)
    }

    property string shellSource: "MainDesktop.qml"

    Loader {
        id: shell
        // 外壳重建次数：形态切换的回归指标，来回切四种形态时它只应在真正跨阈值时增长
        property int swaps: 0
        anchors.fill: parent
        // 安全区：状态栏/导航栏/刘海占掉的边距，直接内缩内容区，页面无需各自猜
        anchors.leftMargin: platform.insetLeft
        anchors.rightMargin: platform.insetRight
        anchors.topMargin: platform.insetTop
        anchors.bottomMargin: platform.insetBottom
        source: win.shellSource
        onLoaded: {
            ++swaps
            win.syncShellState()
        }
    }

    function syncShellState()
    {
        const item = shell.item
        if (!item)
            return
        win.shellSyncing = true
        item.pageIndex = win.pageIndex
        if (item.queueOpen !== undefined)
            item.queueOpen = win.queueOpen
        if (item.lastContentPage !== undefined)
            item.lastContentPage = win.lastContentPage
        win.shellSyncing = false
    }

    Connections {
        target: shell.item
        ignoreUnknownSignals: true
        function onPageIndexChanged() {
            const item = shell.item
            if (!item || win.shellSyncing)
                return
            win.pageIndex = item.pageIndex
            if (item.lastContentPage !== undefined)
                win.lastContentPage = item.lastContentPage
            if (item.queueOpen !== undefined)
                win.queueOpen = item.queueOpen
        }
        function onQueueOpenChanged() {
            const item = shell.item
            if (item && !win.shellSyncing)
                win.queueOpen = item.queueOpen
        }
    }

    // 横竖屏与分屏切换：尺寸、可见状态、屏幕、DPR 任一变化都重新评估
    onWidthChanged: formFactorTimer.restart()
    onHeightChanged: formFactorTimer.restart()
    onVisibilityChanged: {
        platform.refreshInsets(win)
        theme.setViewport(win.width, win.height, true)
        formFactorTimer.restart()
    }
    onScreenChanged: formFactorTimer.restart()

    Component.onCompleted: {
        // 车机冷启动时窗口尺寸可能要到 show 之后才落到真实值，先按当前尺寸定一次形态
        win.evaluateFormFactor()
        formFactorTimer.restart()
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
            x: 0; y: resizeLayer.corner
            width: resizeLayer.edge
            height: parent.height - resizeLayer.corner * 2
        }
        ResizeHandle {
            targetWindow: win
            edges: Qt.RightEdge
            x: parent.width - resizeLayer.edge; y: resizeLayer.corner
            width: resizeLayer.edge
            height: parent.height - resizeLayer.corner * 2
        }
        ResizeHandle {
            targetWindow: win
            edges: Qt.TopEdge
            x: resizeLayer.corner; y: 0
            width: parent.width - resizeLayer.corner * 2
            height: resizeLayer.edge
        }
        ResizeHandle {
            targetWindow: win
            edges: Qt.BottomEdge
            x: resizeLayer.corner; y: parent.height - resizeLayer.edge
            width: parent.width - resizeLayer.corner * 2
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
        function onToast(message) { toastPopup.showMessage(message) }
        function onSearchFinished(count, kind) {
            // count < 0 表示这次搜索被新操作作废，只收起 loading，不弹完成提示
            if (count < 0)
                return
            if (kind === "playlist")
                toastPopup.showMessage("搜索完成，共 " + count + " 个歌单")
            else if (kind === "song")
                toastPopup.showMessage("搜索完成，共 " + count + " 首结果")
        }
        function onLoginStateChanged(sourceId, ok, message) {
            toastPopup.showMessage((ok ? "登录成功：" : "登录失败：") + message)
        }
        function onSubstituteInfo(text) { toastPopup.showMessage(text) }
        function onDownloadFinished(path) { toastPopup.showMessage("下载完成：" + path) }
    }

}
