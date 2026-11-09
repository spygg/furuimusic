import QtQuick 2.15

// 平台筛选胶囊：圆点用平台主色，选中态填充淡色背景
Rectangle {
    id: root

    property string text: ""
    property bool active: false
    property color accent: "#ec4141"
    property bool showDot: true
    signal clicked()

    readonly property color cText: theme.text
    readonly property color cSub: theme.subText
    readonly property color cLine: theme.line

    implicitWidth: contentRow.implicitWidth + 20
    implicitHeight: 30
    radius: 15
    color: active ? Qt.rgba(accent.r, accent.g, accent.b, 0.14) : "transparent"
    border.color: active ? accent : cLine
    border.width: 1

    Behavior on color { ColorAnimation { duration: 120 } }

    Row {
        id: contentRow
        anchors.centerIn: parent
        spacing: 6
        Rectangle {
            visible: root.showDot
            width: 7
            height: 7
            radius: 3.5
            anchors.verticalCenter: parent.verticalCenter
            color: root.accent
        }
        Text {
            text: root.text
            color: root.active ? root.accent : root.cSub
            font.pixelSize: theme.fontSm
            font.bold: root.active
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: root.clicked()
    }
}
