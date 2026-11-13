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

    readonly property string sourceId: browser.mode.indexOf("login:") === 0
                                       ? browser.mode.substring(6) : ""
    readonly property bool loginMode: sourceId.length > 0

    // 遮罩吃掉背后页面的点击
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.55)
        MouseArea { anchors.fill: parent }
    }

    Rectangle {
        id: frame
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 1100)
        height: Math.min(parent.height - 48, 780)
        radius: 14
        color: theme.bg
        border.width: 1
        border.color: theme.line
        clip: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10

            // 导航条：后退/前进/刷新 居左，关闭居右
            RowLayout {
                Layout.fillWidth: true
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
                            color: Qt.rgba(theme.text.r, theme.text.g, theme.text.b, 0.07)
                            Text {
                                anchors.centerIn: parent
                                text: modelData.label
                                color: theme.text
                                font.pixelSize: 12
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    const view = webLoader.item
                                    if (!view) return
                                    if (modelData.action === "back" && view.canGoBack) view.goBack()
                                    else if (modelData.action === "forward" && view.canGoForward) view.goForward()
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

            // 地址栏：独占一行，保证窄屏（手机）上也有足够宽度，不会变成 24px 的窄条
            TextField {
                id: address
                Layout.fillWidth: true
                Layout.preferredHeight: 34
                text: browser.url
                placeholderText: "输入网址后按回车"
                color: theme.text
                placeholderTextColor: theme.mutedText
                font.pixelSize: 12
                leftPadding: 12
                background: Rectangle {
                    color: theme.card
                    radius: 8
                    border.width: 1
                    border.color: theme.line
                }
                onAccepted: {
                    if (webLoader.item) webLoader.item.url = browser.url
                    browser.navigate(text.trim())
                }
            }

            Text {
                Layout.fillWidth: true
                visible: root.loginMode
                text: "请在页面里完成登录（含滑块/短信验证）。检测到登录态后会自动导入并同步歌单，无需手动复制 Cookie。"
                color: theme.subText
                font.pixelSize: 12
                lineHeight: 1.35
                wrapMode: Text.Wrap
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 2
                color: theme.line
            }

            // 网页区：原生 WebView 子窗口只占这一块，不能和工具条重叠，
            // 否则 Qt Quick 绘制的控件会被原生窗口盖住。
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Rectangle {
                    anchors.fill: parent
                    color: "#ffffff"
                }
                Loader {
                    id: webLoader
                    anchors.fill: parent
                    active: root.visible
                    sourceComponent: WebView {
                        url: browser.url
                        // Android 后端有时会把 url 报成相对路径 "/"，
                        // 这时保留启动时传入的完整网址，别让地址栏变成 "/"。
                        onUrlChanged: {
                            if (address.activeFocus) return
                            const u = url.toString()
                            if (u.indexOf("://") > 0)
                                address.text = u
                            else if (address.text.indexOf("://") <= 0)
                                address.text = browser.url
                        }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                text: webLoader.item && webLoader.item.loading
                      ? "正在加载…" : (webLoader.item ? webLoader.item.title : "")
                color: theme.subText
                font.pixelSize: 11
                elide: Text.ElideRight
            }
        }
    }

    // 登录模式：轮询平台原生 Cookie 存储。MUSIC_U 是 HttpOnly，
    // document.cookie 读不到，所以必须走 BrowserController 的 C++/JNI 接口。
    Timer {
        interval: 1500
        repeat: true
        running: root.visible && root.loginMode
        onTriggered: {
            const view = webLoader.item
            if (!view || !view.url || view.url.length === 0) return
            const cookie = browser.cookiesForUrl(view.url.toString())
            if (cookie.indexOf("MUSIC_U=") < 0) return
            running = false
            app.loginByCookie(root.sourceId, cookie)
            browser.close()
        }
    }
}
