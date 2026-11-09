import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// 桌面宽屏外壳：自绘标题栏 + 左侧导航 + 页面区 + 可开合队列抽屉 + 底部播放栏
Item {
    id: root

    readonly property color cBg: theme.bg
    readonly property color cCard: theme.surface
    readonly property color cText: theme.text
    readonly property color cSub: theme.subText
    readonly property color cPrimary: theme.accent
    readonly property color cLine: theme.line
    readonly property color cSoft: theme.accentSoft

    property int pageIndex: 0
    property bool queueOpen: false

    readonly property var pageTitles: ["发现音乐", "我的音乐", "平台账号", "正在播放", "设置"]
    readonly property var pageSubtitles: [
        "聚合搜索各平台歌曲与歌单",
        "跨平台歌单、收藏与历史",
        "每个平台独立登录与同步",
        "播放控制、歌词与音质",
        "主题、下载与账号接口"
    ]
    readonly property var navIcons: ["search", "music", "user", "play", "settings"]
    readonly property var navLabels: ["搜索", "音乐库", "账号", "播放", "设置"]

    Rectangle {
        anchors.fill: parent
        color: cBg
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TitleBar {
            Layout.fillWidth: true
            onThemeToggleRequested: theme.mode = theme.dark ? "light" : "dark"
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // 左侧导航
            Rectangle {
                Layout.preferredWidth: 214
                Layout.fillHeight: true
                color: cCard

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: theme.spaceLg
                    spacing: theme.spaceXs

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Rectangle {
                            Layout.preferredWidth: 38
                            Layout.preferredHeight: 38
                            radius: theme.radiusMd
                            gradient: Gradient {
                                GradientStop { position: 0.0; color: theme.heroStart }
                                GradientStop { position: 1.0; color: theme.heroEnd }
                            }
                            Text {
                                anchors.centerIn: parent
                                text: "♪"
                                color: "#FFFFFF"
                                font.pixelSize: 18
                                font.bold: true
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Text {
                                Layout.fillWidth: true
                                text: app.appName
                                color: cText
                                font.pixelSize: theme.fontLg
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: "多平台音乐聚合"
                                color: cSub
                                font.pixelSize: theme.fontXs
                                elide: Text.ElideRight
                            }
                        }
                    }

                    Item { Layout.preferredHeight: theme.spaceSm }

                    Repeater {
                        model: root.navLabels.length
                        delegate: NavButton {
                            Layout.fillWidth: true
                            text: root.navLabels[index]
                            icon: root.navIcons[index]
                            active: root.pageIndex === index
                            onClicked: root.pageIndex = index
                        }
                    }

                    Item { Layout.fillHeight: true }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 56
                        radius: theme.radiusMd
                        color: cSoft
                        RowLayout {
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
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                color: cLine
            }

            // 页面区
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 56
                    color: cCard

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: cLine
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: theme.spaceLg
                        anchors.rightMargin: theme.spaceLg
                        spacing: theme.spaceMd

                        ColumnLayout {
                            spacing: 1
                            Text {
                                text: root.pageTitles[root.pageIndex]
                                color: cText
                                font.pixelSize: theme.fontLg
                                font.bold: true
                            }
                            Text {
                                text: root.pageSubtitles[root.pageIndex]
                                color: cSub
                                font.pixelSize: theme.fontXs
                            }
                        }

                        Item { Layout.fillWidth: true }

                        // 主题模式三态：跟随系统 / 浅色 / 深色
                        Row {
                            spacing: 2
                            Repeater {
                                model: [
                                    { key: "system", label: "跟随系统" },
                                    { key: "light", label: "浅色" },
                                    { key: "dark", label: "深色" }
                                ]
                                delegate: Rectangle {
                                    width: modeLabel.implicitWidth + 18
                                    height: 28
                                    radius: 14
                                    color: theme.mode === modelData.key ? cSoft : "transparent"
                                    border.width: theme.mode === modelData.key ? 1 : 0
                                    border.color: cPrimary
                                    Behavior on color { ColorAnimation { duration: 120 } }
                                    Text {
                                        id: modeLabel
                                        anchors.centerIn: parent
                                        text: modelData.label
                                        color: theme.mode === modelData.key ? cPrimary : cSub
                                        font.pixelSize: theme.fontXs
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        onClicked: theme.mode = modelData.key
                                    }
                                }
                            }
                        }
                    }
                }

                StackLayout {
                    id: pageStack
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: root.pageIndex
                    onCurrentIndexChanged: pageFade.restart()
                    SequentialAnimation {
                        id: pageFade
                        NumberAnimation { target: pageStack; property: "opacity"; to: 0.55; duration: 80 }
                        NumberAnimation { target: pageStack; property: "opacity"; to: 1; duration: 180 }
                    }

                    SearchPage { }
                    LibraryPage { }
                    LoginPage { }
                    PlayerPage { }
                    SettingsPage { }
                }
            }

            // 队列抽屉
            Rectangle {
                visible: root.queueOpen
                Layout.preferredWidth: root.queueOpen ? 320 : 0
                Layout.fillHeight: true
                color: cCard

                Rectangle {
                    width: 1
                    height: parent.height
                    color: cLine
                }
                QueueDrawer {
                    anchors.left: parent.left
                    anchors.leftMargin: 1
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    onCloseRequested: root.queueOpen = false
                }
            }
        }

        PlayBar {
            Layout.fillWidth: true
            onOpenPlayer: root.pageIndex = 3
            onToggleQueue: root.queueOpen = !root.queueOpen
        }
    }
}
