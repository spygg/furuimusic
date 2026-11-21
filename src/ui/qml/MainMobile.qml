import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
// 切页收焦点时用 root.Window.window，缺这个 import 会报 "Cannot read property
// 'window' of undefined"，焦点清理整行静默失效
import QtQuick.Window 2.15

// 移动端外壳：页面区 + 迷你播放条（贴底） + 底部图标标签栏
// 与桌面共用同一批页面；这里只做窄屏排布，不出现顶部品牌栏与窗口按钮
Item {
    id: root

    readonly property color cBg: theme.bg
    readonly property color cCard: theme.surface
    readonly property color cText: theme.text
    readonly property color cSub: theme.subText
    readonly property color cPrimary: theme.accent
    readonly property color cLine: theme.line
    readonly property color cPrimarySoft: theme.accentSoft

    function fmtTime(ms) {
        var t = Math.max(0, Math.floor(ms / 1000))
        return ("0" + Math.floor(t / 60)).slice(-2) + ":" + ("0" + (t % 60)).slice(-2)
    }

    property int pageIndex: 0
    property bool queueOpen: false
    property bool commentsOpen: false
    // 打开评论抽屉时顺带拉取当前歌曲第一页评论
    function openComments() {
        commentsOpen = true
        app.loadComments(1)
    }
    // MV 页从哪页进的，返回键/关闭就回哪页
    property int mvReturnPage: 1
    // 由 Main.qml 注入：onMvOpened 时回传 MV 载荷与来路页，窗口层常驻保存
    property var saveMvSession: function(mv, ret) { }

    // Android 返回键：内置浏览器 → 队列面板 → MV 页 → 播放页 → 音乐库 → 首页，逐级回退
    // 由 Main.qml 注入：先走网页历史后退，退无可退才关浏览器
    property var browserBack: function() { return false }
    // 由 Main.qml 注入：分屏/多窗口时保持竖屏那套底部标签栏布局
    property bool inMultiWindow: false
    function handleBack() {
        if (browserBack()) {
            return true
        }
        if (queueOpen) {
            queueOpen = false
            return true
        }
        if (commentsOpen) {
            commentsOpen = false
            return true
        }
        if (pageIndex === 4) {
            // close() 会停掉媒体并经 requestClose 回到进入前的页；
            // 只改 pageIndex 的话 MV 会在后台继续出声
            mvPage.close()
            return true
        }
        if (pageIndex === 1) {
            pageIndex = 0
            return true
        }
        if (pageIndex !== 0) {
            pageIndex = 0
            return true
        }
        // 首页的返回键（含全面屏手势的边缘滑动）不再让事件落到 Qt Android 默认
        // finish() 把 App 杀掉——主流音乐 App 的首页返回是「退到后台」，播放继续。
        if (Qt.platform.os === "android") {
            app.backgroundApp()
            return true
        }
        return false
    }

    // Keys 必须挂在 Item 上：ApplicationWindow 是 Window，不能直接接收 Keys。
    // 焦点在任意子控件时，按键会沿父链冒泡到这里。
    focus: true
    Keys.onReleased: function(event) {
        if (event.key === Qt.Key_Back || event.key === Qt.Key_Escape) {
            if (root.handleBack())
                event.accepted = true
            else
                event.accepted = false
        }
    }

    // 搜索已并入音乐库（页头搜索框）；索引与标签栏顺序严格一致：
    // 0 = 曲库 1 = 播放 2 = 账号 3 = 设置
    readonly property var tabIcons: ["music", "play", "user", "settings"]
    readonly property var tabLabels: ["曲库", "播放", "账号", "设置"]

    // 竖排导航（平板横屏 / 手机横屏）：这些形态高度本来就紧张（手机横屏逻辑高度
    // 只剩 270dp 上下），底部标签栏再吃掉 56dp 就没什么内容区了，把导航挪到左侧
    // 换回高度。竖屏仍用底部标签栏（拇指可达、且高度充裕）。
    // 车机分屏同样是「宽而矮」，但用户明确要求跟竖屏一致（底部标签栏），
    // 所以多窗口时不再切左侧导航。
    readonly property bool sideNav: !root.inMultiWindow
                                    && root.width >= 400
                                    && root.width > root.height * 1.12
    // 竖排导航宽度：手机横屏 76dp 够用，平板/车机（>=900dp）给到 92dp，
    // 图标和文字都要按设备尺寸放大，否则 12 寸屏上侧栏像一根细线
    readonly property int navRailWidth: root.width >= 900 ? 92
                                       : (root.width >= 640 ? 82 : 76)
    // 横屏手机（高度 < 420dp）内容区只有 300dp 上下，底部两栏必须进一步收薄
    readonly property bool shortViewport: root.height < 420

    // 兜底色：封面还没解码出来时先铺主题底，避免首帧透明
    Rectangle {
        anchors.fill: parent
        color: cBg
    }

    // 封面氛围底：铺满整个窗口，包括左侧导航栏与状态栏区域。
    // 背景归外壳统一负责之后，横屏才不会出现「左边一条白色侧栏、右边是封面」的割裂感
    // （这正是以前看着像没铺到屏幕左边缘的原因）。页面只负责内容，不再各自画背景。
    Backdrop {
        anchors.fill: parent
        // 设置页关掉「封面作为歌词背景」后整窗退回纯主题底色
        visible: theme.coverLyricBackground
        source: app.currentCover.length > 0 ? app.currentCover
                                            : "qrc:/qml/art/default_cover.jpg"
        dark: theme.dark
        // 播放页纱薄一些（要能看出封面色彩），其它页纱厚一些（列表与表单要压得住背景）
        veil: root.pageIndex === 1 ? 0.62 : 0.90
        Behavior on veil { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
    }

    // 页面区：填满顶部到底部栏之间的区域。不用 ColumnLayout 流式堆叠——Android
    // adjustResize 弹起软键盘时窗口高度被压扁，ColumnLayout 会先把 fillHeight 的
    // 页面区压到 0，迷你条和标签栏就顺着堆到顶端（截图里的挤顶现象）。改为 anchors
    // 绝对定位：底部两栏永远贴底，页面区只占剩余上部空间，窗口怎么压缩都不串位。
    StackLayout {
        id: pageStack
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: bottomBar.top
        anchors.leftMargin: root.sideNav ? root.navRailWidth : 0
        // 兜底裁切：页面自己的 Flickable/ListView 已经裁过，这里再裁一层，
        // 避免任何页面把内容画到迷你条和标签栏底下（页尾一行字被胶囊压住一半）
        clip: true
        // index 4 是外壳级 MV 覆盖层，不属于页面栈；钳到 0..3 避免越界
        currentIndex: Math.min(root.pageIndex, 3)
        onCurrentIndexChanged: {
            // 切页必须主动收键盘：TextField 的焦点会活过页面切换（QML 不会因为
            // 父页被 StackLayout 藏起来就失焦），不处理的话软键盘会一直盖在新页面上
            Qt.inputMethod.hide()
            const af = root.Window.window ? root.Window.window.activeFocusItem : null
            if (af) af.focus = false
            pageFade.restart()
            // 播放页是 index 1（索引必须与底部标签栏顺序一致）
            if (currentIndex === 1)
                playerPageMobile.playEntrance()
        }

        LibraryPage { }
        // 移动端迷你条只有一条进度线，播放页承担完整的进度/音量/切歌控件
        PlayerPage {
            id: playerPageMobile
            showControls: true
            inMultiWindow: root.inMultiWindow
            // 背景已由外壳的 Backdrop 统一铺满整窗，页面内不再重复画一层
            // （重复画会在侧栏右侧形成一条接缝，且左右两侧封面错位）
            ownBackdrop: false
            onRequestClose: root.pageIndex = 0
            onRequestSettings: root.pageIndex = 3
            onRequestQueue: { root.commentsOpen = false; root.queueOpen = !root.queueOpen }
            onRequestComments: { root.queueOpen = false; root.openComments() }
        }
        LoginPage { }
        SettingsPage { }
    }

    // 底部栏容器：迷你条 + 标签栏，整体贴底；高度随两者显隐自适应
    Item {
        id: bottomBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: root.sideNav ? root.navRailWidth : 0
        // 胶囊上下留白。以前写成 miniBar.topMargin（不存在的属性）→ 表达式得 NaN，
        // 被 QML 当成 0，底部栏高度恒为 0：页面区一路铺到屏幕底，设置页/曲库页的
        // 最后一行会被浮动胶囊压住。改成显式常量，两边引用同一个值。
        readonly property int barPad: 4
        height: (tabBar.visible ? tabBar.height : 0)
                + (miniBar.visible ? miniBar.height + barPad * 2 : 0)

        // 迷你播放条：贴底浮动胶囊（Alger 风格：圆角、左右留白、圆形封面），位于页面与底部标签栏之间
        Rectangle {
            id: miniBar
            anchors.left: parent.left
            anchors.right: parent.right
            // 标签栏可见时压在标签栏上方，否则直接贴容器底
            anchors.bottom: tabBar.visible ? tabBar.top : parent.bottom
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            anchors.topMargin: bottomBar.barPad
            anchors.bottomMargin: bottomBar.barPad
            // 横屏用竖排导航时高度更金贵，胶囊压到 56；横屏手机（矮窗）再收到 48
            height: root.shortViewport ? 48 : (root.sideNav ? 56 : 64)
            // 按钮跟胶囊高度联动：48dp 胶囊里塞 48dp 圆钮会把上下边距吃没
            readonly property int buttonSize: Math.min(theme.touchMin, height - 6)
            radius: 18
            color: cCard
            border.color: cLine
            border.width: 1
            // 还没选中任何一首歌时整条隐藏：空胶囊占着 58dp 高度只是噪音
            visible: root.pageIndex !== 1 && root.pageIndex !== 4
                     && app.currentTitle.length > 0
                     // 曲库页搜索时隐藏：软键盘弹出会把页面区压扁，播放栏再占位就盖住搜索结果
                     && !(root.pageIndex === 0 && app.searchActive)

            // 底部内嵌进度：低对比底槽 + 半透明进度填充。
            // 之前是单个满饱和绿 Rectangle 且同时设了 anchors.left/right 与 width——
            // QML 里 anchors 会覆盖 width，所以进度线永远满宽、不随播放变化，
            // 看起来就是一根常亮绿条。现在外层负责定位、内层只绑 width 表达进度。
            Item {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 2
                height: 2
                visible: app.currentTitle.length > 0 && app.duration > 0

                Rectangle {   // 底槽
                    anchors.fill: parent
                    radius: height / 2
                    color: Qt.rgba(cPrimary.r, cPrimary.g, cPrimary.b, theme.dark ? 0.20 : 0.13)
                }
                Rectangle {   // 进度
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: parent.width * Math.min(1, app.position / app.duration)
                    radius: height / 2
                    color: Qt.rgba(cPrimary.r, cPrimary.g, cPrimary.b, 0.55)
                }
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: theme.spaceSm

                // 左侧信息区：封面 + 曲名/艺人。宽屏（车机/平板横屏）不再吃掉整行，
                // 给中间的歌词/时间腾出空间；窄屏仍 fillWidth 占满。
                Item {
                    Layout.fillWidth: !midInfo.visible
                    Layout.preferredWidth: midInfo.visible
                                           ? Math.max(240, Math.min(300, miniBar.width * 0.26)) : -1
                    Layout.fillHeight: true

                    RowLayout {
                        anchors.fill: parent
                        spacing: 10

                        Rectangle {
                            Layout.preferredWidth: Math.min(46, miniBar.height - 10)
                            Layout.preferredHeight: Layout.preferredWidth
                            Layout.alignment: Qt.AlignVCenter
                            radius: width / 2
                            border.width: 3
                            border.color: theme.dark ? Qt.rgba(1, 1, 1, 0.14) : Qt.rgba(0, 0, 0, 0.10)
                            color: cPrimarySoft
                            clip: true
                            Text {
                                anchors.centerIn: parent
                                text: "♪"
                                color: cPrimary
                                font.pixelSize: 18
                                visible: coverImg.status !== Image.Ready
                            }
                            Image {
                                id: coverImg
                                anchors.fill: parent
                                source: app.currentCover
                                sourceSize.width: 92
                                sourceSize.height: 92
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                visible: status === Image.Ready
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                text: app.currentTitle.length > 0 ? app.currentTitle : "还没有播放"
                                color: cText
                                font.pixelSize: 14
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            RowLayout {
                                spacing: theme.spaceXs
                                Text {
                                    Layout.maximumWidth: 130
                                    text: app.currentArtist.length > 0 ? app.currentArtist : "选择一首歌开始"
                                    color: cSub
                                    font.pixelSize: theme.fontSm
                                    elide: Text.ElideRight
                                }
                                Text {
                                    text: app.currentSourceLabel
                                    color: cPrimary
                                    font.pixelSize: theme.fontSm
                                    visible: text.length > 0
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.pageIndex = 1
                    }
                }

                // 中间空白区（车机/平板横屏才显示）：当前歌词 + 播放进度时间。
                // 竖屏手机宽度不够，留空给左侧信息区，避免把歌名/艺人挤成省略号。
                Item {
                    id: midInfo
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: miniBar.width >= 620 && app.currentTitle.length > 0

                    ColumnLayout {
                        anchors.centerIn: parent
                        width: parent.width - 16
                        spacing: 1
                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: app.currentLyricLine.length > 0 ? app.currentLyricLine : "♪"
                            color: cSub
                            font.pixelSize: Math.max(10, Math.round(theme.fontSm
                                                                   * theme.lyricFontScale / 100))
                            elide: Text.ElideRight
                            maximumLineCount: 1
                        }
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: root.fmtTime(app.position) + " / " + root.fmtTime(app.duration)
                            color: cSub
                            opacity: 0.7
                            font.pixelSize: theme.fontXs
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.pageIndex = 1
                    }
                }

                Rectangle {
                    Layout.preferredWidth: miniBar.buttonSize
                    Layout.preferredHeight: miniBar.buttonSize
                    Layout.alignment: Qt.AlignVCenter
                    radius: height / 2
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: theme.heroStart }
                        GradientStop { position: 1.0; color: theme.heroEnd }
                    }
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: app.playing ? "pause" : "play"
                        size: 14
                        color: "#FFFFFF"
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: app.togglePause()
                    }
                }

                Rectangle {
                    Layout.preferredWidth: miniBar.buttonSize
                    Layout.preferredHeight: miniBar.buttonSize
                    Layout.alignment: Qt.AlignVCenter
                    radius: width / 2
                    color: "transparent"
                    border.color: cLine
                    Text {
                        anchors.centerIn: parent
                        text: "›"
                        color: cSub
                        font.pixelSize: 20
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: app.next()
                    }
                }
            }
        }

        // 底部标签栏（横屏改用左侧竖排导航，这里整条隐藏，省下 56dp 高度）
        // 竖屏/分屏进入播放页(1)或 MV(4)时整条收起：播放页自带完整控件，下方再留一条
        // 标签栏既重复又占地方；返回其它页后自动恢复（返回键 handleBack 会把 1 改回 0）。
        Rectangle {
            id: tabBar
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 56
            visible: !root.sideNav && root.pageIndex !== 1 && root.pageIndex !== 4
            color: cCard
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 1
                color: cLine
            }

            Row {
                id: tabRow
                anchors.fill: parent
                anchors.margins: 4
                spacing: 0

                Repeater {
                    model: root.tabLabels.length
                    delegate: Item {
                        width: tabRow.width / root.tabLabels.length
                        height: tabRow.height

                        readonly property bool checked: root.pageIndex === index

                        Column {
                            anchors.centerIn: parent
                            spacing: 1
                            GlyphIcon {
                                anchors.horizontalCenter: parent.horizontalCenter
                                name: root.tabIcons[index]
                                size: checked ? 21 : 19
                                color: checked ? cPrimary : cSub
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: root.tabLabels[index]
                                color: checked ? cPrimary : cSub
                                font.pixelSize: theme.fontXs
                                font.bold: checked
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.pageIndex = index
                        }
                    }
                }
            }
        }
    }

    // 左侧竖排导航（平板横屏 / 手机横屏）：图标在上、文字在下，选中态用主色 + 淡底胶囊。
    // 层级低于队列面板与 MV 覆盖层，弹层照旧盖满全屏。
    Rectangle {
        id: navRail
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.sideNav ? root.navRailWidth : 0
        visible: root.sideNav
        // 透明：让外壳的封面氛围底一路铺到屏幕最左边。
        // 以前这里是 cCard（纯白/纯深），再加一条 1px 分隔线，横屏时左侧就是一块
        // 明显与内容割裂的白板 —— 换任何背景色都还是「一条板」，只有透明才是真铺满。
        color: "transparent"
        clip: true

        Column {
            id: railColumn
            anchors.top: parent.top
            anchors.topMargin: 8
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 4

            Repeater {
                model: root.tabLabels.length
                delegate: Item {
                    width: root.navRailWidth - 12
                    // 矮屏（手机横屏）按可用高度逐项收紧，保证 4 项都落在可见区内
                    height: Math.max(46, Math.min(64,
                        (root.height - 24) / root.tabLabels.length - 4))
                    readonly property bool checked: root.pageIndex === index

                    Rectangle {
                        anchors.fill: parent
                        radius: 14
                        color: checked ? cPrimarySoft
                               : railMa.containsMouse
                                 ? (theme.dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.05))
                                 : "transparent"
                        Behavior on color { ColorAnimation { duration: 140 } }
                    }

                    Column {
                        anchors.centerIn: parent
                        spacing: 1
                        GlyphIcon {
                            anchors.horizontalCenter: parent.horizontalCenter
                            name: root.tabIcons[index]
                            size: checked ? 21 : 19
                            color: checked ? cPrimary : cSub
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: root.tabLabels[index]
                            color: checked ? cPrimary : cSub
                            font.pixelSize: theme.fontXs
                            font.bold: checked
                        }
                    }

                    MouseArea {
                        id: railMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.pageIndex = index
                    }
                }
            }
        }
    }

    // 车机/手机外壳的播放队列：桌面用的是右侧抽屉，窄屏没有横向余量，这里改成底部面板。
    // 这一层必须存在——播放页的队列按钮只发 requestQueue，窄屏外壳不接住的话手指点下去没反应。
    Rectangle {
        id: queueSheet
        anchors.fill: parent
        color: "#66000000"
        visible: opacity > 0
        opacity: root.queueOpen ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }

        MouseArea {
            anchors.fill: parent
            onClicked: root.queueOpen = false
        }

        Rectangle {
            id: queuePanel
            readonly property real corner: theme.radiusLg
            radius: corner
            color: theme.card
            clip: true
            anchors.left: parent.left
            anchors.right: parent.right
            // 不用 anchors.bottom：面板靠 y 做升降动画，和底边锚点会互相覆盖。
            // 高度多出一个圆角并把下半截压到窗口外，Qt5 的 Rectangle 没有「只圆上两角」，
            // 这样贴底时才不会在屏幕角落露出两个圆角缺口。
            height: Math.max(200, Math.min(parent.height - 8, parent.height * 0.62)) + corner
            y: root.queueOpen ? parent.height - height + corner : parent.height + corner
            Behavior on y { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

            QueueDrawer {
                anchors.fill: parent
                anchors.bottomMargin: queuePanel.corner
                onCloseRequested: root.queueOpen = false
            }
        }
    }

    // 歌曲评论底部面板（与队列面板同款升降动画，互斥打开）
    Rectangle {
        id: commentSheet
        anchors.fill: parent
        color: "#66000000"
        visible: opacity > 0
        opacity: root.commentsOpen ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
        MouseArea {
            anchors.fill: parent
            onClicked: root.commentsOpen = false
        }
        Rectangle {
            id: commentPanel
            readonly property real corner: theme.radiusLg
            radius: corner
            color: theme.card
            clip: true
            anchors.left: parent.left
            anchors.right: parent.right
            height: Math.max(260, Math.min(parent.height - 8, parent.height * 0.72)) + corner
            y: root.commentsOpen ? parent.height - height + corner : parent.height + corner
            Behavior on y { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
            CommentDrawer {
                anchors.fill: parent
                anchors.bottomMargin: commentPanel.corner
                onCloseRequested: root.commentsOpen = false
            }
        }
    }

    // 页面切换的轻微呼吸（与桌面外壳同款；不能放进 StackLayout）
    SequentialAnimation {
        id: pageFade
        NumberAnimation { target: pageStack; property: "opacity"; to: 0.6; duration: 70 }
        NumberAnimation { target: pageStack; property: "opacity"; to: 1; duration: 160 }
    }

    // MV 沉浸式覆盖层：盖住底部标签栏、迷你播放条与队列面板，
    // 只保留 pageIndex=4 状态位，Android 返回键仍走 handleBack 逐级回退。
    // 注意不能用 visible 隐藏：ColorOS Android16 上把渲染中的 VideoOutput
    // 置为不可见会销毁 video sink，hwuiTask 线程 SIGABRT（返回退出 100% 崩）。
    // 改用 opacity 0 + enabled false：节点仍在场景图里、sink 保持挂载，输入穿透。
    MvPage {
        id: mvPage
        anchors.fill: parent
        z: 1000
        opacity: root.pageIndex === 4 ? 1 : 0
        enabled: root.pageIndex === 4
        onRequestClose: {
            root.pageIndex = root.mvReturnPage
            app.mvClosed()   // 进入 MV 前在播的歌恢复续播
        }
    }

    // 外壳重建（旋转/分屏）后由窗口层调用，用保存的载荷重新打开 MV
    function restoreMv(mv) { mvPage.open(mv) }

    // app.mvOpened 由 AppController 解析完 MV 地址后发出：
    // 收起队列 → 记住来路页 → 切到 MV 覆盖层 → 把地址交给页面
    Connections {
        target: app
        function onMvOpened(mv) {
            root.queueOpen = false
            if (root.pageIndex !== 4) root.mvReturnPage = root.pageIndex
            root.pageIndex = 4
            mvPage.open(mv)
            if (saveMvSession) saveMvSession(mv, root.mvReturnPage)
        }
    }
}
