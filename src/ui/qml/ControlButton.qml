import QtQuick 2.15

// 桌面播放栏的小型按钮：有 icon 时画圆形图标按钮，否则画文字胶囊
Rectangle {
    id: root
    property string text: ""
    property string icon: ""
    property bool highlighted: false
    property color textColor: theme.subText
    property color accentColor: theme.accent
    property color softColor: theme.accentSoft
    property color lineColor: theme.line
    signal clicked()

    readonly property bool iconMode: icon.length > 0

    implicitWidth: iconMode ? 34 : Math.max(52, label.implicitWidth + 16)
    implicitHeight: iconMode ? 34 : 30
    radius: iconMode ? 17 : 15
    color: root.highlighted ? root.softColor : "transparent"
    border.color: root.highlighted ? root.accentColor : root.lineColor

    Text {
        id: label
        anchors.centerIn: parent
        visible: !root.iconMode
        text: root.text
        color: root.highlighted ? root.accentColor : root.textColor
        font.pixelSize: 11
    }

    GlyphIcon {
        anchors.centerIn: parent
        visible: root.iconMode
        name: root.icon
        size: 15
        color: root.highlighted ? root.accentColor : root.textColor
    }

    MouseArea {
        anchors.fill: parent
        onClicked: root.clicked()
    }
}
