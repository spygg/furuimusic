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
    readonly property color cLine: theme.line
    readonly property color cSoft: theme.accentSoft

    property int pageIndex: 0
    property bool queueOpen: false

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

            // 左侧导航（品牌只保留在标题栏一处，这里直接进入导航）
            Rectangle {
                Layout.preferredWidth: 200
                Layout.fillHeight: true
                color: cCard

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: theme.spaceLg
                    spacing: theme.spaceXs

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

            // 页面区：各页面自带主标题，桌面不再额外叠一层重复的标题栏
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
