import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// 移动端外壳：顶部品牌栏 + 迷你播放条 + 页面区 + 底部图标标签栏
// 与桌面共用同一批页面；这里只做窄屏排布，不出现任何窗口按钮
Item {
    id: root

    readonly property color cBg: theme.bg
    readonly property color cCard: theme.surface
    readonly property color cText: theme.text
    readonly property color cSub: theme.subText
    readonly property color cPrimary: theme.accent
    readonly property color cVip: theme.vip
    readonly property color cLine: theme.line
    readonly property color cPrimarySoft: theme.accentSoft

    property int pageIndex: 0

    readonly property var tabIcons: ["search", "music", "user", "play", "settings"]
    readonly property var tabLabels: ["搜索", "音乐库", "账号", "播放", "设置"]

    Rectangle {
        anchors.fill: parent
        color: cBg
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // 品牌栏：右侧放主题切换，不暴露窗口按钮
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 52
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
                anchors.leftMargin: theme.spaceMd
                anchors.rightMargin: theme.spaceMd
                spacing: 10

                Rectangle {
                    Layout.preferredWidth: 30
                    Layout.preferredHeight: 30
                    radius: theme.radiusSm
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: theme.heroStart }
                        GradientStop { position: 1.0; color: theme.heroEnd }
                    }
                    Text {
                        anchors.centerIn: parent
                        text: "♪"
                        color: "#FFFFFF"
                        font.pixelSize: 15
                        font.bold: true
                    }
                }

                ColumnLayout {
                    spacing: 0
                    Text {
                        text: app.appName
                        color: cText
                        font.pixelSize: 15
                        font.bold: true
                    }
                    Text {
                        text: "多平台音乐聚合"
                        color: cSub
                        font.pixelSize: 9
                    }
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    width: 34
                    height: 30
                    radius: 15
                    color: "transparent"
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: theme.dark ? "sun" : "moon"
                        size: 15
                        color: cSub
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: theme.mode = theme.dark ? "light" : "dark"
                    }
                }

                Rectangle {
                    width: 68
                    height: 28
                    radius: 14
                    color: cPrimarySoft
                    Text {
                        anchors.centerIn: parent
                        text: "账号"
                        color: cPrimary
                        font.pixelSize: theme.fontSm
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.pageIndex = 2
                    }
                }
            }
        }

        // 迷你播放条
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 66
            color: cCard

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
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
                        onClicked: root.pageIndex = 3
                    }
                }

                Rectangle {
                    visible: app.substituteBadge.length > 0
                    Layout.alignment: Qt.AlignVCenter
                    width: badgeText.implicitWidth + 12
                    height: 20
                    radius: 10
                    color: cVip
                    Text {
                        id: badgeText
                        anchors.centerIn: parent
                        text: app.substituteBadge
                        color: "#FFFFFF"
                        font.pixelSize: theme.fontXs
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 38
                    Layout.preferredHeight: 38
                    Layout.alignment: Qt.AlignVCenter
                    radius: 19
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
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 32
                    Layout.alignment: Qt.AlignVCenter
                    radius: 16
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
}
