import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Item {
    id: root

    readonly property bool isDark: theme.dark
    readonly property color cBg: theme.bg
    readonly property color cCard: theme.card
    readonly property color cText: theme.text
    readonly property color cSub: theme.subText
    readonly property color cPrimary: theme.accent
    readonly property color cVip: theme.vip
    readonly property color cLine: theme.line
    readonly property color cPrimarySoft: theme.accentSoft
    readonly property color cPrimaryPress: Qt.darker(theme.accent, 1.18)
    readonly property color cPrimaryHover: Qt.lighter(theme.accent, 1.1)

    function mmss(ms) {
        if (isNaN(ms) || ms < 0)
            ms = 0
        var t = Math.floor(ms / 1000)
        var m = Math.floor(t / 60)
        var s = t % 60
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        Item {
            id: heroArea
            Layout.fillWidth: true
            Layout.preferredHeight: 226
            clip: true

            // 封面驱动的氛围背景：放大的专辑图 + 渐变遮罩，替代大片纯色
            Image {
                id: coverGlow
                anchors.fill: parent
                source: app.currentCover
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                scale: 1.6
                opacity: playerCover.status === Image.Ready ? (isDark ? 0.24 : 0.15) : 0
                Behavior on opacity { NumberAnimation { duration: 360; easing.type: Easing.OutCubic } }
            }
            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    GradientStop { position: 0.0; color: Qt.rgba(cBg.r, cBg.g, cBg.b, 0.30) }
                    GradientStop { position: 0.55; color: Qt.rgba(cBg.r, cBg.g, cBg.b, 0.10) }
                    GradientStop { position: 1.0; color: cBg }
                }
            }

            // 氛围光斑
            Rectangle {
                width: 260
                height: 160
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                radius: 80
                color: cPrimary
                opacity: isDark ? 0.10 : 0.07
            }
            Rectangle {
                width: 140
                height: 140
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                anchors.horizontalCenterOffset: -110
                radius: 70
                color: theme.accentAlt
                opacity: isDark ? 0.12 : 0.10
            }

            // 旋转黑胶：播放时缓慢转动，暂停定格
            Item {
                id: vinyl
                width: 208
                height: 208
                anchors.centerIn: parent
                rotation: 0
                RotationAnimation on rotation {
                    running: app.playing
                    loops: Animation.Infinite
                    from: 0
                    to: 360
                    duration: 18000
                }
                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: isDark ? "#16161B" : "#22222A"
                    border.color: Qt.rgba(cText.r, cText.g, cText.b, 0.14)
                    border.width: 1
                }
                Repeater {
                    model: 5
                    Rectangle {
                        anchors.centerIn: parent
                        width: 198 - index * 30
                        height: width
                        radius: width / 2
                        color: "transparent"
                        border.color: Qt.rgba(1, 1, 1, 0.055)
                        border.width: 1
                    }
                }
                // 让旋转在视觉上可读的锚点
                Rectangle {
                    x: 14
                    y: 101
                    width: 6
                    height: 6
                    radius: 3
                    color: Qt.rgba(cPrimary.r, cPrimary.g, cPrimary.b, 0.85)
                }
            }

            // 封面投影
            Rectangle {
                width: 182
                height: 182
                anchors.centerIn: parent
                anchors.verticalCenterOffset: 9
                radius: 14
                color: Qt.rgba(0, 0, 0, isDark ? 0.38 : 0.14)
            }

            Rectangle {
                id: coverBox
                width: 182
                height: 182
                anchors.centerIn: parent
                radius: 14
                gradient: Gradient {
                    GradientStop { position: 0.0; color: theme.accentSoft }
                    GradientStop { position: 1.0; color: theme.cardHover }
                }
                Text {
                    anchors.centerIn: parent
                    text: "♪"
                    color: "#FFFFFF"
                    font.pixelSize: 34
                    visible: playerCover.status !== Image.Ready
                }
            }
            Image {
                id: playerCover
                width: 182
                height: 182
                anchors.centerIn: parent
                source: app.currentCover
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                opacity: status === Image.Ready ? 1 : 0
                scale: status === Image.Ready ? 1 : 0.90
                Behavior on opacity { NumberAnimation { duration: 280; easing.type: Easing.OutCubic } }
                Behavior on scale { NumberAnimation { duration: 340; easing.type: Easing.OutBack } }
            }
        }

        Text {
            Layout.fillWidth: true
            text: app.currentTitle.length > 0 ? app.currentTitle : "暂无播放"
            color: cText
            font.pixelSize: 18
            font.bold: true
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
        }
        Text {
            Layout.fillWidth: true
            text: app.currentArtist
            color: cSub
            font.pixelSize: 13
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
        }
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 8
            Text {
                text: app.currentSourceLabel
                color: cPrimary
                font.pixelSize: 11
                visible: text.length > 0
            }
            Rectangle {
                visible: app.substituteBadge.length > 0
                width: playerBadgeText.implicitWidth + 12
                height: 20
                radius: 4
                color: cVip
                Text {
                    id: playerBadgeText
                    anchors.centerIn: parent
                    text: app.substituteBadge
                    color: "#FFFFFF"
                    font.pixelSize: 10
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Text { text: root.mmss(app.position); color: cSub; font.pixelSize: 11 }
            Slider {
                id: progress
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                from: 0
                to: app.duration > 0 ? app.duration : 1
                value: app.position
                onMoved: app.seek(value)
                // 拖动中不被 positionChanged 覆盖，松手后重新建立绑定
                onPressedChanged: {
                    if (!pressed)
                        value = Qt.binding(function () { return app.position })
                }
                background: Rectangle {
                    implicitHeight: 4
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
                    implicitWidth: 14
                    implicitHeight: 14
                    radius: 7
                    color: cPrimary
                }
            }
            Text { text: root.mmss(app.duration); color: cSub; font.pixelSize: 11 }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            spacing: 18

            Rectangle {
                Layout.preferredWidth: 44
                Layout.preferredHeight: 44
                radius: 22
                color: cPrimarySoft
                border.color: cLine
                Text { anchors.centerIn: parent; text: "上一首"; color: cPrimary; font.pixelSize: 10 }
                MouseArea { anchors.fill: parent; onClicked: app.previous() }
            }
            Rectangle {
                id: playButton
                Layout.preferredWidth: 56
                Layout.preferredHeight: 56
                radius: 28
                gradient: Gradient {
                    GradientStop { position: 0.0; color: theme.heroStart }
                    GradientStop { position: 1.0; color: theme.heroEnd }
                }
                scale: playMouse.pressed ? 0.90 : 1.0
                Behavior on scale { NumberAnimation { duration: 130; easing.type: Easing.OutCubic } }
                Text {
                    anchors.centerIn: parent
                    text: app.playing ? "暂停" : "播放"
                    color: "#FFFFFF"
                    font.pixelSize: 13
                }
                MouseArea { id: playMouse; anchors.fill: parent; onClicked: app.togglePause() }
            }
            Rectangle {
                Layout.preferredWidth: 44
                Layout.preferredHeight: 44
                radius: 22
                color: cPrimarySoft
                border.color: cLine
                Text { anchors.centerIn: parent; text: "下一首"; color: cPrimary; font.pixelSize: 10 }
                MouseArea { anchors.fill: parent; onClicked: app.next() }
            }
            Button {
                id: modeBtn
                text: app.playModeName
                Layout.preferredWidth: 82
                Layout.preferredHeight: 34
                background: Rectangle { radius: 6; color: cPrimarySoft; border.color: cLine }
                contentItem: Text { text: modeBtn.text; color: cPrimary; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                onClicked: app.cyclePlayMode()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Text { text: "音量"; color: cSub; font.pixelSize: 12 }
            Slider {
                id: volumeSlider
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                from: 0
                to: 100
                stepSize: 1
                value: app.volume
                onMoved: app.setVolume(value)
                onPressedChanged: {
                    if (!pressed)
                        value = Qt.binding(function () { return app.volume })
                }
                background: Rectangle {
                    implicitHeight: 4
                    radius: 2
                    color: cLine
                    Rectangle {
                        width: volumeSlider.visualPosition * parent.width
                        height: parent.height
                        radius: 2
                        color: cPrimary
                    }
                }
                handle: Rectangle {
                    x: volumeSlider.leftPadding + volumeSlider.visualPosition * (volumeSlider.availableWidth - width)
                    y: volumeSlider.topPadding + volumeSlider.availableHeight / 2 - height / 2
                    implicitWidth: 12
                    implicitHeight: 12
                    radius: 6
                    color: cPrimary
                }
            }
            Text { text: app.volume + "%"; color: cSub; font.pixelSize: 12 }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: 10
            color: cCard
            border.color: cLine
            clip: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                Text {
                    id: currentLyric
                    Layout.fillWidth: true
                    text: app.currentLyricLine.length > 0 ? app.currentLyricLine : "暂无歌词"
                    color: cPrimary
                    font.pixelSize: 15
                    font.bold: true
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    onTextChanged: lyricFlash.restart()
                    SequentialAnimation {
                        id: lyricFlash
                        NumberAnimation { target: currentLyric; property: "opacity"; to: 0.3; duration: 90 }
                        NumberAnimation { target: currentLyric; property: "opacity"; to: 1; duration: 200 }
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: cLine }
                Flickable {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: width
                    contentHeight: lyricsBody.implicitHeight
                    clip: true
                    Text {
                        id: lyricsBody
                        width: parent.width
                        text: app.lyricsText.length > 0 ? app.lyricsText : "暂无歌词"
                        color: cSub
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }
        }
    }
}
