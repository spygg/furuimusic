import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// 歌曲评论抽屉（洛雪式多平台评论）：从底部升起，展示当前播放歌曲的评论。
// 评论数据由 app.loadComments(page) 拉取，经 app.comments / app.commentTotal 暴露。
Item {
    id: root
    signal closeRequested()

    readonly property color cText: theme.text
    readonly property color cSub: theme.subText
    readonly property color cPrimary: theme.accent
    readonly property color cLine: theme.line
    readonly property color cSoft: theme.accentSoft

    Rectangle {
        anchors.fill: parent
        color: "transparent"

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // 头部
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: theme.touchMin + 8
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                spacing: 8
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        text: qsTr("评论")
                        color: cText
                        font.pixelSize: 15
                        font.bold: true
                    }
                    Text {
                        text: app.commentTotal >= 0
                              ? (app.commentSourceName + " · " + app.commentTotal + " 条")
                              : (app.commentSourceName + " · 评论")
                        color: cSub
                        font.pixelSize: 11
                    }
                }
                Item { Layout.fillWidth: true }
                Rectangle {
                    width: theme.touchMin
                    height: theme.touchMin
                    radius: width / 2
                    color: closeMa.containsMouse ? cSoft : "transparent"
                    // ✕ 在部分 Android 机型上没有对应字形会显示成豆腐块，用内置矢量图标
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: "close"
                        size: 14
                        color: cSub
                    }
                    MouseArea {
                        id: closeMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.closeRequested()
                    }
                }
            }

            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: cLine }

            BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
                Layout.topMargin: 8
                running: app.commentsLoading && commentList.count === 0
                visible: running
            }

            ListView {
                id: commentList
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.topMargin: 4
                clip: true
                model: app.comments
                spacing: 0
                ScrollBar.vertical: ScrollBar { }
                onMovementEnded: {
                    if (atYEnd && app.commentTotal > commentList.count && !app.commentsLoading)
                        app.loadComments(Math.floor(commentList.count / 20) + 2)
                }
                delegate: Item {
                    width: commentList.width
                    height: row.implicitHeight + 14
                    RowLayout {
                        id: row
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.topMargin: 7
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 10

                        Rectangle {
                            Layout.preferredWidth: 36
                            Layout.preferredHeight: 36
                            Layout.alignment: Qt.AlignTop
                            radius: 18
                            color: cSoft
                            clip: true
                            Image {
                                anchors.fill: parent
                                source: model.avatar || ""
                                fillMode: Image.PreserveAspectCrop
                                visible: model.avatar !== ""
                                onStatusChanged: { if (status === Image.Error) visible = false }
                            }
                            Text {
                                anchors.centerIn: parent
                                visible: !model.avatar
                                text: (model.userName || "?").slice(0, 1)
                                color: cPrimary
                                font.pixelSize: 15
                                font.bold: true
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                Text {
                                    text: model.userName || qsTr("匿名")
                                    color: cPrimary
                                    font.pixelSize: 12
                                    font.bold: true
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Text {
                                    text: model.timeText || ""
                                    color: cSub
                                    font.pixelSize: 10
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: model.text || ""
                                color: cText
                                font.pixelSize: 13
                                wrapMode: Text.Wrap
                                textFormat: Text.PlainText
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: model.replyText && model.replyText !== ""
                                text: qsTr("回复 ") + (model.replyText || "")
                                color: cSub
                                font.pixelSize: 11
                                wrapMode: Text.WordWrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 12
                                RowLayout {
                                    spacing: 3
                                    GlyphIcon {
                                        name: "heart"
                                        size: 10
                                        color: cSub
                                    }
                                    Text {
                                        text: String(model.likedCount || 0)
                                        color: cSub
                                        font.pixelSize: 10
                                    }
                                }
                                RowLayout {
                                    visible: (model.replyCount || 0) > 0
                                    spacing: 3
                                    GlyphIcon {
                                        name: "comment"
                                        size: 10
                                        color: cSub
                                    }
                                    Text {
                                        text: String(model.replyCount)
                                        color: cSub
                                        font.pixelSize: 10
                                    }
                                }
                            }
                        }
                    }
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: cLine
                        opacity: 0.5
                    }
                }

                // 空态提示
                Text {
                    anchors.centerIn: parent
                    visible: !app.commentsLoading && commentList.count === 0
                    text: qsTr("该平台暂无评论或不支持评论")
                    color: cSub
                    font.pixelSize: 13
                }
            }
        }
    }
}
