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
    // 取链顺序：与后端 Settings 的 sourceFirst 取值一一对应
    readonly property var sourceFirstKeys: ["platform", "vip", "custom"]
    // 响应式左右留白
    readonly property real sideMargin: Math.max(16, Math.min(36, Math.round(root.width * 0.035)))
    // 表单最大宽度：横屏手机只有 300dp 高，再让一行下拉框铺满 700dp 宽会非常难读；
    // 大屏（平板横屏/车机）反而要宽一点，免得 900dp 宽度只用到中间一条。
    readonly property real formMaxWidth: root.width >= 1000 ? 900
                                        : (root.width > root.height * 1.12 ? 680 : 820)
    readonly property real pageTopMargin: root.height < 480 ? 12 : (root.width < 700 ? 16 : 28)
    // 在线音源（LX 脚本）列表默认收起：脚本可能有十几个，每项又有能力说明、状态和时间，
    // 全展开会把设置页撑得很长；点标题即可展开/收起。
    property bool sourcesExpanded: false

    // themeCombo.currentIndex 已经绑到 theme.mode，外部（标题栏快捷按钮）改主题时会自动跟随，
    // 这里再用 Connections 赋值等于把绑定改成一次性数值，反而让后续变化失效

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: formCol.height + formCol.anchors.topMargin + 28
        clip: true

        ColumnLayout {
            id: formCol
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            width: Math.max(240, Math.min(parent.width - 2 * root.sideMargin, root.formMaxWidth))
            anchors.topMargin: root.pageTopMargin
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
                text: "外观、音质与在线音源"
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
                Text { Layout.fillWidth: true; text: "取链顺序"; color: cText; font.pixelSize: 13 }
                ComboBox {
                    id: orderCombo
                    Layout.preferredWidth: 168
                    Layout.preferredHeight: theme.touchMin
                    model: ["平台优先", "VIP 优先", "全部自定义"]
                    currentIndex: Math.max(0, root.sourceFirstKeys.indexOf(app.sourceFirstOrder))
                    background: Rectangle { color: cCard; radius: 6; border.color: cLine }
                    contentItem: Text {
                        text: orderCombo.displayText
                        color: cText
                        font.pixelSize: 13
                        leftPadding: 8
                        verticalAlignment: Text.AlignVCenter
                    }
                    onActivated: app.setSetting("sourceFirst", root.sourceFirstKeys[index])
                }
            }

            Text {
                Layout.fillWidth: true
                visible: app.sourceFirstOrder !== "platform"
                text: app.sourceFirstOrder === "custom"
                      ? "所有歌曲都先向自定义/在线音源取链，取不到再回落原平台直链。自定义音源失败少、起播快，但音质取决于音源本身。"
                      : "VIP / 平台判定不可播的歌曲跳过原平台直链，直接从自定义/在线音源取链，省掉一次注定失败的请求；普通歌曲仍走平台正版直链。"
                color: cSub
                font.pixelSize: 11
                wrapMode: Text.WordWrap
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
                visible: platform.isMobile
                Text { Layout.fillWidth: true; text: "播放时屏幕常亮"; color: cText; font.pixelSize: 13 }
                Switch {
                    checked: app.keepScreenOn
                    onToggled: app.setSetting("keepScreenOn", checked)
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
                text: app.openApiEnabled
                      ? ("已开启 · 浏览器打开 " + app.openApiAddress
                         + " 即是本机网页控制台：播放状态、上一/下一首、进度与音量、播放队列，"
                         + "以及查看并一键复制运行日志；同一局域网里的手机/平板/电脑打开这个地址也能用")
                      : "关闭状态。开启后本机开放一个 HTTP 接口，供第三方脚本/工具（如桌面歌词、遥控脚本）读取播放状态与控制播放，与洛雪音乐开放 API 同类；同一局域网的手机/平板/电脑也能直接打开"
                color: cSub
                font.pixelSize: theme.fontSm
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Text { Layout.fillWidth: true; text: "写入日志文件"; color: cText; font.pixelSize: 13 }
                Switch {
                    checked: app.logToFile
                    onToggled: app.setSetting("logToFile", checked)
                }
            }

            Text {
                Layout.fillWidth: true
                text: app.logToFile
                      ? ("日志写入 " + app.logFilePath + "（单文件超过 8 MB 自动轮转一份）")
                      : "关闭状态。开启后把启动、播放失败、音源异常等运行日志写入文件，排查问题时才有据可查；随时可在开放 API 网页里查看并复制最近 400 行"
                color: cSub
                font.pixelSize: theme.fontSm
                wrapMode: Text.Wrap
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: cLine }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: theme.touchMin
                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6
                        Text {
                            text: root.sourcesExpanded ? "▾" : "▸"
                            color: cPrimary
                            font.pixelSize: 14
                            font.bold: true
                        }
                        Text {
                            text: "在线音源（LX 脚本）"
                                  + (app.scriptSources.length > 0 ? "（" + app.scriptSources.length + "）" : "")
                                  + (root.sourcesExpanded ? "" : " · 点击展开")
                            color: cText
                            font.pixelSize: 13
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.sourcesExpanded = !root.sourcesExpanded
                    }
                }
                Rectangle {
                    Layout.preferredWidth: 72
                    Layout.preferredHeight: theme.touchMin
                    radius: 6
                    color: cPrimarySoft
                    Text { anchors.centerIn: parent; text: "发现音源"; color: cPrimary; font.pixelSize: theme.fontSm }
                    MouseArea { anchors.fill: parent; onClicked: app.rescanScriptSources() }
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
            // 列表详情（说明 + 导入框 + 每个源的状态卡片）整体可折叠
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.sourcesExpanded
                spacing: 8

                Text {
                    Layout.fillWidth: true
                    text: "在线音源自动从 pdone/lx-music-source 仓库页面解析全部脚本（LX 脚本协议），首次启动全部导入、当日可用的排在前面；也可自行导入其它脚本地址。多数公开源只提供播放取链、不提供搜索，能力会如实标注在下方。列表顺序即实际取链优先级（成功升权、失败降权）。"
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
            } // 在线音源详情（默认收起）

            Rectangle { Layout.fillWidth: true; height: 1; color: cLine }

            Text {
                Layout.fillWidth: true
                text: "登录全部在应用内完成：网易云 / QQ / 酷狗 / 咪咕支持内置二维码登录，网易云与咪咕另支持账号密码登录。"
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

            // 内置浏览器：地址栏可打开任意网页；登录页的官网登录也复用这个外壳。
            // 没有 QtWebView 后端的 Qt 5 兼容构建自动隐藏。
            RowLayout {
                Layout.fillWidth: true
                visible: browser.available
                spacing: 8
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text { text: "内置浏览器"; color: cText; font.pixelSize: theme.fontMd }
                    Text {
                        Layout.fillWidth: true
                        text: "在应用内打开任意网页，地址栏可手动输入网址"
                        color: cSub
                        font.pixelSize: theme.fontSm
                        wrapMode: Text.Wrap
                    }
                }
                Button {
                    id: openBrowserBtn
                    text: "打开"
                    Layout.preferredWidth: 72
                    Layout.preferredHeight: theme.touchMin
                    background: Rectangle { radius: 6; color: cPrimarySoft; border.color: cLine }
                    contentItem: Text {
                        text: openBrowserBtn.text
                        color: cPrimary
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: browser.open("https://music.163.com/", "browse")
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: cLine }

            // 汽水音乐账号同步：资料/歌单/收藏已改走免签名的 web 接口族
            // （/luna/me、/luna/me/playlist、/luna/playlist/detail），不再需要手工填
            // X-Helios / X-Medusa。旧版本这里有两个签名输入框，现已移除；若本地
            // settings.ini 里还留着 qishuiXHelios/qishuiXMedusa，也不会再被账号接口使用。
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6
                Text {
                    text: "汽水音乐 · 账号同步"
                    color: cText
                    font.pixelSize: theme.fontMd
                }
                Text {
                    Layout.fillWidth: true
                    text: "汽水的资料、歌单与收藏已改走免签名的 web 账号接口，登录后点「同步全部账号」即可，"
                          + "无需再填 X-Helios / X-Medusa。若同步仍失败，请重新登录汽水后再试。"
                    color: cSub
                    font.pixelSize: theme.fontSm
                    wrapMode: Text.Wrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text { text: "汽水漫游"; color: cText; font.pixelSize: 13 }
                        Text {
                            Layout.fillWidth: true
                            text: "播放汽水歌曲时，下一首改由汽水的相似歌曲推荐决定（队列播到末尾也会自动续上）；"
                                  + "关闭后仍按原播放队列切歌"
                            color: cSub
                            font.pixelSize: theme.fontSm
                            wrapMode: Text.Wrap
                        }
                    }
                    Switch {
                        checked: app.qishuiRoam
                        onToggled: app.setSetting("qishuiRoam", checked)
                    }
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
                            // 只认 C++ 侧按点分版本号比较得出的结论：不能拿远端版本和本地版本做
                            // 字符串不等判断，否则远端清单比本机旧（如本机 1.4.9、镜像缓存里的
                            // 清单还是 1.4.8）时会误报「发现新版本 v1.4.8」
                            if (updater.hasUpdate) return "发现新版本 v" + updater.remoteVersion
                            // 「拿到清单且确实不新」与「一条清单都没拉到」要分开显示，
                            // 否则断网时也会写「已是最新版本」
                            return updater.lastResult.length > 0 ? updater.lastResult : "已是最新版本"
                        }
                        color: updater.hasUpdate ? cPrimary : cSub
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
                                 && (updater.hasUpdate || updater.downloadedPath.length > 0)
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
