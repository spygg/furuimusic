import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// 桌面宽屏右侧播放队列抽屉
Item {
    id: root
    signal closeRequested()

    readonly property bool isDark: theme.dark
    readonly property color cText: theme.text
    readonly property color cSub: theme.subText
    readonly property color cPrimary: theme.accent
    readonly property color cLine: theme.line
    readonly property color cSoft: theme.accentSoft

    Rectangle {
        anchors.fill: parent
        // 卡片底色交给容器画：桌面抽屉的外层 Rectangle 已经是 cCard，
        // 窄屏底部面板要保留圆角，这里再铺一层方角底色就会把圆角盖掉。
        color: "transparent"

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                // 头部行高与两枚按钮都跟触控最小值走：车机上手指比鼠标难点得中
                Layout.preferredHeight: theme.touchMin
                Layout.leftMargin: 12
                Layout.rightMargin: 12
                spacing: 8

                Text {
                    text: "播放队列"
                    color: cText
                    font.pixelSize: 14
                    font.bold: true
                }
                Item { Layout.fillWidth: true }
                Rectangle {
                    Layout.preferredWidth: theme.touchMin
                    Layout.preferredHeight: theme.touchMin
                    radius: height / 2
                    color: clearMa.pressed ? Qt.rgba(cPrimary.r, cPrimary.g, cPrimary.b, 0.24)
                                           : (clearMa.containsMouse ? cPrimary : cSoft)
                    Behavior on color { ColorAnimation { duration: theme.durationFast } }
                    scale: clearMa.pressed ? 0.94 : 1.0
                    Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                    Text {
                        anchors.centerIn: parent
                        text: "清空"
                        color: clearMa.containsMouse ? cText : cSub
                        font.pixelSize: theme.fontSm
                    }
                    MouseArea {
                        id: clearMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        Accessible.name: "清空播放队列"
                        onClicked: app.clearQueue()
                    }
                }
                Rectangle {
                    Layout.preferredWidth: theme.touchMin
                    Layout.preferredHeight: theme.touchMin
                    radius: width / 2
                    color: closeMa.containsMouse ? cSoft : "transparent"
                    border.color: cLine
                    Behavior on color { ColorAnimation { duration: theme.durationFast } }
                    Text {
                        anchors.centerIn: parent
                        text: "×"
                        color: cSub
                        font.pixelSize: 13
                    }
                    MouseArea {
                        id: closeMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        Accessible.name: "关闭播放队列"
                        onClicked: root.closeRequested()
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: cLine
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ListView {
                    id: queueList
                    anchors.fill: parent
                    clip: true
                    cacheBuffer: visible ? height * 2 : 100   // 预渲染两屏：默认只多备 100px，长队列滚动时委托反复重建
                    model: app.queue
                    spacing: 2
                    topMargin: 6
                    bottomMargin: 6

                    delegate: Rectangle {
                    width: ListView.view.width
                    height: Math.max(54, theme.touchMin)
                    radius: 6
                    color: index === app.currentQueueIndex ? cSoft : "transparent"

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        // 点当前这首 = 暂停/继续；点别的才是切歌，避免误以为点播放却从头重播
                        onClicked: index === app.currentQueueIndex ? app.togglePause() : app.playQueue(index)
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 8

                        Text {
                            text: index + 1
                            color: cSub
                            font.pixelSize: theme.fontSm
                            Layout.preferredWidth: 18
                            horizontalAlignment: Text.AlignHCenter
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                // 状态词跟工具条走：暂停时不能还标「正在播放」
                                text: (index === app.currentQueueIndex
                                       ? (app.playing ? "正在播放 · " : "已暂停 · ") : "") + title
                                color: index === app.currentQueueIndex ? cPrimary : cText
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: artist + (sourceName.length > 0 ? " · " + sourceName : "")
                                color: cSub
                                font.pixelSize: theme.fontXs
                                elide: Text.ElideRight
                            }
                        }
                        Text {
                            text: "移除"
                            color: cSub
                            font.pixelSize: theme.fontXs
                            // 文字即按钮：热区撑到触控最小值，不然只有两字宽的手指点不到
                            Layout.preferredWidth: Math.max(theme.touchMin, implicitWidth)
                            Layout.preferredHeight: theme.touchMin
                            verticalAlignment: Text.AlignVCenter
                            horizontalAlignment: Text.AlignHCenter
                            MouseArea {
                                anchors.fill: parent
                                Accessible.name: "从播放队列移除"
                                onClicked: app.removeQueue(index)
                            }
                        }
                    }
                    }
                } // ListView

                EmptyState {
                    anchors.centerIn: parent
                    visible: queueList.count === 0
                    compact: root.height < 360
                    iconName: "list"
                    title: "播放队列是空的"
                    hint: "在歌曲行点「＋」把歌曲加入队列"
                }
            }
        }
    }
}
