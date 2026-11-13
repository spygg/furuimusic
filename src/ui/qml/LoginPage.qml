import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// 平台账号页
// 版式参考 AlgerMusicPlayer 的登录视图：平台先选、方式用分段 Tab、二维码带
// 「加载 / 已扫码 / 已过期」三态遮罩，而不是只甩一行文字。
Item {
    id: root
    clip: true

    readonly property bool isDark: theme.dark
    readonly property bool narrow: width < 640
    readonly property color cBg: theme.bg
    readonly property color cCard: theme.card
    readonly property color cText: theme.text
    readonly property color cSub: theme.subText
    readonly property color cPrimary: theme.accent
    readonly property color cVip: theme.vip
    readonly property color cLine: theme.line
    readonly property color cSoft: theme.accentSoft
    readonly property color cAlt: theme.accentAlt
    // 输入框/分段控件的「凹陷」底色：比卡片低一层，暗色下也不会糊成一片
    readonly property color cField: theme.dark ? Qt.rgba(1, 1, 1, 0.06) : Qt.rgba(0, 0, 0, 0.035)

    property string selectedSource: ""
    property string method: ""
    property var methods: []
    property string hint: ""
    property bool hintError: false
    property string apiStatus: ""

    readonly property bool hasQr: root.method === "qrcode"
    readonly property bool hasPhone: root.method === "phone"
    readonly property bool hasPassword: root.method === "password"
    readonly property bool qrMine: root.selectedSource.length > 0
                                   && app.qrSource === root.selectedSource
    readonly property int qrSide: root.width < 560 ? 148 : 184
    readonly property int platCols: root.width < 620 ? 2 : (root.width >= 900 ? 3 : 2)
    // accounts 是 QAbstractListModel，QML 里既没有 length 也不能按下标取，
    // 已登录数量只能由 C++ 算好再推过来（登录/登出时手动刷新）
    property int loggedCount: 0
    property int totalCount: 0
    property bool loggedNow: false

    function refreshCounts() {
        root.totalCount = app.accountTotal()
        root.loggedCount = app.loggedAccountCount()
        root.loggedNow = root.selectedSource.length > 0
                         && app.accountLogged(root.selectedSource)
    }

    // 把 C++ 侧的状态文案收敛成有限相位，遮罩只认相位，不猜字符串
    readonly property string qrPhase: {
        if (!root.qrMine) return "idle"
        var s = app.qrStatus || ""
        if (s.indexOf("过期") >= 0) return "expired"
        if (s.indexOf("已扫码") >= 0) return "scanned"
        if (s.indexOf("成功") >= 0) return "done"
        if (s.indexOf("失败") >= 0 || s.indexOf("风控") >= 0 || s.indexOf("拒绝") >= 0)
            return "error"
        if (app.qrActive && app.qrImagePath.length === 0) return "loading"
        if (app.qrActive) return "waiting"
        return "idle"
    }

    function methodLabel(m) {
        // 酷我的三个 Tab 对齐官网文案：帐号登录 / 快捷登录 / 微信
        if (root.selectedSource === "kuwo") {
            if (m === "qrcode") return "微信扫码"
            if (m === "phone") return "快捷登录"
            if (m === "password") return "帐号登录"
        }
        if (m === "qrcode") return "二维码"
        if (m === "phone") return "手机号"
        if (m === "password") return "密码"
        return m
    }

    // 酷我要求先有图形验证码；进页面/切 Tab 时自动取一张，用户不用先点一下图片
    function ensureCaptcha() {
        if (root.selectedSource !== "kuwo") return
        if (app.captchaImagePath.length === 0)
            app.refreshCaptcha(root.selectedSource)
    }

    // 二维码区本身就是「获取 / 刷新」按钮：不再单放一颗「获取二维码」按钮。
    // 已登录态不允许点，避免刚拿到登录态又把它作废。
    function requestQr() {
        if (root.selectedSource.length === 0 || root.qrPhase === "done") return
        app.startQrLogin(root.selectedSource)
    }

    onMethodChanged: root.ensureCaptcha()

    function updateMethods() {
        root.methods = root.selectedSource.length > 0 ? app.loginMethods(root.selectedSource) : []
        // 登录结果回来时也会调这里刷新，别把用户切到一半的 Tab 强行弹回第一个：
        // 只有当前方式已经不在列表里（换平台 / 方式被移除）才回退到首个可用方式。
        if (root.methods.indexOf(root.method) < 0)
            root.method = root.methods.length > 0 ? root.methods[0] : ""
        root.apiStatus = root.selectedSource.length > 0
                        ? app.accountApiStatus(root.selectedSource) : ""
        if (root.selectedSource.length > 0)
            apiBaseField.text = app.accountApiBaseFor(root.selectedSource)
    }

    onSelectedSourceChanged: {
        // 换平台时把上一个平台的失败原因清掉，否则会挂在新平台卡片底下误导人
        root.hint = ""
        root.hintError = false
        // 换平台回到该平台自己的默认方式（酷狗/咪咕=二维码，网易云=密码）。
        // 清空后 updateMethods() 会落到第一个方式；而登录结果回来时的
        // updateMethods() 不清空，用户当前选的 Tab 得以保留。
        root.method = ""
        root.updateMethods()
        root.refreshCounts()
        root.ensureCaptcha()
    }

    Component.onCompleted: {
        app.refreshAccounts()
        if (app.sourceIds.length > 0)
            root.selectedSource = app.sourceIds[0]
        root.updateMethods()
        root.refreshCounts()
        root.ensureCaptcha()
    }

    Connections {
        target: app
        function onLoginStateChanged(sourceId, ok, message) {
            root.hint = (ok ? "登录成功：" : "登录失败：") + message
            // 8821 / 行为验证码是服务端风控，重试只会更糟；直接指到下面的浏览器登录入口
            if (!ok && (message.indexOf("8821") >= 0 || message.indexOf("行为验证码") >= 0))
                root.hint += "\n不要用密码反复重试：请拉到本卡片下方，用「打开官网登录」后导入 Cookie。"
            root.hintError = !ok
            app.refreshAccounts()
            root.updateMethods()
            root.refreshCounts()
            // 酷我登录成功后旧验证码已作废，顺手换一张，别让下次进来看到一张废码
            if (ok) root.ensureCaptcha()
        }
        function onQrChanged() {
            // 只有本平台的扫码会话才能改这页的提示：后台给别的平台出码时，
            // 当前平台的二维码区不该跟着变成「等待扫码」。
            // 另外用户已经切到密码/手机号登录时，二维码轮询的定时状态更新
            // 不能把「登录失败：<原因>」这条提示冲掉。
            if (!root.qrMine || root.method !== "qrcode") return
            root.hint = app.qrStatus.length > 0 ? app.qrStatus : app.qrHint
            // 风控/接口异常用醒目的错误色，别让"未登录"看起来像正常等待
            root.hintError = app.qrStatus.indexOf("风控") >= 0
                    || app.qrStatus.indexOf("失败") >= 0
                    || app.qrStatus.indexOf("过期") >= 0
                    || app.qrStatus.indexOf("拒绝") >= 0
            // 正常的「等待扫码 / 已扫码」二维码区已经写了一份，底部不再重复，
            // 只有出错时才把原因常驻在卡片底部
            if (!root.hintError)
                root.hint = ""

            // 8821 / 账号需要二次验证属终态，继续留在扫码页只会让用户反复重试。
            // 有密码方式就直接切过去，并把原因留在卡片底部。
            var st = app.qrStatus || ""
            if ((st.indexOf("8821") >= 0 || st.indexOf("二次验证") >= 0)
                    && root.methods.indexOf("password") >= 0 && root.method === "qrcode") {
                root.method = "password"
                root.hint = "网易云拒绝了扫码登录（账号级风控），已切到密码登录"
                root.hintError = true
            }
        }
        function onSyncFinished(sourceId, ok, message) {
            root.hint = (ok ? "同步完成：" : "同步失败：") + message
            root.hintError = !ok
        }
        function onSettingsChanged() {
            if (root.selectedSource.length > 0)
                root.apiStatus = app.accountApiStatus(root.selectedSource)
        }
    }

    // 页面底色 + 顶部一层品牌色淡出，跟播放页的沉浸感呼应，又不抢内容
    Rectangle {
        anchors.fill: parent
        color: cBg
    }
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(cPrimary.r, cPrimary.g, cPrimary.b, root.isDark ? 0.16 : 0.10) }
            GradientStop { position: 0.42; color: "transparent" }
        }
    }
    // 两个极淡的圆环，纯装饰
    Rectangle {
        width: 320
        height: 320
        radius: 160
        x: -120
        y: -140
        color: "transparent"
        border.width: 1
        border.color: Qt.rgba(cPrimary.r, cPrimary.g, cPrimary.b, 0.10)
    }
    Rectangle {
        width: 220
        height: 220
        radius: 110
        anchors.right: parent.right
        anchors.rightMargin: -80
        y: 120
        color: "transparent"
        border.width: 1
        border.color: Qt.rgba(cAlt.r, cAlt.g, cAlt.b, 0.12)
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: body.height + 40
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: body
            width: Math.min(parent.width - (root.narrow ? 28 : 56), 880)
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: root.narrow ? 18 : 26
            // 不写 height: implicitHeight —— 那是对自身 implicitHeight 的自绑定，
            // 内容一换行就会和子项一起被判成绑定循环；ColumnLayout 默认就按 implicitHeight 定高
            spacing: 16

            // ---- 页头 ----
            RowLayout {
                Layout.fillWidth: true
                spacing: 14

                Rectangle {
                    Layout.preferredWidth: root.narrow ? 40 : 46
                    Layout.preferredHeight: root.narrow ? 40 : 46
                    radius: 14
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: theme.heroStart }
                        GradientStop { position: 1.0; color: theme.heroEnd }
                    }
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: "user"
                        size: root.narrow ? 20 : 23
                        color: "#FFFFFF"
                    }
                }

                ColumnLayout {
                    spacing: 3
                    Text {
                        text: "账号"
                        color: cText
                        font.pixelSize: root.narrow ? 20 : 23
                        font.bold: true
                    }
                    Text {
                        text: "每个平台独立登录，登录态只保存在本机"
                        color: cSub
                        font.pixelSize: 11
                    }
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    Layout.preferredWidth: loggedTag.implicitWidth + 20
                    Layout.preferredHeight: 26
                    radius: 13
                    color: Qt.rgba(cPrimary.r, cPrimary.g, cPrimary.b, 0.10)
                    Text {
                        id: loggedTag
                        anchors.centerIn: parent
                        text: root.loggedCount + " / " + root.totalCount + " 已登录"
                        color: cPrimary
                        font.pixelSize: 11
                        font.bold: true
                    }
                }
            }

            // ---- 平台选择 ----
            Flow {
                id: accountFlow
                Layout.fillWidth: true
                Layout.preferredHeight: implicitHeight
                spacing: 10

                Repeater {
                    model: app.accounts
                    delegate: Rectangle {
                        width: (accountFlow.width - (root.platCols - 1) * 10) / root.platCols
                        height: 64
                        radius: 14
                        color: model.source === root.selectedSource ? cSoft : cCard
                        border.width: model.source === root.selectedSource ? 1.5 : 1
                        border.color: model.source === root.selectedSource
                                      ? app.sourceColor(model.source) : cLine

                        property color accent: app.sourceColor(model.source)
                        property bool canLogin: app.loginMethods(model.source).length > 0

                        // 选中态左侧色条，扫一眼就知道在看哪个平台
                        Rectangle {
                            width: 3
                            height: 26
                            radius: 2
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            color: accent
                            visible: model.source === root.selectedSource
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.selectedSource = model.source
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 10
                            spacing: 9

                            Rectangle {
                                Layout.preferredWidth: 34
                                Layout.preferredHeight: 34
                                radius: 17
                                color: Qt.rgba(accent.r, accent.g, accent.b, 0.14)

                                Image {
                                    id: avatarImg
                                    anchors.fill: parent
                                    anchors.margins: 2
                                    source: model.avatar.length > 0
                                            ? (model.avatar.indexOf("http") === 0
                                               ? model.avatar : platform.toFileUrl(model.avatar))
                                            : ""
                                    sourceSize.width: 96
                                    sourceSize.height: 96
                                    fillMode: Image.PreserveAspectCrop
                                    visible: status === Image.Ready
                                    asynchronous: true
                                }
                                Text {
                                    anchors.centerIn: parent
                                    visible: avatarImg.status !== Image.Ready
                                    text: model.name.length > 0 ? model.name.charAt(0) : "♪"
                                    color: accent
                                    font.pixelSize: 15
                                    font.bold: true
                                }
                                Rectangle {
                                    width: 9
                                    height: 9
                                    radius: 4.5
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    color: model.logged ? theme.success : theme.line
                                    border.width: 1.5
                                    border.color: model.source === root.selectedSource ? cSoft : cCard
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    Layout.fillWidth: true
                                    text: model.name
                                    color: cText
                                    font.pixelSize: 13
                                    font.bold: true
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    // 只拿到 uid 没有昵称时别重复右上角的「已登录」，改显示歌单数
                                    text: model.logged
                                          ? (model.nickname.length > 0
                                             ? model.nickname
                                             : (model.playlistCount > 0
                                                ? model.playlistCount + " 个歌单"
                                                : "登录态有效"))
                                          : (canLogin ? "未登录" : "不支持登录")
                                    color: model.logged ? accent : cSub
                                    font.pixelSize: theme.fontXs
                                    elide: Text.ElideRight
                                }
                            }

                            GlyphIcon {
                                Layout.preferredWidth: 14
                                Layout.preferredHeight: 14
                                name: "chevron"
                                size: 14
                                color: model.source === root.selectedSource ? accent : cSub
                                opacity: model.source === root.selectedSource ? 1 : 0.45
                                visible: !root.narrow
                            }
                        }
                    }
                }
            }

            // ---- 登录卡 ----
            Rectangle {
                Layout.fillWidth: true
                // 用 implicitHeight 而不是 Layout.preferredHeight：后者会和内部
                // 自动换行 Text 的 implicitHeight 形成 preferredHeight 绑定循环
                implicitHeight: cardCol.height + (root.narrow ? 32 : 40)
                radius: 20
                color: cCard
                border.width: 1
                border.color: cLine

                ColumnLayout {
                    id: cardCol
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: root.narrow ? 16 : 20
                    // 显式给宽度：用 anchors.right 让布局反推宽度时，会把「宽度→换行高度→
                    // 卡片高度」这条链绕成绑定循环（Qt 会报 implicitHeight 循环）
                    width: parent.width - 2 * (root.narrow ? 16 : 20)
                    spacing: 14

                    // 卡头：平台身份 + 账号状态
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Text {
                            text: root.selectedSource.length > 0
                                  ? app.sourceName(root.selectedSource) : "平台"
                            color: cText
                            font.pixelSize: 15
                            font.bold: true
                        }

                        Rectangle {
                            Layout.preferredWidth: 22
                            Layout.preferredHeight: 3
                            radius: 2
                            color: root.selectedSource.length > 0
                                   ? app.sourceColor(root.selectedSource) : cLine
                        }

                        Item { Layout.fillWidth: true }

                        // 已登录徽标 / 可用性徽标
                        Rectangle {
                            Layout.preferredWidth: stateTag.implicitWidth + 18
                            Layout.preferredHeight: 24
                            radius: 12
                            color: {
                                if (root.methods.length === 0)
                                    return Qt.rgba(cVip.r, cVip.g, cVip.b, 0.14)
                                return Qt.rgba(cPrimary.r, cPrimary.g, cPrimary.b, 0.10)
                            }
                            Text {
                                id: stateTag
                                anchors.centerIn: parent
                                text: root.methods.length === 0 ? "不可用"
                                      : (root.loggedNow ? "已登录" : "未登录")
                                color: root.methods.length === 0 ? cVip
                                       : (root.loggedNow ? theme.success : cPrimary)
                                font.pixelSize: theme.fontXs
                                font.bold: true
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: cLine
                        visible: root.methods.length > 0
                    }

                    // 登录方式：分段 Tab（内部上下各留 4 边距，所以容器比触控最小值高 8）
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.max(40, theme.touchMin + 8)
                        radius: 12
                        color: cField
                        visible: root.methods.length > 1

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 4
                            spacing: 4

                            Repeater {
                                model: root.methods
                                delegate: Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    radius: 9
                                    color: root.method === modelData ? cCard : "transparent"
                                    border.width: root.method === modelData ? 1 : 0
                                    border.color: cLine

                                    Text {
                                        anchors.centerIn: parent
                                        text: root.methodLabel(modelData)
                                        color: root.method === modelData ? cText : cSub
                                        font.pixelSize: 12
                                        font.bold: root.method === modelData
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        onClicked: {
                                            if (root.method !== modelData) {
                                                root.method = modelData
                                                root.hint = ""
                                                root.hintError = false
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // ---- 该平台没有可用的应用内登录方式 ----
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.selectedSource.length > 0 && root.methods.length === 0
                        spacing: 10

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Rectangle {
                                Layout.preferredWidth: 26
                                Layout.preferredHeight: 26
                                radius: 13
                                color: Qt.rgba(cVip.r, cVip.g, cVip.b, 0.16)
                                Text {
                                    anchors.centerIn: parent
                                    text: "!"
                                    color: cVip
                                    font.pixelSize: 13
                                    font.bold: true
                                }
                            }
                            Text {
                                text: "该平台暂不支持应用内登录"
                                color: cText
                                font.pixelSize: 13
                                font.bold: true
                            }
                            Item { Layout.fillWidth: true }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: app.loginUnavailableReason(root.selectedSource)
                            color: cSub
                            font.pixelSize: 11
                            lineHeight: 1.4
                            wrapMode: Text.Wrap
                        }

                        Text {
                            Layout.fillWidth: true
                            text: "搜索与播放不受影响；要同步「我的歌单 / 收藏」，在下方填写账号 API 服务地址即可。"
                            color: cSub
                            font.pixelSize: 11
                            lineHeight: 1.4
                            wrapMode: Text.Wrap
                        }
                    }

                    // ---- 二维码 ----
                    // 窄屏（手机竖屏）必须上下堆叠：并排时说明文字只剩一百多像素宽，
                    // 「需要先配置账号 API 服务」这种长句会被压成七八行、还把码面挤歪。
                    GridLayout {
                        Layout.fillWidth: true
                        visible: root.hasQr
                        columns: root.narrow ? 1 : 2
                        columnSpacing: root.narrow ? 0 : 22
                        rowSpacing: root.narrow ? 14 : 0

                        // 码面 + 三态遮罩
                        Item {
                            Layout.preferredWidth: root.qrSide
                            Layout.preferredHeight: root.qrSide
                            Layout.alignment: root.narrow ? Qt.AlignHCenter : Qt.AlignVCenter

                            Rectangle {
                                anchors.fill: parent
                                radius: 18
                                color: "#FFFFFF"
                                border.width: 1
                                border.color: Qt.rgba(0, 0, 0, 0.08)
                            }

                            Image {
                                anchors.centerIn: parent
                                width: parent.width - 12
                                height: parent.height - 12
                                fillMode: Image.PreserveAspectFit
                                smooth: false
                                source: (root.qrMine && app.qrImagePath.length > 0)
                                        ? (app.qrImagePath.indexOf("http") === 0
                                           ? app.qrImagePath : platform.toFileUrl(app.qrImagePath))
                                        : ""
                            }

                            // 占位：还没出码
                            Column {
                                anchors.centerIn: parent
                                spacing: 8
                                visible: root.qrPhase === "idle" || root.qrPhase === "loading"
                                GlyphIcon {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    name: "refresh"
                                    size: 26
                                    color: theme.mutedText
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: root.qrPhase === "loading" ? "正在生成…" : "点击获取二维码"
                                    color: theme.mutedText
                                    font.pixelSize: 11
                                }
                            }

                            // 遮罩：已扫码 / 已过期 / 失败 / 已登录（登录成功后码面已清空，
                            // 这里补一个成功态，避免白白留一块空框）
                            Rectangle {
                                id: qrOverlay
                                readonly property bool done: root.qrPhase === "done"
                                anchors.fill: parent
                                radius: 18
                                color: done ? root.cSoft
                                            : (root.qrPhase === "expired" ? Qt.rgba(0, 0, 0, 0.62)
                                                                          : Qt.rgba(0, 0, 0, 0.55))
                                border.width: done ? 1 : 0
                                border.color: cLine
                                visible: root.qrPhase === "scanned"
                                         || root.qrPhase === "expired"
                                         || root.qrPhase === "error"
                                         || done

                                Column {
                                    anchors.centerIn: parent
                                    spacing: 8

                                    Rectangle {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        width: 38
                                        height: 38
                                        radius: 19
                                        color: qrOverlay.done ? theme.success
                                               : (root.qrPhase === "scanned" ? Qt.rgba(1, 1, 1, 0.95)
                                                                             : Qt.rgba(1, 1, 1, 0.14))
                                        GlyphIcon {
                                            anchors.centerIn: parent
                                            name: (root.qrPhase === "scanned" || qrOverlay.done) ? "check"
                                                  : (root.qrPhase === "error" ? "close" : "refresh")
                                            size: 20
                                            color: (root.qrPhase === "scanned" || qrOverlay.done)
                                                   ? "#111318" : "#FFFFFF"
                                        }
                                    }
                                    Text {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: qrOverlay.done ? "已登录"
                                              : (root.qrPhase === "scanned" ? "已扫码"
                                                 : (root.qrPhase === "error" ? "登录失败" : "二维码已过期"))
                                        color: qrOverlay.done ? cText : "#FFFFFF"
                                        font.pixelSize: 12
                                        font.bold: true
                                    }
                                    Text {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        visible: root.qrPhase !== "error"
                                        text: qrOverlay.done ? "登录态已保存在本机"
                                              : (root.qrPhase === "scanned" ? "请在手机上确认登录" : "点击二维码重新获取")
                                        color: qrOverlay.done ? cSub : Qt.rgba(1, 1, 1, 0.72)
                                        font.pixelSize: theme.fontXs
                                    }
                                }
                            }

                            // 整个码面就是「获取 / 刷新二维码」的点击区，覆盖在遮罩之上：
                            // 出码前、等待扫码、已过期、失败态都可以直接点一下重来。
                            MouseArea {
                                anchors.fill: parent
                                z: 10
                                cursorShape: Qt.PointingHandCursor
                                enabled: root.selectedSource.length > 0 && root.qrPhase !== "done"
                                onClicked: root.requestQr()
                            }
                        }

                        // 说明 + 操作
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 10

                            RowLayout {
                                spacing: 7
                                Rectangle {
                                    Layout.preferredWidth: 7
                                    Layout.preferredHeight: 7
                                    radius: 3.5
                                    color: {
                                        if (root.qrPhase === "scanned") return theme.success
                                        if (root.qrPhase === "expired" || root.qrPhase === "error")
                                            return theme.danger
                                        return cPrimary
                                    }
                                }
                                Text {
                                    Layout.fillWidth: true
                                    // 扫码状态可能是长句（例如「需要先配置账号 API 服务」），
                                    // 不限宽会把整块布局顶出面板，这里按可用宽度硬约束并换行；
                                    // 窄屏已经上下堆叠成整行，就不必再压窄
                                    Layout.maximumWidth: root.narrow
                                                         ? Number.POSITIVE_INFINITY
                                                         : Math.max(120, root.width * 0.46)
                                    text: root.qrMine && app.qrStatus.length > 0
                                          ? app.qrStatus : "扫码登录"
                                    color: root.qrPhase === "expired" || root.qrPhase === "error"
                                           ? theme.danger : cText
                                    font.pixelSize: 13
                                    font.bold: true
                                    wrapMode: Text.Wrap
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                // 已登录就不必再教怎么扫码了
                                visible: root.qrPhase !== "done"
                                text: root.selectedSource === "kuwo"
                                      ? "打开微信「扫一扫」扫描二维码，手机上确认后这台设备自动登录酷我"
                                      : "打开「" + app.loginAppName(root.selectedSource)
                                        + "」→ 右上角扫一扫；手机确认后这台设备自动登录"
                                color: cSub
                                font.pixelSize: 11
                                lineHeight: 1.4
                                wrapMode: Text.Wrap
                            }

                            // 取代原来的「获取二维码 / 重新获取」按钮：码面本身就是入口
                            Text {
                                Layout.fillWidth: true
                                visible: root.qrPhase !== "done"
                                text: root.qrPhase === "idle" || root.qrPhase === "loading"
                                      ? "点击左侧二维码区域即可获取"
                                      : "点击左侧二维码可随时重新获取（旧码立即作废）"
                                color: theme.mutedText
                                font.pixelSize: theme.fontXs
                                wrapMode: Text.Wrap
                            }

                            // 网易云的扫码是账号级风控的重点对象：把 8821 的含义直接说清，
                            // 并给一键切密码的入口，避免用户反复重试反而加重风控
                            Text {
                                Layout.fillWidth: true
                                visible: root.selectedSource === "netease"
                                text: "提示：网易云常以 8821 账号级风控拒绝第三方客户端扫码，"
                                      + "反复重试只会让风控更重，扫码不成功请直接用密码登录。"
                                color: Qt.rgba(cVip.r, cVip.g, cVip.b, 0.92)
                                font.pixelSize: theme.fontXs
                                lineHeight: 1.45
                                wrapMode: Text.Wrap
                            }

                            // 用 Flow 而不是 RowLayout：窄屏下按钮会自动换行，
                            // 不会把右列撑宽、挤坏二维码
                            Flow {
                                Layout.fillWidth: true
                                Layout.preferredHeight: implicitHeight
                                spacing: 8
                                Rectangle {
                                    width: 72
                                    height: theme.touchMin
                                    radius: height / 2
                                    color: "transparent"
                                    border.width: 1
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
                                // 扫码是本平台的次选方式时，给一个一键切到密码的入口
                                Rectangle {
                                    width: 96
                                    height: theme.touchMin
                                    radius: height / 2
                                    visible: root.methods.indexOf("password") >= 0
                                             && root.methods.length > 1
                                    color: Qt.rgba(cPrimary.r, cPrimary.g, cPrimary.b, 0.10)
                                    Text {
                                        anchors.centerIn: parent
                                        text: "用密码登录"
                                        color: cPrimary
                                        font.pixelSize: 12
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        onClicked: root.method = "password"
                                    }
                                }
                            }
                        }
                    }

                    // ---- 密码 ----
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.hasPassword
                        spacing: 12

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Text {
                                text: root.selectedSource === "migu" ? "手机号 / 邮箱 / 用户名"
                                      : (root.selectedSource === "kuwo" ? "酷我账号 / 手机号 / 邮箱"
                                         : (root.selectedSource === "kugou" ? "酷狗账号 / 手机号 / 邮箱"
                                                                            : "手机号 或 邮箱"))
                                color: cSub
                                font.pixelSize: 11
                            }
                            TextField {
                                id: pwdAccount
                                Layout.fillWidth: true
                                Layout.preferredHeight: theme.touchMin
                                placeholderText: root.selectedSource === "migu"
                                                 ? "咪咕账号"
                                                 : (root.selectedSource === "kuwo"
                                                    ? "酷我账号"
                                                    : (root.selectedSource === "kugou"
                                                       ? "酷狗账号" : "手机号 / 邮箱"))
                                color: cText
                                placeholderTextColor: theme.mutedText
                                font.pixelSize: 13
                                leftPadding: 12
                                background: Rectangle {
                                    color: cField
                                    radius: 11
                                    border.width: 1
                                    border.color: pwdAccount.activeFocus ? cPrimary : cLine
                                }
                                onAccepted: pwdField.forceActiveFocus()
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Text {
                                text: "登录密码"
                                color: cSub
                                font.pixelSize: 11
                            }
                            TextField {
                                id: pwdField
                                Layout.fillWidth: true
                                Layout.preferredHeight: theme.touchMin
                                placeholderText: "密码"
                                color: cText
                                placeholderTextColor: theme.mutedText
                                font.pixelSize: 13
                                leftPadding: 12
                                echoMode: TextInput.Password
                                background: Rectangle {
                                    color: cField
                                    radius: 11
                                    border.width: 1
                                    border.color: pwdField.activeFocus ? cPrimary : cLine
                                }
                                onAccepted: app.loginByPassword(root.selectedSource,
                                                                pwdAccount.text, pwdField.text,
                                                                pwdCaptcha.text)
                            }
                        }

                        CaptchaField {
                            id: pwdCaptcha
                            Layout.fillWidth: true
                            visible: root.selectedSource === "kuwo"
                            sourceId: root.selectedSource
                            imagePath: app.captchaImagePath
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: theme.touchMin
                            radius: height / 2
                            gradient: Gradient {
                                GradientStop { position: 0.0; color: theme.heroStart }
                                GradientStop { position: 1.0; color: theme.heroEnd }
                            }
                            Text {
                                anchors.centerIn: parent
                                text: "登录并同步"
                                color: "#FFFFFF"
                                font.pixelSize: 13
                                font.bold: true
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: app.loginByPassword(root.selectedSource,
                                                               pwdAccount.text, pwdField.text,
                                                               pwdCaptcha.text)
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: root.selectedSource === "migu"
                                  ? "密码在本地用服务端下发的 RSA 公钥加密后提交，不会以明文离开本机。"
                                  : (root.selectedSource === "kuwo"
                                     ? "酷我与官网一致：密码随 HTTPS 提交（不做本地哈希），并要求图形验证码；"
                                       + "点验证码图片可换一张，登录失败后会自动刷新。"
                                     : (root.selectedSource === "kugou"
                                        ? "密码在本地用 AES-256 加密后提交。若酷狗要求人机验证，"
                                          + "请先在官方 App/网页完成一次登录验证再重试，或改用「二维码」。"
                                        : "密码以 MD5 提交，不会以明文离开本机；若提示行为验证或 IP 异常，请勿连续重试。"))
                            color: cSub
                            font.pixelSize: theme.fontXs
                            lineHeight: 1.4
                            wrapMode: Text.Wrap
                        }

                        // 网易云的 8821 是账号级风控：服务端要求行为验证码（滑块），
                        // 本工程没链 WebEngine，应用内做不了这一步；但浏览器里能正常登录。
                        // 所以给一条确定可用的退路：官网登录 → 复制 Cookie → 粘回来。
                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: root.selectedSource === "netease"
                            spacing: 8

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 1
                                color: cLine
                            }
                            Text {
                                Layout.fillWidth: true
                                text: "密码登录提示「行为验证码 / 8821」时，请用浏览器登录"
                                color: cText
                                font.pixelSize: 12
                                font.bold: true
                                wrapMode: Text.Wrap
                            }
                            Text {
                                Layout.fillWidth: true
                                text: "这是网易云对第三方客户端的账号级风控，换协议或重试都绕不过。"
                                      + "在官网登录成功后，按 F12 → Network → 任一请求的 Request Headers，"
                                      + "复制整行 Cookie 或它的值（必须含 MUSIC_U=…）粘到下面即可。"
                                      + "带不带「Cookie: 」前缀都能识别。"
                                color: cSub
                                font.pixelSize: theme.fontXs
                                lineHeight: 1.4
                                wrapMode: Text.Wrap
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Rectangle {
                                    Layout.preferredWidth: 116
                                    Layout.preferredHeight: theme.touchMin
                                    radius: height / 2
                                    color: Qt.rgba(cPrimary.r, cPrimary.g, cPrimary.b, 0.10)
                                    Text {
                                        anchors.centerIn: parent
                                        text: "打开官网登录"
                                        color: cPrimary
                                        font.pixelSize: 12
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: app.openExternal("https://music.163.com/")
                                    }
                                }
                                TextField {
                                    id: browserCookieField
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: theme.touchMin
                                    placeholderText: "粘贴登录后的 Cookie（含 MUSIC_U=…）"
                                    color: cText
                                    placeholderTextColor: theme.mutedText
                                    font.pixelSize: 12
                                    leftPadding: 12
                                    background: Rectangle {
                                        color: cField
                                        radius: 11
                                        border.width: 1
                                        border.color: browserCookieField.activeFocus ? cPrimary : cLine
                                    }
                                    onAccepted: app.loginByCookie(root.selectedSource, text)
                                }
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: theme.touchMin
                                radius: height / 2
                                color: "transparent"
                                border.width: 1
                                border.color: cLine
                                Text {
                                    anchors.centerIn: parent
                                    text: "导入浏览器登录态并同步"
                                    color: cSub
                                    font.pixelSize: 12
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: app.loginByCookie(root.selectedSource,
                                                                 browserCookieField.text)
                                }
                            }
                        }
                    }

                    // ---- 手机号 ----
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.hasPhone
                        spacing: 12

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            TextField {
                                id: phoneField
                                Layout.fillWidth: true
                                Layout.preferredHeight: theme.touchMin
                                placeholderText: "手机号"
                                color: cText
                                placeholderTextColor: theme.mutedText
                                font.pixelSize: 13
                                leftPadding: 12
                                background: Rectangle {
                                    color: cField
                                    radius: 11
                                    border.width: 1
                                    border.color: phoneField.activeFocus ? cPrimary : cLine
                                }
                            }
                            Rectangle {
                                Layout.preferredWidth: 100
                                Layout.preferredHeight: theme.touchMin
                                radius: 11
                                color: "transparent"
                                border.width: 1
                                border.color: cLine
                                Text {
                                    anchors.centerIn: parent
                                    text: "发送验证码"
                                    color: cSub
                                    font.pixelSize: 11
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: app.sendPhoneCode(root.selectedSource, phoneField.text,
                                                                 phoneCaptcha.text)
                                }
                            }
                        }

                        CaptchaField {
                            id: phoneCaptcha
                            Layout.fillWidth: true
                            visible: root.selectedSource === "kuwo"
                            sourceId: root.selectedSource
                            imagePath: app.captchaImagePath
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: root.selectedSource === "kuwo"
                            text: "酷我的短信登录要先过图形验证码：填好手机号与图形验证码再点「发送验证码」。"
                            color: cSub
                            font.pixelSize: theme.fontXs
                            lineHeight: 1.4
                            wrapMode: Text.Wrap
                        }

                        TextField {
                            id: codeField
                            Layout.fillWidth: true
                            Layout.preferredHeight: theme.touchMin
                            placeholderText: "短信验证码"
                            color: cText
                            placeholderTextColor: theme.mutedText
                            font.pixelSize: 13
                            leftPadding: 12
                            background: Rectangle {
                                color: cField
                                radius: 11
                                border.width: 1
                                border.color: codeField.activeFocus ? cPrimary : cLine
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: theme.touchMin
                            radius: height / 2
                            gradient: Gradient {
                                GradientStop { position: 0.0; color: theme.heroStart }
                                GradientStop { position: 1.0; color: theme.heroEnd }
                            }
                            Text {
                                anchors.centerIn: parent
                                text: "手机号登录"
                                color: "#FFFFFF"
                                font.pixelSize: 13
                                font.bold: true
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: app.loginByPhone(root.selectedSource,
                                                            phoneField.text, codeField.text)
                            }
                        }
                    }

                    // 结果 / 进度：贴一条状态栏，成功失败都落在同一个位置
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 2
                        visible: root.hint.length > 0
                        spacing: 7
                        Rectangle {
                            Layout.preferredWidth: 6
                            Layout.preferredHeight: 6
                            radius: 3
                            color: root.hintError ? theme.danger : theme.success
                        }
                        Text {
                            Layout.fillWidth: true
                            text: root.hint
                            color: root.hintError ? theme.danger : cSub
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }

            // ---- 账号操作 ----
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                visible: root.selectedSource.length > 0

                Rectangle {
                    Layout.preferredWidth: syncRow.implicitWidth + 22
                    Layout.preferredHeight: theme.touchMin
                    radius: height / 2
                    color: cSoft
                    RowLayout {
                        id: syncRow
                        anchors.centerIn: parent
                        spacing: 6
                        GlyphIcon {
                            Layout.preferredWidth: 13
                            Layout.preferredHeight: 13
                            name: "sync"
                            size: 13
                            color: cPrimary
                        }
                        Text {
                            text: "同步歌单收藏"
                            color: cPrimary
                            font.pixelSize: 11
                            font.bold: true
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: app.syncUserLibrary(root.selectedSource)
                    }
                }

                Rectangle {
                    Layout.preferredWidth: 96
                    Layout.preferredHeight: theme.touchMin
                    radius: height / 2
                    color: "transparent"
                    border.width: 1
                    border.color: cLine
                    RowLayout {
                        anchors.centerIn: parent
                        spacing: 6
                        GlyphIcon {
                            Layout.preferredWidth: 13
                            Layout.preferredHeight: 13
                            name: "trash"
                            size: 13
                            color: cSub
                        }
                        Text {
                            text: "删除账号"
                            color: cSub
                            font.pixelSize: 11
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            app.logout(root.selectedSource)
                            root.hint = "已删除该平台本地账号与登录态"
                            root.hintError = false
                            root.refreshCounts()
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                Text {
                    text: root.apiStatus
                    color: cSub
                    font.pixelSize: theme.fontXs
                    elide: Text.ElideRight
                    Layout.maximumWidth: root.width * 0.45
                    visible: !root.narrow
                }
            }

            // ---- 账号 API 服务 ----
            // 这里刻意不用「外框 Rectangle + 内部 ColumnLayout implicitHeight」的包裹写法：
            // 内层 Text 是 wrap 的，高度依赖宽度、宽度又来自外框，QML 会报 preferredHeight 绑定循环。
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: cLine
                visible: root.selectedSource.length > 0
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: root.selectedSource.length > 0
                spacing: 8

                Text {
                    text: "账号 API 服务（可选）"
                    color: cText
                    font.pixelSize: 12
                    font.bold: true
                }
                Text {
                    Layout.fillWidth: true
                    text: "未内置账号接口的平台，可自建服务后在此填写地址，用于同步歌单与收藏。"
                    color: cSub
                    font.pixelSize: theme.fontXs
                    lineHeight: 1.4
                    wrapMode: Text.Wrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    TextField {
                        id: apiBaseField
                        Layout.fillWidth: true
                        Layout.preferredHeight: theme.touchMin
                        placeholderText: "http://127.0.0.1:9764"
                        color: cText
                        placeholderTextColor: theme.mutedText
                        font.pixelSize: 12
                        leftPadding: 11
                        background: Rectangle { color: cField; radius: 10; border.width: 1; border.color: cLine }
                    }
                    Rectangle {
                        Layout.preferredWidth: 62
                        Layout.preferredHeight: theme.touchMin
                        radius: 10
                        color: cField
                        Text {
                            anchors.centerIn: parent
                            text: "保存"
                            color: cText
                            font.pixelSize: 12
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                app.setAccountApiBaseFor(root.selectedSource, apiBaseField.text)
                                root.apiStatus = app.accountApiStatus(root.selectedSource)
                                root.hint = "账号接口地址已保存"
                                root.hintError = false
                            }
                        }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                text: "「删除账号」只清除本机保存的登录态，可随时重新扫码或登录。"
                color: cSub
                font.pixelSize: theme.fontXs
                wrapMode: Text.Wrap
                bottomPadding: 8
            }
        }
    }

}
