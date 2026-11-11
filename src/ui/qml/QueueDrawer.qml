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
                    width: theme.touchMin
                    height: theme.touchMin
                    radius: height / 2
                    color: cSoft
                    Text {
                        anchors.centerIn: parent
                        text: "清空"
                        color: cSub
                        font.pixelSize: theme.fontSm
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: app.clearQueue()
                    }
                }
                Rectangle {
                    width: theme.touchMin
                    height: theme.touchMin
                    radius: width / 2
                    color: "transparent"
                    border.color: cLine
                    Text {
                        anchors.centerIn: parent
                        text: "×"
                        color: cSub
                        font.pixelSize: 13
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.closeRequested()
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: cLine
            }

            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                cacheBuffer: height * 2   // 预渲染两屏：默认只多备 100px，长队列滚动时委托反复重建
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
                        onClicked: app.playQueue(index)
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
                                text: (index === app.currentQueueIndex ? "正在播放 · " : "") + title
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
                                onClicked: app.removeQueue(index)
                            }
                        }
                    }
                }
            }
        }
    }
}
