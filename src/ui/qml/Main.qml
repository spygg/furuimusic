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

    // 窗口底色与主题同步，避免切换深色时闪白；加渐变防止明暗切换瞬间硬跳
    color: theme.bg
    Behavior on color { ColorAnimation { duration: theme.durationBase } }

    // 全窗封面氛围底（仅移动端）：铺满整个窗口，含状态栏/挖孔区。
    // Android 上状态栏已设为透明，透出的必须是这张图本身；若背景留在外壳
    // （被安全区 topMargin 内缩），状态栏那条就只剩窗口纯色，和下面封面背景
    // 之间出现明显分界。参考 legado：窗口级铺图，状态栏与页面共用同一张底图。
    // 桌面端外壳有自己的背景，这里不启用；账号/设置页仍用纯主题底。
    Backdrop {
        // Qt 把内容区按安全区内缩了 osSafeTop（状态栏/挖孔高度），insetTop 此时为 0。
        // 背景要盖住状态栏那条，必须反向延伸回去：topMargin = -osSafeTop。
        anchors.fill: parent
        anchors.topMargin: -platform.osSafeTop
        // 横屏挖孔在左侧：Qt 把内容区从左边内缩 osSafeLeft，背景反向延伸盖住那条白边
        anchors.leftMargin: -platform.osSafeLeft
        visible: platform.isMobile && theme.coverLyricBackground
                 && shell.item && shell.item.pageIndex !== 2 && shell.item.pageIndex !== 3
        source: app.currentCover.length > 0 ? app.currentCover
                                            : "qrc:/qml/art/default_cover.jpg"
        dark: theme.dark
        veil: shell.item && shell.item.pageIndex === 1 ? 0.62 : 0.90
        Behavior on veil { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
    }

    // ---- Android 返回键主通道（onClosing）----
    // Keys.onReleased（见下方 Loader）依赖 activeFocusItem 的焦点链，实测在部分
    // 机型/页面状态下收不到返回键（ColorOS Android16 上 MV 页 100% 收不到）；
    // 一旦没人 accept，窗口走关闭流程 → quitOnLastWindowClosed → main() 返回 →
    // Activity 销毁，若此时 MV 视频在渲染，hwuiTask 线程会撞上已销毁的 mutex
    // 直接 SIGABRT（表现为「从 MV 返回整个 App 闪退」）。
    // onClosing 不依赖焦点链，是 Android 返回键唯一可靠的拦截点：
    // 一律不关闭窗口，逐级回退交给 handleBack；没有层级可退则退到后台。
    // 桌面端（托盘可用时）也拦：按记住的 closeAction 决定最小化到托盘 / 直接退出，
    // 没记住过就弹询问框，让用户选并可选「记住我的选择」。
    onClosing: function(close) {
        if (Qt.platform.os !== "android") {
            if (platform.isDesktop && typeof tray !== "undefined" && tray.available) {
                const action = app.setting("closeAction", "ask")
                if (action === "tray") {
                    close.accepted = false
                    tray.hideWindow()
                    return
                }
                if (action === "exit") return   // 记住了退出：走系统默认关闭流程
                close.accepted = false
                closeAsk.openCloseDialog()
                return
            }
            return   // 无托盘平台保持默认关闭行为
        }
        close.accepted = false
        if (win.handleBrowserBack()) return
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
    // 分屏/多窗口也算「必须移动壳」：车机半屏可能仍有 960dp 宽，只看宽度会误选桌面壳。
    property bool inMultiWindow: false
    readonly property bool forceMobileShell: platform.isMobile
                                             && (!platform.isTablet || inMultiWindow)
    // compact 始终保持为绑定：forceMobileShell 由 platform 的状态推导，
    // 宽度滞后判定只写 backing 属性，避免命令式赋值把绑定永久打断。
    property bool compactByWidth: false
    property bool compact: forceMobileShell || compactByWidth
    // 用 visibility 而不是平台查询，最大化状态变化时才能自动刷新
    readonly property bool maximized: win.visibility === Window.Maximized
                      || win.visibility === Window.FullScreen

    // 外壳（移动/桌面）切换会销毁重建子树，状态必须常驻在窗口层，否则切回来就回首页
    property int pageIndex: 0
    property bool queueOpen: false
    property bool commentsOpen: false
    property int lastContentPage: 0
    // MV 覆盖层的来路页与播放载荷也常驻窗口层：形态切换会销毁重建外壳，
    // 不同步的话新外壳拿到 pageIndex=4 但 MvPage 从没 open 过 → 黑屏 / 返回错页。
    property int mvReturnPage: 1
    property var mvSession: null
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
        // 旧写法直接写死字符串，会打断 shellSource 的声明式绑定：之后
        // platform 报 formFactorChanged 时外壳再也切不回正确形态。
        // 这里用 Qt.binding 重绑，forceMobileShell / compact 任一变化都能自动跟随。
        win.shellSource = Qt.binding(function () {
            return (win.forceMobileShell || win.compact)
                   ? "MainMobile.qml" : "MainDesktop.qml"
        })
    }

    function evaluateFormFactor()
    {
        // 先判多窗口：车机分屏进入/退出时窗口宽度未必跨过 760/830 阈值，
        // 但形态必须和竖屏一致（移动壳 + 底部标签栏），不能等宽度触发。
        win.inMultiWindow = platform.isMultiWindow(win)
        // 手机（非平板）横竖屏一律用移动壳：手机横屏宽度常越过 760 阈值，若按宽度
        // 判定会窜到桌面壳，在窄高屏上布局极不合理。平板（含车机矮宽屏）仍按窗口宽度
        // 判定（横屏用桌面壳，用户已确认平板横屏体验正常）。
        var forceMobile = win.forceMobileShell
        var nextCompact = forceMobile ? true
                : (win.compact
                   ? win.width <= win.compactLeaveWidth
                   : win.width < win.compactEnterWidth)
        if (nextCompact !== win.compact) {
            win.compactByWidth = nextCompact
            // 命令式写值会打断 compact 绑定；立刻用 Qt.binding 恢复，
            // 让 platform 后续上报的形态/触控变化继续驱动 compact。
            win.compact = Qt.binding(function () {
                return win.forceMobileShell || win.compactByWidth
            })
        } else
            win.applyQueuedFormFactor()
        // 令牌缩放与安全区每次尺寸变化都重新上报（与外壳无关）
        theme.setViewport(win.width, win.height, true)
        platform.refreshInsets(win)
        // 外壳没重建时也要把多窗口状态送进去（只同步这一项，别碰 pageIndex：
        // 用户可能正好在这一刻点了标签页）
        const shellItem = shell.item
        if (shellItem && shellItem.inMultiWindow !== undefined)
            shellItem.inMultiWindow = win.inMultiWindow
    }

    property string shellSource: forceMobileShell ? "MainMobile.qml" : "MainDesktop.qml"

    Loader {
        id: shell
        // 外壳重建次数：形态切换的回归指标，来回切四种形态时它只应在真正跨阈值时增长
        property int swaps: 0
        anchors.fill: parent
        // 安全区：状态栏/导航栏/刘海占掉的边距，直接内缩内容区，页面无需各自猜
        // 左侧：横屏挖孔在左侧时 Qt 已把 contentItem 内缩 osSafeLeft，这里反向减掉，
        // 让内容顶到屏幕最左、左侧不留那条 margin。竖屏 osSafeLeft=0 不受影响
        // （竖屏顶部仍保留安全区，按钮要避开挖孔）；桌面恒 0。
        anchors.leftMargin: platform.insetLeft - platform.osSafeLeft
        anchors.rightMargin: platform.insetRight
        anchors.topMargin: platform.insetTop
        anchors.bottomMargin: platform.insetBottom
        source: win.shellSource
        // Qt 只把按键派发给 activeFocusItem，焦点为空时事件直接被丢弃（不是发给 contentItem），
        // 所以外壳自己持有焦点；页面里的输入框被点按时仍会正常抢走焦点。
        focus: true
        // 媒体实体键兜底：车机的 KeyEvent 若没被 Activity 接住（例如落在 WebView
        // 原生子窗口上），Qt 会把它派发成 Key_Media* 到这里。只处理按下，
        // 避免同一次按键在按下/抬起各触发一次切歌。
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_MediaNext) {
                app.next(); event.accepted = true
            } else if (event.key === Qt.Key_MediaPrevious) {
                app.previous(); event.accepted = true
            } else if (event.key === Qt.Key_MediaTogglePlayPause
                       || event.key === Qt.Key_MediaPlay
                       || event.key === Qt.Key_MediaPause) {
                app.togglePause(); event.accepted = true
            }
        }
        // 返回键 / Esc 兜底：焦点落在页面内任意子控件时，事件沿父链冒泡到 Loader，
        // 这里再转给外壳处理，顺序仍是「队列面板 → 播放页 → 首页」逐级回退。
        // 不能挂在 ApplicationWindow 上：Window 不是 Item，Keys 是无效的附加属性。
        Keys.onReleased: function(event) {
            if (event.key === Qt.Key_Back || event.key === Qt.Key_Escape) {
                if (win.handleBrowserBack()) {
                    event.accepted = true
                    return
                }
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
            // 外壳自己也有 Keys.onReleased（更内层，会先吃掉返回键），所以把
            // 「浏览器返回」作为回调注入外壳：无论事件落在哪一层，都先走网页
            // 历史后退、退无可退才关浏览器，并且共用同一套去抖逻辑。
            if (item) {
                item.browserBack = function() { return win.handleBrowserBack() }
                // 外壳在 onMvOpened 时主动回传 MV 载荷与来路页，窗口层常驻保存
                item.saveMvSession = function(mv, ret) {
                    win.mvSession = mv
                    win.mvReturnPage = ret
                }
            }
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

    // PlatformAdapter 在启动 1200ms 后才上报最终形态/触控方式（分屏、外接屏、
    // 触屏切换），此前一直没有消费者，外壳会停留在冷启动时的错误形态。
    Connections {
        target: platform
        function onFormFactorChanged() { win.evaluateFormFactor() }
        function onTouchPrimaryChanged() { win.evaluateFormFactor() }
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
        // MV 状态回填：新外壳重建后，若正处于 MV 页，用保存的载荷重新 open。
        if (item.mvReturnPage !== undefined)
            item.mvReturnPage = win.mvReturnPage
        if (item.restoreMv !== undefined && win.pageIndex === 4 && win.mvSession)
            item.restoreMv(win.mvSession)
        // 分屏状态也要进外壳：移动壳在宽而矮的半屏里默认会切成左侧导航，
        // 车机要求跟竖屏一致（底部标签栏），所以由它关掉侧栏布局。
        if (item.inMultiWindow !== undefined)
            item.inMultiWindow = win.inMultiWindow
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
        // 首次运行许可协议：未签署就全屏弹出（在 showPlayerOnStart 之前，
        // 协议未接受时不该直接进播放页）
        pact.init()
        // 车机冷启动时窗口尺寸可能要到 show 之后才落到真实值，先按当前尺寸定一次形态
        win.evaluateFormFactor()
        formFactorTimer.restart()
        // 有播放历史/续播状态：冷启动直接进播放页（用户上次在听歌）。
        // 同步外壳：Loader.onLoaded 可能早于这里发生，必须主动推一次 pageIndex。
        if (app.showPlayerOnStart && win.pageIndex === 0) {
            win.pageIndex = 1
            win.lastContentPage = 0
            win.syncShellState()
        }
        // 桌面端托盘：把窗口交给托盘控制器（托盘菜单「显示主界面」、点 ✕ 隐藏用）
        if (Qt.platform.os !== "android" && Qt.platform.os !== "ios"
                && typeof tray !== "undefined")
            tray.attach(win)
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
        id: browserLoader
        anchors.fill: parent
        z: 9000
        source: browser.available ? "qrc:/qml/BrowserWindow.qml" : ""
    }

    // 返回键优先级：内置浏览器（先网页历史后退，退无可退再关闭）最优先，
    // 否则外层页面的逐级回退会动到浮层背后的页面。
    //
    // Android 的返回键会同时走 Keys.onReleased 和 onClosing 两条通道（两条都要接，
    // 因为部分机型只剩其中一条）。同一次按键如果两边都处理，第一条浏览器后退、
    // 第二条又退一级（甚至把浏览器关掉）。用时间戳把同一次按键的后续调用吞掉。
    property double browserBackAt: 0
    function handleBrowserBack() {
        const now = Date.now()
        // 统一去抖：无论浏览器是否打开，同一次按键的两条通道间隔仅几 ms，
        // 这里只放第一条通过。150ms 足够覆盖双通道，又不误伤用户的下一次独立按键。
        // 关键：以前这个时间戳只在浏览器打开时更新，浏览器关闭后走 handleBack 的两条
        // 通道都不更新它 → 一次按键退两级（播放页→首页→直接退后台）。
        if (now - win.browserBackAt < 150) return true
        win.browserBackAt = now
        if (browser.opened) {
            // 浏览器场景始终吞掉本次按键：退网页历史，退无可退才关浏览器，
            // 绝不让底层页面同时回退。
            const item = browserLoader.item
            if (!(item && item.handleBack && item.handleBack()))
                browser.close()
            return true
        }
        // 浏览器未打开：放行给调用方逐级 handleBack；时间戳已更新，
        // 同一次按键的第二条通道会被上面的去抖吞掉。
        return false
    }

    // 无边框窗口没有系统边框：浅色主题下窗口边缘与桌面/白底完全融合，拖动和
    // 缩放时找不到边界。补一圈 1px 强边界（最大化时贴屏幕边，不需要）。
    Rectangle {
        anchors.fill: parent
        visible: platform.framelessSupported && !win.maximized
        color: "transparent"
        border.width: 1
        border.color: theme.lineStrong
        z: 8500
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

    // 首次运行许可协议（参照 lx-music-mobile 的 PactModal）：
    // 未签署前全屏盖住整个应用，「不接受」直接退出；签署状态持久化 pactAccepted。
    PactDialog {
        id: pact
        z: 10050
        visible: false
        afterAccept: function() {
            toast.show("本软件完全免费且开源，如果你是花钱购买的，请直接给差评！", "info")
        }
    }

    // ---- 点 ✕ 时的行为询问（托盘可用且没记住过选择时才弹）----
    // 「最小化到托盘」= 隐藏窗口、音乐继续、托盘点开恢复；「退出」= 真正结束进程。
    // 勾选「记住我的选择」后写 settings.ini 的 closeAction 键，onClosing 直接走记住的行为。
    Popup {
        id: closeAsk
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(420, win.width - 48)
        modal: true
        dim: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: theme.card
            border.color: theme.line
            border.width: 1
            radius: theme.radiusLg
        }
        function openCloseDialog() { rememberClose.checked = false; open() }
        contentItem: ColumnLayout {
            spacing: theme.spaceSm
            Label {
                text: "关闭福瑞音乐"
                font.pixelSize: theme.fontLg
                font.weight: Font.DemiBold
                color: theme.text
            }
            Label {
                text: "点关闭按钮时想执行什么？勾选后下次将不再询问。"
                wrapMode: Text.WordWrap
                color: theme.subText
                font.pixelSize: theme.fontSm
            }
            CheckBox {
                id: rememberClose
                text: "记住我的选择"
                font.pixelSize: theme.fontSm
                contentItem: Label {
                    text: rememberClose.text
                    font: rememberClose.font
                    color: theme.text
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: rememberClose.indicator ? rememberClose.indicator.width + rememberClose.spacing : 0
                }
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: theme.spaceSm
                Button {
                    text: "最小化到托盘"
                    onClicked: {
                        if (rememberClose.checked)
                            app.setSetting("closeAction", "tray")
                        tray.hideWindow()
                        closeAsk.close()
                    }
                }
                Button {
                    text: "退出"
                    highlighted: true
                    onClicked: {
                        if (rememberClose.checked)
                            app.setSetting("closeAction", "exit")
                        closeAsk.close()
                        tray.quitApp()
                    }
                }
            }
        }
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
