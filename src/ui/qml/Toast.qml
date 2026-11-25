import QtQuick 2.15

// 全局轻提示 —— Android Material snackbar 风格：
// 深色半透明胶囊 + 语义色圆点 + 白字，底部居中上滑淡入。
// 亮暗主题统一用深底白字（toast 是浮层，固定深底在任何页面背景上都清晰、不突兀）；
// 纯 QML Rectangle 实现，Qt5 桌面与 Qt6 Android 表现一致。
Item {
    id: root

    property string message: ""
    // info / success / error / warning
    property string kind: "info"
    property int autoHideMs: 2200
    property bool opened: false

    // 文案最大宽度：窗口宽 - 胶囊内边距，超长换行（最多两行）
    readonly property real maxLabelW: (parent ? parent.width : 560) - 96
    readonly property bool twoLines: label.lineCount > 1

    width: Math.min((parent ? parent.width : 560) - 48,
                    Math.max(160, label.implicitWidth + 56))
    height: twoLines ? 68 : 44
    opacity: root.opened ? 1 : 0
    // opacity 归零后不再接收鼠标事件，避免挡住底部播放栏
    visible: root.opened || root.opacity > 0.01
    scale: root.opened ? 1.0 : 0.94

    Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
    Behavior on scale { NumberAnimation { duration: 220; easing.type: Easing.OutBack } }
    Behavior on width { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
    Behavior on height { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

    transform: Translate {
        id: slide
        y: root.opened ? 0 : 26
        Behavior on y { NumberAnimation { duration: 240; easing.type: Easing.OutCubic } }
    }

    readonly property color tone: root.kind === "success" ? "#4ADE80"
                                : root.kind === "error" ? "#F87171"
                                : root.kind === "warning" ? "#FBBF24"
                                : "#60A5FA"

    // 单层柔和落影：半透明胶囊下压一片更淡的同形矩形即可，多层投影反而显重
    Rectangle {
        x: 2; y: 4
        width: pill.width; height: pill.height
        radius: pill.radius
        color: Qt.rgba(0, 0, 0, 0.18)
        z: -1
    }

    Rectangle {
        id: pill
        anchors.fill: parent
        radius: height / 2
        // 深色磨砂底：固定深底白字，亮暗主题、任何页面背景上都清晰
        color: Qt.rgba(0.10, 0.11, 0.14, 0.92)
        border.color: Qt.rgba(1, 1, 1, 0.08)
        border.width: 1

        Row {
            anchors.centerIn: parent
            spacing: 10

            // 语义色圆点：一眼区分成功 / 失败 / 警告 / 信息，比大图标轻盈
            Rectangle {
                width: 8; height: 8
                anchors.verticalCenter: parent.verticalCenter
                radius: 4
                color: root.tone
            }

            Text {
                id: label
                anchors.verticalCenter: parent.verticalCenter
                text: root.message
                color: "#F4F6FA"
                font.pixelSize: theme.fontSm
                lineHeight: 1.15
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
                width: Math.min(implicitWidth, root.maxLabelW)
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
