import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtWebView

// 应用内浏览器：桌面与 Android 共用这一份 QML，网页区统一由 QtWebView 的
// WebView 提供（Windows 是随包部署的 WebView2 后端，Android 是系统 WebView）。
//
// 两种模式：
//   browse          普通浏览，地址栏可打开任意网页
//   login:<sourceId> 登录态导入，检测到带 MUSIC_U 的 Cookie 后自动回灌 AuthManager
//
// 网页区用 Loader 动态创建/销毁，避免 QML 隐藏时原生 WebView 子窗口残留。
Item {
    id: root
    anchors.fill: parent
    visible: browser.opened
    z: 9000

    // 移动端（手机/车机）整页铺满：浮层卡片在竖屏手机上只会剩一小块，
    // 网页本身又需要宽度，所以直接占满可用区域。桌面仍保留居中卡片，
    // 否则会盖住无边框窗口的拖动/最小化控件。
    readonly property bool fullscreenMode: platform.isMobile
    // 全屏模式下网页要真正贴边，只给工具条/地址栏这些「外壳」留左右内边距
    readonly property int chromePad: fullscreenMode ? 10 : 0

    readonly property string sourceId: browser.mode.indexOf("login:") === 0
                                       ? browser.mode.substring(6) : ""
    readonly property bool loginMode: sourceId.length > 0

    // 地址栏回车/跳转的唯一入口。
    // 不依赖 `url: browser.url` 绑定：站点内部跳转后 browser.url 是旧的，
    // 一旦绑定在别处被重新求值就会把旧地址写回 WebView。这里显式赋值，
    // 保证「输入什么就一定加载什么」。
    // 1 秒内对同一个目标地址只发一次导航：onAccepted / Keys.onReturn /
    // 「前往」按钮可能在同一按键里都触发，重复 set WebView.url 会加载两次
    property string lastNavigateUrl: ""
    property double lastNavigateAt: 0
    // 用户一开始手动改地址就置位：网页后台继续加载/跳转时，任何自动同步
    // 都不能再写 address.text，否则退格/删除会像「删不掉、还剩字母」
    property bool addressEditing: false
    onVisibleChanged: { if (!visible) root.addressEditing = false }
    function loadUrl(raw) {
        const text = (raw || "").replace(/^\s+|\s+$/g, "")
        if (text.length === 0) return
        browser.navigate(text)          // 先规范化（补 https://）并更新 browser.url
        const target = browser.url      // 再读回规范化结果
        address.text = target
        root.addressEditing = false     // 用户已提交，之后的自动同步才允许更新
        const now = Date.now()
        if (target === root.lastNavigateUrl && now - root.lastNavigateAt < 1000)
            return
        root.lastNavigateUrl = target
        root.lastNavigateAt = now
        const view = webLoader.item
        if (view && view.url.toString() !== target) view.url = target
    }

    // 返回键/关闭：先在网页历史里后退，退无可退再交给外层关闭。
    // 注意不能直接信 view.canGoBack：Android 后端 post 到 UI 线程后只等 250ms，
    // UI 线程忙时超时就返回 false，后退键与系统返回键就都变成「直接关浏览器」。
    // 原生助手把等待放宽到 1500ms，结果可靠得多。
    function canGoBackNow() {
        const view = webLoader.item
        if (!view) return false
        return Qt.platform.os === "android" ? browser.webCanGoBack() : view.canGoBack
    }
    function canGoForwardNow() {
        const view = webLoader.item
        if (!view) return false
        return Qt.platform.os === "android" ? browser.webCanGoForward() : view.canGoForward
    }
    function handleBack() {
        if (!canGoBackNow()) return false
        webLoader.item.goBack()
        return true
    }

    // 遮罩吃掉背后页面的点击：只有桌面浮层需要，移动端整页铺满。
    Rectangle {
        anchors.fill: parent
        visible: !root.fullscreenMode
        color: Qt.rgba(0, 0, 0, 0.55)
        MouseArea { anchors.fill: parent }
    }

    Rectangle {
        id: frame
        x: root.fullscreenMode ? 0 : Math.round((parent.width - width) / 2)
        y: root.fullscreenMode ? 0 : Math.round((parent.height - height) / 2)
        width: root.fullscreenMode ? parent.width : Math.min(parent.width - 48, 1100)
        height: root.fullscreenMode ? parent.height : Math.min(parent.height - 48, 780)
        radius: root.fullscreenMode ? 0 : 14
        color: theme.bg
        border.width: root.fullscreenMode ? 0 : 1
        border.color: theme.lineStrong
        clip: true

        ColumnLayout {
            anchors.fill: parent
            // 全屏模式下不留外边距：网页区必须真正铺满整块可用区域；
            // 工具条/地址栏的左右留白由各自的 Layout.leftMargin 提供
            anchors.margins: root.fullscreenMode ? 0 : 12
            spacing: 10

            // 导航条：后退/前进/刷新 居左，关闭居右
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: root.chromePad
                Layout.rightMargin: root.chromePad
                Layout.topMargin: root.fullscreenMode ? 6 : 0
                spacing: 8
                Row {
                    spacing: 8
                    Repeater {
                        model: [
                            { label: "后退", action: "back" },
                            { label: "前进", action: "forward" },
                            { label: "刷新", action: "reload" }
                        ]
                        Rectangle {
                            width: 52
                            height: 34
                            radius: 8
                            // 前进/后退按键在无可回退/前进历史时灰显。
                            // 读一下 url/loading：canGoBack 是即时查询而不是带 NOTIFY
                            // 的属性，不读这两个依赖，绑定会停在 WebView 创建那一刻。
                            readonly property bool isEnabled: {
                                const view = webLoader.item
                                if (!view) return false
                                if (modelData.action === "reload") return true // 刷新始终可用
                                view.url
                                view.loading
                                return modelData.action === "back"
                                        ? root.canGoBackNow()
                                        : root.canGoForwardNow()
                            }
                            color: isEnabled
                                ? Qt.rgba(theme.text.r, theme.text.g, theme.text.b, 0.07)
                                : Qt.rgba(theme.text.r, theme.text.g, theme.text.b, 0.03)
                            Text {
                                anchors.centerIn: parent
                                text: modelData.label
                                color: isEnabled ? theme.text : theme.mutedText
                                font.pixelSize: 12
                            }
                            MouseArea {
                                anchors.fill: parent
                                enabled: isEnabled
                                cursorShape: isEnabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onClicked: {
                                    const view = webLoader.item
                                    if (!view) return
                                    if (modelData.action === "back" && root.canGoBackNow()) view.goBack()
                                    else if (modelData.action === "forward" && root.canGoForwardNow()) view.goForward()
                                    else if (modelData.action === "reload") view.reload()
                                }
                            }
                        }
                    }
                }
                Item { Layout.fillWidth: true }
                Rectangle {
                    width: 52
                    height: 34
                    radius: 8
                    color: Qt.rgba(theme.text.r, theme.text.g, theme.text.b, 0.07)
                    Text {
                        anchors.centerIn: parent
                        text: "关闭"
                        color: theme.text
                        font.pixelSize: 12
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: browser.close()
                    }
                }
            }

    // 地址栏：独占一行，保证窄屏（手机）上也有足够宽度，不会变成 24px 的窄条。
    // 右侧「前往」按钮是回车失效时的兜底（车机输入法不一定发 accepted）。
    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: root.chromePad
        Layout.rightMargin: root.chromePad
        spacing: 8
        TextField {
            id: address
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            placeholderText: "输入网址后按回车"
            color: theme.text
            placeholderTextColor: theme.mutedText
            font.pixelSize: 12
            leftPadding: 12
            // 地址栏是网址输入：关掉预测/联想和自动大写，否则候选词（com/co 之类）
            // 会被塞进输入框，退格时看起来像删不掉、还有残余字母
            inputMethodHints: Qt.ImhUrlCharactersOnly
                              | Qt.ImhNoPredictiveText
                              | Qt.ImhNoAutoUppercase
                              | Qt.ImhSensitiveData
            // 不用 text: browser.url 绑定：用户一输入就会打断绑定，之后再打开
            // 浏览器地址栏还停在上一次的旧内容上。改为在下面显式同步。
            background: Rectangle {
                color: theme.card
                radius: 8
                border.width: 1
                border.color: theme.lineStrong
            }
            // 用户手动编辑（含退格/删除）一旦开始，就禁止后台把地址写回来
            onTextEdited: root.addressEditing = true
            onActiveFocusChanged: { if (activeFocus) root.addressEditing = true }
            onAccepted: {
                root.loadUrl(text)
                // 移动端跳转后收起软键盘：竖屏上键盘会占掉一半高度，
                // 页面加载完用户还得自己点一下空白处才能看全
                if (platform.isMobile) focus = false
            }
        }
        Rectangle {
            Layout.preferredWidth: 56
            Layout.preferredHeight: 34
            radius: 8
            color: theme.accent
            Text {
                anchors.centerIn: parent
                text: "前往"
                color: "#FFFFFF"
                font.pixelSize: 12
            }
            MouseArea {
                anchors.fill: parent
                onClicked: {
                    // 先收起输入法，强制把组合输入（下划线候选词）提交进 text，
                    // 再读取，避免取到未提交的半成品网址
                    if (platform.isMobile) address.focus = false
                    root.loadUrl(address.text)
                }
            }
        }
    }

            Text {
                Layout.fillWidth: true
                Layout.leftMargin: root.chromePad
                Layout.rightMargin: root.chromePad
                visible: root.loginMode
                text: "请在页面里完成登录（含滑块/短信验证）。检测到登录态后会自动导入并同步歌单，无需手动复制 Cookie。"
                color: theme.subText
                font.pixelSize: 12
                lineHeight: 1.35
                wrapMode: Text.Wrap
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.leftMargin: root.chromePad
                Layout.rightMargin: root.chromePad
                Layout.preferredHeight: 1
                color: theme.lineStrong
            }

            // 网页区：原生 WebView 子窗口只占这一块，不能和工具条重叠，
            // 否则 Qt Quick 绘制的控件会被原生窗口盖住。
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Rectangle {
                    anchors.fill: parent
                    color: "#ffffff"
                    // 浅色主题下外框背景同为纯白，网页区边界看不出来：补 1px 描边。
                    // 移动端整页铺满时这条边就是「工具栏/网页」的分界，同样需要看得见。
                    border.width: 1
                    border.color: theme.lineStrong
                }
                Loader {
                    id: webLoader
                    anchors.fill: parent
                    active: root.visible
                    sourceComponent: WebView {
                        url: browser.url
                        // 站点 30x 跳转后 Android 后端不一定再发 urlChanged，地址栏
                        // 会停在用户输入的原始地址。载入成功时再用 WebView 自己的
                        // url 同步一次（不用带回调的 runJavaScript：Android 上
                        // QV4 回调闭包曾触发 use-after-free）。
                        onLoadingChanged: function (request) {
                            if (request.status !== WebView.LoadSucceededStatus) return
                            const u = url.toString()
                            if (u.indexOf("://") > 0 && !address.activeFocus
                                    && !root.addressEditing)
                                address.text = u
                        }
                        // Android 后端有时会把 url 报成相对路径 "/"，这时保留地址栏
                        // 里已有的完整网址，别让它变成 "/"。站点内部跳转（点击链接）
                        // 时 address 会跟着更新，用户再按回车就是当前真实地址。
                        onUrlChanged: {
                            const u = url.toString()
                            if (u.indexOf("://") <= 0) return
                            if (!address.activeFocus && !root.addressEditing) address.text = u
                        }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                Layout.leftMargin: root.chromePad
                Layout.rightMargin: root.chromePad
                Layout.bottomMargin: root.fullscreenMode ? 6 : 0
                text: webLoader.item && webLoader.item.loading
                      ? "正在加载…" : (webLoader.item ? webLoader.item.title : "")
                color: theme.subText
                font.pixelSize: 11
                elide: Text.ElideRight
            }
        }
    }

    // 地址栏初值与外部跳转同步：browser.url 变化时（打开/切平台/代码里 open）
    // 刷新地址栏，正在输入时不打断用户。
    Component.onCompleted: address.text = browser.url
    Connections {
        target: browser
        function onUrlChanged() {
            if (!address.activeFocus && !root.addressEditing) address.text = browser.url
        }
    }

    // 登录模式：轮询平台原生 Cookie 存储。MUSIC_U / sessionid 这类是 HttpOnly，
    // document.cookie 读不到，所以必须走 BrowserController 的 C++/JNI 接口。
    // 判定交给 browser.looksLoggedIn：它按平台读取 WebLoginSpec 里声明的 Cookie 名
    // （网易云 MUSIC_U、汽水 sessionid/sessionid_ss/sid_tt），不再写死 MUSIC_U。
    Timer {
        interval: 1500
        repeat: true
        running: root.visible && root.loginMode
        onTriggered: {
            const view = webLoader.item
            if (!view || !view.url || view.url.length === 0) return
            const cookie = browser.cookiesForUrl(view.url.toString())
            if (cookie.length === 0) return
            if (!browser.looksLoggedIn(root.sourceId, cookie)) return
            running = false
            app.loginByCookie(root.sourceId, cookie)
            browser.close()
        }
    }
}
