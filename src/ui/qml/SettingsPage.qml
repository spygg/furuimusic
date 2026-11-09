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

    Component.onCompleted: {
        qualityCombo.currentIndex = Math.max(0, root.qualityKeys.indexOf(app.quality))
        downloadDirField.text = app.setting("downloadDir", "")
        proxyField.text = app.setting("proxy", "")
        timeoutField.text = app.setting("timeout", "")
        accountApiField.text = app.accountApiBase
        localDirField.text = app.customLocalDirectory
        app.refreshCustomSources()
    }

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: formCol.height + 24
        clip: true

        ColumnLayout {
            id: formCol
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            height: implicitHeight
            spacing: 8

            Text {
                Layout.fillWidth: true
                text: "设置"
                color: cText
                font.pixelSize: 17
                font.bold: true
            }

            Text { Layout.fillWidth: true; text: "外观主题"; color: cSub; font.pixelSize: 12 }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                ComboBox {
                    id: themeCombo
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
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
                    text: "当前平台不支持跟随系统"
                    color: cSub
                    font.pixelSize: 11
                }
            }

            Text { Layout.fillWidth: true; text: "音质"; color: cSub; font.pixelSize: 12 }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                ComboBox {
                    id: qualityCombo
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    model: ["标准", "高品", "无损", "Hi-Res"]
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
                font.pixelSize: 11
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
                    Text { text: modelData; color: cSub; font.pixelSize: 10 }
                    Rectangle {
                        Layout.preferredWidth: 48
                        Layout.preferredHeight: 28
                        radius: 5
                        color: cPrimarySoft
                        Text { anchors.centerIn: parent; text: "删除"; color: cPrimary; font.pixelSize: 11 }
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
                    Layout.preferredHeight: 34
                    placeholderText: "音源名称"
                    color: cText
                    font.pixelSize: 12
                    leftPadding: 8
                    background: Rectangle { color: cCard; radius: 6; border.color: cLine }
                }
                TextField {
                    id: httpUrl
                    Layout.fillWidth: true
                    Layout.preferredHeight: 34
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
                    Layout.preferredHeight: 34
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
                    Layout.preferredHeight: 34
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
                    Layout.preferredHeight: 34
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
                    Layout.preferredHeight: 28
                    radius: 6
                    color: cPrimarySoft
                    Text { anchors.centerIn: parent; text: "全部检查"; color: cPrimary; font.pixelSize: 11 }
                    MouseArea { anchors.fill: parent; onClicked: app.checkAllScriptSources() }
                }
            }
            Text {
                Layout.fillWidth: true
                text: "支持在线 JS 音源（洛雪 LX 协议）。多数公开源只提供播放取链、不提供搜索，能力会如实标注在下方。"
                color: cSub
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                TextField {
                    id: scriptUrlField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 34
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
                    Layout.preferredHeight: 34
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
                            Layout.preferredWidth: 44
                            Layout.preferredHeight: 20
                            radius: 4
                            color: modelData.status === "ok" ? cPrimarySoft
                                 : (modelData.status === "error" ? "#33FF5A5A" : cLine)
                            Text {
                                anchors.centerIn: parent
                                text: modelData.statusText
                                color: modelData.status === "ok" ? cPrimary
                                     : (modelData.status === "error" ? "#FF6B6B" : cSub)
                                font.pixelSize: 10
                            }
                        }
                        Text {
                            text: modelData.enabled ? "禁用" : "启用"
                            color: cSub
                            font.pixelSize: 11
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
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                        Text {
                            text: "更新"
                            color: cPrimary
                            font.pixelSize: 11
                            MouseArea { anchors.fill: parent; onClicked: app.updateScriptSource(modelData.id) }
                        }
                        Text {
                            text: "检查"
                            color: cPrimary
                            font.pixelSize: 11
                            MouseArea { anchors.fill: parent; onClicked: app.checkScriptSource(modelData.id) }
                        }
                        Text {
                            text: "删除"
                            color: "#FF6B6B"
                            font.pixelSize: 11
                            MouseArea { anchors.fill: parent; onClicked: app.removeScriptSource(modelData.id) }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: modelData.sourcesText.length > 0
                        text: modelData.sourcesText
                        color: cSub
                        font.pixelSize: 10
                        wrapMode: Text.Wrap
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: modelData.status === "error" && modelData.errorMsg.length > 0
                        text: "失败原因（" + modelData.errorType + "）：" + modelData.errorMsg
                        color: "#FF6B6B"
                        font.pixelSize: 10
                        wrapMode: Text.Wrap
                    }
                    Text {
                        Layout.fillWidth: true
                        text: "检查于 " + modelData.checkedText + " · 更新于 " + modelData.updatedText
                        color: cSub
                        font.pixelSize: 10
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
                    Layout.preferredHeight: 34
                    color: cText
                    font.pixelSize: 12
                    leftPadding: 8
                    background: Rectangle { color: cCard; radius: 6; border.color: cLine }
                }
                Button {
                    id: downloadDirBtn
                    text: "保存"
                    Layout.preferredWidth: 52
                    Layout.preferredHeight: 34
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
                    Layout.preferredHeight: 34
                    color: cText
                    font.pixelSize: 12
                    leftPadding: 8
                    background: Rectangle { color: cCard; radius: 6; border.color: cLine }
                }
                Button {
                    id: proxyBtn
                    text: "保存"
                    Layout.preferredWidth: 52
                    Layout.preferredHeight: 34
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
                    Layout.preferredHeight: 34
                    color: cText
                    font.pixelSize: 12
                    leftPadding: 8
                    background: Rectangle { color: cCard; radius: 6; border.color: cLine }
                }
                Button {
                    id: timeoutBtn
                    text: "保存"
                    Layout.preferredWidth: 52
                    Layout.preferredHeight: 34
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
                    Layout.preferredHeight: 34
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
                    Layout.preferredHeight: 34
                    background: Rectangle { radius: 6; color: cPrimarySoft; border.color: cLine }
                    contentItem: Text { text: accountApiBtn.text; color: cPrimary; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    onClicked: app.setSetting("accountApiBase", accountApiField.text.trim())
                }
            }
            Text {
                Layout.fillWidth: true
                text: "QQ音乐 / 酷狗 / 酷我 / 咪咕 的用户歌单与收藏需自建合规账号 API 服务（示例 tools/account_api_server.py）；网易云为内置真实实现，无需此项。"
                color: cSub
                font.pixelSize: 11
                wrapMode: Text.Wrap
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
}
