import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// 图形验证码一行（酷我账号密码 / 手机短信登录共用）
// 左边输入框、右边验证码图片；点图片即换一张 —— 与官网「点击图片刷新」一致。
// 验证码图由 AuthManager 落盘成两个文件名交替写，避免 QML 的 Image 按 URL 缓存住旧图。
RowLayout {
    id: root

    property string sourceId: ""
    property string imagePath: ""
    // 输入内容由使用方读取，避免再往外抛一层信号
    readonly property string text: field.text

    readonly property color cField: theme.dark ? Qt.rgba(1, 1, 1, 0.06)
                                               : Qt.rgba(0, 0, 0, 0.035)

    spacing: 8

    TextField {
        id: field
        Layout.fillWidth: true
        Layout.preferredHeight: theme.touchMin
        placeholderText: "图形验证码"
        color: theme.text
        placeholderTextColor: theme.mutedText
        font.pixelSize: 13
        leftPadding: 12
        maximumLength: 6
        background: Rectangle {
            color: root.cField
            radius: 11
            border.width: 1
            border.color: field.activeFocus ? theme.accent : theme.line
        }
    }

    Rectangle {
        Layout.preferredWidth: 104
        Layout.preferredHeight: theme.touchMin
        radius: 11
        color: "#FFFFFF"
        border.width: 1
        border.color: theme.line
        clip: true

        Image {
            anchors.fill: parent
            anchors.margins: 2
            fillMode: Image.PreserveAspectFit
            smooth: false
            asynchronous: true
            source: root.imagePath.length > 0 ? platform.toFileUrl(root.imagePath) : ""
        }

        Text {
            anchors.centerIn: parent
            visible: root.imagePath.length === 0
            text: "点击获取"
            color: theme.mutedText
            font.pixelSize: theme.fontSm
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: app.refreshCaptcha(root.sourceId)
        }
    }
}
