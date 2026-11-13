import QtQuick 2.15
import QtQuick.Layouts 1.15

// 全局轻提示：替代原来 44px 白底描边的裸矩形。
// 设计要点：语义色图标 + 主题卡片 + 三层伪投影 + 下方滑入淡出；
// 纯 QML Rectangle 实现，不依赖 QtGraphicalEffects / MultiEffect，
// Qt5 桌面与 Qt6 Android 表现一致。
Item {
    id: root

    property string message: ""
    // info / success / error / warning
    property string kind: "info"
    property int autoHideMs: 2400
    property bool opened: false

    width: Math.min(420, Math.max(200, (parent ? parent.width : 420) - 32))
    height: card.height + 12
    opacity: root.opened ? 1 : 0
    // opacity 归零后不再接收鼠标事件，避免挡住底部播放栏
    visible: root.opened || root.opacity > 0.01

    Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }

    transform: Translate {
        id: slide
        y: root.opened ? 0 : 18
        Behavior on y { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
    }

    readonly property color tone: root.kind === "success" ? theme.success
                                : root.kind === "error" ? theme.danger
                                : root.kind === "warning" ? theme.vip
                                : theme.accent
    readonly property string glyph: root.kind === "success" ? "check"
                                  : root.kind === "error" ? "close"
                                  : root.kind === "warning" ? "refresh"
                                  : "music"

    // 三层错位圆角矩形模拟柔和落影：浅色用带冷调的深灰，深色用极低透明度黑
    Repeater {
        model: 3
        delegate: Rectangle {
            readonly property int level: index + 1
            x: -level * 3
            y: level * 3
            width: card.width + level * 6
            height: card.height + level * 6
            radius: card.radius + level * 3
            color: Qt.rgba(theme.shadowTint.r, theme.shadowTint.g, theme.shadowTint.b,
                           (theme.dark ? 0.10 : 0.055) / level)
            z: -level
        }
    }

    Rectangle {
        id: card
        width: root.width
        height: Math.max(58, body.implicitHeight + 22)
        radius: theme.radiusMd
        color: theme.card
        border.color: theme.line
        border.width: 1

        // 左侧 3px 语义色条：一眼区分成功 / 失败 / 警告 / 普通信息
        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.margins: 1
            width: 3
            radius: 2
            color: root.tone
            opacity: 0.9
        }

        RowLayout {
            id: body
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 10
            spacing: 12

            Rectangle {
                Layout.preferredWidth: 34
                Layout.preferredHeight: 34
                Layout.alignment: Qt.AlignVCenter
                radius: 17
                color: Qt.rgba(root.tone.r, root.tone.g, root.tone.b, 0.14)
                GlyphIcon {
                    anchors.centerIn: parent
                    name: root.glyph
                    size: 16
                    color: root.tone
                }
            }

            Text {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                text: root.message
                color: theme.text
                font.pixelSize: theme.fontMd
                wrapMode: Text.Wrap
                // 参考洛雪的轻提示：最多两行，长文案截断。整段异常（HTTP 错误、
                // 脚本报错）留给状态栏与设置页，不该糊在屏幕上
                maximumLineCount: 2
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }

            Rectangle {
                Layout.preferredWidth: 26
                Layout.preferredHeight: 26
                Layout.alignment: Qt.AlignVCenter
                radius: 13
                color: closeArea.containsMouse ? theme.cardHover : "transparent"
                GlyphIcon {
                    anchors.centerIn: parent
                    name: "close"
                    size: 11
                    color: theme.subText
                }
                MouseArea {
                    id: closeArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.hide()
                }
            }
        }
    }

    Timer {
        id: hideTimer
        interval: root.autoHideMs
        repeat: false
        onTriggered: root.hide()
    }

    function show(msg, type) {
        // 统一洗一遍：去掉换行/制表符、压掉连续空白、超长截断，
        // 保证任何来源（QMediaPlayer、脚本引擎、HTTP）都只占一两行
        var s = msg === undefined || msg === null ? "" : String(msg)
        s = s.replace(/[\r\n\t]+/g, " ").replace(/\s{2,}/g, " ").trim()
        if (s.length > 80)
            s = s.substring(0, 79) + "…"
        root.message = s
        root.kind = type && type.length > 0 ? type : "info"
        root.opened = true
        hideTimer.restart()
    }

    function hide() {
        hideTimer.stop()
        root.opened = false
    }
}
