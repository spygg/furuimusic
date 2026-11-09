import QtQuick 2.15

// 桌面播放栏的小型文字按钮，配色由外部通过属性传入，避免内联组件跨作用域取色
Rectangle {
    id: root
    property string text: ""
    property bool highlighted: false
    property color textColor: theme.subText
    property color accentColor: theme.accent
    property color softColor: theme.accentSoft
    property color lineColor: theme.line
    signal clicked()

    implicitWidth: Math.max(52, label.implicitWidth + 16)
    implicitHeight: 30
    radius: 15
    color: root.highlighted ? root.softColor : "transparent"
    border.color: root.highlighted ? root.accentColor : root.lineColor

    Text {
        id: label
        anchors.centerIn: parent
        text: root.text
        color: root.highlighted ? root.accentColor : root.textColor
        font.pixelSize: 11
    }

    MouseArea {
        anchors.fill: parent
        onClicked: root.clicked()
    }
}
