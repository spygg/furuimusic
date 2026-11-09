import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// 桌面宽屏底部播放栏：所有状态都来自 app，不持有播放逻辑
Rectangle {
    id: root
    signal openPlayer()
    signal toggleQueue()

    readonly property bool isDark: theme.dark
    readonly property color cCard: theme.surface
    readonly property color cText: theme.text
    readonly property color cSub: theme.subText
    readonly property color cPrimary: theme.accent
    readonly property color cLine: theme.line
    readonly property color cSoft: theme.accentSoft

    implicitHeight: 88
    color: cCard

    function mmss(ms) {
        if (isNaN(ms) || ms < 0)
            ms = 0
        var t = Math.floor(ms / 1000)
        var m = Math.floor(t / 60)
        var s = t % 60
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1
        color: cLine
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 12

        Item {
            Layout.preferredWidth: 260
            Layout.fillHeight: true
            RowLayout {
                anchors.fill: parent
                spacing: 10
                Rectangle {
                    Layout.preferredWidth: 52
                    Layout.preferredHeight: 52
                    radius: 10
                    color: cSoft
                    Text {
                        anchors.centerIn: parent
                        text: "♪"
                        color: cPrimary
                        font.pixelSize: 20
                        visible: barCover.status !== Image.Ready
                    }
                    Image {
                        id: barCover
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
                        text: app.currentTitle.length > 0 ? app.currentTitle : "尚未开始播放"
                        color: cText
                        font.pixelSize: 14
                        font.bold: true
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        text: app.currentArtist.length > 0 ? app.currentArtist : "从搜索或音乐库选歌"
                        color: cSub
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                    Row {
                        spacing: 6
                        visible: app.currentSourceLabel.length > 0 || app.substituteBadge.length > 0
                        Rectangle {
                            visible: app.currentSourceLabel.length > 0
                            width: barSource.implicitWidth + 10
                            height: 16
                            radius: 8
                            color: cSoft
                            Text {
                                id: barSource
                                anchors.centerIn: parent
                                text: app.currentSourceLabel
                                color: cPrimary
                                font.pixelSize: 9
                            }
                        }
                        Rectangle {
                            visible: app.substituteBadge.length > 0
                            width: barSub.implicitWidth + 10
                            height: 16
                            radius: 8
                            color: theme.vip
                            Text {
                                id: barSub
                                anchors.centerIn: parent
                                text: app.substituteBadge
                                color: "#FFFFFF"
                                font.pixelSize: 9
                            }
                        }
                    }
                }
            }
            MouseArea {
                anchors.fill: parent
                onClicked: root.openPlayer()
            }
        }

        RowLayout {
            spacing: 8
            Layout.alignment: Qt.AlignVCenter

            ControlButton {
                icon: "prev"
                textColor: cSub
                accentColor: cPrimary
                softColor: cSoft
                lineColor: cLine
                onClicked: app.previous()
            }
            Rectangle {
                width: 46
                height: 46
                radius: 23
                gradient: Gradient {
                    GradientStop { position: 0.0; color: theme.heroStart }
                    GradientStop { position: 1.0; color: theme.heroEnd }
                }
                GlyphIcon {
                    anchors.centerIn: parent
                    name: app.playing ? "pause" : "play"
                    size: 18
                    color: "#FFFFFF"
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: app.togglePause()
                }
            }
            ControlButton {
                icon: "next"
                textColor: cSub
                accentColor: cPrimary
                softColor: cSoft
                lineColor: cLine
                onClicked: app.next()
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Slider {
                id: progress
                Layout.fillWidth: true
                from: 0
                to: app.duration > 0 ? app.duration : 1
                value: app.position
                onMoved: app.seek(value)
                onPressedChanged: {
                    if (!pressed)
                        value = Qt.binding(function () { return app.position })
                }
                background: Rectangle {
                    x: progress.leftPadding
                    y: progress.topPadding + progress.availableHeight / 2 - height / 2
                    width: progress.availableWidth
                    height: 4
                    radius: 2
                    color: cLine
                    Rectangle {
                        width: progress.visualPosition * parent.width
                        height: parent.height
                        radius: 2
                        color: cPrimary
                    }
                }
                handle: Rectangle {
                    x: progress.leftPadding + progress.visualPosition * (progress.availableWidth - width)
                    y: progress.topPadding + progress.availableHeight / 2 - height / 2
                    width: 12
                    height: 12
                    radius: 6
                    color: cPrimary
                    border.color: cCard
                    border.width: 2
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Text { text: root.mmss(app.position); color: cSub; font.pixelSize: 11 }
                Item { Layout.fillWidth: true }
                Text { text: root.mmss(app.duration); color: cSub; font.pixelSize: 11 }
            }
        }

        ControlButton {
            text: app.currentFavorite ? "已收藏" : "收藏"
            highlighted: app.currentFavorite
            textColor: cSub
            accentColor: cPrimary
            softColor: cSoft
            lineColor: cLine
            onClicked: app.toggleFavoriteCurrent()
        }

        ControlButton {
            text: app.playModeName
            textColor: cSub
            accentColor: cPrimary
            softColor: cSoft
            lineColor: cLine
            onClicked: app.cyclePlayMode()
        }

        RowLayout {
            spacing: 6
            Text { text: "音量"; color: cSub; font.pixelSize: 11 }
            Slider {
                id: volume
                Layout.preferredWidth: 96
                from: 0
                to: 100
                value: app.volume
                onMoved: app.setVolume(value)
                onPressedChanged: {
                    if (!pressed)
                        value = Qt.binding(function () { return app.volume })
                }
            }
        }

        ControlButton {
            text: "队列"
            textColor: cSub
            accentColor: cPrimary
            softColor: cSoft
            lineColor: cLine
            onClicked: root.toggleQueue()
        }
    }
}
