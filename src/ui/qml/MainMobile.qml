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

    // Android 返回键：队列面板 → 播放页 → 音乐库 → 首页，逐级回退
    function handleBack() {
        if (queueOpen) {
            queueOpen = false
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

    Rectangle {
        anchors.fill: parent
        color: cBg
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        StackLayout {
            id: pageStack
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.pageIndex
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

        // 迷你播放条：贴底，位于页面与底部标签栏之间；进入全屏播放页（pageIndex=1）时隐藏，避免与播放页控件重复
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 66
            color: cCard
            visible: root.pageIndex !== 1

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 1
                color: cLine
            }

            Rectangle {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                height: 2
                width: parent.width * (app.duration > 0 ? Math.min(1, app.position / app.duration) : 0)
                color: cPrimary
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
                            radius: theme.radiusSm
                            color: cPrimarySoft
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

        // 底部标签栏
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
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
                                size: 17
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

                        Rectangle {
                            visible: checked
                            anchors.top: parent.top
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: 18
                            height: 2
                            radius: 1
                            color: cPrimary
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
}
