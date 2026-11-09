import QtQuick 2.15

// 桌面宽屏左侧导航项：图标 + 文案 + 选中态
Rectangle {
    id: root

    property string text: ""
    property string icon: ""
    property bool active: false
    property color textColor: theme.subText
    property color activeColor: theme.accent
    property color softColor: theme.accentSoft
    signal clicked()

    implicitHeight: 44
    radius: theme.radiusSm
    color: root.active ? root.softColor
                       : (hover.hovered ? Qt.rgba(activeColor.r, activeColor.g, activeColor.b, 0.06)
                                        : "transparent")

    Behavior on color { ColorAnimation { duration: 120 } }

    Rectangle {
        visible: root.active
        width: 3
        height: 18
        radius: 1.5
        color: root.activeColor
        anchors.left: parent.left
        anchors.leftMargin: 1
        anchors.verticalCenter: parent.verticalCenter
    }

    Row {
        anchors.left: parent.left
        anchors.leftMargin: theme.spaceMd
        anchors.verticalCenter: parent.verticalCenter
        spacing: 10

        Rectangle {
            width: 24
            height: 24
            radius: 7
            anchors.verticalCenter: parent.verticalCenter
            color: root.active
                   ? Qt.rgba(activeColor.r, activeColor.g, activeColor.b, 0.14)
                   : "transparent"
            GlyphIcon {
                anchors.centerIn: parent
                name: root.icon
                size: 15
                color: root.active ? root.activeColor : root.textColor
            }
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.text
            color: root.active ? root.activeColor : root.textColor
            font.pixelSize: theme.fontMd
            font.bold: root.active
        }
    }

    MouseArea {
        id: hover
        anchors.fill: parent
        hoverEnabled: true
        onClicked: root.clicked()
    }
}
