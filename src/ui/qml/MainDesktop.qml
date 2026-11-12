import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
// 切页收焦点时用 root.Window.window，缺这个 import 会报 "Cannot read property
// 'window' of undefined"，焦点清理整行静默失效
import QtQuick.Window 2.15

// 桌面宽屏外壳：自绘标题栏 + 左侧导航 + 页面区 + 可开合队列抽屉 + 底部播放栏
Item {
    id: root

    readonly property color cBg: theme.bg
    readonly property color cCard: theme.surface
    readonly property color cText: theme.text
    readonly property color cSub: theme.subText
    readonly property color cLine: theme.line
    readonly property color cSoft: theme.accentSoft

    property int pageIndex: 0
    property bool queueOpen: false
    // 播放页是沉浸页：从它退出时回到进入前的那一页
    property int lastContentPage: 0
    // MV 页同理：从哪页进的，返回键/关闭就回哪页
    property int mvReturnPage: 1
    onPageIndexChanged: {
        // 切页收起软键盘并放掉输入焦点：焦点会活过页面切换，不处理的话
        // 键盘会一直盖在下一个页面上（触屏笔记本/二合一设备上很明显）
        Qt.inputMethod.hide()
        const af = root.Window.window ? root.Window.window.activeFocusItem : null
        if (af) af.focus = false
        if (pageIndex !== 1) {
            lastContentPage = pageIndex
        } else {
            // 进页动画由外壳显式触发：StackLayout 会反复切换子项 visible，
            // 页面自己监听 visibleChanged 会把动画一直重启在起始帧
            playerPage.playEntrance()
        }
    }

    // 返回键 / Esc：队列 → MV → 播放页 → 首页，逐级回退；已在首页则交还系统
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
            pageIndex = lastContentPage
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

    // 搜索已并入音乐库（页头搜索框），导航不再有独立搜索页。
    // 索引与导航顺序严格一致：0 = 曲库（首页） 1 = 播放 2 = 账号 3 = 设置
    readonly property var navIcons: ["music", "play", "user", "settings"]
    readonly property var navLabels: ["曲库", "播放", "账号", "设置"]

    Rectangle {
        anchors.fill: parent
        color: cBg
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TitleBar {
            // 移动端/平板整条隐藏：品牌、主题按钮、拖动区都是桌面概念，
            // 主题切换保留在设置页；隐藏项在 ColumnLayout 里不占高度
            visible: !platform.isMobile
            Layout.fillWidth: true
            onThemeToggleRequested: theme.mode = theme.dark ? "light" : "dark"
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // 左侧导航（品牌只保留在标题栏一处，这里直接进入导航）
            // 用 Flickable 承载：车机横屏高度较矮时内容超出也不裁切（可滚动查看全部分项），
            // 高度充足时 fillHeight 占位把底部信息条顶到底部。
            // 平板/窄窗口：左侧只保留图标（参考洛雪平板版），栏宽从 200 收到 68，
            // 省下的横向空间全部给内容区；宽屏桌面仍显示图标 + 文案
            Rectangle {
                id: rail
                readonly property bool compactNav: root.width < 1100
                // 车机/横屏平板高度紧张时（窗口不足 560dp）进一步压缩行高与间距，
                // 让 5 个导航项 + 底部状态点都留在视口内，不必滚动
                readonly property bool denseNav: root.height < 560
                Layout.preferredWidth: compactNav ? 68 : 200
                Layout.fillHeight: true
                color: cCard

                Flickable {
                    id: sideFlick
                    anchors.fill: parent
                    contentWidth: width
                    contentHeight: sideNav.height
                    clip: true

                    // 用普通 Column 而不是 ColumnLayout：之前 height 绑定
                    // max(implicitHeight, parent.height) 与内部 fillHeight 子项互相依赖，
                    // 每次布局都报 Binding loop。这里弹性高度只依赖 navCol 的自然高度，
                    // 不回指自身，彻底断开循环。
                    Column {
                        id: sideNav
                        x: rail.compactNav ? 8 : theme.spaceLg
                        width: parent.width - x * 2
                        spacing: rail.denseNav ? 2 : theme.spaceXs

                        // 图标模式下曾在栏顶放一枚品牌图标，与下面第一项「曲库」
                        // 的图标完全重复，已去掉；这里只保留一点顶部留白
                        // 导航按钮组（自然高度）
                        Column {
                            id: navCol
                            width: parent.width
                            spacing: rail.denseNav ? 2 : theme.spaceXs

                            Item { width: 1; height: rail.compactNav ? (rail.denseNav ? 8 : 14)
                                                                     : theme.spaceSm }

                            Repeater {
                                model: root.navLabels.length
                                delegate: NavButton {
                                    width: navCol.width
                                    text: root.navLabels[index]
                                    icon: root.navIcons[index]
                                    active: root.pageIndex === index
                                    compact: rail.compactNav
                                    dense: rail.denseNav
                                    onClicked: root.pageIndex = index
                                }
                            }
                        }

                        // 弹性占位：视口高时把 footer 推到底部
                        // sideNav 现在只有 navCol / 本占位 / footerBox 三个子项 → 两段间距
                        Item {
                            width: 1
                            height: Math.max(0, sideFlick.height - navCol.height
                                                - footerBox.height - sideNav.spacing * 2)
                        }

                        // 底部状态：宽屏是「平台 · 版本 + 状态」卡片，
                        // 图标模式收成一枚状态点，悬停看详情
                        Rectangle {
                            id: footerBox
                            width: parent.width
                            height: rail.denseNav ? (rail.compactNav ? 32 : 46)
                                                   : (rail.compactNav ? 40 : 56)
                            radius: rail.compactNav ? 12 : theme.radiusMd
                            color: cSoft

                            RowLayout {
                                visible: !rail.compactNav
                                anchors.fill: parent
                                anchors.margins: theme.spaceMd
                                spacing: theme.spaceSm
                                Rectangle {
                                    Layout.preferredWidth: 8
                                    Layout.preferredHeight: 8
                                    radius: 4
                                    color: theme.success
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Text {
                                        Layout.fillWidth: true
                                        text: platform.platformName + " · v" + app.version
                                        color: cText
                                        font.pixelSize: theme.fontXs
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: app.statusMessage
                                        color: cSub
                                        font.pixelSize: theme.fontXs
                                        elide: Text.ElideRight
                                    }
                                }
                            }

                            Item {
                                visible: rail.compactNav
                                anchors.fill: parent
                                Rectangle {
                                    anchors.centerIn: parent
                                    width: 10
                                    height: 10
                                    radius: 5
                                    color: theme.success
                                }
                                ToolTip.visible: statusHover.containsMouse
                                ToolTip.text: platform.platformName + " · v" + app.version
                                             + (app.statusMessage.length > 0
                                                ? "\n" + app.statusMessage : "")
                                ToolTip.delay: 260
                                MouseArea {
                                    id: statusHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                }
                            }
                        }
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                color: cLine
            }

            // 页面区：各页面自带主标题，桌面不再额外叠一层重复的标题栏
            StackLayout {
                id: pageStack
                Layout.fillWidth: true
                Layout.fillHeight: true
                // index 4 是外壳级 MV 覆盖层，不属于页面栈；钳到 0..3 避免越界，
                // MV 关闭后还能原样看到进入前那一页
                currentIndex: Math.min(root.pageIndex, 3)
                onCurrentIndexChanged: pageFade.restart()

                LibraryPage { }
                // 播放页自己做沉浸背景与全部控件（底部播放栏在这一页隐藏），
                // 版式与交互参照 AlgerMusicPlayer 的播放抽屉。
                // 子项顺序必须与左侧导航一一对应：0 曲库 / 1 播放 / 2 账号 / 3 设置
                Item {
                    PlayerPage {
                        id: playerPage
                        anchors.fill: parent
                        onRequestClose: root.pageIndex = root.lastContentPage
                        onRequestSettings: root.pageIndex = 3
                        onRequestQueue: root.queueOpen = !root.queueOpen
                    }
                }
                LoginPage { }
                SettingsPage { }
            }

            // 队列抽屉
            Rectangle {
                property real drawerWidth: root.queueOpen ? 320 : 0
                Behavior on drawerWidth {
                    NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
                }
                Layout.preferredWidth: drawerWidth
                Layout.fillHeight: true
                color: theme.card
                clip: true

                Rectangle {
                    width: 1
                    height: parent.height
                    color: cLine
                }
                QueueDrawer {
                    width: 319
                    anchors.left: parent.left
                    anchors.leftMargin: 1
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    onCloseRequested: root.queueOpen = false
                }
            }
        }

        PlayBar {
            Layout.fillWidth: true
            // 播放页与 MV 页自带同一套控件，常驻栏让位（否则同屏两套进度/音量）
            visible: root.pageIndex !== 1 && root.pageIndex !== 4
            onOpenPlayer: root.pageIndex = 1
            onOpenSettings: root.pageIndex = 3
            onToggleQueue: root.queueOpen = !root.queueOpen
        }
    }

    // 页面切换时的轻微呼吸：不能放进 StackLayout（布局只接受 Item，
    // 而且随子项切换反复启停会导致动画停在中间帧）
    SequentialAnimation {
        id: pageFade
        NumberAnimation { target: pageStack; property: "opacity"; to: 0.6; duration: 70 }
        NumberAnimation { target: pageStack; property: "opacity"; to: 1; duration: 160 }
    }

    // MV 沉浸式覆盖层：挂在外壳根级而非页面栈里，桌面左侧导航、标题栏、
    // 底部播放栏都会被盖住，与 AlgerMusicPlayer 的全屏 drawer 一致。
    // 只留 pageIndex=4 这个状态位，返回键/关闭仍走原来的逐级回退。
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
