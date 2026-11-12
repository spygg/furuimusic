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
    readonly property color cPrimarySoft: theme.accentSoft
    readonly property color cPrimaryPress: Qt.darker(theme.accent, 1.18)
    readonly property color cPrimaryHover: Qt.lighter(theme.accent, 1.1)

    readonly property var qualityKeys: ["standard", "high", "lossless", "hires"]
    // 响应式左右留白
    readonly property real sideMargin: Math.max(16, Math.min(36, Math.round(root.width * 0.035)))

    // themeCombo.currentIndex 已经绑到 theme.mode，外部（标题栏快捷按钮）改主题时会自动跟随，
    // 这里再用 Connections 赋值等于把绑定改成一次性数值，反而让后续变化失效
    Component.onCompleted: {
        downloadDirField.text = app.setting("downloadDir", "")
        proxyField.text = app.setting("proxy", "")
        timeoutField.text = app.setting("timeout", "")
        realIpField.text = app.setting("neteaseRealIp", "")
        accountApiField.text = app.accountApiBase
        localDirField.text = app.customLocalDirectory
        app.refreshCustomSources()
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: formCol.height + formCol.anchors.topMargin + 24
        clip: true

        ColumnLayout {
            id: formCol
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            width: Math.max(240, Math.min(parent.width - 2 * root.sideMargin, 860))
            anchors.topMargin: root.width < 700 ? 16 : 28
            height: implicitHeight
            spacing: 8

            // 大标题（与搜索/音乐库同版式）
            Text {
                Layout.fillWidth: true
                text: "设置"
                color: cText
                font.pixelSize: 30
                font.bold: true
                font.letterSpacing: -0.5
            }
            Text {
                Layout.fillWidth: true
                text: "外观、音质、下载与账号 API"
                color: cSub
                font.pixelSize: 14
            }

            Text { Layout.fillWidth: true; text: "外观主题"; color: cSub; font.pixelSize: 12 }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6
                ComboBox {
                    id: themeCombo
                    Layout.fillWidth: true
                    Layout.preferredHeight: theme.touchMin
                    // 跟随系统 / 浅色 / 深色，选择后立即生效并写入设置
                    model: ["跟随系统", "浅色", "深色"]
                    currentIndex: theme.mode === "light" ? 1 : (theme.mode === "dark" ? 2 : 0)
                    background: Rectangle { color: cCard; radius: 6; border.color: cLine }
                    contentItem: Text {
                        text: themeCombo.displayText
                        color: cText
                        font.pixelSize: 13
                        leftPadding: 8
                        verticalAlignment: Text.AlignVCenter
                    }
                    onActivated: theme.mode = index === 0 ? "system" : (index === 1 ? "light" : "dark")
                }
                Text {
                    visible: !theme.systemThemeSupported
                    Layout.fillWidth: true
                    text: "当前平台不支持跟随系统"
                    color: cSub
                    font.pixelSize: theme.fontSm
                    wrapMode: Text.Wrap
                }
            }

            // 车机/触屏主机跑的常是桌面外壳，但输入是手指：不打开就会丢掉只在 hover 出现的入口
            RowLayout {
                visible: platform.isDesktop
                Layout.fillWidth: true
                spacing: 8
                Text { Layout.fillWidth: true; text: "触控优先（车机/触屏）"; color: cText; font.pixelSize: theme.fontMd }
                Switch {
                    Layout.preferredHeight: theme.touchMin
                    checked: platform.touchPrimary
                    onToggled: platform.setTouchPrimary(checked)
                }
            }

            Text { Layout.fillWidth: true; text: "音质"; color: cSub; font.pixelSize: 12 }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                ComboBox {
                    id: qualityCombo
                    Layout.fillWidth: true
                    Layout.preferredHeight: theme.touchMin
                    model: ["标准", "高品", "无损", "Hi-Res"]
                    // 绑定而不是在 onCompleted 里赋一次值：开放接口/其它页面改 app.quality 时这里才不会留在旧档
                    currentIndex: Math.max(0, root.qualityKeys.indexOf(app.quality))
                    background: Rectangle { color: cCard; radius: 6; border.color: cLine }
                    contentItem: Text {
                        text: qualityCombo.displayText
                        color: cText
                        font.pixelSize: 13
                        leftPadding: 8
                        verticalAlignment: Text.AlignVCenter
                    }
                    onActivated: app.setSetting("quality", root.qualityKeys[index])
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Text { Layout.fillWidth: true; text: "VIP 替代音源"; color: cText; font.pixelSize: 13 }
                Switch {
                    checked: app.vipSubstituteEnabled
                    onToggled: app.setSetting("vipSubstitute", checked)
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Text { Layout.fillWidth: true; text: "自动跨源换源"; color: cText; font.pixelSize: 13 }
                Switch {
                    checked: app.autoToggleSource
                    onToggled: app.setSetting("autoToggleSource", checked)
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Text { Layout.fillWidth: true; text: "开放 API"; color: cText; font.pixelSize: 13 }
                Switch {
                    checked: app.openApiEnabled
                    onToggled: app.setSetting("openApi", checked)
                }
            }

            Text {
                Layout.fillWidth: true
                text: app.openApiEnabled ? ("服务地址 " + app.openApiAddress + "  /status /lyric /play /pause /skip-next") : "开放 API 未启用"
                color: cSub
                font.pixelSize: theme.fontSm
                wrapMode: Text.Wrap
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: cLine }

            Text { Layout.fillWidth: true; text: "自定义音源"; color: cSub; font.pixelSize: 12 }
            Repeater {
                model: app.customSourceIds
                delegate: RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Text {
                        Layout.fillWidth: true
                        text: (index < app.customSourceNames.length ? app.customSourceNames[index] : modelData)
                        color: cText
                        font.pixelSize: 13
                        elide: Text.ElideRight
                    }
                    Text { text: modelData; color: cSub; font.pixelSize: theme.fontXs }
                    Rectangle {
                        Layout.preferredWidth: Math.max(48, theme.touchMin)
                        Layout.preferredHeight: theme.touchMin
                        radius: 5
                        color: cPrimarySoft
                        Text { anchors.centerIn: parent; text: "删除"; color: cPrimary; font.pixelSize: theme.fontSm }
                        MouseArea { anchors.fill: parent; onClicked: app.removeCustomSource(modelData) }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                TextField {
                    id: httpName
                    Layout.fillWidth: true
                    Layout.preferredHeight: theme.touchMin
                    placeholderText: "音源名称"
                    color: cText
                    font.pixelSize: 12
                    leftPadding: 8
                    background: Rectangle { color: cCard; radius: 6; border.color: cLine }
                }
                TextField {
                    id: httpUrl
                    Layout.fillWidth: true
                    Layout.preferredHeight: theme.touchMin
                    placeholderText: "接口地址 BaseUrl"
                    color: cText
                    font.pixelSize: 12
                    leftPadding: 8
                    background: Rectangle { color: cCard; radius: 6; border.color: cLine }
                }
                Button {
                    id: addHttpBtn
                    text: "添加"
                    Layout.preferredWidth: 56
                    Layout.preferredHeight: theme.touchMin
                    background: Rectangle { radius: 6; color: addHttpBtn.down ? cPrimaryPress : cPrimary }
                    contentItem: Text { text: addHttpBtn.text; color: "#FFFFFF"; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    onClicked: {
                        app.addCustomHttpSource(httpName.text, httpUrl.text)
                        app.refreshCustomSources()
                    }
                }
            }

            Text { Layout.fillWidth: true; text: "本地音源目录（直接输入路径）"; color: cSub; font.pixelSize: 12 }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                TextField {
                    id: localDirField
                    Layout.fillWidth: true
                    Layout.preferredHeight: theme.touchMin
                    placeholderText: "本地音乐目录"
                    color: cText
                    font.pixelSize: 12
                    leftPadding: 8
                    background: Rectangle { color: cCard; radius: 6; border.color: cLine }
                }
                Button {
                    id: localDirBtn
                    text: "保存"
                    Layout.preferredWidth: 56
                    Layout.preferredHeight: theme.touchMin
                    background: Rectangle { radius: 6; color: localDirBtn.down ? cPrimaryPress : cPrimary }
                    contentItem: Text { text: localDirBtn.text; color: "#FFFFFF"; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    onClicked: app.setCustomLocalDirectory(localDirField.text)
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: cLine }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    text: "在线音源（LX 脚本）"
                    color: cSub
                    font.pixelSize: 12
                }
                Rectangle {
                    Layout.preferredWidth: 72
                    Layout.preferredHeight: theme.touchMin
                    radius: 6
                    color: cPrimarySoft
                    Text { anchors.centerIn: parent; text: "全部检查"; color: cPrimary; font.pixelSize: theme.fontSm }
                    MouseArea { anchors.fill: parent; onClicked: app.checkAllScriptSources() }
                }
            }
            Text {
                Layout.fillWidth: true
                text: "支持在线 JS 音源（洛雪 LX 协议）。多数公开源只提供播放取链、不提供搜索，能力会如实标注在下方。"
                color: cSub
                font.pixelSize: theme.fontSm
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                TextField {
                    id: scriptUrlField
                    Layout.fillWidth: true
                    Layout.preferredHeight: theme.touchMin
                    placeholderText: "https://.../latest.js"
                    color: cText
                    font.pixelSize: 12
                    leftPadding: 8
                    background: Rectangle { color: cCard; radius: 6; border.color: cLine }
                }
                Button {
                    id: importScriptBtn
                    text: "导入"
                    Layout.preferredWidth: 56
                    Layout.preferredHeight: theme.touchMin
                    background: Rectangle { radius: 6; color: importScriptBtn.down ? cPrimaryPress : cPrimary }
                    contentItem: Text { text: importScriptBtn.text; color: "#FFFFFF"; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    onClicked: {
                        var u = scriptUrlField.text.trim()
                        if (!u) return
                        app.importScriptSource(u)
                        scriptUrlField.text = ""
                    }
                }
            }
            Repeater {
                model: app.scriptSources
                delegate: ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Text {
                            Layout.fillWidth: true
                            text: modelData.name + "  " + modelData.version
                            color: cText
                            font.pixelSize: 13
                            elide: Text.ElideRight
                        }
                        Rectangle {
                            // 状态徽标不可点，宽度跟着文字走：字号随 uiScale 放大才不会挤出边界
                            Layout.preferredWidth: Math.max(44, statusLabel.implicitWidth + 12)
                            Layout.preferredHeight: 20
                            radius: 4
                            color: modelData.status === "ok" ? cPrimarySoft
                                 : (modelData.status === "error" ? "#33FF5A5A" : cLine)
                            Text {
                                id: statusLabel
                                anchors.centerIn: parent
                                text: modelData.statusText
                                color: modelData.status === "ok" ? cPrimary
                                     : (modelData.status === "error" ? theme.danger : cSub)
                                font.pixelSize: theme.fontXs
                            }
                        }
                        Text {
                            text: modelData.enabled ? "禁用" : "启用"
                            color: cSub
                            font.pixelSize: theme.fontSm
                            // 行内文字按钮：热区撑到触控最小值，否则两个字宽手指点不准
                            Layout.preferredWidth: Math.max(theme.touchMin, implicitWidth)
                            Layout.preferredHeight: theme.touchMin
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            MouseArea {
                                anchors.fill: parent
                                onClicked: app.setScriptSourceEnabled(modelData.id, !modelData.enabled)
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Text {
                            Layout.fillWidth: true
                            text: modelData.capability
                            color: cSub
                            font.pixelSize: theme.fontSm
                            elide: Text.ElideRight
                        }
                        Text {
                            text: "更新"
                            color: cPrimary
                            font.pixelSize: theme.fontSm
                            Layout.preferredWidth: Math.max(theme.touchMin, implicitWidth)
                            Layout.preferredHeight: theme.touchMin
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            MouseArea { anchors.fill: parent; onClicked: app.updateScriptSource(modelData.id) }
                        }
                        Text {
                            text: "检查"
                            color: cPrimary
                            font.pixelSize: theme.fontSm
                            Layout.preferredWidth: Math.max(theme.touchMin, implicitWidth)
                            Layout.preferredHeight: theme.touchMin
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            MouseArea { anchors.fill: parent; onClicked: app.checkScriptSource(modelData.id) }
                        }
                        Text {
                            text: "删除"
                            color: theme.danger
                            font.pixelSize: theme.fontSm
                            Layout.preferredWidth: Math.max(theme.touchMin, implicitWidth)
                            Layout.preferredHeight: theme.touchMin
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            MouseArea { anchors.fill: parent; onClicked: app.removeScriptSource(modelData.id) }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: modelData.sourcesText.length > 0
                        text: modelData.sourcesText
                        color: cSub
                        font.pixelSize: theme.fontXs
                        wrapMode: Text.Wrap
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: modelData.status === "error" && modelData.errorMsg.length > 0
                        text: "失败原因（" + modelData.errorType + "）：" + modelData.errorMsg
                        color: theme.danger
                        font.pixelSize: theme.fontXs
                        wrapMode: Text.Wrap
                    }
                    Text {
                        Layout.fillWidth: true
                        text: "检查于 " + modelData.checkedText + " · 更新于 " + modelData.updatedText
                        color: cSub
                        font.pixelSize: theme.fontXs
                    }
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: cLine }

            Text { Layout.fillWidth: true; text: "下载与网络"; color: cSub; font.pixelSize: 12 }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Text { text: "下载目录"; color: cText; font.pixelSize: 12; Layout.preferredWidth: 60 }
                TextField {
                    id: downloadDirField
                    Layout.fillWidth: true
                    Layout.preferredHeight: theme.touchMin
                    color: cText
                    font.pixelSize: 12
                    leftPadding: 8
                    background: Rectangle { color: cCard; radius: 6; border.color: cLine }
                }
                Button {
                    id: downloadDirBtn
                    text: "保存"
                    Layout.preferredWidth: 52
                    Layout.preferredHeight: theme.touchMin
                    background: Rectangle { radius: 6; color: cPrimarySoft; border.color: cLine }
                    contentItem: Text { text: downloadDirBtn.text; color: cPrimary; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    onClicked: app.setSetting("downloadDir", downloadDirField.text)
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Text { text: "代理"; color: cText; font.pixelSize: 12; Layout.preferredWidth: 60 }
                TextField {
                    id: proxyField
                    Layout.fillWidth: true
                    Layout.preferredHeight: theme.touchMin
                    color: cText
                    font.pixelSize: 12
                    leftPadding: 8
                    background: Rectangle { color: cCard; radius: 6; border.color: cLine }
                }
                Button {
                    id: proxyBtn
                    text: "保存"
                    Layout.preferredWidth: 52
                    Layout.preferredHeight: theme.touchMin
                    background: Rectangle { radius: 6; color: cPrimarySoft; border.color: cLine }
                    contentItem: Text { text: proxyBtn.text; color: cPrimary; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    onClicked: app.setSetting("proxy", proxyField.text)
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Text { text: "超时"; color: cText; font.pixelSize: 12; Layout.preferredWidth: 60 }
                TextField {
                    id: timeoutField
                    Layout.fillWidth: true
                    Layout.preferredHeight: theme.touchMin
                    color: cText
                    font.pixelSize: 12
                    leftPadding: 8
                    background: Rectangle { color: cCard; radius: 6; border.color: cLine }
                }
                Button {
                    id: timeoutBtn
                    text: "保存"
                    Layout.preferredWidth: 52
                    Layout.preferredHeight: theme.touchMin
                    background: Rectangle { radius: 6; color: cPrimarySoft; border.color: cLine }
                    contentItem: Text { text: timeoutBtn.text; color: cPrimary; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    onClicked: {
                        var t = parseInt(timeoutField.text)
                        app.setSetting("timeout", isNaN(t) ? 30 : t)
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Text { text: "账号 API"; color: cText; font.pixelSize: 12; Layout.preferredWidth: 60 }
                TextField {
                    id: accountApiField
                    Layout.fillWidth: true
                    Layout.preferredHeight: theme.touchMin
                    placeholderText: "http://127.0.0.1:9764"
                    color: cText
                    font.pixelSize: 12
                    leftPadding: 8
                    background: Rectangle { color: cCard; radius: 6; border.color: cLine }
                }
                Button {
                    id: accountApiBtn
                    text: "保存"
                    Layout.preferredWidth: 52
                    Layout.preferredHeight: theme.touchMin
                    background: Rectangle { radius: 6; color: cPrimarySoft; border.color: cLine }
                    contentItem: Text { text: accountApiBtn.text; color: cPrimary; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    onClicked: app.setSetting("accountApiBase", accountApiField.text.trim())
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Text { text: "网易云IP"; color: cText; font.pixelSize: 12; Layout.preferredWidth: 60 }
                TextField {
                    id: realIpField
                    Layout.fillWidth: true
                    Layout.preferredHeight: theme.touchMin
                    placeholderText: "可选：出口 IP 被判异常时填国内住宅 IP"
                    color: cText
                    placeholderTextColor: cSub
                    font.pixelSize: 12
                    leftPadding: 8
                    background: Rectangle { color: cCard; radius: 6; border.color: cLine }
                }
                Button {
                    id: realIpBtn
                    text: "保存"
                    Layout.preferredWidth: 52
                    Layout.preferredHeight: theme.touchMin
                    background: Rectangle { radius: 6; color: cPrimarySoft; border.color: cLine }
                    contentItem: Text { text: realIpBtn.text; color: cPrimary; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignHCenter }
                    onClicked: app.setSetting("neteaseRealIp", realIpField.text.trim())
                }
            }
            Text {
                Layout.fillWidth: true
                text: "登录全部在应用内完成：网易云 / QQ / 酷狗 / 咪咕支持内置二维码登录，网易云与咪咕另支持账号密码登录；"
                      + "无内置二维码的平台需在「设置」里配置自建账号 API 服务（示例 src/tools/account_api_server.py）后同步歌单。"
                      + "「网易云IP」仅在返回 460（出口 IP 异常）时按需填写，留空即可。"
                color: cSub
                font.pixelSize: theme.fontSm
                wrapMode: Text.Wrap
            }

            // Android / 车机：没有物理返回键兜底，给一个显式的「退出应用」入口。
            // 桌面（含触控优先的车机桌面壳）不展示，避免多余入口。
            RowLayout {
                Layout.fillWidth: true
                visible: !platform.isDesktop
                spacing: 8
                Text { Layout.fillWidth: true; text: "退出应用"; color: cText; font.pixelSize: 12 }
                Button {
                    id: quitAppBtn
                    text: "退出"
                    Layout.preferredWidth: 72
                    Layout.preferredHeight: theme.touchMin
                    background: Rectangle { radius: 6; color: cPrimarySoft; border.color: cLine }
                    contentItem: Text { text: quitAppBtn.text; color: cPrimary; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    onClicked: app.quitApp()
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: cLine }

            // 在线检测更新：检查 -> 弹层展示更新说明 -> 同意后应用内直接下载 -> 安装
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text { text: app.appName + " v" + app.version; color: cText; font.pixelSize: theme.fontMd }
                    Text {
                        Layout.fillWidth: true
                        text: {
                            if (updater.checking) return "正在检查更新…"
                            if (updater.downloading)
                                return "正在下载更新 " + (updater.progress >= 0 ? updater.progress + "%" : "…")
                            if (updater.downloadedPath.length > 0) return "更新包已就绪：" + updater.downloadedPath
                            if (updater.remoteVersion.length > 0
                                && updater.remoteVersion !== app.version)
                                return "发现新版本 v" + updater.remoteVersion
                            return "已是最新版本"
                        }
                        color: (updater.remoteVersion.length > 0
                                && updater.remoteVersion !== app.version) ? cPrimary : cSub
                        font.pixelSize: theme.fontSm
                        elide: Text.ElideMiddle
                    }
                }
                Button {
                    id: checkBtn
                    text: updater.checking ? "检查中…" : "检查更新"
                    Layout.preferredHeight: theme.touchMin
                    enabled: !updater.checking && !updater.downloading
                    background: Rectangle { radius: 6; color: cPrimarySoft; border.color: cLine }
                    contentItem: Text { text: checkBtn.text; color: cPrimary; font.pixelSize: theme.fontSm; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    onClicked: updater.checkForUpdates()
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: cLine }

            Text {
                Layout.fillWidth: true
                text: app.appName + " " + app.version + "\n仅用于学习研究；请使用你拥有合法授权的自定义音源"
                color: cSub
                font.pixelSize: 12
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }

    // 更新弹层：设置页内聚展示，有新版本时由 checkFinished 打开
    Rectangle {
        id: updateOverlay
        anchors.fill: parent
        z: 20
        color: "#88000000"
        visible: opacity > 0
        opacity: show ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
        property bool show: false
        property string errorText: ""
        // 最新版本说明 + 历史版本合并成一份滚动文本
        readonly property string changelogAll: {
            var t = updater.changelog
            for (var i = 0; i < updater.history.length; ++i) {
                var h = updater.history[i]
                t += "\n\nv" + h.version + "\n" + h.desc
            }
            return t
        }
        // 阻断穿透点击：空白处点击不关闭，避免误触丢掉更新提示
        MouseArea { anchors.fill: parent }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 32, 520)
            height: Math.min(parent.height - 40, 620)
            radius: theme.radiusLg
            color: cCard

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 20
                spacing: 10

                Text {
                    Layout.fillWidth: true
                    text: "发现新版本 v" + updater.remoteVersion
                    color: cText
                    font.pixelSize: 18
                    font.bold: true
                }
                Text {
                    Layout.fillWidth: true
                    text: "当前版本 v" + app.version
                    color: cSub
                    font.pixelSize: theme.fontSm
                }
                Text {
                    Layout.fillWidth: true
                    text: "更新内容"
                    color: cSub
                    font.pixelSize: theme.fontSm
                }
                Flickable {
                    id: logFlick
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    contentWidth: width
                    contentHeight: logText.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds
                    Text {
                        id: logText
                        width: logFlick.width
                        text: updateOverlay.changelogAll
                        color: cText
                        font.pixelSize: theme.fontSm
                        wrapMode: Text.Wrap
                    }
                    ScrollBar.vertical: ScrollBar { }
                }

                Text {
                    Layout.fillWidth: true
                    visible: updateOverlay.errorText.length > 0
                    text: updateOverlay.errorText
                    color: theme.danger
                    font.pixelSize: theme.fontSm
                    wrapMode: Text.Wrap
                }
                ProgressBar {
                    Layout.fillWidth: true
                    visible: updater.downloading || updater.downloadedPath.length > 0
                    from: 0
                    to: 100
                    value: updater.progress >= 0 ? updater.progress : 0
                    indeterminate: updater.downloading && updater.progress < 0
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Button {
                        id: updateBtn
                        Layout.fillWidth: true
                        Layout.preferredHeight: theme.touchMin
                        text: updater.downloading ? "下载中…"
                              : updater.downloadedPath.length > 0 ? "安装更新"
                              : "下载更新"
                        enabled: !updater.downloading
                        background: Rectangle { radius: 6; color: updateBtn.down ? cPrimaryPress : cPrimary }
                        contentItem: Text { text: updateBtn.text; color: "#FFFFFF"; font.pixelSize: theme.fontSm; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        onClicked: {
                            updateOverlay.errorText = ""
                            if (updater.downloadedPath.length > 0)
                                updater.installUpdate()
                            else
                                updater.downloadUpdate()
                        }
                    }
                    Button {
                        id: laterBtn
                        Layout.preferredWidth: Math.max(72, theme.touchMin)
                        Layout.preferredHeight: theme.touchMin
                        text: "稍后"
                        enabled: !updater.downloading
                        background: Rectangle { radius: 6; color: cPrimarySoft; border.color: cLine }
                        contentItem: Text { text: laterBtn.text; color: cPrimary; font.pixelSize: theme.fontSm; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        onClicked: updateOverlay.show = false
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: "更新包将直接在应用内下载，完成后自动拉起安装（桌面端打开所在文件夹）"
                    color: cSub
                    font.pixelSize: theme.fontXs
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    Connections {
        target: updater
        function onCheckFinished(hasUpdate, message) {
            if (hasUpdate) {
                updateOverlay.errorText = ""
                updateOverlay.show = true
            }
        }
        function onDownloadFinished(path) {
            // 下载完成即自动拉起安装器（Android）或打开所在文件夹（桌面）
            updater.installUpdate()
        }
        function onDownloadFailed(error) {
            updateOverlay.errorText = error
        }
    }
}
