import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// 桌面底部播放栏 —— 版式参照 AlgerMusicPlayer 的 PlayBar：
//   左：圆角封面 + 标题/艺人（整块可点，进播放页，悬停浮出「展开」提示）
//   中：上一首 / 胶囊形播放键 / 下一首（参考项目就是一整颗胶囊，不是圆钮）
//   右：音量（悬停向上弹出竖滑块）、播放模式、收藏、展开播放页、设置、队列
// 顶边一条 2px 细进度：不占版面但保留拖动定位能力（参考项目没有，是本工程的补偿）。
Rectangle {
    id: root
    signal openPlayer()
    signal toggleQueue()
    signal openSettings()

    // 栏高要容纳触控按钮：内容区高度 = 栏高 - 上下边距(12+10)
    implicitHeight: Math.max(76, theme.touchMin + 22)

    readonly property bool isDark: theme.dark
    readonly property color cText: theme.text
    readonly property color cSub: theme.subText
    readonly property color cAccent: theme.accent
    readonly property color softBg: isDark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.05)
    readonly property color softBgHover: isDark ? Qt.rgba(1, 1, 1, 0.14) : Qt.rgba(0, 0, 0, 0.09)

    color: theme.surface

    // 顶缘向上的渐隐投影：让底栏“浮”在内容之上，与 2px 进度线构成层次
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.top
        height: 14
        gradient: Gradient {
            orientation: Gradient.Vertical
            GradientStop {
                position: 0.0
                color: Qt.rgba(theme.shadowTint.r, theme.shadowTint.g, theme.shadowTint.b, 0)
            }
            GradientStop {
                position: 1.0
                color: Qt.rgba(theme.shadowTint.r, theme.shadowTint.g, theme.shadowTint.b,
                               theme.dark ? 0.22 : 0.08)
            }
        }
    }

    function mmss(ms) {
        if (isNaN(ms) || ms < 0)
            ms = 0
        var t = Math.floor(ms / 1000)
        var m = Math.floor(t / 60)
        var s = t % 60
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    function modeIcon() {
        if (app.playMode === 0) return "sequential"
        if (app.playMode === 2) return "repeatOne"
        if (app.playMode === 3) return "shuffle"
        return "repeat"
    }

    // 顶边细进度：高度 2，悬停变 4，整条可拖
    Slider {
        id: topSeek
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 10
        padding: 0
        from: 0
        to: app.duration > 0 ? app.duration : 1
        value: app.position
        onMoved: app.seek(value)
        onPressedChanged: {
            if (!pressed)
                value = Qt.binding(function () { return app.position })
        }
        background: Rectangle {
            x: 0
            y: 0
            width: topSeek.availableWidth
            height: topSeek.pressed || topSeek.hovered ? 4 : 2
            color: theme.line
            Behavior on height { NumberAnimation { duration: 140 } }
            Rectangle {
                width: topSeek.visualPosition * parent.width
                height: parent.height
                color: cAccent
            }
        }
        handle: Rectangle { width: 0; height: 0; color: "transparent" }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 20
        anchors.rightMargin: 16
        anchors.topMargin: 12
        anchors.bottomMargin: 10
        spacing: 8

        // ---------- 左：封面 + 曲名 ----------
        Item {
            Layout.preferredWidth: 258
            Layout.fillHeight: true

            Rectangle {
                anchors.fill: parent
                anchors.margins: -6
                radius: 14
                color: coverArea.containsMouse ? softBg : "transparent"
                Behavior on color { ColorAnimation { duration: 160 } }
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 12

                Item {
                    Layout.preferredWidth: 52
                    Layout.preferredHeight: 52
                    Layout.alignment: Qt.AlignVCenter

                    Rectangle {
                        anchors.fill: parent
                        radius: 14
                        color: theme.accentSoft
                        clip: true

                        Text {
                            anchors.centerIn: parent
                            text: "♪"
                            color: cAccent
                            font.pixelSize: 20
                            visible: barCover.status !== Image.Ready
                        }
                        Image {
                            id: barCover
                            anchors.fill: parent
                            source: app.currentCover
                            sourceSize.width: 104
                            sourceSize.height: 104
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            visible: status === Image.Ready
                        }

                        // 悬停浮出的「展开」遮罩，和参考项目一致
                        Rectangle {
                            anchors.fill: parent
                            radius: 14
                            color: Qt.rgba(0, 0, 0, 0.45)
                            opacity: coverArea.containsMouse ? 1 : 0
                            Behavior on opacity { NumberAnimation { duration: 200 } }
                            Column {
                                anchors.centerIn: parent
                                spacing: 1
                                GlyphIcon {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    name: "expand"
                                    size: 13
                                    color: "#FFFFFF"
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: "展开"
                                    color: "#FFFFFF"
                                    font.pixelSize: theme.fontXs
                                }
                            }
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
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
                        font.pixelSize: theme.fontSm
                        elide: Text.ElideRight
                    }
                }
            }

            MouseArea {
                id: coverArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.openPlayer()
            }
        }

        // ---------- 中：切歌 / 播放 + 时间 ----------
        ColumnLayout {
            Layout.alignment: Qt.AlignHCenter | Qt.AlignVCenter
            Layout.fillWidth: true
            spacing: 1

            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 2

            Item {
                Layout.preferredWidth: theme.touchMin
                Layout.preferredHeight: theme.touchMin
                Layout.alignment: Qt.AlignVCenter
                Rectangle {
                    anchors.fill: parent
                    radius: height / 2
                    color: prevArea.containsMouse ? root.softBg : "transparent"
                    Behavior on color { ColorAnimation { duration: 140 } }
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: "prev"
                        size: 20
                        color: prevArea.containsMouse ? root.cAccent : root.cText
                        Behavior on color { ColorAnimation { duration: 140 } }
                    }
                }
                MouseArea {
                    id: prevArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: app.previous()
                }
            }

            // 胶囊播放键：参考项目 80x48、全圆角
            Rectangle {
                Layout.preferredWidth: 78
                Layout.preferredHeight: theme.touchMin
                Layout.alignment: Qt.AlignVCenter
                radius: height / 2
                color: playArea.pressed ? root.softBgHover
                                        : (playArea.containsMouse ? root.softBgHover : root.softBg)
                Behavior on color { ColorAnimation { duration: 160 } }
                scale: playArea.pressed ? 0.96 : (playArea.containsMouse ? 1.03 : 1.0)
                Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }

                GlyphIcon {
                    anchors.centerIn: parent
                    name: app.playing ? "pause" : "play"
                    size: 20
                    color: root.cText
                }
                MouseArea {
                    id: playArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: app.togglePause()
                }
            }

            Item {
                Layout.preferredWidth: theme.touchMin
                Layout.preferredHeight: theme.touchMin
                Layout.alignment: Qt.AlignVCenter
                Rectangle {
                    anchors.fill: parent
                    radius: height / 2
                    color: nextArea.containsMouse ? root.softBg : "transparent"
                    Behavior on color { ColorAnimation { duration: 140 } }
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: "next"
                        size: 20
                        color: nextArea.containsMouse ? root.cAccent : root.cText
                        Behavior on color { ColorAnimation { duration: 140 } }
                    }
                }
                MouseArea {
                    id: nextArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: app.next()
                }
            }
            }

            // 时间：与参考项目一致，放在播放键下方，跟随顶边进度条一起读
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 6
                Text {
                    text: root.mmss(app.position)
                    color: root.cSub
                    font.pixelSize: theme.fontXs
                }
                Text {
                    text: "/"
                    color: root.cSub
                    font.pixelSize: theme.fontXs
                    opacity: 0.6
                }
                Text {
                    text: root.mmss(app.duration)
                    color: root.cSub
                    font.pixelSize: theme.fontXs
                }
            }
        }

        // ---------- 右：功能按钮 ----------
        RowLayout {
            Layout.alignment: Qt.AlignVCenter
            spacing: 2

            // 音量：悬停向上弹出竖滑块
            Item {
                id: volBox
                Layout.preferredWidth: theme.touchMin
                Layout.preferredHeight: theme.touchMin
                Layout.alignment: Qt.AlignVCenter

                Rectangle {
                    anchors.fill: parent
                    radius: height / 2
                    color: volArea.containsMouse ? root.softBg : "transparent"
                    Behavior on color { ColorAnimation { duration: 140 } }
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: app.volume > 0 ? "volume" : "volumeMute"
                        size: 18
                        color: volArea.containsMouse ? root.cAccent : root.cSub
                        Behavior on color { ColorAnimation { duration: 140 } }
                    }
                }

                MouseArea {
                    id: volArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: app.toggleMute()
                }

                // 弹出面板：贴在按钮正上方，稍微重叠 2px，鼠标从喇叭移上去不会掉出悬停区
                Rectangle {
                    id: volPop
                    width: 36
                    height: 132
                    radius: 18
                    x: (parent.width - width) / 2
                    y: -height + 2
                    color: theme.card
                    border.color: theme.line
                    border.width: 1
                    // 悬停喇叭、悬停面板、拖动滑块时都保持展开
                    opacity: (volArea.containsMouse || volPopArea.containsMouse
                              || volSlider.pressed) ? 1 : 0
                    visible: opacity > 0.01

                    Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

                    MouseArea {
                        id: volPopArea
                        anchors.fill: parent
                        hoverEnabled: true
                        // 只用来维持悬停态，拖动交给里面的 Slider
                    }

                    // 竖向 Slider 的 visualPosition 是翻转值(1 - position)，用它会把
                    // 80% 的滑块画到下方 20%，这里统一用逻辑 position。
                    Slider {
                        id: volSlider
                        orientation: Qt.Vertical
                        anchors.centerIn: parent
                        width: 24
                        height: parent.height - 24
                        padding: 0
                        from: 0
                        to: 100
                        value: app.volume
                        onMoved: app.setVolume(value)
                        onPressedChanged: {
                            if (!pressed)
                                value = Qt.binding(function () { return app.volume })
                        }
                        background: Rectangle {
                            x: (volSlider.width - width) / 2
                            y: 0
                            width: 4
                            height: volSlider.availableHeight
                            radius: 2
                            color: theme.line
                            Rectangle {
                                width: parent.width
                            height: volSlider.position * parent.height
                            radius: 2
                            color: root.cAccent
                                anchors.bottom: parent.bottom
                            }
                        }
                        handle: Rectangle {
                            x: (volSlider.width - width) / 2
                            y: Math.max(0, Math.min(volSlider.availableHeight - height,
                                                     (1 - volSlider.position) * volSlider.availableHeight - height / 2))
                            width: 11; height: 11; radius: 5.5
                            color: root.cAccent
                        }
                    }
                }
            }

            // 播放模式
            Item {
                Layout.preferredWidth: theme.touchMin
                Layout.preferredHeight: theme.touchMin
                Layout.alignment: Qt.AlignVCenter
                Rectangle {
                    anchors.fill: parent
                    radius: height / 2
                    color: modeArea.containsMouse ? root.softBg : "transparent"
                    Behavior on color { ColorAnimation { duration: 140 } }
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: root.modeIcon()
                        size: 18
                        color: modeArea.containsMouse ? root.cAccent : root.cSub
                        Behavior on color { ColorAnimation { duration: 140 } }
                    }
                }
                MouseArea {
                    id: modeArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: app.cyclePlayMode()
                }
            }

            // 收藏
            Item {
                Layout.preferredWidth: theme.touchMin
                Layout.preferredHeight: theme.touchMin
                Layout.alignment: Qt.AlignVCenter
                Rectangle {
                    anchors.fill: parent
                    radius: height / 2
                    color: favArea.containsMouse ? root.softBg : "transparent"
                    Behavior on color { ColorAnimation { duration: 140 } }
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: app.currentFavorite ? "heartFill" : "heart"
                        size: 18
                        color: app.currentFavorite ? root.cAccent
                                                  : (favArea.containsMouse ? root.cAccent : root.cSub)
                        // 收藏时给一下放大回弹
                        scale: favArea.pressed ? 0.82 : 1.0
                        Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutBack } }
                    }
                }
                MouseArea {
                    id: favArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: app.toggleFavoriteCurrent()
                }
            }

            // 展开播放页
            Item {
                Layout.preferredWidth: theme.touchMin
                Layout.preferredHeight: theme.touchMin
                Layout.alignment: Qt.AlignVCenter
                Rectangle {
                    anchors.fill: parent
                    radius: height / 2
                    color: expArea.containsMouse ? root.softBg : "transparent"
                    Behavior on color { ColorAnimation { duration: 140 } }
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: "expand"
                        size: 17
                        color: expArea.containsMouse ? root.cAccent : root.cSub
                        Behavior on color { ColorAnimation { duration: 140 } }
                    }
                }
                MouseArea {
                    id: expArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.openPlayer()
                }
            }

            // 设置
            Item {
                Layout.preferredWidth: theme.touchMin
                Layout.preferredHeight: theme.touchMin
                Layout.alignment: Qt.AlignVCenter
                Rectangle {
                    anchors.fill: parent
                    radius: height / 2
                    color: setArea.containsMouse ? root.softBg : "transparent"
                    Behavior on color { ColorAnimation { duration: 140 } }
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: "settings"
                        size: 17
                        color: setArea.containsMouse ? root.cAccent : root.cSub
                        Behavior on color { ColorAnimation { duration: 140 } }
                    }
                }
                MouseArea {
                    id: setArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.openSettings()
                }
            }

            // 队列
            Item {
                Layout.preferredWidth: theme.touchMin
                Layout.preferredHeight: theme.touchMin
                Layout.alignment: Qt.AlignVCenter
                Rectangle {
                    anchors.fill: parent
                    radius: height / 2
                    color: listArea.containsMouse ? root.softBg : "transparent"
                    Behavior on color { ColorAnimation { duration: 140 } }
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: "list"
                        size: 18
                        color: listArea.containsMouse ? root.cAccent : root.cSub
                        Behavior on color { ColorAnimation { duration: 140 } }
                    }
                }
                MouseArea {
                    id: listArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.toggleQueue()
                }
            }
        }
    }
}
