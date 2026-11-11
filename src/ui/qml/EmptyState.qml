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
    // 矮视口（横屏平板/车机分屏，逻辑高不足 620）里页面只剩一两百像素给结果区，
    // 64 的圆环加两行说明会把内容顶出可视区，压缩版只留小图标 + 标题。
    property bool compact: false

    readonly property color cSub: theme.subText
    readonly property color cLine: theme.line

    spacing: root.compact ? 4 : theme.spaceSm

    Rectangle {
        Layout.alignment: Qt.AlignHCenter
        width: root.compact ? 40 : 64
        height: width
        radius: width / 2
        color: "transparent"
        border.color: cLine
        Text {
            anchors.centerIn: parent
            visible: root.iconName.length === 0
            text: root.glyph
            color: cSub
            font.pixelSize: root.compact ? 17 : 26
        }
        GlyphIcon {
            anchors.centerIn: parent
            visible: root.iconName.length > 0
            name: root.iconName
            size: root.compact ? 17 : 26
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
        visible: root.hint.length > 0 && !root.compact
        text: root.hint
        color: root.cSub
        font.pixelSize: 12
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
    }
}
