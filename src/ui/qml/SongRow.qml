import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// 统一歌曲行：搜索结果 / 收藏 / 历史 / 队列都复用它，避免每页各写一套
Rectangle {
    id: root

    property int rowNumber: -1
    property string title: ""
    property string artist: ""
    property string album: ""
    property string cover: ""
    property string sourceName: ""
    property string sourceId: ""
    property color accent: "#ec4141"
    property int duration: 0
    property bool vip: false
    property bool favorite: false
    property bool playing: false
    property bool showAdd: true
    property bool showFavorite: true
    property bool showDownload: false
    property bool showRemove: false

    signal playRequested()
    signal favoriteRequested()
    signal addRequested()
    signal downloadRequested()
    signal removeRequested()

    readonly property color cCard: theme.card
    readonly property color cHover: theme.cardHover
    readonly property color cText: theme.text
    readonly property color cSub: theme.subText
    readonly property color cLine: theme.line
    readonly property color cSoft: Qt.rgba(accent.r, accent.g, accent.b, 0.12)

    implicitHeight: 68
    radius: theme.radiusMd
    color: hover.hovered ? cHover : cCard
    border.width: playing ? 1 : 1
    border.color: playing ? accent : cLine

    Behavior on color { ColorAnimation { duration: 120 } }

    function mmss(ms) {
        if (isNaN(ms) || ms < 0)
            ms = 0
        var t = Math.floor(ms / 1000)
        var m = Math.floor(t / 60)
        var s = t % 60
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    MouseArea {
        id: hover
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        onClicked: root.playRequested()
    }

    Rectangle {
        id: accentBar
        visible: root.playing
        width: 3
        height: parent.height - 20
        radius: 1.5
        anchors.left: parent.left
        anchors.leftMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        color: accent
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 10
        spacing: 10

        Text {
            visible: root.rowNumber >= 0
            Layout.preferredWidth: 22
            text: root.playing ? "▶" : (root.rowNumber + 1)
            color: root.playing ? accent : cSub
            font.pixelSize: 12
            horizontalAlignment: Text.AlignHCenter
        }

        // 封面：没有封面时用平台色兜底，避免出现空白方块
        Rectangle {
            Layout.preferredWidth: 46
            Layout.preferredHeight: 46
            radius: 10
            color: cSoft
            Text {
                anchors.centerIn: parent
                text: "♪"
                color: accent
                font.pixelSize: 20
                visible: coverImage.status !== Image.Ready
            }
            Image {
                id: coverImage
                anchors.fill: parent
                anchors.margins: 1
                source: root.cover
                fillMode: Image.PreserveAspectCrop
                opacity: status === Image.Ready ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
                asynchronous: true
                cache: true
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 3

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Text {
                    Layout.fillWidth: true
                    text: root.title.length > 0 ? root.title : "未知曲目"
                    color: cText
                    font.pixelSize: 14
                    font.bold: true
                    elide: Text.ElideRight
                }
                Rectangle {
                    visible: root.vip
                    width: 30
                    height: 16
                    radius: 4
                    color: theme.vip
                    Text {
                        anchors.centerIn: parent
                        text: "VIP"
                        color: "#FFFFFF"
                        font.pixelSize: 9
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Text {
                    Layout.fillWidth: true
                    text: root.artist + (root.album.length > 0 ? " · " + root.album : "")
                    color: cSub
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
                Rectangle {
                    visible: root.sourceName.length > 0
                    width: sourceLabel.implicitWidth + 12
                    height: 18
                    radius: 9
                    color: cSoft
                    Text {
                        id: sourceLabel
                        anchors.centerIn: parent
                        text: root.sourceName
                        color: accent
                        font.pixelSize: 10
                    }
                }
                Text {
                    visible: root.duration > 0
                    text: root.mmss(root.duration)
                    color: cSub
                    font.pixelSize: 11
                }
            }
        }

        // 触屏端也保留图标按钮，只去掉文字说明
        Rectangle {
            visible: root.showFavorite
            Layout.preferredWidth: 30
            Layout.preferredHeight: 30
            radius: 15
            color: root.favorite ? cSoft : "transparent"
            border.color: root.favorite ? accent : cLine
            Text {
                anchors.centerIn: parent
                text: root.favorite ? "♥" : "♡"
                color: root.favorite ? accent : cSub
                font.pixelSize: 14
            }
            MouseArea {
                anchors.fill: parent
                onClicked: root.favoriteRequested()
            }
        }

        Rectangle {
            visible: root.showAdd
            Layout.preferredWidth: 30
            Layout.preferredHeight: 30
            radius: 15
            color: "transparent"
            border.color: cLine
            Text {
                anchors.centerIn: parent
                text: "＋"
                color: cSub
                font.pixelSize: 15
            }
            MouseArea {
                anchors.fill: parent
                onClicked: root.addRequested()
            }
        }

        Rectangle {
            visible: root.showDownload
            Layout.preferredWidth: 30
            Layout.preferredHeight: 30
            radius: 15
            color: "transparent"
            border.color: cLine
            Text {
                anchors.centerIn: parent
                text: "↓"
                color: cSub
                font.pixelSize: 14
            }
            MouseArea {
                anchors.fill: parent
                onClicked: root.downloadRequested()
            }
        }

        Rectangle {
            visible: root.showRemove
            Layout.preferredWidth: 30
            Layout.preferredHeight: 30
            radius: 15
            color: "transparent"
            border.color: cLine
            Text {
                anchors.centerIn: parent
                text: "×"
                color: cSub
                font.pixelSize: 14
            }
            MouseArea {
                anchors.fill: parent
                onClicked: root.removeRequested()
            }
        }
    }
}
