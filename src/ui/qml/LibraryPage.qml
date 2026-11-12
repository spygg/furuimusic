import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Item {
    id: root
    clip: true

    // 平板横屏 / 车机矮屏：窗口逻辑高度常常只有 450~500dp，
    // 标题栏 + 播放栏 + 页头会把结果区压到几十 dp，看起来像「不能滚动」。
    // 这里压缩页头间距、给结果区保底高度，并给每个结果视图挂可见滚动条；
    // 不套整页 Flickable，避免与 ListView/GridView 形成同向嵌套滚动互抢手势。
    readonly property bool shortViewport: root.height < 620
    readonly property real pageTop: shortViewport ? 12 : (root.width < 700 ? 16 : 28)
    readonly property real pageGap: shortViewport ? 8 : 14
    readonly property real pageBottom: shortViewport ? 8 : 12

    readonly property bool isDark: theme.dark
    readonly property color cBg: theme.bg
    readonly property color cCard: theme.card
    readonly property color cText: theme.text
    readonly property color cSub: theme.subText
    readonly property color cPrimary: theme.accent
    readonly property color cLine: theme.line
    readonly property color cSoft: theme.accentSoft
    readonly property color cChip: theme.dark ? Qt.rgba(1, 1, 1, 0.06)
                                              : Qt.rgba(0, 0, 0, 0.05)
    readonly property color cChipHover: theme.dark ? Qt.rgba(1, 1, 1, 0.12)
                                                   : Qt.rgba(0, 0, 0, 0.09)

    property string hint: ""
    property int tabIndex: 0
    // 0 = 全部平台
    property int selectedPlatform: 0
    // 点开歌单后显示曲目详情，空串表示停留在歌单列表
    property string openPlaylistTitle: ""
    // 点开「发现榜单」里的公开歌单后显示曲目详情
    property string discoveryTitle: ""
    // 歌单封面卡片墙 / 列表
    property bool gridView: true

    // ---- 搜索（原独立搜索页已并入音乐库）----
    // 搜索框有内容即进入搜索结果视图，清空则回到当前 Tab；两者共用 app.results
    property string searchKeyword: ""
    property bool searching: false
    readonly property bool searchMode: searchKeyword.length > 0
    readonly property int searchViewIndex: root.tabTitles.length

    function runSearch()
    {
        // 以输入框内容为准：root.searchKeyword 只在真正发起搜索时才写入，
        // 早先读 root.searchKeyword 会让「敲完字直接点放大镜」变成空搜索
        const kw = keyword.text.trim()
        if (kw.length === 0)
            return
        root.searchKeyword = kw
        app.search(kw, root.selectedSourceId())
    }

    // 退出搜索视图：输入框是命令式赋值的（用户一敲字绑定就断），
    // 所以这里必须同时清输入框，否则清空后残留旧关键词
    function exitSearch()
    {
        if (keyword.text.length > 0)
            keyword.text = ""
        root.searching = false
        root.searchKeyword = ""
    }

    function clearSearch()
    {
        root.exitSearch()
        root.reloadSongs()
    }

    readonly property int gridColumns: Math.max(2, Math.min(6,
        Math.floor(Math.max(340, libraryArea.width) / 170)))
    readonly property real gridCellWidth: Math.max(120, libraryArea.width / gridColumns)
    // 封面正方形边长 = 格宽 - 卡片内边距 32，文字区固定 76；按整格宽算会每格多 32px 空白
    readonly property real gridCellHeight: gridCellWidth + 44
    // 响应式左右留白
    readonly property real sideMargin: Math.max(16, Math.min(36, Math.round(root.width * 0.035)))

    // MV 做成曲库的第 6 个 Tab：外壳导航仍保持「曲库/播放/账号/设置」4 项不变
    readonly property var tabTitles: ["我的歌单", "收藏", "播放历史", "播放队列", "发现榜单", "MV"]
    readonly property var mvAreas: ["全部", "内地", "港台", "欧美", "日本", "韩国"]
    property string mvArea: "全部"

    // 播放量压缩成「万 / 亿」，避免六位数把封面角标撑破
    function formatCount(n) {
        if (!n || n < 0) return "0"
        if (n >= 100000000) return (n / 100000000).toFixed(1) + "亿"
        if (n >= 10000) return (n / 10000).toFixed(1) + "万"
        return String(Math.floor(n))
    }

    function selectedSourceId() {
        if (selectedPlatform <= 0)
            return ""
        if (selectedPlatform - 1 < app.sourceIds.length)
            return app.sourceIds[selectedPlatform - 1]
        return ""
    }

    function reloadSongs() {
        root.openPlaylistTitle = ""
        root.discoveryTitle = ""
        var sid = root.selectedSourceId()
        if (root.tabIndex === 1)
            app.loadFavoritesBySource(sid)
        else if (root.tabIndex === 2)
            app.loadHistoryBySource(sid)
        else if (root.tabIndex === 4)
            app.loadDiscovery(root.selectedSourceId())
        else if (root.tabIndex === 5)
            app.loadMvList(root.mvArea, false)
    }

    function openPlaylist(title, uid) {
        app.loadPlaylist(uid)
        root.openPlaylistTitle = title
    }

    function openDiscovery(title, uid) {
        app.openDiscoveryPlaylist(uid)
        root.discoveryTitle = title
    }

    Connections {
        target: app
        function onPlaylistsLoaded() { root.hint = "歌单列表已刷新" }
        function onSyncFinished(sourceId, ok, message) {
            root.hint = (ok ? "同步完成：" : "同步失败：") + message
        }
        function onSearchStarted() { root.searching = true }
        function onSearchFinished(count, kind) {
            root.searching = false
            // kind 为空是「榜单/歌单详情」的回包，别把它当成搜索结果显示成「搜索到 N 首」
            if (count >= 0 && kind.length > 0)
                root.hint = kind === "playlist" ? ("搜索到 " + count + " 个歌单")
                                                : ("搜索到 " + count + " 首歌曲")
        }
    }

    Component.onCompleted: {
        app.refreshPlaylists()
        // 公开榜单不依赖登录：先拉一次，未登录且本地没有歌单时默认直接展示这一页
        app.loadDiscovery(root.selectedSourceId())
        if (app.loggedAccountCount() === 0 && app.playlistCount() === 0)
            root.tabIndex = 4
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: root.sideMargin
        anchors.rightMargin: root.sideMargin
        anchors.topMargin: root.pageTop
        anchors.bottomMargin: root.pageBottom
        spacing: root.pageGap


        // 操作按钮只定义一次：宽窗口是胶囊带文字，窄窗口收成圆形图标。
    // 两处（宽屏/窄屏）不再各实例化一份，靠 compact 属性切换外观。
    Component {
        id: actionBar

        RowLayout {
            spacing: 8

            readonly property bool compact: root.width < 780

            // 刷新歌单：主色实填充（与搜索页「播放全部」同一套主按钮语言）
            Rectangle {
                visible: !parent.compact
                implicitWidth: refreshText.implicitWidth + 32
                implicitHeight: theme.touchMin
                radius: height / 2
                color: refreshMa.containsMouse ? Qt.darker(cPrimary, 1.15) : cPrimary
                Text {
                    id: refreshText
                    anchors.centerIn: parent
                    text: root.tabIndex === 4 ? "刷新榜单"
                          : (root.tabIndex === 5 ? "刷新 MV" : "刷新歌单")
                    color: "#FFFFFF"
                    font.pixelSize: 13
                    font.bold: true
                }
                MouseArea {
                    id: refreshMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (root.tabIndex === 4) {
                            app.loadDiscovery(root.selectedSourceId())
                            root.hint = "正在刷新各平台公开榜单"
                        } else if (root.tabIndex === 5) {
                            app.loadMvList(root.mvArea, false)
                            root.hint = "正在刷新 MV 榜单"
                        } else {
                            app.refreshPlaylists()
                            root.hint = "正在刷新歌单"
                        }
                    }
                }
            }

            // 刷新（窄屏图标版）
            Rectangle {
                visible: parent.compact
                implicitWidth: theme.touchMin
                implicitHeight: theme.touchMin
                radius: height / 2
                color: refreshIconMa.containsMouse ? Qt.darker(cPrimary, 1.15) : cPrimary
                GlyphIcon {
                    anchors.centerIn: parent
                    name: "refresh"
                    size: 16
                    color: "#FFFFFF"
                }
                MouseArea {
                    id: refreshIconMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (root.tabIndex === 4) {
                            app.loadDiscovery(root.selectedSourceId())
                            root.hint = "正在刷新各平台公开榜单"
                        } else if (root.tabIndex === 5) {
                            app.loadMvList(root.mvArea, false)
                            root.hint = "正在刷新 MV 榜单"
                        } else {
                            app.refreshPlaylists()
                            root.hint = "正在刷新歌单"
                        }
                    }
                }
                ToolTip.visible: refreshIconMa.containsMouse
                ToolTip.text: "刷新"
                ToolTip.delay: 400
            }

            // 同步全部账号：中性胶囊
            Rectangle {
                visible: !parent.compact
                implicitWidth: syncText.implicitWidth + 32
                implicitHeight: theme.touchMin
                radius: height / 2
                color: syncMa.containsMouse ? root.cChipHover : root.cChip
                Text {
                    id: syncText
                    anchors.centerIn: parent
                    text: "同步全部账号"
                    color: cSub
                    font.pixelSize: 13
                }
                MouseArea {
                    id: syncMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { app.syncAllUserLibraries(); root.hint = "已开始同步全部账号" }
                }
            }

            // 同步（窄屏图标版）
            Rectangle {
                visible: parent.compact
                implicitWidth: theme.touchMin
                implicitHeight: theme.touchMin
                radius: height / 2
                color: syncIconMa.containsMouse ? root.cChipHover : root.cChip
                GlyphIcon {
                    anchors.centerIn: parent
                    name: "sync"
                    size: 16
                    color: cSub
                }
                MouseArea {
                    id: syncIconMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: { app.syncAllUserLibraries(); root.hint = "已开始同步全部账号" }
                }
                ToolTip.visible: syncIconMa.containsMouse
                ToolTip.text: "同步全部账号"
                ToolTip.delay: 400
            }

            // 网格/列表切换：圆形图标按钮（MV 页固定网格，不展示这个开关）
            Rectangle {
                visible: root.tabIndex !== 5
                implicitWidth: theme.touchMin
                implicitHeight: theme.touchMin
                radius: height / 2
                color: gridMa.containsMouse ? root.cChipHover : root.cChip
                GlyphIcon {
                    anchors.centerIn: parent
                    name: root.gridView ? "list" : "grid"
                    size: 17
                    color: cSub
                }
                MouseArea {
                    id: gridMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.gridView = !root.gridView
                }
            }
        }
    }

    // 搜索行：音乐库自己的搜索框（原独立搜索页并入这里）
    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: theme.touchMin
        spacing: 8

        // 玻璃胶囊搜索框 + 清除键
        Rectangle {
            Layout.fillWidth: true
            // 只写 fillWidth 会被右侧图标按钮压到 0：RowLayout 按 preferred 分配余量，
            // 而 TextField 空内容时 implicitWidth 约等于 0，手机竖屏就塌成一枚图标。
            Layout.preferredWidth: 300
            Layout.minimumWidth: 120
            Layout.maximumWidth: 520
            Layout.preferredHeight: theme.touchMin
            radius: height / 2
            color: keyword.activeFocus ? (root.isDark ? Qt.rgba(1, 1, 1, 0.10)
                                                       : Qt.rgba(0, 0, 0, 0.03))
                                       : root.cChip
            border.width: 1
            border.color: keyword.activeFocus ? Qt.rgba(cPrimary.r, cPrimary.g, cPrimary.b, 0.6)
                                              : "transparent"
            Behavior on color { ColorAnimation { duration: 140 } }
            Behavior on border.color { ColorAnimation { duration: 140 } }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 6
                spacing: 8

                GlyphIcon {
                    id: fieldSearchIcon
                    Layout.alignment: Qt.AlignVCenter
                    name: "search"
                    size: 15
                    color: keyword.activeFocus ? cPrimary : cSub
                    // 窄屏把右侧独立搜索按钮省掉（下方 visible 控制），这枚放大镜兼作按钮
                    MouseArea {
                        anchors.centerIn: parent
                        width: 28
                        height: 28
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.runSearch()
                    }
                }

                TextField {
                    id: keyword
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredWidth: 120
                    text: root.searchKeyword
                    placeholderText: "搜索歌曲、歌手或歌单"
                    color: root.cText
                    placeholderTextColor: cSub
                    font.pixelSize: 13
                    background: null
                    selectByMouse: true
                    // 输入框始终以 root 为准：清空搜索时外部把 searchKeyword 置空，输入框跟着清
                    onTextChanged: {
                        if (text.length === 0 && root.searchKeyword.length > 0)
                            root.clearSearch()
                    }
                    onAccepted: root.runSearch()
                }

                // 搜索中/有结果时给一枚清除按钮，一键回到当前 Tab
                Rectangle {
                    Layout.preferredWidth: 26
                    Layout.preferredHeight: 26
                    Layout.alignment: Qt.AlignVCenter
                    radius: 13
                    visible: root.searchMode || keyword.text.length > 0
                    color: clearMa.containsMouse ? root.cChipHover : "transparent"
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: "close"
                        size: 12
                        color: cSub
                    }
                    MouseArea {
                        id: clearMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            keyword.text = ""
                            root.clearSearch()
                        }
                    }
                }
            }
        }

        // 搜索按钮：主色圆形（触控友好，键盘回车同样触发）
        // 窄屏（手机竖屏）藏起来：44dp 的圆按钮会吃掉本就紧张的宽度，
        // 搜索框左侧的放大镜和键盘回车都能发起搜索
        Rectangle {
            visible: root.width >= 780
            Layout.preferredWidth: theme.touchMin
            Layout.preferredHeight: theme.touchMin
            radius: height / 2
            color: searchMa.pressed ? Qt.darker(cPrimary, 1.1)
                   : searchMa.containsMouse ? Qt.darker(cPrimary, 1.15) : cPrimary
            GlyphIcon {
                anchors.centerIn: parent
                name: "search"
                size: 16
                color: "#FFFFFF"
            }
            MouseArea {
                id: searchMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.runSearch()
            }
        }

        Item { Layout.fillWidth: true; Layout.preferredWidth: 0 }

        Loader {
            Layout.alignment: Qt.AlignVCenter
            sourceComponent: actionBar
        }
    }

    // 页头主行：平台筛选图标 + 胶囊 Tab（可横向滚动）
    // 行高必须 ≥ 按钮的触控尺寸：这里 clip: true，行高不够会把按钮裁掉
    Item {
        Layout.fillWidth: true
        Layout.preferredHeight: theme.touchMin
        clip: true

            // 平台筛选收成一枚图标按钮：点开弹出「全部平台 + 各平台」，
            // 选中平台后按钮描边/图标取该平台主色，省掉原来整行胶囊占的高度
            Rectangle {
                id: platformBtn
                z: 2
                width: theme.touchMin
                height: theme.touchMin
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                radius: height / 2

                readonly property color platformAccent: root.selectedPlatform === 0
                                                         ? cPrimary
                                                         : app.sourceColor(root.selectedSourceId())
                color: platformMa.containsMouse ? root.cChipHover : root.cChip
                border.width: root.selectedPlatform === 0 ? 0 : 1
                border.color: Qt.rgba(platformAccent.r, platformAccent.g, platformAccent.b, 0.55)

                GlyphIcon {
                    anchors.centerIn: parent
                    name: "filter"
                    size: 17
                    color: platformBtn.platformAccent
                }

                MouseArea {
                    id: platformMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: platformMenu.popup(platformBtn)
                }

                ToolTip.visible: platformMa.containsMouse
                ToolTip.text: root.selectedPlatform === 0
                              ? "全部平台"
                              : (root.selectedPlatform <= app.sourceNames.length
                                 ? app.sourceNames[root.selectedPlatform - 1] : "")
                ToolTip.delay: 400

                // 菜单项由 Instantiator 动态挂载（index 0 = 全部平台，其余按平台顺序）
                Menu {
                    id: platformMenu
                    padding: 6
                    background: Rectangle {
                        implicitWidth: 168
                        radius: 14
                        color: theme.card
                        border.color: cLine
                        border.width: 1
                    }
                    Instantiator {
                        model: app.sourceIds.length + 1
                        delegate: MenuItem {
                            id: sourceItem
                            height: Math.max(40, theme.touchMin)
                            contentItem: Text {
                                text: index === 0
                                      ? "全部平台"
                                      : (index - 1 < app.sourceNames.length
                                         ? app.sourceNames[index - 1] : "")
                                color: root.selectedPlatform === index ? cPrimary : cText
                                font.pixelSize: 14
                                font.bold: root.selectedPlatform === index
                                verticalAlignment: Text.AlignVCenter
                                leftPadding: 10
                                elide: Text.ElideRight
                            }
                            background: Rectangle {
                                radius: 10
                                color: sourceItem.hovered ? root.cChipHover : "transparent"
                            }
                            onClicked: {
                                platformMenu.close()
                                root.selectedPlatform = index
                                app.filterPlaylists(index === 0 ? "" : app.sourceIds[index - 1])
                                root.reloadSongs()
                            }
                        }
                        onObjectAdded: function(index, object) { platformMenu.insertItem(index, object) }
                        onObjectRemoved: function(index, object) { platformMenu.removeItem(object) }
                    }
                }
            }

            // 胶囊 Tab：选中 = 主色底白字 + 淡投影；放不下时横向滚动，右缘渐隐提示可左滑
            Flickable {
                id: tabFlick
                anchors.left: platformBtn.right
                anchors.leftMargin: 8
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                contentWidth: tabInner.implicitWidth
                contentHeight: height
                clip: true
                flickableDirection: Flickable.HorizontalFlick
                Row {
                    id: tabInner
                    height: parent.height
                    spacing: 8

                    Repeater {
                        model: root.tabTitles
                        delegate: Rectangle {
                            property bool active: root.tabIndex === index
                            width: tabText.implicitWidth + 24
                            height: theme.touchMin
                            radius: height / 2
                            color: active ? cPrimary
                                  : tabMa.containsMouse ? root.cChipHover : root.cChip
                            Behavior on color { ColorAnimation { duration: 140 } }

                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -1
                                radius: parent.radius + 1
                                color: cPrimary
                                opacity: active ? 0.25 : 0
                                z: -1
                                Behavior on opacity { NumberAnimation { duration: 180 } }
                            }

                            Text {
                                id: tabText
                                anchors.centerIn: parent
                                text: modelData
                                color: active ? "#FFFFFF" : cSub
                                font.pixelSize: 13
                                font.bold: active
                            }
                            MouseArea {
                                id: tabMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    root.tabIndex = index
                                    // 切 Tab 即退出搜索视图，回到该 Tab 自己的内容
                                    root.exitSearch()
                                    root.reloadSongs()
                                }
                            }
                        }
                    }
                }
            }

            // 右缘渐隐：有更多内容可左滑时显示，滑到尽头自动隐藏
            Rectangle {
                anchors.right: tabFlick.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 26
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: Qt.rgba(cBg.r, cBg.g, cBg.b, 0) }
                    GradientStop { position: 1.0; color: cBg }
                }
                opacity: tabFlick.contentX < tabFlick.contentWidth - tabFlick.width - 1 ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 150 } }
            }
        }

        Item {
            id: libraryArea
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: root.shortViewport ? 180 : 120

            StackLayout {
                anchors.fill: parent
                // 搜索模式下显示最后一页「搜索结果」，清空搜索框回到当前 Tab
                currentIndex: root.searchMode ? root.searchViewIndex : root.tabIndex

                // 我的歌单
                Item {
                    ListView {
                        id: playlistList
                            cacheBuffer: height * 2   // 预渲染两屏：默认只多备 100px，长列表滚动时委托反复重建
                        anchors.fill: parent
                        clip: true
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        spacing: 8
                        model: app.playlists
                        visible: !root.gridView && root.openPlaylistTitle === ""

                        delegate: Rectangle {
                            width: playlistList.width
                            // 行内右侧胶囊是触控尺寸，行高 = 胶囊 + 上下各 10 的留白
                            height: Math.max(68, theme.touchMin + 20)
                            radius: 12
                            color: delegateHover.hovered ? theme.cardHover : cCard
                            border.color: cLine

                            property color accent: app.sourceColor(model.source)

                            MouseArea {
                                id: delegateHover
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: root.openPlaylist(model.title, model.uid)
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 10

                                Rectangle {
                                    Layout.preferredWidth: 46
                                    Layout.preferredHeight: 46
                                    radius: 10
                                    color: Qt.rgba(accent.r, accent.g, accent.b, 0.12)
                                    Text {
                                        anchors.centerIn: parent
                                        text: "♪"
                                        color: accent
                                        font.pixelSize: 18
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 3
                                    Text {
                                        Layout.fillWidth: true
                                        text: model.title
                                        color: cText
                                        font.pixelSize: 14
                                        font.bold: true
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: model.count + " 首 · " + model.sourceName
                                              + (model.owner.length > 0 ? " · " + model.owner : "")
                                        color: cSub
                                        font.pixelSize: theme.fontSm
                                        elide: Text.ElideRight
                                    }
                                }

                                Rectangle {
                                    Layout.preferredWidth: 76
                                    Layout.preferredHeight: theme.touchMin
                                    radius: height / 2
                                    color: Qt.rgba(accent.r, accent.g, accent.b, 0.12)
                                    Text {
                                        anchors.centerIn: parent
                                        text: "加入队列"
                                        color: accent
                                        font.pixelSize: theme.fontSm
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        onClicked: app.addPlaylistToQueue(model.uid)
                                    }
                                }
                            }
                        }
                    }

                    // 我的歌单（封面卡片墙）
                    GridView {
                        id: playlistGrid
                        anchors.fill: parent
                        clip: true
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        model: app.playlists
                        cellWidth: root.gridCellWidth
                        cellHeight: root.gridCellHeight
                        visible: root.gridView && root.openPlaylistTitle === ""

                        delegate: AlbumCard {
                            width: playlistGrid.cellWidth
                            height: playlistGrid.cellHeight
                            title: model.title
                            subtitle: model.count + " 首 · " + model.sourceName
                                      + (model.owner.length > 0 ? " · " + model.owner : "")
                            cover: model.cover
                            badge: model.sourceName
                            accent: app.sourceColor(model.source)
                            onClicked: root.openPlaylist(model.title, model.uid)
                        }
                    }

                    EmptyState {
                        anchors.centerIn: parent
                        visible: playlistList.count === 0 && root.openPlaylistTitle === ""
                        iconName: "music"
                        title: "还没有歌单"
                        hint: "登录各平台账号后点「刷新歌单」，或先同步全部账号"
                    }

                    // 歌单详情：点开歌单后在这里直接看曲目
                    ColumnLayout {
                        anchors.fill: parent
                        visible: root.openPlaylistTitle !== ""
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Rectangle {
                                width: 86
                                height: theme.touchMin
                                radius: height / 2
                                color: "transparent"
                                border.color: cLine
                                Text {
                                    anchors.centerIn: parent
                                    text: "← 返回歌单"
                                    color: cSub
                                    font.pixelSize: theme.fontSm
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: root.openPlaylistTitle = ""
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: root.openPlaylistTitle
                                color: cText
                                font.pixelSize: 14
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Text {
                                text: detailList.count + " 首"
                                color: cSub
                                font.pixelSize: theme.fontSm
                            }
                        }

                        ListView {
                            id: detailList
                                cacheBuffer: height * 2   // 预渲染两屏：默认只多备 100px，长列表滚动时委托反复重建
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                            spacing: 8
                            model: app.results
                            delegate: SongRow {
                                width: detailList.width
                                rowNumber: index
                                title: model.title
                                artist: model.artist
                                album: model.album
                                cover: model.cover
                                sourceName: model.sourceName
                                sourceId: model.source
                                accent: app.sourceColor(model.source)
                                duration: model.duration
                                vip: model.vip
                                favorite: model.favorite
                                showMv: model.source === "netease"
                                onPlayRequested: app.playResult(index)
                                onFavoriteRequested: app.toggleFavoriteResult(index)
                                onAddRequested: app.addResultToQueue(index)
                                onMvRequested: app.playMv(index)
                                onDownloadRequested: app.downloadResult(index)
                                showDownload: true
                            }
                        }

                        EmptyState {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 120
                            visible: detailList.count === 0
                            iconName: "music"
                            title: "这个歌单是空的"
                            hint: "可能需要在对应平台登录后刷新歌单"
                        }
                    }
                }

                // 收藏
                Item {
                    ListView {
                        id: favoriteList
                            cacheBuffer: height * 2   // 预渲染两屏：默认只多备 100px，长列表滚动时委托反复重建
                        anchors.fill: parent
                        clip: true
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        spacing: 8
                        model: app.results
                        delegate: SongRow {
                            width: favoriteList.width
                            rowNumber: index
                            title: model.title
                            artist: model.artist
                            album: model.album
                            cover: model.cover
                            sourceName: model.sourceName
                            sourceId: model.source
                            accent: app.sourceColor(model.source)
                            duration: model.duration
                            vip: model.vip
                            favorite: model.favorite
                            showMv: model.source === "netease"
                            onPlayRequested: app.playResult(index)
                            onFavoriteRequested: app.toggleFavoriteResult(index)
                            onAddRequested: app.addResultToQueue(index)
                            onMvRequested: app.playMv(index)
                        }
                    }
                    EmptyState {
                        anchors.centerIn: parent
                        visible: favoriteList.count === 0
                        glyph: "♥"
                        title: "还没有收藏"
                        hint: "搜索歌曲后点行尾的心形按钮即可收藏"
                    }
                }

                // 播放历史
                Item {
                    ListView {
                        id: historyList
                            cacheBuffer: height * 2   // 预渲染两屏：默认只多备 100px，长列表滚动时委托反复重建
                        anchors.fill: parent
                        clip: true
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        spacing: 8
                        model: app.results
                        delegate: SongRow {
                            width: historyList.width
                            rowNumber: index
                            title: model.title
                            artist: model.artist
                            album: model.album
                            cover: model.cover
                            sourceName: model.sourceName
                            sourceId: model.source
                            accent: app.sourceColor(model.source)
                            duration: model.duration
                            vip: model.vip
                            favorite: model.favorite
                            showMv: model.source === "netease"
                            onPlayRequested: app.playResult(index)
                            onFavoriteRequested: app.toggleFavoriteResult(index)
                            onAddRequested: app.addResultToQueue(index)
                            onMvRequested: app.playMv(index)
                        }
                    }
                    EmptyState {
                        anchors.centerIn: parent
                        visible: historyList.count === 0
                        glyph: "◷"
                        title: "还没有播放记录"
                        hint: "播放过的歌曲会在这里按平台聚合"
                    }
                }

                // 播放队列
                Item {
                    ListView {
                        id: queueList
                            cacheBuffer: height * 2   // 预渲染两屏：默认只多备 100px，长列表滚动时委托反复重建
                        anchors.fill: parent
                        clip: true
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        spacing: 8
                        model: app.queue
                        delegate: SongRow {
                            width: queueList.width
                            rowNumber: index
                            title: model.title
                            artist: model.artist
                            album: model.album
                            cover: model.cover
                            sourceName: model.sourceName
                            sourceId: model.source
                            accent: app.sourceColor(model.source)
                            duration: model.duration
                            vip: model.vip
                            favorite: model.favorite
                            playing: index === app.currentQueueIndex
                            showAdd: false
                            showRemove: true
                            onPlayRequested: app.playQueue(index)
                            onFavoriteRequested: app.toggleFavoriteQueue(index)
                            onRemoveRequested: app.removeQueue(index)
                        }
                    }
                    EmptyState {
                        anchors.centerIn: parent
                        visible: queueList.count === 0
                        iconName: "play"
                        title: "播放队列是空的"
                        hint: "在搜索结果里点「＋」把歌曲加入队列"
                    }

                    Rectangle {
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: 4
                        width: 80
                        height: theme.touchMin
                        radius: height / 2
                        visible: queueList.count > 0
                        color: cSoft
                        Text {
                            anchors.centerIn: parent
                            text: "清空队列"
                            color: cPrimary
                            font.pixelSize: theme.fontSm
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: app.clearQueue()
                        }
                    }
                }
                // 发现榜单：各平台公开榜单 / 推荐歌单，完全不需要登录
                Item {
                    GridView {
                        id: discoveryGrid
                        anchors.fill: parent
                        clip: true
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        model: app.discovery
                        cellWidth: root.gridCellWidth
                        cellHeight: root.gridCellHeight
                        visible: root.discoveryTitle === ""

                        delegate: AlbumCard {
                            width: discoveryGrid.cellWidth
                            height: discoveryGrid.cellHeight
                            title: model.title
                            subtitle: (model.count > 0 ? model.count + " 首 · " : "")
                                      + model.sourceName
                            cover: model.cover
                            badge: model.sourceName
                            accent: app.sourceColor(model.source)
                            onClicked: root.openDiscovery(model.title, model.uid)
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: app.discoveryLoading && discoveryGrid.count === 0
                        text: "正在加载各平台公开榜单…"
                        color: cSub
                        font.pixelSize: 13
                    }

                    EmptyState {
                        anchors.centerIn: parent
                        visible: !app.discoveryLoading && discoveryGrid.count === 0
                                 && root.discoveryTitle === ""
                        iconName: "music"
                        title: "还没有公开榜单"
                        hint: "点右上角「刷新歌单」重新拉取，或检查网络后重试"
                    }

                    // 榜单曲目详情（与「我的歌单」共用 app.results，互不干扰）
                    ColumnLayout {
                        anchors.fill: parent
                        visible: root.discoveryTitle !== ""
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            Rectangle {
                                width: 86
                                height: theme.touchMin
                                radius: height / 2
                                color: "transparent"
                                border.color: cLine
                                Text {
                                    anchors.centerIn: parent
                                    text: "← 返回榜单"
                                    color: cSub
                                    font.pixelSize: theme.fontSm
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: root.discoveryTitle = ""
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: root.discoveryTitle
                                color: cText
                                font.pixelSize: 14
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Text {
                                text: discoveryDetail.count + " 首"
                                color: cSub
                                font.pixelSize: theme.fontSm
                            }
                            Rectangle {
                                width: 72
                                height: theme.touchMin
                                radius: height / 2
                                color: cSoft
                                visible: discoveryDetail.count > 0
                                Text {
                                    anchors.centerIn: parent
                                    text: "播放全部"
                                    color: cPrimary
                                    font.pixelSize: theme.fontSm
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: app.playResult(0)
                                }
                            }
                        }

                        ListView {
                            id: discoveryDetail
                                cacheBuffer: height * 2   // 预渲染两屏：默认只多备 100px，长列表滚动时委托反复重建
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                            spacing: 8
                            model: app.results
                            delegate: SongRow {
                                width: discoveryDetail.width
                                rowNumber: index
                                title: model.title
                                artist: model.artist
                                album: model.album
                                cover: model.cover
                                sourceName: model.sourceName
                                sourceId: model.source
                                accent: app.sourceColor(model.source)
                                duration: model.duration
                                vip: model.vip
                                favorite: model.favorite
                                showMv: model.source === "netease"
                                onPlayRequested: app.playResult(index)
                                onFavoriteRequested: app.toggleFavoriteResult(index)
                                onAddRequested: app.addResultToQueue(index)
                                onMvRequested: app.playMv(index)
                                onDownloadRequested: app.downloadResult(index)
                                showDownload: true
                            }
                        }

                        EmptyState {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 120
                            visible: discoveryDetail.count === 0
                            iconName: "music"
                            title: "这个榜单暂时没有歌曲"
                            hint: "该平台的公共歌单接口可能不可用，试试其他平台榜单"
                        }
                    }
                }

                // MV 榜单（对齐 AlgerMusicPlayer 的 MV 页）：地区筛选 + 16:9 封面网格 +
                // 播放量角标 + 滚到底自动续页；点封面进沉浸式播放器。
                Item {
                    id: mvView

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: root.pageGap

                        Flickable {
                            Layout.fillWidth: true
                            Layout.preferredHeight: theme.touchMin
                            contentWidth: mvAreaRow.implicitWidth
                            contentHeight: height
                            clip: true
                            flickableDirection: Flickable.HorizontalFlick

                            Row {
                                id: mvAreaRow
                                height: parent.height
                                spacing: 8
                                Repeater {
                                    model: root.mvAreas
                                    delegate: Rectangle {
                                        readonly property bool active: root.mvArea === modelData
                                        width: mvAreaText.implicitWidth + 24
                                        height: theme.touchMin
                                        radius: height / 2
                                        color: active ? cPrimary
                                              : (mvAreaMa.containsMouse ? root.cChipHover : root.cChip)
                                        Behavior on color { ColorAnimation { duration: 140 } }
                                        Text {
                                            id: mvAreaText
                                            anchors.centerIn: parent
                                            text: modelData
                                            color: parent.active ? "#FFFFFF" : cSub
                                            font.pixelSize: 13
                                            font.bold: parent.active
                                        }
                                        MouseArea {
                                            id: mvAreaMa
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                if (root.mvArea === modelData) return
                                                root.mvArea = modelData
                                                app.loadMvList(modelData, false)
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        GridView {
                            id: mvGrid
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: app.mvList
                            readonly property int cols: Math.max(2, Math.min(5,
                                Math.floor(Math.max(320, width) / 220)))
                            cellWidth: width / cols
                            cellHeight: cellWidth * 9 / 16 + 62
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                            onMovementEnded: {
                                if (atYEnd && app.mvHasMore && !app.mvLoading)
                                    app.loadMvList(root.mvArea, true)
                            }

                            delegate: Item {
                                width: mvGrid.cellWidth
                                height: mvGrid.cellHeight

                                Column {
                                    anchors.fill: parent
                                    anchors.margins: 6
                                    spacing: 6

                                    Rectangle {
                                        width: parent.width
                                        height: width * 9 / 16
                                        radius: theme.radiusMd
                                        color: root.cChip
                                        clip: true

                                        Image {
                                            id: mvCoverImage
                                            anchors.fill: parent
                                            // StackLayout 隐藏页里的远程 Image 不会真正完成加载：
                                            // 把 source 挂到 mvView.visible，进 MV Tab 时才开始取图，
                                            // 既保证切进来就有封面，也避免后台白拉 24 张图浪费流量。
                                            source: mvView.visible ? modelData.cover : ""
                                            // 网格单元只有 16:9 小卡，限制解码尺寸，别按 CDN 原图开显存
                                            sourceSize.width: 400
                                            sourceSize.height: 225
                                            fillMode: Image.PreserveAspectCrop
                                            asynchronous: true
                                            cache: true
                                            opacity: status === Image.Ready ? 1 : 0
                                            Behavior on opacity {
                                                NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
                                            }
                                        }

                                        // 悬停/触屏常驻的播放圆钮
                                        Rectangle {
                                            anchors.centerIn: parent
                                            width: 46
                                            height: 46
                                            radius: 23
                                            visible: mvMa.containsMouse || platform.isMobile
                                            color: Qt.rgba(0, 0, 0, 0.45)
                                            border.color: Qt.rgba(1, 1, 1, 0.35)
                                            border.width: 1
                                            GlyphIcon {
                                                anchors.centerIn: parent
                                                name: "play"
                                                size: 20
                                                color: "#FFFFFF"
                                            }
                                        }

                                        // 播放量角标
                                        Rectangle {
                                            anchors.top: parent.top
                                            anchors.right: parent.right
                                            anchors.margins: 8
                                            width: mvCountText.implicitWidth + 16
                                            height: 22
                                            radius: 11
                                            color: Qt.rgba(0, 0, 0, 0.58)
                                            Row {
                                                anchors.centerIn: parent
                                                spacing: 4
                                                GlyphIcon {
                                                    name: "play"
                                                    size: 10
                                                    color: "#FFFFFF"
                                                    anchors.verticalCenter: parent.verticalCenter
                                                }
                                                Text {
                                                    id: mvCountText
                                                    text: root.formatCount(modelData.playCount)
                                                    color: "#FFFFFF"
                                                    font.pixelSize: 10
                                                    anchors.verticalCenter: parent.verticalCenter
                                                }
                                            }
                                        }
                                    }

                                    Text {
                                        width: parent.width
                                        text: modelData.title
                                        color: cText
                                        font.pixelSize: 13
                                        font.bold: true
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        width: parent.width
                                        text: modelData.artist
                                        color: cSub
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                }

                                MouseArea {
                                    id: mvMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: app.playMvItem(index)
                                }
                            }

                            footer: Item {
                                width: mvGrid.width
                                height: 46
                                Text {
                                    anchors.centerIn: parent
                                    text: app.mvLoading ? "正在加载更多 MV…"
                                          : (app.mvHasMore ? "下滑加载更多" : "没有更多了")
                                    color: cSub
                                    font.pixelSize: 12
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    enabled: app.mvHasMore && !app.mvLoading
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: app.loadMvList(root.mvArea, true)
                                }
                            }
                        }
                    }

                    BusyIndicator {
                        anchors.centerIn: parent
                        running: app.mvLoading && app.mvList.length === 0
                        visible: running
                    }

                    EmptyState {
                        anchors.centerIn: parent
                        visible: app.mvList.length === 0 && !app.mvLoading
                        compact: root.shortViewport
                        iconName: "video"
                        title: "暂时没有 MV"
                        hint: "检查网络后点刷新，或切换地区试试"
                    }
                }

                // 搜索结果（原独立搜索页并入音乐库）：歌曲 / 歌单 + 网格 / 列表
                Item {
                    id: searchView
                    readonly property bool songMode: app.searchType !== "songlist"

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10

                            Text {
                                Layout.fillWidth: true
                                text: root.searching
                                      ? "正在搜索「" + root.searchKeyword + "」…"
                                      : (searchView.songMode
                                         ? (searchSongList.count > 0
                                            ? searchSongList.count + " 首歌曲"
                                            : "没有找到相关歌曲")
                                         : (searchPlaylistList.count > 0
                                            ? searchPlaylistList.count + " 个歌单"
                                            : "没有找到相关歌单"))
                                color: cSub
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }

                            // 类型胶囊：歌曲 / 歌单
                            Row {
                                spacing: 8
                                Repeater {
                                    model: ["歌曲", "歌单"]
                                    delegate: Rectangle {
                                        property bool active: index === 0 ? searchView.songMode
                                                                          : !searchView.songMode
                                        width: typeText.implicitWidth + 28
                                        height: theme.touchMin
                                        radius: height / 2
                                        color: active ? cPrimary
                                              : typeMa.containsMouse ? root.cChipHover : root.cChip
                                        Behavior on color { ColorAnimation { duration: 140 } }
                                        Text {
                                            id: typeText
                                            anchors.centerIn: parent
                                            text: modelData
                                            color: active ? "#FFFFFF" : cSub
                                            font.pixelSize: 13
                                            font.bold: active
                                        }
                                        MouseArea {
                                            id: typeMa
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: app.setSearchType(index === 0 ? "song" : "songlist")
                                        }
                                    }
                                }
                            }

                            // 播放全部
                            Rectangle {
                                visible: searchView.songMode && searchSongList.count > 0
                                implicitWidth: playAllRow.implicitWidth + 32
                                implicitHeight: theme.touchMin
                                radius: height / 2
                                color: playAllMa.pressed ? Qt.darker(cPrimary, 1.1)
                                       : playAllMa.containsMouse ? Qt.darker(cPrimary, 1.12) : cPrimary
                                Row {
                                    id: playAllRow
                                    anchors.centerIn: parent
                                    spacing: 6
                                    GlyphIcon {
                                        anchors.verticalCenter: parent.verticalCenter
                                        name: "play"
                                        size: 14
                                        color: "#FFFFFF"
                                    }
                                    Text {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: "播放全部"
                                        color: "#FFFFFF"
                                        font.pixelSize: 13
                                        font.bold: true
                                    }
                                }
                                MouseArea {
                                    id: playAllMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: app.playResult(0)
                                }
                            }
                        }

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumHeight: 120

                            // 歌曲（列表）
                            ListView {
                                id: searchSongList
                                cacheBuffer: height * 2
                                anchors.fill: parent
                                clip: true
                                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                                spacing: 6
                                model: app.results
                                visible: searchView.songMode && !root.gridView && count > 0
                                delegate: SongRow {
                                    width: searchSongList.width
                                    rowNumber: index
                                    title: model.title
                                    artist: model.artist
                                    album: model.album
                                    cover: model.cover
                                    sourceName: model.sourceName
                                    sourceId: model.source
                                    accent: app.sourceColor(model.source)
                                    duration: model.duration
                                    vip: model.vip
                                    favorite: model.favorite
                                    showMv: model.source === "netease"
                                    showDownload: true
                                    onPlayRequested: app.playResult(index)
                                    onFavoriteRequested: app.toggleFavoriteResult(index)
                                    onAddRequested: app.addResultToQueue(index)
                                    onMvRequested: app.playMv(index)
                                    onDownloadRequested: app.downloadResult(index)
                                }
                            }

                            // 歌曲（封面卡片墙）
                            GridView {
                                id: searchSongGrid
                                anchors.fill: parent
                                clip: true
                                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                                model: app.results
                                cellWidth: root.gridCellWidth
                                cellHeight: root.gridCellHeight
                                visible: searchView.songMode && root.gridView && count > 0
                                delegate: AlbumCard {
                                    width: searchSongGrid.cellWidth
                                    height: searchSongGrid.cellHeight
                                    title: model.title
                                    subtitle: model.artist
                                              + (model.album.length > 0 ? " · " + model.album : "")
                                    cover: model.cover
                                    badge: model.sourceName
                                    accent: app.sourceColor(model.source)
                                    onClicked: app.playResult(index)
                                }
                            }

                            // 歌单（列表）
                            ListView {
                                id: searchPlaylistList
                                cacheBuffer: height * 2
                                anchors.fill: parent
                                clip: true
                                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                                spacing: 8
                                model: app.playlists
                                visible: !searchView.songMode && !root.gridView && count > 0
                                delegate: Rectangle {
                                    width: searchPlaylistList.width
                                    height: Math.max(68, theme.touchMin + 20)
                                    radius: 12
                                    color: plRowMa.containsMouse ? theme.cardHover : cCard
                                    border.color: cLine
                                    property color accent: app.sourceColor(model.source)

                                    MouseArea {
                                        id: plRowMa
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            root.exitSearch()
                                            root.openPlaylistTitle = model.title
                                            root.tabIndex = 0
                                            app.loadPlaylist(model.uid)
                                        }
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.margins: 10
                                        spacing: 10
                                        Rectangle {
                                            Layout.preferredWidth: 46
                                            Layout.preferredHeight: 46
                                            radius: 10
                                            color: Qt.rgba(app.sourceColor(model.source).r,
                                                           app.sourceColor(model.source).g,
                                                           app.sourceColor(model.source).b, 0.12)
                                            Text {
                                                anchors.centerIn: parent
                                                text: "♪"
                                                color: app.sourceColor(model.source)
                                                font.pixelSize: 18
                                            }
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 3
                                            Text {
                                                Layout.fillWidth: true
                                                text: model.title
                                                color: cText
                                                font.pixelSize: 14
                                                font.bold: true
                                                elide: Text.ElideRight
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                text: model.count + " 首 · " + model.sourceName
                                                      + (model.owner.length > 0 ? " · " + model.owner : "")
                                                color: cSub
                                                font.pixelSize: theme.fontSm
                                                elide: Text.ElideRight
                                            }
                                        }
                                    }
                                }
                            }

                            // 歌单（封面卡片墙）
                            GridView {
                                id: searchPlaylistGrid
                                anchors.fill: parent
                                clip: true
                                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                                model: app.playlists
                                cellWidth: root.gridCellWidth
                                cellHeight: root.gridCellHeight
                                visible: !searchView.songMode && root.gridView && count > 0
                                delegate: AlbumCard {
                                    width: searchPlaylistGrid.cellWidth
                                    height: searchPlaylistGrid.cellHeight
                                    title: model.title
                                    subtitle: model.count + " 首 · " + model.sourceName
                                              + (model.owner.length > 0 ? " · " + model.owner : "")
                                    cover: model.cover
                                    badge: model.sourceName
                                    accent: app.sourceColor(model.source)
                                    onClicked: {
                                        root.exitSearch()
                                        root.openPlaylistTitle = model.title
                                        root.tabIndex = 0
                                        app.loadPlaylist(model.uid)
                                    }
                                }
                            }

                            EmptyState {
                                anchors.centerIn: parent
                                visible: !root.searching
                                         && (searchView.songMode ? searchSongList.count === 0
                                                                 : searchPlaylistList.count === 0)
                                iconName: "search"
                                compact: root.shortViewport
                                title: searchView.songMode ? "没有找到相关歌曲" : "没有找到相关歌单"
                                hint: "换个关键词试试，或在左上角切换平台范围"
                            }
                        }
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: root.hint.length > 0
            text: root.hint
            color: cSub
            font.pixelSize: theme.fontSm
            elide: Text.ElideRight
        }
    }       // ColumnLayout
}           // root
