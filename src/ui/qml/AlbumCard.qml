import QtQuick 2.15
import QtQuick.Layouts 1.15

// 封面驱动的专辑卡片：歌单/榜单/搜索结果的网格视图单元。
// 与 SongRow 列表视图共存，同一个 model，只是呈现方式不同。
Item {
    id: root

    property string title: ""
    property string subtitle: ""
    property string cover: ""
    property string badge: ""
    property color accent: theme.accent
    property color cardColor: theme.card
    property color hoverColor: theme.cardHover
    property color textColor: theme.text
    property color subColor: theme.subText
    property color lineColor: theme.line

    signal clicked()

    // 封面宽度 = delegate 宽 - card 的 6*2 margins - ColumnLayout 的 10*2 margins
    readonly property real innerWidth: Math.max(40, width - 32)

    Rectangle {
        id: card
        anchors.fill: parent
        anchors.margins: 6
        radius: theme.radiusMd
        color: hover.hovered ? root.hoverColor : root.cardColor
        border.color: root.lineColor
        border.width: 1
        scale: hover.pressed ? 0.97 : (hover.hovered ? 1.02 : 1.0)
        Behavior on scale { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        Behavior on color { ColorAnimation { duration: 160 } }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8

            // 正方形封面区
            Item {
                id: coverArea
                Layout.fillWidth: true
                Layout.preferredHeight: root.innerWidth
                clip: true

                Rectangle {
                    id: coverBox
                    anchors.fill: parent
                    radius: theme.radiusSm
                    gradient: Gradient {
                        GradientStop {
                            position: 0.0
                            color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.45)
                        }
                        GradientStop {
                            position: 1.0
                            color: Qt.rgba(root.accent.r, root.accent.g, root.accent.b, 0.16)
                        }
                    }

                    // 兜底纹样：无封面时不是一块空白，而是唱片质感的同心圆
                    Repeater {
                        model: 3
                        Rectangle {
                            width: coverBox.width * (0.72 - index * 0.20)
                            height: width
                            radius: width / 2
                            anchors.centerIn: parent
                            color: "transparent"
                            border.color: Qt.rgba(1, 1, 1, 0.14)
                            border.width: 1
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        text: "♪"
                        color: "#FFFFFF"
                        font.pixelSize: Math.max(22, coverBox.width * 0.20)
                        opacity: coverImage.opacity < 0.5 ? 0.9 : 0
                        Behavior on opacity { NumberAnimation { duration: 180 } }
                    }
                }

                Image {
                    id: coverImage
                    anchors.fill: parent
                    source: root.cover
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    cache: true
                    opacity: status === Image.Ready ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
                }

                // 封面底部渐暗，保证角标/播放按钮在浅色封面上也看得清
                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 46
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0) }
                        GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.42) }
                    }
                }

                // 平台角标
                Rectangle {
                    visible: root.badge.length > 0
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 8
                    width: Math.min(badgeText.implicitWidth + 14, coverArea.width - 16)
                    height: 22
                    radius: 11
                    color: Qt.rgba(0, 0, 0, 0.46)
                    Text {
                        id: badgeText
                        anchors.centerIn: parent
                        text: root.badge
                        color: "#FFFFFF"
                        font.pixelSize: theme.fontSm - 1
                        elide: Text.ElideRight
                    }
                }

                // hover 时浮现的播放按钮
                Rectangle {
                    width: 36
                    height: 36
                    radius: 18
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 8
                    color: root.accent
                    opacity: hover.hovered ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
                    Text {
                        anchors.centerIn: parent
                        text: "▶"
                        color: "#FFFFFF"
                        font.pixelSize: 14
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                text: root.title
                color: root.textColor
                font.pixelSize: 13
                font.bold: true
                elide: Text.ElideRight
            }

            Text {
                Layout.fillWidth: true
                text: root.subtitle
                color: root.subColor
                font.pixelSize: 11
                elide: Text.ElideRight
            }

            Item { Layout.fillHeight: true }
        }

        MouseArea {
            id: hover
            anchors.fill: parent
            hoverEnabled: true
            onClicked: root.clicked()
        }
    }
}
