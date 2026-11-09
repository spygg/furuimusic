import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// 桌面宽屏右侧播放队列抽屉
Item {
    id: root
    signal closeRequested()

    readonly property bool isDark: theme.dark
    readonly property color cCard: theme.card
    readonly property color cText: theme.text
    readonly property color cSub: theme.subText
    readonly property color cPrimary: theme.accent
    readonly property color cLine: theme.line
    readonly property color cSoft: theme.accentSoft

    Rectangle {
        anchors.fill: parent
        color: cCard

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 48
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
                    width: 44
                    height: 26
                    radius: 13
                    color: cSoft
                    Text {
                        anchors.centerIn: parent
                        text: "清空"
                        color: cSub
                        font.pixelSize: 11
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: app.clearQueue()
                    }
                }
                Rectangle {
                    width: 26
                    height: 26
                    radius: 13
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
                model: app.queue
                spacing: 2
                topMargin: 6
                bottomMargin: 6

                delegate: Rectangle {
                    width: ListView.view.width
                    height: 54
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
                            font.pixelSize: 11
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
                                font.pixelSize: 10
                                elide: Text.ElideRight
                            }
                        }
                        Text {
                            text: "移除"
                            color: cSub
                            font.pixelSize: 10
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
