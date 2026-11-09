import QtQuick 2.15
import QtQuick.Layouts 1.15

// 空列表占位：图标 + 标题 + 一行说明，页面只需传三句话
ColumnLayout {
    id: root

    property string glyph: "♫"
    // 传入图标名时用矢量图标绘制，避免个别平台缺字形
    property string iconName: ""
    property string title: "这里还没有内容"
    property string hint: ""

    readonly property color cSub: theme.subText
    readonly property color cLine: theme.line

    spacing: theme.spaceSm

    Rectangle {
        Layout.alignment: Qt.AlignHCenter
        width: 64
        height: 64
        radius: 32
        color: "transparent"
        border.color: cLine
        Text {
            anchors.centerIn: parent
            visible: root.iconName.length === 0
            text: root.glyph
            color: cSub
            font.pixelSize: 26
        }
        GlyphIcon {
            anchors.centerIn: parent
            visible: root.iconName.length > 0
            name: root.iconName
            size: 26
            color: cSub
        }
    }

    Text {
        Layout.alignment: Qt.AlignHCenter
        text: root.title
        color: root.cSub
        font.pixelSize: 14
        font.bold: true
    }

    Text {
        Layout.fillWidth: true
        visible: root.hint.length > 0
        text: root.hint
        color: root.cSub
        font.pixelSize: 12
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
    }
}
