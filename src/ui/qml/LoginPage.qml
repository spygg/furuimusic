import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Item {
    id: root

    readonly property bool isDark: theme.dark
    readonly property color cBg: theme.bg
    readonly property color cCard: theme.card
    readonly property color cText: theme.text
    readonly property color cSub: theme.subText
    readonly property color cPrimary: theme.accent
    readonly property color cVip: theme.vip
    readonly property color cLine: theme.line
    readonly property color cSoft: theme.accentSoft

    property string selectedSource: ""
    property string method: ""
    property var methods: []
    property string hint: ""
    property bool hintError: false
    property string apiStatus: ""

    readonly property bool hasQr: root.method === "qrcode"
    readonly property bool hasCookie: root.method === "cookie"
    readonly property bool hasPhone: root.method === "phone"
    readonly property bool hasPassword: root.method === "password"
    readonly property bool qrMine: app.qrSource === root.selectedSource

    function methodLabel(m) {
        if (m === "qrcode") return "二维码"
        if (m === "cookie") return "Cookie"
        if (m === "phone") return "手机号"
        if (m === "password") return "密码"
        return m
    }

    function updateMethods() {
        root.methods = root.selectedSource.length > 0 ? app.loginMethods(root.selectedSource) : []
        root.method = root.methods.length > 0 ? root.methods[0] : ""
        root.apiStatus = root.selectedSource.length > 0
                        ? app.accountApiStatus(root.selectedSource) : ""
        if (root.selectedSource.length > 0)
            apiBaseField.text = app.accountApiBaseFor(root.selectedSource)
    }

    onSelectedSourceChanged: root.updateMethods()

    Component.onCompleted: {
        app.refreshAccounts()
        if (app.sourceIds.length > 0)
            root.selectedSource = app.sourceIds[0]
        root.updateMethods()
    }

    Connections {
        target: app
        onLoginStateChanged: {
            root.hint = (ok ? "登录成功：" : "登录失败：") + message
            root.hintError = !ok
            app.refreshAccounts()
            root.updateMethods()
        }
        onQrChanged: {
            root.hint = app.qrStatus.length > 0 ? app.qrStatus : app.qrHint
            // 风控/接口异常用醒目的错误色，别让"未登录"看起来像正常等待
            root.hintError = app.qrStatus.indexOf("风控") >= 0
                    || app.qrStatus.indexOf("失败") >= 0
                    || app.qrStatus.indexOf("过期") >= 0
        }
        onSyncFinished: {
            root.hint = (ok ? "同步完成：" : "同步失败：") + message
            root.hintError = !ok
        }
        onSettingsChanged: {
            if (root.selectedSource.length > 0)
                root.apiStatus = app.accountApiStatus(root.selectedSource)
        }
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: body.height + 32
        clip: true

        ColumnLayout {
            id: body
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 16
            height: implicitHeight
            spacing: 14

            // 页头
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Rectangle {
                    Layout.preferredWidth: 42
                    Layout.preferredHeight: 42
                    radius: 12
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: theme.heroStart }
                        GradientStop { position: 1.0; color: theme.heroEnd }
                    }
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: "user"
                        size: 22
                        color: "#FFFFFF"
                    }
                }
                ColumnLayout {
                    spacing: 2
                    Text {
                        text: "平台账号"
                        color: cText
                        font.pixelSize: 18
                        font.bold: true
                    }
                    Text {
                        text: "每个平台独立登录；登录后自动同步歌单与收藏"
                        color: cSub
                        font.pixelSize: 11
                    }
                }
                Item { Layout.fillWidth: true }
            }

            // 各平台账号卡片
            Flow {
                id: accountFlow
                Layout.fillWidth: true
                Layout.preferredHeight: implicitHeight
                spacing: 10

                Repeater {
                    model: app.accounts
                    delegate: Rectangle {
                        width: accountFlow.width >= 560 ? (accountFlow.width - 10) / 2 : accountFlow.width
                        height: 92
                        radius: 14
                        color: model.source === root.selectedSource ? cSoft : cCard
                        border.width: model.source === root.selectedSource ? 1.5 : 1
                        border.color: model.source === root.selectedSource
                                      ? app.sourceColor(model.source) : cLine

                        property color accent: app.sourceColor(model.source)

                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.selectedSource = model.source
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 12
                            spacing: 10

                            Rectangle {
                                Layout.preferredWidth: 44
                                Layout.preferredHeight: 44
                                radius: 22
                                color: Qt.rgba(accent.r, accent.g, accent.b, 0.14)
                                Image {
                                    id: avatarImg
                                    anchors.fill: parent
                                    anchors.margins: 2
                                    source: model.avatar.length > 0
                                            ? (model.avatar.indexOf("http") === 0
                                               ? model.avatar : platform.toFileUrl(model.avatar))
                                            : ""
                                    fillMode: Image.PreserveAspectCrop
                                    visible: status === Image.Ready
                                    asynchronous: true
                                }
                                Text {
                                    anchors.centerIn: parent
                                    visible: avatarImg.status !== Image.Ready
                                    text: model.name.length > 0 ? model.name.charAt(0) : "♪"
                                    color: accent
                                    font.pixelSize: 18
                                    font.bold: true
                                }
                                // 已登录小圆点
                                Rectangle {
                                    width: 10
                                    height: 10
                                    radius: 5
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    anchors.margins: 2
                                    color: model.logged ? theme.success : theme.line
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 3
                                Text {
                                    Layout.fillWidth: true
                                    text: model.name
                                    color: cText
                                    font.pixelSize: 14
                                    font.bold: true
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: model.logged
                                          ? (model.nickname.length > 0 ? model.nickname : "已登录")
                                          : "未登录"
                                    color: model.logged ? accent : cSub
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                                Row {
                                    spacing: 6
                                    Rectangle {
                                        width: apiTag.implicitWidth + 10
                                        height: 18
                                        radius: 9
                                        color: model.apiAvailable
                                               ? Qt.rgba(accent.r, accent.g, accent.b, 0.12)
                                               : theme.bg
                                        Text {
                                            id: apiTag
                                            anchors.centerIn: parent
                                            text: model.apiAvailable
                                                  ? ("歌单同步 · " + model.playlistCount)
                                                  : "仅播放鉴权"
                                            color: model.apiAvailable ? accent : cSub
                                            font.pixelSize: 10
                                        }
                                    }
                                }
                            }

                            Text {
                                Layout.alignment: Qt.AlignTop
                                text: model.logged ? "已登录" : "去登录"
                                color: model.logged ? theme.success : cSub
                                font.pixelSize: 11
                                font.bold: model.logged
                            }
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: cLine
            }

            // 当前平台的登录方式
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Text {
                    text: root.selectedSource.length > 0
                          ? app.sourceName(root.selectedSource) + " 登录方式"
                          : "登录方式"
                    color: cText
                    font.pixelSize: 14
                    font.bold: true
                }
                Item { Layout.fillWidth: true }
                Repeater {
                    model: root.methods
                    delegate: PlatformChip {
                        text: root.methodLabel(modelData)
                        active: root.method === modelData
                        accent: cPrimary
                        showDot: false
                        onClicked: root.method = modelData
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                visible: root.selectedSource.length > 0
                text: root.apiStatus
                color: cSub
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }

            // 未内置直连接口的平台，可在这里填账号 API 服务地址后同步歌单
            RowLayout {
                Layout.fillWidth: true
                visible: root.selectedSource.length > 0
                spacing: 8

                TextField {
                    id: apiBaseField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    placeholderText: "账号 API 服务地址（可选，如 http://127.0.0.1:8787）"
                    color: cText
                    placeholderTextColor: cSub
                    font.pixelSize: 12
                    leftPadding: 10
                    background: Rectangle { color: cCard; radius: 10; border.color: cLine }
                }
                Rectangle {
                    Layout.preferredWidth: 64
                    Layout.preferredHeight: 32
                    radius: 16
                    color: "transparent"
                    border.color: cLine
                    Text {
                        anchors.centerIn: parent
                        text: "保存"
                        color: cSub
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            app.setAccountApiBaseFor(root.selectedSource, apiBaseField.text)
                            root.apiStatus = app.accountApiStatus(root.selectedSource)
                            root.hint = "账号接口地址已保存"
                        }
                    }
                }
            }

            // ---- 二维码 ----
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.hasQr
                spacing: 10

                // 网易云扫码实测会被服务端风控拦截（8821），必须提前讲清楚
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: qrRisk.implicitHeight + 16
                    visible: root.selectedSource === "netease"
                    radius: 10
                    color: Qt.rgba(theme.danger.r, theme.danger.g, theme.danger.b, 0.10)
                    border.width: 1
                    border.color: Qt.rgba(theme.danger.r, theme.danger.g, theme.danger.b, 0.35)
                    Text {
                        id: qrRisk
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: 8
                        text: "网易云扫码登录受服务端风控拦截（8821），当前不可用；请改用 Cookie 登录"
                        color: theme.danger
                        font.pixelSize: 11
                        wrapMode: Text.Wrap
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 216
                    Layout.preferredHeight: 216
                    Layout.alignment: Qt.AlignHCenter
                    radius: 14
                    color: "#FFFFFF"
                    border.color: cLine

                    Image {
                        anchors.centerIn: parent
                        width: 188
                        height: 188
                        fillMode: Image.PreserveAspectFit
                        source: (root.qrMine && app.qrActive && app.qrImagePath.length > 0)
                                ? (app.qrImagePath.indexOf("http") === 0
                                   ? app.qrImagePath : platform.toFileUrl(app.qrImagePath))
                                : ""
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: !(root.qrMine && app.qrActive && app.qrImagePath.length > 0)
                        text: "点击下方按钮获取二维码"
                        color: theme.mutedText
                        font.pixelSize: 12
                    }
                }

                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 8
                    Rectangle {
                        width: 108
                        height: 32
                        radius: 16
                        color: cPrimary
                        Text {
                            anchors.centerIn: parent
                            text: "获取二维码"
                            color: "#FFFFFF"
                            font.pixelSize: 12
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: app.startQrLogin(root.selectedSource)
                        }
                    }
                    Rectangle {
                        width: 72
                        height: 32
                        radius: 16
                        color: "transparent"
                        border.color: cLine
                        Text {
                            anchors.centerIn: parent
                            text: "取消"
                            color: cSub
                            font.pixelSize: 12
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: app.cancelQrLogin()
                        }
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: root.qrMine ? (app.qrStatus.length > 0 ? app.qrStatus : app.qrHint) : ""
                    color: cPrimary
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                }
            }

            Text {
                Layout.fillWidth: true
                visible: root.selectedSource.length > 0 && root.methods.indexOf("qrcode") < 0
                text: app.qrUnavailableReason(root.selectedSource)
                color: cSub
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }

            // ---- Cookie ----
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.hasCookie
                spacing: 8

                Text {
                    Layout.fillWidth: true
                    text: app.cookieLoginHint(root.selectedSource)
                    color: cSub
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }
                TextArea {
                    id: cookieArea
                    Layout.fillWidth: true
                    Layout.preferredHeight: 88
                    placeholderText: "粘贴该平台的完整 Cookie"
                    color: cText
                    placeholderTextColor: cSub
                    font.pixelSize: 12
                    wrapMode: TextEdit.Wrap
                    background: Rectangle {
                        color: cCard
                        radius: 10
                        border.color: cLine
                    }
                }
                Rectangle {
                    width: 116
                    height: 32
                    radius: 16
                    color: cPrimary
                    Text {
                        anchors.centerIn: parent
                        text: "验证并同步"
                        color: "#FFFFFF"
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: app.loginByCookie(root.selectedSource, cookieArea.text)
                    }
                }
            }

            // ---- 密码 ----
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.hasPassword
                spacing: 8

                Text {
                    Layout.fillWidth: true
                    text: "支持手机号或邮箱登录，密码以 MD5 提交，不会以明文离开本机。"
                    color: cSub
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }
                TextField {
                    id: pwdAccount
                    Layout.fillWidth: true
                    Layout.preferredHeight: 38
                    placeholderText: "手机号 或 邮箱"
                    color: cText
                    placeholderTextColor: cSub
                    font.pixelSize: 13
                    leftPadding: 10
                    background: Rectangle { color: cCard; radius: 10; border.color: cLine }
                }
                TextField {
                    id: pwdField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 38
                    placeholderText: "登录密码"
                    color: cText
                    placeholderTextColor: cSub
                    font.pixelSize: 13
                    leftPadding: 10
                    echoMode: TextInput.Password
                    background: Rectangle { color: cCard; radius: 10; border.color: cLine }
                }
                Rectangle {
                    width: 116
                    height: 32
                    radius: 16
                    color: cPrimary
                    Text {
                        anchors.centerIn: parent
                        text: "登录并同步"
                        color: "#FFFFFF"
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: app.loginByPassword(root.selectedSource, pwdAccount.text, pwdField.text)
                    }
                }
            }

            // ---- 手机号 ----
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.hasPhone
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    TextField {
                        id: phoneField
                        Layout.fillWidth: true
                        Layout.preferredHeight: 38
                        placeholderText: "手机号"
                        color: cText
                        placeholderTextColor: cSub
                        font.pixelSize: 13
                        leftPadding: 10
                        background: Rectangle { color: cCard; radius: 10; border.color: cLine }
                    }
                    TextField {
                        id: codeField
                        Layout.preferredWidth: 110
                        Layout.preferredHeight: 38
                        placeholderText: "验证码"
                        color: cText
                        placeholderTextColor: cSub
                        font.pixelSize: 13
                        leftPadding: 10
                        background: Rectangle { color: cCard; radius: 10; border.color: cLine }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Rectangle {
                        width: 100
                        height: 32
                        radius: 16
                        color: "transparent"
                        border.color: cLine
                        Text {
                            anchors.centerIn: parent
                            text: "发送验证码"
                            color: cSub
                            font.pixelSize: 12
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: app.sendPhoneCode(root.selectedSource, phoneField.text)
                        }
                    }
                    Rectangle {
                        width: 100
                        height: 32
                        radius: 16
                        color: cPrimary
                        Text {
                            anchors.centerIn: parent
                            text: "手机号登录"
                            color: "#FFFFFF"
                            font.pixelSize: 12
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: app.loginByPhone(root.selectedSource, phoneField.text, codeField.text)
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: cLine
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Rectangle {
                    width: 88
                    height: 32
                    radius: 16
                    color: "transparent"
                    border.color: cLine
                    Text {
                        anchors.centerIn: parent
                        text: "删除账号"
                        color: cSub
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            app.logout(root.selectedSource)
                            root.hint = "已删除该平台本地账号与 Cookie，可重新粘贴 Cookie 登录"
                            root.hintError = false
                        }
                    }
                }
                Rectangle {
                    width: 124
                    height: 32
                    radius: 16
                    color: cSoft
                    Text {
                        anchors.centerIn: parent
                        text: "同步歌单收藏"
                        color: cPrimary
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: app.syncUserLibrary(root.selectedSource)
                    }
                }
                Item { Layout.fillWidth: true }
            }

            Text {
                Layout.fillWidth: true
                text: "「删除账号」会清除本地保存的 Cookie 与登录态；重新粘贴 Cookie 即可再次登录。"
                color: cSub
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }

            Text {
                Layout.fillWidth: true
                text: root.hint.length > 0 ? root.hint : "选择平台与登录方式；已内置账号接口的平台会自动同步歌单"
                color: root.hintError ? theme.danger : cSub
                font.pixelSize: root.hintError ? 12 : 11
                font.bold: root.hintError
                wrapMode: Text.Wrap
            }
        }
    }
}
