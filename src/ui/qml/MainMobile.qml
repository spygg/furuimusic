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

    property int pageIndex: 0
    property bool queueOpen: false
    // MV 页从哪页进的，返回键/关闭就回哪页
    property int mvReturnPage: 1

    // Android 返回键：队列面板 → MV 页 → 播放页 → 音乐库 → 首页，逐级回退
    function handleBack() {
        if (queueOpen) {
            queueOpen = false
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
    readonly property bool sideNav: root.width >= 400 && root.width > root.height * 1.12
    readonly property int navRailWidth: 76

    Rectangle {
        anchors.fill: parent
        color: cBg
    }

    ColumnLayout {
        anchors.fill: parent
        // 竖排导航模式下把内容列右移，给左侧导航条让位（不用 RowLayout 嵌套，
        // 页面栈/迷你条/标签栏的层级保持不变）
        anchors.leftMargin: root.sideNav ? root.navRailWidth : 0
        // 横屏矮窗口里各页的最小高度之和可能超过可用高度，撑破后会顶掉底部迷你条。
        // 这里允许压缩并裁剪，页面有自己的滚动区，不会丢内容
        clip: true
        spacing: 0

        StackLayout {
            id: pageStack
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 0
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
                onRequestClose: root.pageIndex = 0
                onRequestSettings: root.pageIndex = 3
                onRequestQueue: root.queueOpen = !root.queueOpen
            }
            LoginPage { }
            SettingsPage { }
        }

        // 迷你播放条：贴底浮动胶囊（Alger 风格：圆角、左右留白、圆形封面），位于页面与底部标签栏之间
        Rectangle {
            Layout.fillWidth: true
            // 横屏用竖排导航时高度更金贵，胶囊压到 58
            Layout.preferredHeight: root.sideNav ? 58 : 64
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.topMargin: 4
            Layout.bottomMargin: 4
            radius: 18
            color: cCard
            border.color: cLine
            border.width: 1
            // 还没选中任何一首歌时整条隐藏：空胶囊占着 58dp 高度只是噪音
            visible: root.pageIndex !== 1 && root.pageIndex !== 4
                     && app.currentTitle.length > 0

            // 底部内嵌细进度（胶囊内缩 14，避免圆角处溢出）
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 2
                height: 2
                radius: 1
                width: (parent.width - 28) * (app.duration > 0 ? Math.min(1, app.position / app.duration) : 0)
                color: cPrimary
                visible: app.currentTitle.length > 0
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: theme.spaceSm

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    RowLayout {
                        anchors.fill: parent
                        spacing: 10

                        Rectangle {
                            Layout.preferredWidth: 46
                            Layout.preferredHeight: 46
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

                Rectangle {
                    Layout.preferredWidth: theme.touchMin
                    Layout.preferredHeight: theme.touchMin
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
                    Layout.preferredWidth: theme.touchMin
                    Layout.preferredHeight: theme.touchMin
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
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            visible: !root.sideNav
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
        color: cCard
        clip: true

        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 1
            color: cLine
        }

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

    // 页面切换的轻微呼吸（与桌面外壳同款；不能放进 StackLayout）
    SequentialAnimation {
        id: pageFade
        NumberAnimation { target: pageStack; property: "opacity"; to: 0.6; duration: 70 }
        NumberAnimation { target: pageStack; property: "opacity"; to: 1; duration: 160 }
    }

    // MV 沉浸式覆盖层：盖住底部标签栏、迷你播放条与队列面板，
    // 只保留 pageIndex=4 状态位，Android 返回键仍走 handleBack 逐级回退
    MvPage {
        id: mvPage
        anchors.fill: parent
        z: 1000
        visible: root.pageIndex === 4
        onRequestClose: root.pageIndex = root.mvReturnPage
    }

    // app.mvOpened 由 AppController 解析完 MV 地址后发出：
    // 收起队列 → 记住来路页 → 切到 MV 覆盖层 → 把地址交给页面
    Connections {
        target: app
        function onMvOpened(mv) {
            root.queueOpen = false
            if (root.pageIndex !== 4) root.mvReturnPage = root.pageIndex
            root.pageIndex = 4
            mvPage.open(mv)
        }
    }
}
