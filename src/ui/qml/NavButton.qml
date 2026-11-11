import QtQuick 2.15
import QtQuick.Controls 2.15

// 桌面宽屏左侧导航项：图标 + 文案 + 选中态
// compact = true 时只显示图标（平板/窄窗口），整条导航可以收窄把宽度让给内容区
Rectangle {
    id: root

    property string text: ""
    property string icon: ""
    property bool active: false
    property bool compact: false
    // 窗口很矮时（车机/横屏平板）进一步压缩行高，保证导航项全部可见
    property bool dense: false
    // 悬停态：ToolTip 的附加属性在组件创建时就会求值，那时子项 MouseArea 还不存在，
    // 直接绑 hover.containsMouse 会刷 "Unable to assign [undefined] to bool"，这里用自身属性中转
    property bool hovered: false
    property color textColor: theme.subText
    property color activeColor: theme.accent
    property color softColor: theme.accentSoft
    signal clicked()

    implicitHeight: root.dense ? 40 : (root.compact ? 48 : 44)
    radius: theme.radiusSm
    color: root.active ? root.softColor
                       : (root.hovered ? Qt.rgba(activeColor.r, activeColor.g, activeColor.b, 0.06)
                                       : "transparent")

    Behavior on color { ColorAnimation { duration: 120 } }

    // 仅宽屏模式显示左侧选中指示条；图标模式下选中态由底色表达
    Rectangle {
        visible: root.active && !root.compact
        width: 3
        height: 18
        radius: 1.5
        color: root.activeColor
        anchors.left: parent.left
        anchors.leftMargin: 1
        anchors.verticalCenter: parent.verticalCenter
    }

    Row {
        anchors.left: root.compact ? undefined : parent.left
        anchors.horizontalCenter: root.compact ? parent.horizontalCenter : undefined
        anchors.leftMargin: theme.spaceMd
        anchors.verticalCenter: parent.verticalCenter
        spacing: 10

        Rectangle {
            width: root.compact ? 34 : 24
            height: width
            radius: root.compact ? 10 : 7
            anchors.verticalCenter: parent.verticalCenter
            color: root.active
                   ? Qt.rgba(activeColor.r, activeColor.g, activeColor.b, 0.14)
                   : "transparent"
            GlyphIcon {
                anchors.centerIn: parent
                name: root.icon
                size: root.compact ? 18 : 15
                color: root.active ? root.activeColor : root.textColor
            }
        }

        Text {
            visible: !root.compact
            anchors.verticalCenter: parent.verticalCenter
            text: root.text
            color: root.active ? root.activeColor : root.textColor
            font.pixelSize: theme.fontMd
            font.bold: root.active
        }
    }

    // 图标模式没有文案，悬停时补一个提示（触屏不会触发 hover）
    ToolTip.visible: root.compact && root.hovered
    ToolTip.text: root.text
    ToolTip.delay: 260

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onContainsMouseChanged: root.hovered = containsMouse
        onClicked: root.clicked()
    }
}
