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

    // ---- Android 返回键主通道（onClosing）----
    // Keys.onReleased（见下方 Loader）依赖 activeFocusItem 的焦点链，实测在部分
    // 机型/页面状态下收不到返回键（ColorOS Android16 上 MV 页 100% 收不到）；
    // 一旦没人 accept，窗口走关闭流程 → quitOnLastWindowClosed → main() 返回 →
    // Activity 销毁，若此时 MV 视频在渲染，hwuiTask 线程会撞上已销毁的 mutex
    // 直接 SIGABRT（表现为「从 MV 返回整个 App 闪退」）。
    // onClosing 不依赖焦点链，是 Android 返回键唯一可靠的拦截点：
    // 一律不关闭窗口，逐级回退交给 handleBack；没有层级可退则退到后台。
    onClosing: function(close) {
        close.accepted = false
        if (Qt.platform.os !== "android") return   // 桌面保持默认关闭行为
        const item = shell.item
        const handled = item && item.handleBack && item.handleBack()
        if (!handled)
            app.backgroundApp()
    }

    // ---- 形态判定：车机四种形态（横屏/竖屏/横分屏/竖分屏）靠这里统一决策 ----
    // 单一阈值（width<760）在分屏拖动时会在阈值附近来回换外壳，每换一次整棵页面树
    // 重建、滚动位置与在播高亮全丢。改为「进入窄阈值 + 离开宽阈值」的滞后带 + 去抖。
    readonly property int compactEnterWidth: 760
    readonly property int compactLeaveWidth: 830
    // 手机（非平板）始终用移动壳；这里提前算出来，让冷启动第一次装载就是正确外壳。
    // 否则手机上会先按桌面壳建一整棵页面树、再在 Component.onCompleted 里销毁重建。
    readonly property bool forceMobileShell: platform.isMobile && !platform.isTablet
    property bool compact: forceMobileShell
    // 用 visibility 而不是平台查询，最大化状态变化时才能自动刷新
    readonly property bool maximized: win.visibility === Window.Maximized
                      || win.visibility === Window.FullScreen

    // 外壳（移动/桌面）切换会销毁重建子树，状态必须常驻在窗口层，否则切回来就回首页
    property int pageIndex: 0
    property bool queueOpen: false
    property bool commentsOpen: false
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
        // 手机（非平板）横竖屏一律用移动壳：手机横屏宽度常越过 760 阈值，若按宽度
        // 判定会窜到桌面壳，在窄高屏上布局极不合理。平板（含车机矮宽屏）仍按窗口宽度
        // 判定（横屏用桌面壳，用户已确认平板横屏体验正常）。
        var forceMobile = win.forceMobileShell
        var nextCompact = forceMobile ? true
                : (win.compact
                   ? win.width <= win.compactLeaveWidth
                   : win.width < win.compactEnterWidth)
        if (nextCompact !== win.compact)
            win.compact = nextCompact
        else
            win.applyQueuedFormFactor()
        // 令牌缩放与安全区每次尺寸变化都重新上报（与外壳无关）
        theme.setViewport(win.width, win.height, true)
        platform.refreshInsets(win)
    }

    property string shellSource: forceMobileShell ? "MainMobile.qml" : "MainDesktop.qml"

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
        // Qt 只把按键派发给 activeFocusItem，焦点为空时事件直接被丢弃（不是发给 contentItem），
        // 所以外壳自己持有焦点；页面里的输入框被点按时仍会正常抢走焦点。
        focus: true
        // 返回键 / Esc 兜底：焦点落在页面内任意子控件时，事件沿父链冒泡到 Loader，
        // 这里再转给外壳处理，顺序仍是「队列面板 → 播放页 → 首页」逐级回退。
        // 不能挂在 ApplicationWindow 上：Window 不是 Item，Keys 是无效的附加属性。
        Keys.onReleased: function(event) {
            if (event.key === Qt.Key_Back || event.key === Qt.Key_Escape) {
                const item = shell.item
                // 关键：Android 上必须无条件 accept 返回键。若 handleBack() 返回 false
                // （例如桌面壳已在首页）却又不 accept，事件会落到 Qt Android 默认处理
                // → finish() 直接杀进程。这就是全面屏手势偶发「滑一下就退出」的根因；
                // 正确行为是退到后台，播放/媒体会话继续存活。
                const handled = item && item.handleBack && item.handleBack()
                console.log("[返回] shell Keys 捕获 Back，handled=", handled,
                            "platform=", Qt.platform.os)
                if (handled) {
                    event.accepted = true
                } else if (Qt.platform.os === "android") {
                    app.backgroundApp()
                    event.accepted = true
                }
            }
        }
        onLoaded: {
            ++swaps
            win.syncShellState()
        }
    }

    // 页面切走时会主动清掉输入框焦点（见 MainMobile），此时 activeFocusItem 变空，
    // 返回键就没人接收了。这里把焦点还回外壳，保证「键盘收起后按返回仍能逐级回退」。
    Connections {
        target: win
        function onActiveFocusItemChanged() {
            if (!win.activeFocusItem && win.visible)
                shell.forceActiveFocus()
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
        if (item.commentsOpen !== undefined)
            item.commentsOpen = win.commentsOpen
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
            if (item.commentsOpen !== undefined)
                win.commentsOpen = item.commentsOpen
        }
        function onQueueOpenChanged() {
            const item = shell.item
            if (item && !win.shellSyncing)
                win.queueOpen = item.queueOpen
        }
        function onCommentsOpenChanged() {
            const item = shell.item
            if (item && !win.shellSyncing)
                win.commentsOpen = item.commentsOpen
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

    // 内置浏览器（应用内 WebView）：用字符串 source 条件加载，Qt 5 兼容构建
    // 没有 QtWebView 模块时根本不解析这个文件，不会把整个 Main.qml 拖垮。
    Loader {
        anchors.fill: parent
        z: 9000
        source: browser.available ? "qrc:/qml/BrowserWindow.qml" : ""
    }

    // 全局轻提示：所有页面通过 app 信号统一在这里展示。
    // 挂在窗口层而不是 Overlay.overlay，跨 Qt5/Qt6 行为一致；
    // z 高于窗口拉伸热区，但隐藏时 visible=false 不拦截点击。
    Toast {
        id: toast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Math.round(theme.spaceLg * 5)
        z: 10000
    }

    Connections {
        target: app
        function onToast(message) { toast.show(message, "info") }
        // 带语义色的通知（登录失败 / MV 解析失败 / 无数据等）
        function onNotify(message, kind) { toast.show(message, kind) }
        // 搜索完成 / 换源成功之类的好消息不再弹条：
        // 页面里已经有「搜索到 N 首」和「VIP 替代 · X」角标，这里再弹只是噪音
        function onLoginStateChanged(sourceId, ok, message) {
            toast.show((ok ? "登录成功：" : "登录失败：") + message,
                       ok ? "success" : "error")
        }
        function onDownloadFinished(path) { toast.show("下载完成：" + path, "success") }
    }

    // 在线更新：启动自动检查与下载失败用全局 Toast；有新版本时引导到「设置」页，
    // 弹层（更新说明/下载进度/安装）内聚在 SettingsPage 里
    Connections {
        target: updater
        function onCheckFinished(hasUpdate, message) {
            if (hasUpdate)
                toast.show("发现新版本 " + updater.remoteVersion + "，可到「设置」页更新", "info")
        }
        function onDownloadFinished(path) { toast.show("更新包下载完成：" + path, "success") }
        function onDownloadFailed(error) { toast.show("更新下载失败：" + error, "error") }
    }

}
