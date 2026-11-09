import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15

// 桌面自绘标题栏：拖动 / 双击最大化 / 窗口按钮
// macOS 用左侧红绿灯样式，Windows 与 Linux 用右侧方形按钮；样式由 platform 适配层给出
Rectangle {
    id: root

    implicitHeight: theme.titleBarHeight
    color: theme.surface

    readonly property var win: Window.window
    readonly property bool isMac: platform.titleBarStyle === "mac"
    readonly property bool maximized: win.visibility === Window.Maximized

    signal themeToggleRequested()

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: theme.line
    }

    // 拖动区：铺满整条标题栏，窗口按钮声明在后面自然盖住它
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        onPressed: platform.startSystemMove(root.win)
        onDoubleClicked: platform.toggleMaximize(root.win)
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: root.isMac ? 12 : 14
        anchors.rightMargin: 8
        spacing: 10

        // macOS 交通灯
        Row {
            visible: root.isMac
            spacing: 8
            anchors.verticalCenter: parent.verticalCenter

            Repeater {
                model: [
                    { color: "#FF5F57", glyph: "✕", action: "close" },
                    { color: "#FEBC2E", glyph: "−", action: "min" },
                    { color: "#28C840", glyph: "+", action: "max" }
                ]
                delegate: Rectangle {
                    width: 12
                    height: 12
                    radius: 6
                    color: modelData.color
                    Text {
                        anchors.centerIn: parent
                        text: dotHover.containsMouse ? modelData.glyph : ""
                        color: "#5A1A16"
                        font.pixelSize: 8
                        font.bold: true
                    }
                    MouseArea {
                        id: dotHover
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            if (modelData.action === "close")
                                platform.closeWindow(root.win)
                            else if (modelData.action === "min")
                                platform.minimizeWindow(root.win)
                            else
                                platform.toggleMaximize(root.win)
                        }
                    }
                }
            }
        }

        // 品牌
        Rectangle {
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: 22
            implicitHeight: 22
            radius: 7
            gradient: Gradient {
                GradientStop { position: 0.0; color: theme.heroStart }
                GradientStop { position: 1.0; color: theme.heroEnd }
            }
            Text {
                anchors.centerIn: parent
                text: "♪"
                color: "#FFFFFF"
                font.pixelSize: 12
                font.bold: true
            }
        }

        Text {
            Layout.alignment: Qt.AlignVCenter
            text: app.appName
            color: theme.text
            font.pixelSize: theme.fontSm
            font.bold: true
        }

        Text {
            Layout.alignment: Qt.AlignVCenter
            text: "多平台聚合"
            color: theme.mutedText
            font.pixelSize: theme.fontXs
        }

        Item { Layout.fillWidth: true }

        // 主题切换：点击在浅色/深色之间切换（跟随系统在设置页选择）
        Rectangle {
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: 30
            implicitHeight: 30
            color: "transparent"

            Rectangle {
                anchors.fill: parent
                radius: 15
                color: themeBtn.containsMouse ? theme.cardHover : "transparent"
                Behavior on color { ColorAnimation { duration: 120 } }
            }
            GlyphIcon {
                anchors.centerIn: parent
                name: theme.dark ? "sun" : "moon"
                size: 15
                color: theme.subText
            }
            MouseArea {
                id: themeBtn
                anchors.fill: parent
                hoverEnabled: true
                onClicked: root.themeToggleRequested()
            }
        }

        // Windows / Linux 窗口按钮
        Row {
            visible: !root.isMac
            spacing: 2
            anchors.verticalCenter: parent.verticalCenter

            Repeater {
                model: ["min", "max", "close"]
                delegate: Rectangle {
                    width: 44
                    height: 32
                    radius: 8
                    color: {
                        if (modelData === "close")
                            return btnHover.containsMouse ? theme.danger : "transparent"
                        return btnHover.containsMouse ? theme.cardHover : "transparent"
                    }
                    Behavior on color { ColorAnimation { duration: 120 } }

                    Text {
                        anchors.centerIn: parent
                        text: modelData === "min" ? "─"
                            : modelData === "max" ? (root.maximized ? "❐" : "□")
                            : "✕"
                        color: modelData === "close" && btnHover.containsMouse
                               ? "#FFFFFF" : theme.subText
                        font.pixelSize: modelData === "min" ? 12 : 11
                    }

                    MouseArea {
                        id: btnHover
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            if (modelData === "min")
                                platform.minimizeWindow(root.win)
                            else if (modelData === "max")
                                platform.toggleMaximize(root.win)
                            else
                                platform.closeWindow(root.win)
                        }
                    }
                }
            }
        }
    }
}
