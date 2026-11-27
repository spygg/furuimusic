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
    // 页头留白：安全区（状态栏/挖孔）已由外壳 platform.insetTop 统一内缩，
    // 这里只留少量呼吸空间，避免与 insetTop 叠加后顶部空白偏大（旧值 14/24/8/12）。
    readonly property real pageTop: shortViewport ? (root.height < 420 ? 4 : 6)
                                                  : (root.width < 700 ? 5 : 10)
    readonly property real pageGap: shortViewport ? 8 : 12
    readonly property real pageBottom: shortViewport ? 8 : 12
    // 页头两行（搜索行 + 我的歌单/刷新歌单 胶囊行）默认各 48dp，合计近百：
    // 平板横屏/车机矮屏的逻辑高度常常只有 270~450dp，光页头就吃掉一半，
    // 矮窗口里把它们压到 40dp，把高度让给内容区
    readonly property int headerRowH: root.shortViewport ? 40 : theme.touchMin

    readonly property bool isDark: theme.dark
    readonly property color cBg: theme.bg
    readonly property color cCard: theme.card
    readonly property color cText: theme.text
    readonly property color cSub: theme.subText
    readonly property color cPrimary: theme.accent
    readonly property color cLine: theme.line
    readonly property color cSoft: theme.accentSoft
    // 移动端外壳会在整页背后铺封面氛围底（见 Backdrop）。此时 5% 黑色胶囊
    // 几乎完全透明，未选中的 Tab / 头部图标会被封面大字淹没；由外壳把这个
    // 属性置真后改用带白纱的玻璃底，桌面（纯色底）仍保持原来的轻量色块。
    property bool onArtBackdrop: false
    readonly property color cChip: root.onArtBackdrop
                                   ? (theme.dark ? Qt.rgba(1, 1, 1, 0.18)
                                                 : Qt.rgba(1, 1, 1, 0.80))
                                   : (theme.dark ? Qt.rgba(1, 1, 1, 0.06)
                                                 : Qt.rgba(0, 0, 0, 0.05))
    readonly property color cChipHover: root.onArtBackdrop
                                   ? (theme.dark ? Qt.rgba(1, 1, 1, 0.26)
                                                 : Qt.rgba(1, 1, 1, 0.92))
                                   : (theme.dark ? Qt.rgba(1, 1, 1, 0.12)
                                                 : Qt.rgba(0, 0, 0, 0.09))

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
    // 用户手动点过「卡片 ↔ 列表」后就不再按形态自动切，避免横竖屏来回转时反复覆盖用户选择
    property bool gridViewByUser: false
    // 横屏手机内容高度只剩 300dp 上下，封面卡片一格就占满，等于看不见几行
    readonly property bool shortLandscape: root.height < 460 && root.width > root.height
    onShortLandscapeChanged: {
        if (!root.gridViewByUser)
            root.gridView = !root.shortLandscape
    }
    // 详情曲目正在从平台接口拉取：此时列表为空不等于「没有歌」，
    // 头部直接显示「0 首」会让人以为榜单是空的
    property bool detailLoading: false
    // 汽水发现面板默认展示「模式探索」歌单网格；推荐歌曲由右上角「看歌曲」切换，
    // 这样熟悉/新鲜模式生成的歌曲流仍有入口，但不再挤占歌单的滚动空间。
    property bool qishuiShowSongs: false

    // 只有「我的歌单」「发现榜单」和搜索结果有两种排布；收藏/历史/队列本身就是单列曲目列表，
    // 在这些 Tab 上显示切换按钮会是死按钮，直接不展示
    readonly property bool gridToggleAvailable: root.searchMode
                                               || root.tabIndex === 0
                                               || root.tabIndex === 4

    // ---- 搜索（原独立搜索页已并入音乐库）----
    // 搜索框有内容即进入搜索结果视图，清空则回到当前 Tab；两者共用 app.results
    property string searchKeyword: ""
    property bool searching: false
    readonly property bool searchMode: searchKeyword.length > 0
    readonly property int searchViewIndex: root.tabTitles.length
    // 输入法修复（同浏览器地址栏 addressEditing）：用户一开始手动输入就置位，
    // 任何后台自动同步都不能再写 keyword.text，否则退格/删除时后台把旧词写回，
    // 看起来像「删不掉、还剩字」。提交搜索后复位，才允许外部清空。
    property bool keywordEditing: false
    onVisibleChanged: { if (!visible) root.keywordEditing = false }
    // 搜索态变化时通知外壳：搜索时隐藏迷你播放条，给结果区和软键盘腾出空间，
    // 避免播放栏压住搜索内容（矮屏/键盘弹出时尤其明显）。输入框持有焦点（键盘开着）
    // 同样算搜索态，与 keyword.onActiveFocusChanged 双向对齐。
    onSearchModeChanged: app.searchActive = root.searchMode || keyword.activeFocus
    // 软键盘收起（返回键 / 点空白）时若输入框已空，结束搜索态。
    // 焦点往往还留在输入框上，光靠 onActiveFocusChanged 清不掉，迷你播放条会一直不回来。
    // 用属性绑定监听输入法可见性（比 Connections target 到 Qt.inputMethod 更稳）。
    readonly property bool imeVisible: Qt.inputMethod.visible
    onImeVisibleChanged: {
        if (!imeVisible && keyword.text.length === 0)
            app.searchActive = false
    }
    // 曲库页退到后台/被切走时也要撤销搜索态，否则播放栏一直不回来
    Component.onDestruction: app.searchActive = false

    // 搜索的唯一入口：放大镜、软键盘回车、类型切换重搜都走这里。
    function runSearch()
    {
        // 移动端先收起输入法，强制把组合输入（拼音候选词）提交进 text 再读取，
        // 否则取到的是未提交的半成品（同浏览器地址栏「前往」按钮的处理）。
        if (platform.isMobile && keyword.activeFocus) keyword.focus = false
        // 以输入框内容为准：root.searchKeyword 只在真正发起搜索时才写入，
        // 早先读 root.searchKeyword 会让「敲完字直接点放大镜」变成空搜索
        const kw = keyword.text.trim()
        if (kw.length === 0)
            return
        root.searchKeyword = kw
        root.keywordEditing = false       // 已提交，之后外部同步才允许写回
        app.search(kw, root.selectedSourceId())
    }

    // 退出搜索视图：输入框是命令式赋值的（用户一敲字绑定就断），
    // 所以这里必须同时清输入框，否则清空后残留旧关键词
    function exitSearch()
    {
        // 先清状态再动输入框：keyword.text 被清空会触发 onTextChanged，
        // 旧顺序下 searchKeyword 还非空 → 又调一次 clearSearch() → 重复 reloadSongs。
        root.searchKeyword = ""
        if (keyword.text.length > 0)
            keyword.text = ""
        root.searching = false
    }

    function clearSearch()
    {
        root.exitSearch()
        root.reloadSongs()
    }

    // 搜索历史下拉点选：先收起面板再写文本、发搜索（runSearch 以输入框文本为准）
    function applySearchHistory(kw)
    {
        const text = String(kw).trim()
        if (text.length === 0) return
        root.historyOpen = false
        keyword.text = text
        root.runSearch()
    }

    // 失焦延时收起：下拉项被点击时输入框会先失焦，立即隐藏会让点击落空
    property bool historyOpen: false
    // 无人值守截图用：绕过窗口激活抢焦点，直接展开历史面板
    function probeShowHistory()
    {
        historyHideTimer.stop()
        root.historyOpen = true
    }
    // 无人值守截图用：拿第一张歌单卡片弹出长按菜单，验证收藏/取消收藏入口
    function probePlaylistMenu()
    {
        const item = playlistGrid.itemAtIndex ? playlistGrid.itemAtIndex(0) : null
        if (item)
            openPlaylistMenu(item.playlistUid, item.playlistTitle, item, 20, 20, function() {}, true)
        else
            openPlaylistMenu("probe:uid", "测试歌单", searchBox, 20, 20, function() {}, true)
    }
    Timer {
        id: historyHideTimer
        interval: 250
        onTriggered: root.historyOpen = false
    }

    // 封面长按菜单：一份共享 Menu 服务所有歌单网格，弹出前写入当前项上下文
    function openPlaylistMenu(uid, title, item, posX, posY, openCallback, canCollect)
    {
        playlistMenu.menuUid = uid
        playlistMenu.menuCollected = app.playlistsEpoch >= 0 && app.isPlaylistCollected(uid)
        playlistMenu.canCollect = canCollect
        playlistMenu.openCallback = openCallback
        const p = item.mapToItem(Overlay.overlay, posX, posY)
        playlistMenu.x = p.x
        playlistMenu.y = p.y
        playlistMenu.open()
    }

    // 用 Popup + 按钮列表而不是 Menu：Menu 嵌在 StackLayout 的页面里时
    // 弹不出来（Qt 6 Quick Controls 2 的父级/关闭策略问题），Popup 同工程已验证
    Popup {
        id: playlistMenu
        parent: Overlay.overlay
        property string menuUid: ""
        property bool menuCollected: false
        property bool canCollect: true
        property var openCallback: null
        width: 168
        height: menuCol.implicitHeight + 12
        padding: 6
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: theme.card
            border.color: cLine
            border.width: 1
            radius: 14
        }
        contentItem: ColumnLayout {
            id: menuCol
            spacing: 2

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                radius: 8
                color: openMa.containsMouse ? theme.cardHover : "transparent"
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    text: "打开歌单"
                    color: cText
                    font.pixelSize: 13
                }
                MouseArea {
                    id: openMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        playlistMenu.close()
                        if (playlistMenu.openCallback) playlistMenu.openCallback()
                    }
                }
            }

            Rectangle {
                // 本地自建歌单没有收藏概念，不显示这一项
                visible: playlistMenu.canCollect
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                radius: 8
                color: collectMa.containsMouse ? theme.cardHover : "transparent"
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    text: playlistMenu.menuCollected ? "取消收藏" : "添加收藏"
                    color: cText
                    font.pixelSize: 13
                }
                MouseArea {
                    id: collectMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        playlistMenu.close()
                        if (playlistMenu.menuUid.length === 0) return
                        if (playlistMenu.menuCollected) app.uncollectPlaylist(playlistMenu.menuUid)
                        else app.collectPlaylistByUid(playlistMenu.menuUid)
                    }
                }
            }
        }
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
        else if (root.tabIndex === 4) {
            // 汽水音乐走官方车机版的推荐模式（默认/熟悉/新鲜 + 场景领域）。
            // 传空串沿用上次选的模式：用户在音源菜单选过「熟悉」后，
            // 切走再切回不会被悄悄重置成默认。
            if (root.selectedSourceId() === "qishui")
                app.loadQishuiRecommend("", "")
            else
                app.loadDiscovery(root.selectedSourceId())
        }
        else if (root.tabIndex === 5)
            app.loadMvList(root.mvArea, false)
    }

    function openPlaylist(title, uid) {
        root.detailLoading = true
        detailTimeout.restart()
        app.loadPlaylist(uid)
        root.openPlaylistTitle = title
        // 本地缓存路径上结果已经就绪：跳到正在播放的那首（异步路径由 onSearchFinished 接力）
        Qt.callLater(root.locatePlayingInDetail)
    }

    // 定位正在播放的歌：滚动到可视区并高亮（SongRow 按 uid 匹配 app.currentSongUid 变色）。
    // 没在播放 / 这首歌不在当前歌单里则不动。
    function locatePlayingInDetail() {
        if (root.openPlaylistTitle === "")
            return
        const row = app.playingDetailRow()
        if (row >= 0)
            detailList.positionViewAtIndex(row, ListView.Contain)
    }

    function openDiscovery(title, uid) {
        root.detailLoading = true
        detailTimeout.restart()
        app.openDiscoveryPlaylist(uid)
        root.discoveryTitle = title
    }

    // ---- 搜索结果「加入歌单」选择弹层（lx-music-mobile 式）----
    // 记录待加入的歌曲行号（app.results 里的下标），弹层里点歌单或新建后调用
    // app.addResultToPlaylist(index, uid)。
    property int pickerSongIndex: -1
    property string pickerSongTitle: ""

    function openPlaylistPicker(index) {
        root.pickerSongIndex = index
        root.pickerSongTitle = app.resultTitle(index)
        pickerNewName.text = ""
        pickerPopup.open()
    }

    // 兜底：接口彻底没回包（网络挂掉/榜单 id 失效）时别让头部一直停在「载入中…」
    Timer {
        id: detailTimeout
        interval: 15000
        onTriggered: root.detailLoading = false
    }

    Connections {
        target: app
        // 歌单刷新成功不再往页尾写提示：这条文案每次进页面都会闪一次，
        // 属于成功路径的噪音；失败/同步结果仍然提示
        function onPlaylistsLoaded() { }

        function onSyncFinished(sourceId, ok, message) {
            root.hint = (ok ? "同步完成：" : "同步失败：") + message
        }
        function onSearchStarted() { root.searching = true }
        function onSearchFinished(count, kind) {
            root.searching = false
            root.detailLoading = false
            detailTimeout.stop()
            // 歌单详情异步加载完成：定位正在播放的那首（缓存路径已在 openPlaylist 里做过）
            if (root.openPlaylistTitle !== "")
                root.locatePlayingInDetail()
            // kind 为 song/playlist 才是真正的搜索回包；空串是榜单/歌单详情，
            // "qishui" 是汽水推荐加载完成（不是搜索），都别显示「搜索到 N 首」
            if (count >= 0 && (kind === "song" || kind === "playlist"))
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
                implicitHeight: root.headerRowH
                radius: height / 2
                color: refreshMa.pressed ? Qt.darker(cPrimary, 1.3)
                       : (refreshMa.containsMouse ? Qt.darker(cPrimary, 1.15) : cPrimary)
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
                            if (root.selectedSourceId() === "qishui") {
                                root.qishuiShowSongs = false
                                app.loadQishuiRecommend("", "")
                                root.hint = "正在刷新汽水模式歌单"
                            } else {
                                app.loadDiscovery(root.selectedSourceId())
                                root.hint = "正在刷新各平台公开榜单"
                            }
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
                implicitWidth: root.headerRowH
                implicitHeight: root.headerRowH
                radius: height / 2
                color: refreshIconMa.pressed ? Qt.darker(cPrimary, 1.3)
                       : (refreshIconMa.containsMouse ? Qt.darker(cPrimary, 1.15) : cPrimary)
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
                            if (root.selectedSourceId() === "qishui") {
                                root.qishuiShowSongs = false
                                app.loadQishuiRecommend("", "")
                                root.hint = "正在刷新汽水模式歌单"
                            } else {
                                app.loadDiscovery(root.selectedSourceId())
                                root.hint = "正在刷新各平台公开榜单"
                            }
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
                implicitHeight: root.headerRowH
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
                implicitWidth: root.headerRowH
                implicitHeight: root.headerRowH
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

            // 网格/列表切换：圆形图标按钮
            // 只在真正有「卡片墙 ↔ 列表」两种排布的页里出现（见 gridToggleAvailable），
            // 否则点了只换个图标、内容不动，看起来就是坏按钮
            Rectangle {
                visible: root.gridToggleAvailable
                implicitWidth: root.headerRowH
                implicitHeight: root.headerRowH
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
                    onClicked: {
                        root.gridViewByUser = true
                        root.gridView = !root.gridView
                    }
                }
                ToolTip.visible: gridMa.containsMouse
                ToolTip.text: root.gridView ? "切换为列表视图" : "切换为封面视图"
                ToolTip.delay: 400
            }
        }
    }

    // 搜索行：音乐库自己的搜索框（原独立搜索页并入这里）
    RowLayout {
        Layout.fillWidth: true
        Layout.preferredHeight: root.headerRowH
        spacing: 8

        // 玻璃胶囊搜索框 + 清除键
        Rectangle {
            id: searchBox
            z: 2   // 历史下拉盖在曲库内容之上
            Layout.fillWidth: true
            // 只写 fillWidth 会被右侧图标按钮压到 0：RowLayout 按 preferred 分配余量，
            // 而 TextField 空内容时 implicitWidth 约等于 0，手机竖屏就塌成一枚图标。
            Layout.preferredWidth: 300
            Layout.minimumWidth: 120
            Layout.maximumWidth: 520
            Layout.preferredHeight: root.headerRowH
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
                    placeholderText: "搜索歌曲、歌手或歌单"
                    color: root.cText
                    placeholderTextColor: cSub
                    font.pixelSize: 13
                    background: null
                    selectByMouse: true
                    // 输入法修复：关预测/联想和自动大写，否则候选词（拼音 co/com、
                    // 英文自动大写）会被塞进输入框，退格时像「删不掉」。
                    // 切勿加 ImhSensitiveData —— 国产 ROM（ColorOS/MIUI 等）会把它
                    // 当成密码/敏感输入框，强制弹「安全键盘」，无法输入中文。
                    inputMethodHints: Qt.ImhNoPredictiveText
                                      | Qt.ImhNoAutoUppercase
                    // 不用 text: root.searchKeyword 绑定：用户一输入就打断绑定，
                    // 后台重求值会把旧词写回、打断拼音组合输入。改为下面的显式同步。
                    Component.onCompleted: text = root.searchKeyword
                    // 用户手动编辑（含退格/删除）一旦开始，禁止后台写回
                    onTextEdited: root.keywordEditing = true
                    onTextChanged: {
                        if (text.length === 0 && root.searchKeyword.length > 0)
                            root.clearSearch()
                    }
                    // 一聚焦就进入搜索态：此时软键盘刚弹起，先把迷你播放条隐藏，
                    // 避免它和键盘挤压搜索结果（原先要敲出内容才隐藏，聚焦空档播放栏仍占位）
                    onActiveFocusChanged: {
                        if (activeFocus) {
                            root.keywordEditing = true
                            historyHideTimer.stop()
                            root.historyOpen = true
                        } else {
                            // 点历史项时先失焦，延时收起让 onClicked 先落地
                            historyHideTimer.start()
                        }
                        app.searchActive = activeFocus || root.searchMode
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

            // 聚焦搜索框弹出历史搜索：后端已有 search_history 表，这里只做展示和点选重搜。
            // 用 Popup 而不是 Rectangle + mapToItem：Popup 挂在窗口 overlay 上，天然盖过下一排
            // 胶囊；坐标相对 parent 计算，布局变化会自动跟随。手写 mapToItem 不会建立绑定
            // 依赖（布局稳定前的值会被永久缓存），手机上曾出现贴屏幕最左、偏下。
            Popup {
                id: histPopup
                parent: searchBox
                x: 14                    // 放大镜距胶囊左边 14dp，下拉左缘与它对齐
                y: searchBox.height + 2  // 贴着搜索框下边缘
                width: searchBox.width - 14
                height: Math.min(320, 38 + searchHistoryList.contentHeight)
                visible: root.historyOpen && searchHistoryList.count > 0
                padding: 0
                margins: 0
                focus: false
                closePolicy: Popup.NoAutoClose
                background: Rectangle {
                    radius: theme.radiusMd
                    color: theme.card
                    border.color: cLine
                    border.width: 1
                }
                contentItem: ColumnLayout {
                    spacing: 0

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 38
                        spacing: 8
                        Text {
                            Layout.fillWidth: true
                            Layout.leftMargin: 12
                            text: "历史搜索"
                            color: cSub
                            font.pixelSize: theme.fontSm
                        }
                        Text {
                            text: "清空"
                            color: cPrimary
                            font.pixelSize: theme.fontSm
                            rightPadding: 12
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    app.clearSearchHistory()
                                    root.historyOpen = false
                                }
                            }
                        }
                    }

                    ListView {
                        id: searchHistoryList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 2
                        // 显式引用 epoch：Q_INVOKABLE 调用不会自己建立依赖
                        model: app.searchHistoryEpoch >= 0 ? app.searchHistory() : []
                        delegate: Rectangle {
                            width: searchHistoryList.width
                            height: 38
                            color: histMa.containsMouse ? theme.cardHover : "transparent"
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                anchors.leftMargin: 12
                                anchors.right: parent.right
                                anchors.rightMargin: 12
                                text: modelData
                                color: cText
                                font.pixelSize: 13
                                elide: Text.ElideRight
                            }
                            MouseArea {
                                id: histMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.applySearchHistory(modelData)
                            }
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
            Layout.preferredWidth: root.headerRowH
            Layout.preferredHeight: root.headerRowH
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
        Layout.preferredHeight: root.headerRowH
        clip: true

            // 平台筛选收成一枚图标按钮：点开弹出「全部平台 + 各平台」，
            // 选中平台后按钮描边/图标取该平台主色，省掉原来整行胶囊占的高度
            Rectangle {
                id: platformBtn
                z: 2
                width: root.headerRowH
                height: root.headerRowH
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
                    // 显式定位到按钮正下方：默认 popup(item) 会顶到按钮左上角并向上回让，
                // Android 上页面头部贴近屏幕顶，菜单会被顶得过于靠上甚至越过头部
                onClicked: platformMenu.popup(platformBtn, 0, platformBtn.height + 6)
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
                                if (index > 0 && app.sourceIds[index - 1] === "qishui") {
                                    // 汽水音乐：切到发现页并直接展示「发现 → 模式探索」的
                                    // 场景模式/电台歌单（不再是其它平台榜单，也不弹模式菜单）。
                                    // 模式切换/场景筛选留在发现页顶部的下拉与场景胶囊里。
                                    root.tabIndex = 4
                                    root.discoveryTitle = ""
                                    root.openPlaylistTitle = ""
                                    root.qishuiShowSongs = false
                                    app.loadQishuiRecommend("", "")
                                } else {
                                    root.reloadSongs()
                                }
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
                            height: root.headerRowH
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
                            cacheBuffer: visible ? height * 2 : 100   // 预渲染两屏：默认只多备 100px，长列表滚动时委托反复重建
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
                            color: delegateHover.containsMouse ? theme.cardHover : cCard
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
                                        text: (model.count > 0 ? model.count + " 首 · " : "")
                                              + model.sourceName
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
                                    color: addQueueMa.pressed ? Qt.rgba(accent.r, accent.g, accent.b, 0.30)
                                         : (addQueueMa.containsMouse ? Qt.rgba(accent.r, accent.g, accent.b, 0.22)
                                                                     : Qt.rgba(accent.r, accent.g, accent.b, 0.12))
                                    Behavior on color { ColorAnimation { duration: theme.durationFast } }
                                    scale: addQueueMa.pressed ? 0.96 : 1.0
                                    Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                                    Text {
                                        anchors.centerIn: parent
                                        text: "加入队列"
                                        color: accent
                                        font.pixelSize: theme.fontSm
                                    }
                                    MouseArea {
                                        id: addQueueMa
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        Accessible.name: "将歌单加入播放队列"
                                        onClicked: app.addPlaylistToQueue(model.uid)
                                    }
                                }

                                // 收藏歌单点心形取消收藏，取消后直接从「我的歌单」移除；
                                // 本地自建歌单没有收藏概念，不给这个按钮
                                Item {
                                    Layout.preferredWidth: theme.touchMin
                                    Layout.preferredHeight: theme.touchMin
                                    Layout.alignment: Qt.AlignVCenter
                                    visible: model.source !== "local"
                                    property bool collected: app.playlistsEpoch >= 0
                                                             && app.isPlaylistCollected(model.uid)
                                    GlyphIcon {
                                        name: parent.collected ? "heartFill" : "heart"
                                        size: 18
                                        anchors.centerIn: parent
                                        color: parent.collected ? "#e64c4c" : cSub
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: parent.collected
                                                   ? app.uncollectPlaylist(model.uid)
                                                   : app.collectPlaylistByUid(model.uid)
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
                        cacheBuffer: 320
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        model: app.playlists
                        cellWidth: root.gridCellWidth
                        cellHeight: root.gridCellHeight
                        visible: root.gridView && root.openPlaylistTitle === ""

                        delegate: Item {
                            id: myPlCard
                            width: playlistGrid.cellWidth
                            height: playlistGrid.cellHeight
                            property string playlistUid: model.uid
                            property string playlistTitle: model.title
                            property bool collected: app.playlistsEpoch >= 0
                                                     && app.isPlaylistCollected(model.uid)
                            AlbumCard {
                                anchors.fill: parent
                                anchors.margins: 1
                                title: model.title
                                subtitle: (model.count > 0 ? model.count + " 首 · " : "")
                                          + model.sourceName
                                          + (model.owner.length > 0 ? " · " + model.owner : "")
                                cover: model.cover
                                badge: model.sourceName
                                accent: app.sourceColor(model.source)
                                onClicked: root.openPlaylist(model.title, model.uid)
                                onLongPressed: root.openPlaylistMenu(
                                    model.uid, model.title, myPlCard, posX, posY,
                                    function() { root.openPlaylist(model.title, model.uid) },
                                    model.source !== "local")
                            }
                            // 已收藏的外部歌单：点右上角红心直接取消收藏
                            Rectangle {
                                width: 34
                                height: 34
                                radius: 17
                                anchors.top: parent.top
                                anchors.right: parent.right
                                anchors.margins: 6
                                visible: model.source !== "local" && myPlCard.collected
                                color: myPlHeartMa.containsMouse
                                       ? Qt.rgba(0, 0, 0, 0.25) : Qt.rgba(0, 0, 0, 0.12)
                                GlyphIcon {
                                    name: "heartFill"
                                    size: 16
                                    anchors.centerIn: parent
                                    color: "#FB7185"
                                }
                                MouseArea {
                                    id: myPlHeartMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: app.uncollectPlaylist(model.uid)
                                }
                            }
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
                                Layout.preferredWidth: 86
                                Layout.preferredHeight: theme.touchMin
                                radius: height / 2
                                color: backPlaylistMa.containsMouse ? root.cChipHover : "transparent"
                                border.color: cLine
                                Behavior on color { ColorAnimation { duration: theme.durationFast } }
                                Text {
                                    anchors.centerIn: parent
                                    text: "← 返回歌单"
                                    color: cSub
                                    font.pixelSize: theme.fontSm
                                }
                                MouseArea {
                                    id: backPlaylistMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    Accessible.name: "返回歌单列表"
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
                                text: root.detailLoading && detailList.count === 0
                                      ? "载入中…"
                                      : (detailList.count > 0 ? detailList.count + " 首"
                                                              : "暂无曲目")
                                color: cSub
                                font.pixelSize: theme.fontSm
                            }
                        }

                        // 歌单详情歌曲列表：直接用 app.results（歌单内搜索框已移除，
                        // 找歌统一走顶部全局搜索），不再经过过滤代理。
                        ListView {
                            id: detailList
                            cacheBuffer: visible ? height * 2 : 100   // 预渲染两屏：默认只多备 100px，长列表滚动时委托反复重建
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
                                inQueue: model.inQueue
                                playing: model.uid === app.currentSongUid
                                showMv: model.source === "netease"
                                onPlayRequested: app.playResult(index)
                                onFavoriteRequested: app.toggleFavoriteResult(index)
                                onAddRequested: app.addResultToQueue(index)
                                onAddToPlaylistRequested: root.openPlaylistPicker(index)
                                onMvRequested: app.playMv(index)
                                onDownloadRequested: app.downloadResult(index)
                                showDownload: true
                            }
                        }

                        EmptyState {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 120
                            visible: detailList.count === 0 && !root.detailLoading
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
                            cacheBuffer: visible ? height * 2 : 100   // 预渲染两屏：默认只多备 100px，长列表滚动时委托反复重建
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
                            inQueue: model.inQueue
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
                        iconName: "heart"
                        title: "还没有收藏"
                        hint: "搜索歌曲后点行尾的心形按钮即可收藏"
                    }
                }

                // 播放历史
                Item {
                    ListView {
                        id: historyList
                            cacheBuffer: visible ? height * 2 : 100   // 预渲染两屏：默认只多备 100px，长列表滚动时委托反复重建
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
                            inQueue: model.inQueue
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
                        iconName: "history"
                        title: "还没有播放记录"
                        hint: "播放过的歌曲会在这里按平台聚合"
                    }
                }

                // 播放队列
                Item {
                    ListView {
                        id: queueList
                            cacheBuffer: visible ? height * 2 : 100   // 预渲染两屏：默认只多备 100px，长列表滚动时委托反复重建
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
                        color: clearQueueMa.pressed ? Qt.rgba(cPrimary.r, cPrimary.g, cPrimary.b, 0.30)
                             : (clearQueueMa.containsMouse ? Qt.rgba(cPrimary.r, cPrimary.g, cPrimary.b, 0.22) : cSoft)
                        Behavior on color { ColorAnimation { duration: theme.durationFast } }
                        Text {
                            anchors.centerIn: parent
                            text: "清空队列"
                            color: cPrimary
                            font.pixelSize: theme.fontSm
                        }
                        MouseArea {
                            id: clearQueueMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            Accessible.name: "清空播放队列"
                            onClicked: app.clearQueue()
                        }
                    }
                }
                // 发现榜单：各平台公开榜单 / 推荐歌单，完全不需要登录
                Item {
                    // 汽水音乐推荐面板：默认/熟悉/新鲜三种模式 + 场景子领域
                    Rectangle {
                        id: qishuiPanel
                        anchors.fill: parent
                        z: 2
                        visible: root.tabIndex === 4
                                 && root.selectedSourceId() === "qishui"
                                 && root.discoveryTitle === ""
                        color: "transparent"
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 8
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Text { text: "推荐模式"; color: cSub; font.pixelSize: theme.fontSm }
                                ComboBox {
                                    id: qishuiModeCombo
                                    Layout.preferredWidth: 140
                                    Layout.preferredHeight: theme.touchMin
                                    model: [
                                        { key: "default", label: "默认模式" },
                                        { key: "familiar", label: "熟悉模式" },
                                        { key: "fresh", label: "新鲜模式" }
                                    ]
                                    textRole: "label"
                                    currentIndex: 0
                                    onActivated: app.loadQishuiRecommend(model[index].key, "")
                                    background: Rectangle { color: cCard; radius: 6; border.color: cLine }
                                    contentItem: Text {
                                        text: qishuiModeCombo.displayText
                                        color: cText
                                        font.pixelSize: theme.fontSm
                                        verticalAlignment: Text.AlignVCenter
                                        leftPadding: 8
                                    }
                                }
                                Text { Layout.fillWidth: true }
                                Text { text: "场景"; color: cSub; font.pixelSize: theme.fontSm }
                            }
                            Flickable {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 34
                                contentWidth: qishuiCatRow.implicitWidth
                                clip: true
                                Row {
                                    id: qishuiCatRow
                                    spacing: 6
                                    Repeater {
                                        model: app.qishuiCategories()
                                        delegate: Rectangle {
                                            width: catText.implicitWidth + 20
                                            height: 30
                                            radius: 15
                                            color: cSoft
                                            border.color: cLine
                                            Text {
                                                id: catText
                                                anchors.centerIn: parent
                                                text: modelData
                                                color: cPrimary
                                                font.pixelSize: theme.fontSm
                                            }
                                            MouseArea {
                                                anchors.fill: parent
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: app.loadQishuiRecommend(
                                                    qishuiModeCombo.model[qishuiModeCombo.currentIndex].key,
                                                    modelData)
                                            }
                                        }
                                    }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text {
                                    text: root.qishuiShowSongs
                                          ? "推荐歌曲"
                                          : ("模式探索 · " + qishuiModeGrid.count + " 个歌单")
                                    color: cSub
                                    font.pixelSize: theme.fontSm
                                    elide: Text.ElideRight
                                }
                                Item { Layout.fillWidth: true }
                                Rectangle {
                                    implicitWidth: toggleText.implicitWidth + 20
                                    implicitHeight: 28
                                    radius: 14
                                    color: root.cChip
                                    border.color: cLine
                                    Text {
                                        id: toggleText
                                        anchors.centerIn: parent
                                        text: root.qishuiShowSongs ? "看歌单" : "看歌曲"
                                        color: cPrimary
                                        font.pixelSize: theme.fontSm
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.qishuiShowSongs = !root.qishuiShowSongs
                                    }
                                }
                            }
                            // 汽水「发现 → 模式探索」歌单墙：场景模式 + 电台 + 官方歌单，
                            // 竖向网格一直往上滑（不再是一排 92px 的横向卡片）。
                            GridView {
                                id: qishuiModeGrid
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                visible: !root.qishuiShowSongs
                                model: app.discovery
                                cellWidth: root.gridCellWidth
                                cellHeight: root.gridCellHeight
                                cacheBuffer: cellHeight * 2
                                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                                delegate: AlbumCard {
                                    id: modeCard
                                    width: qishuiModeGrid.cellWidth
                                    height: qishuiModeGrid.cellHeight
                                    title: model.title
                                    subtitle: (model.count > 0 ? model.count + " 首 · " : "")
                                              + model.sourceName
                                    cover: model.cover
                                    badge: model.sourceName
                                    accent: app.sourceColor(model.source)
                                    onClicked: root.openDiscovery(model.title, model.uid)
                                    onLongPressed: root.openPlaylistMenu(
                                        model.uid, model.title, modeCard, posX, posY,
                                        function() { root.openDiscovery(model.title, model.uid) }, true)
                                }
                                EmptyState {
                                    anchors.centerIn: parent
                                    visible: qishuiModeGrid.count === 0 && !app.discoveryLoading
                                    iconName: "music"
                                    title: "暂无模式歌单"
                                    hint: "检查网络后点「刷新榜单」重试"
                                }
                            }
                            ListView {
                                id: qishuiSongList
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                visible: root.qishuiShowSongs
                                spacing: 8
                                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                                model: app.results
                                delegate: SongRow {
                                    width: ListView.view.width
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
                                    inQueue: model.inQueue
                                    showMv: false
                                    onPlayRequested: app.playResult(index)
                                    onFavoriteRequested: app.toggleFavoriteResult(index)
                                    onAddRequested: app.addResultToQueue(index)
                                }
                                EmptyState {
                                    anchors.centerIn: parent
                                    visible: qishuiSongList.count === 0 && !app.discoveryLoading
                                    iconName: "music"
                                    title: "暂无推荐内容"
                                    hint: "检查网络后切换一次模式或场景再试"
                                }
                            }
                        }
                    }

                    GridView {
                        id: discoveryGrid
                        anchors.fill: parent
                        clip: true
                        cacheBuffer: 320
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        model: app.discovery
                        cellWidth: root.gridCellWidth
                        cellHeight: root.gridCellHeight
                        visible: root.gridView && root.discoveryTitle === "" && !qishuiPanel.visible

                        delegate: AlbumCard {
                            id: discCard
                            width: discoveryGrid.cellWidth
                            height: discoveryGrid.cellHeight
                            title: model.title
                            subtitle: (model.count > 0 ? model.count + " 首 · " : "")
                                      + model.sourceName
                            cover: model.cover
                            badge: model.sourceName
                            accent: app.sourceColor(model.source)
                            onClicked: root.openDiscovery(model.title, model.uid)
                            // 长按榜单封面：收藏到「我的歌单」/ 已收藏时取消
                            onLongPressed: root.openPlaylistMenu(
                                model.uid, model.title, discCard, posX, posY,
                                function() { root.openDiscovery(model.title, model.uid) }, true)
                        }
                    }

                    // 榜单列表视图：与「我的歌单」同一套行样式，跟着右上角的视图开关走
                    ListView {
                        id: discoveryList
                        anchors.fill: parent
                        clip: true
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        spacing: 8
                        model: app.discovery
                        visible: !root.gridView && root.discoveryTitle === "" && !qishuiPanel.visible

                        delegate: Rectangle {
                            width: discoveryList.width
                            height: Math.max(68, theme.touchMin + 20)
                            radius: 12
                            color: discoveryRowHover.containsMouse ? theme.cardHover : cCard
                            border.color: cLine

                            property color accent: app.sourceColor(model.source)

                            MouseArea {
                                id: discoveryRowHover
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.openDiscovery(model.title, model.uid)
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 10
                                spacing: 10

                                Rectangle {
                                    Layout.preferredWidth: 46
                                    Layout.preferredHeight: 46
                                    radius: 10
                                    clip: true
                                    color: Qt.rgba(accent.r, accent.g, accent.b, 0.12)
                                    Text {
                                        anchors.centerIn: parent
                                        text: "♪"
                                        color: accent
                                        font.pixelSize: 18
                                    }
                                    // 榜单都有封面，列表里也带一张小图；取不到时留上面的音符占位
                                    Image {
                                        anchors.fill: parent
                                        source: model.cover
                                        fillMode: Image.PreserveAspectCrop
                                        // 这里只显示 46px 的小图，没必要把整张榜单封面解进显存
                                        sourceSize.width: 96
                                        sourceSize.height: 96
                                        cache: true
                                        visible: status === Image.Ready
                                        asynchronous: true
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
                                        text: (model.count > 0 ? model.count + " 首 · " : "")
                                              + model.sourceName
                                        color: cSub
                                        font.pixelSize: theme.fontSm
                                        elide: Text.ElideRight
                                    }
                                }

                                Rectangle {
                                    Layout.preferredWidth: 76
                                    Layout.preferredHeight: theme.touchMin
                                    radius: height / 2
                                    // 整行已有 MouseArea，这里只借它的 hover/press 做反馈，
                                    // 不再叠一层 MouseArea 抢掉行的点击
                                    color: Qt.rgba(accent.r, accent.g, accent.b,
                                                   discoveryRowHover.pressed ? 0.30
                                                   : (discoveryRowHover.containsMouse ? 0.22 : 0.12))
                                    Behavior on color { ColorAnimation { duration: theme.durationFast } }
                                    Text {
                                        anchors.centerIn: parent
                                        text: "查看"
                                        color: accent
                                        font.pixelSize: theme.fontSm
                                    }
                                }
                            }
                        }
                    }

                    Row {
                        anchors.centerIn: parent
                        spacing: theme.spaceSm
                        visible: app.discoveryLoading && discoveryGrid.count === 0
                        BusyIndicator {
                            running: parent.visible
                            width: 28
                            height: 28
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "正在加载各平台公开榜单…"
                            color: cSub
                            font.pixelSize: theme.fontSm
                        }
                    }

                    EmptyState {
                        anchors.centerIn: parent
                        visible: !app.discoveryLoading && discoveryGrid.count === 0
                                 && root.discoveryTitle === ""
                        iconName: "music"
                        title: "还没有公开榜单"
                        hint: "点右上角「刷新榜单」重新拉取，或检查网络后重试"
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
                                Layout.preferredWidth: 86
                                Layout.preferredHeight: theme.touchMin
                                radius: height / 2
                                color: backDiscoveryMa.containsMouse ? root.cChipHover : "transparent"
                                border.color: cLine
                                Behavior on color { ColorAnimation { duration: theme.durationFast } }
                                Text {
                                    anchors.centerIn: parent
                                    text: "← 返回榜单"
                                    color: cSub
                                    font.pixelSize: theme.fontSm
                                }
                                MouseArea {
                                    id: backDiscoveryMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    Accessible.name: "返回榜单列表"
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
                                text: root.detailLoading && discoveryDetail.count === 0
                                      ? "载入中…"
                                      : (discoveryDetail.count > 0 ? discoveryDetail.count + " 首"
                                                                   : "暂无曲目")
                                color: cSub
                                font.pixelSize: theme.fontSm
                            }
                            Rectangle {
                                Layout.preferredWidth: 72
                                Layout.preferredHeight: theme.touchMin
                                radius: height / 2
                                color: discoveryPlayAllMa.pressed ? Qt.rgba(cPrimary.r, cPrimary.g, cPrimary.b, 0.30)
                                     : (discoveryPlayAllMa.containsMouse ? Qt.rgba(cPrimary.r, cPrimary.g, cPrimary.b, 0.22) : cSoft)
                                visible: discoveryDetail.count > 0
                                Behavior on color { ColorAnimation { duration: theme.durationFast } }
                                Text {
                                    anchors.centerIn: parent
                                    text: "播放全部"
                                    color: cPrimary
                                    font.pixelSize: theme.fontSm
                                }
                                MouseArea {
                                    id: discoveryPlayAllMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    Accessible.name: "播放全部"
                                    onClicked: app.playResult(0)
                                }
                            }
                        }

                        ListView {
                            id: discoveryDetail
                                cacheBuffer: visible ? height * 2 : 100   // 预渲染两屏：默认只多备 100px，长列表滚动时委托反复重建
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
                                inQueue: model.inQueue
                                showMv: model.source === "netease"
                                onPlayRequested: app.playResult(index)
                                onFavoriteRequested: app.toggleFavoriteResult(index)
                                onAddRequested: app.addResultToQueue(index)
                                onAddToPlaylistRequested: root.openPlaylistPicker(index)
                                onMvRequested: app.playMv(index)
                                onDownloadRequested: app.downloadResult(index)
                                showDownload: true
                            }
                        }

                        EmptyState {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 120
                            visible: discoveryDetail.count === 0 && !root.detailLoading
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
                            cacheBuffer: 320
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
                                    model: ["歌单", "歌曲"]
                                    delegate: Rectangle {
                                        property bool active: index === 0 ? !searchView.songMode
                                                                          : searchView.songMode
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
                                            onClicked: {
                                                const t = index === 0 ? "songlist" : "song"
                                                if (t === app.searchType) return
                                                app.setSearchType(t)
                                                // 切换类型后用当前关键词重搜：否则列表停在上一类型的旧结果
                                                if (keyword.text.trim().length > 0) root.runSearch()
                                            }
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
                                cacheBuffer: visible ? height * 2 : 100
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
                                    inQueue: model.inQueue
                                    showMv: model.source === "netease"
                                    showDownload: true
                                    onPlayRequested: app.playResult(index)
                                    onFavoriteRequested: app.toggleFavoriteResult(index)
                                    onAddRequested: app.addResultToQueue(index)
                                    onAddToPlaylistRequested: root.openPlaylistPicker(index)
                                    onMvRequested: app.playMv(index)
                                    onDownloadRequested: app.downloadResult(index)
                                }
                            }

                            // 歌曲（封面卡片墙）
                            GridView {
                                id: searchSongGrid
                                anchors.fill: parent
                                clip: true
                                cacheBuffer: 320
                                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                                model: app.results
                                cellWidth: root.gridCellWidth
                                cellHeight: root.gridCellHeight
                                visible: searchView.songMode && root.gridView && count > 0
                                // 歌曲卡片同样带收藏红心：列表/卡片墙两种视图都有入口
                                delegate: Item {
                                    width: searchSongGrid.cellWidth
                                    height: searchSongGrid.cellHeight
                                    AlbumCard {
                                        anchors.fill: parent
                                        anchors.margins: 1
                                        title: model.title
                                        subtitle: model.artist
                                                  + (model.album.length > 0 ? " · " + model.album : "")
                                        cover: model.cover
                                        badge: model.sourceName
                                        accent: app.sourceColor(model.source)
                                        onClicked: app.playResult(index)
                                    }
                                    Rectangle {
                                        width: 34
                                        height: 34
                                        radius: 17
                                        anchors.top: parent.top
                                        anchors.right: parent.right
                                        anchors.margins: 6
                                        color: songHeartMa.containsMouse
                                               ? Qt.rgba(0, 0, 0, 0.25) : Qt.rgba(0, 0, 0, 0.12)
                                        GlyphIcon {
                                            name: model.favorite ? "heartFill" : "heart"
                                            size: 16
                                            anchors.centerIn: parent
                                            color: model.favorite ? "#FB7185" : "#FFFFFF"
                                        }
                                        MouseArea {
                                            id: songHeartMa
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: app.toggleFavoriteResult(index)
                                        }
                                    }
                                }
                            }

                            // 歌单（列表）
                            ListView {
                                id: searchPlaylistList
                                cacheBuffer: visible ? height * 2 : 100
                                anchors.fill: parent
                                clip: true
                                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                                spacing: 8
                                model: app.searchPlaylists
                                visible: !searchView.songMode && !root.gridView && count > 0
                                delegate: Rectangle {
                                    id: plDelegate
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

                                    // 收藏歌单到「我的歌单」：lx-music-mobile 式，
                                    // 点按钮不触发整行打开；已收藏的显示实心
                                    property bool collected: app.playlistsEpoch >= 0
                                                             && app.isPlaylistCollected(model.uid)

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
                                                text: (model.count > 0 ? model.count + " 首 · " : "")
                                                      + model.sourceName
                                                      + (model.owner.length > 0 ? " · " + model.owner : "")
                                                color: cSub
                                                font.pixelSize: theme.fontSm
                                                elide: Text.ElideRight
                                            }
                                        }
                                        // 行尾收藏按钮：必须放在 RowLayout 里
                                        // （Layout 附加属性只在 Layout 内生效，放顶层会堆到左上角）
                                        Item {
                                            Layout.preferredWidth: theme.touchMin
                                            Layout.preferredHeight: theme.touchMin
                                            Layout.alignment: Qt.AlignVCenter
                                            GlyphIcon {
                                                name: plDelegate.collected ? "heartFill" : "heart"
                                                size: 18
                                                anchors.centerIn: parent
                                                color: plDelegate.collected ? "#e64c4c" : root.cSub
                                            }
                                            MouseArea {
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: app.collectSearchPlaylist(index)
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
                                cacheBuffer: 320
                                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                                model: app.searchPlaylists
                                cellWidth: root.gridCellWidth
                                cellHeight: root.gridCellHeight
                                visible: !searchView.songMode && root.gridView && count > 0
                                // 包一层 Item 挂收藏角标：AlbumCard 只有 clicked 信号，
                                // 右上角心形不挡卡片点击（MouseArea 在卡片上层，只占角上 34dp）
                                delegate: Item {
                                    id: searchPlCard
                                    width: searchPlaylistGrid.cellWidth
                                    height: searchPlaylistGrid.cellHeight
                                    property bool collected: app.playlistsEpoch >= 0
                                                             && app.isPlaylistCollected(model.uid)
                                    AlbumCard {
                                        anchors.fill: parent
                                        anchors.margins: 1
                                        title: model.title
                                        subtitle: (model.count > 0 ? model.count + " 首 · " : "")
                                                  + model.sourceName
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
                                        onLongPressed: root.openPlaylistMenu(
                                            model.uid, model.title, searchPlCard, posX, posY,
                                            function() {
                                                root.exitSearch()
                                                root.openPlaylistTitle = model.title
                                                root.tabIndex = 0
                                                app.loadPlaylist(model.uid)
                                            }, true)
                                    }
                                    Rectangle {
                                        width: 34
                                        height: 34
                                        radius: 17
                                        anchors.top: parent.top
                                        anchors.right: parent.right
                                        anchors.margins: 6
                                        color: heartMa.containsMouse
                                               ? Qt.rgba(0, 0, 0, 0.25) : Qt.rgba(0, 0, 0, 0.12)
                                        GlyphIcon {
                                            name: parent.parent.collected ? "heartFill" : "heart"
                                            size: 16
                                            anchors.centerIn: parent
                                            color: parent.parent.collected ? "#FB7185" : "#FFFFFF"
                                        }
                                        MouseArea {
                                            id: heartMa
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: app.collectSearchPlaylist(index)
                                        }
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

    // ---- 「加入歌单」弹层：列出我的歌单，支持就地新建 ----
    Popup {
        id: pickerPopup
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(420, root.width - 48)
        height: Math.min(500, root.height - 96)
        modal: true
        dim: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        background: Rectangle {
            color: theme.card
            border.color: theme.line
            border.width: 1
            radius: theme.radiusLg
        }
        contentItem: ColumnLayout {
            spacing: theme.spaceSm

            Label {
                text: root.pickerSongTitle.length > 0
                      ? "把「" + root.pickerSongTitle + "」加入歌单"
                      : "加入歌单"
                font.pixelSize: theme.fontLg
                font.weight: Font.DemiBold
                color: theme.text
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            ListView {
                id: pickerList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 4
                model: app.playlists
                delegate: Rectangle {
                    width: pickerList.width
                    height: Math.max(52, theme.touchMin)
                    radius: 10
                    color: pickerMa.pressed ? theme.cardHover : "transparent"

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 10

                        Rectangle {
                            Layout.preferredWidth: 38
                            Layout.preferredHeight: 38
                            radius: 9
                            clip: true
                            color: Qt.rgba(app.sourceColor(model.source).r,
                                           app.sourceColor(model.source).g,
                                           app.sourceColor(model.source).b, 0.12)
                            Text {
                                anchors.centerIn: parent
                                visible: model.cover === ""
                                text: "♪"
                                color: app.sourceColor(model.source)
                                font.pixelSize: 15
                            }
                            Image {
                                anchors.fill: parent
                                visible: model.cover !== ""
                                source: model.cover
                                // 38x38 的小方块，按原图解码纯属浪费
                                sourceSize.width: 96
                                sourceSize.height: 96
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                            }
                        }
                        Text {
                            text: model.title
                            color: theme.text
                            font.pixelSize: theme.fontMd
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Text {
                            text: model.count > 0 ? model.count + " 首" : ""
                            color: theme.subText
                            font.pixelSize: theme.fontSm
                        }
                    }
                    MouseArea {
                        id: pickerMa
                        anchors.fill: parent
                        onClicked: {
                            app.addResultToPlaylist(root.pickerSongIndex, model.uid)
                            pickerPopup.close()
                        }
                    }
                }

                // 没有任何歌单时的空态
                Text {
                    anchors.centerIn: parent
                    visible: pickerList.count === 0
                    text: "还没有歌单，在下方新建一个吧"
                    color: theme.subText
                    font.pixelSize: theme.fontSm
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: theme.spaceSm

                TextField {
                    id: pickerNewName
                    Layout.fillWidth: true
                    placeholderText: "新建歌单名称…"
                    font.pixelSize: theme.fontMd
                    color: theme.text
                    selectByMouse: true
                    onAccepted: pickerCreateBtn.clicked()
                }
                Button {
                    id: pickerCreateBtn
                    text: "新建"
                    highlighted: true
                    onClicked: {
                        const uid = app.createMyPlaylist(pickerNewName.text)
                        if (uid !== "") {
                            app.addResultToPlaylist(root.pickerSongIndex, uid)
                            pickerPopup.close()
                        }
                    }
                }
            }
        }
    }

    // 点击搜索框以外的任何位置：立即收起历史下拉并让输入框失焦（软键盘同步收起）。
    // QML 里点到非输入类区域不会自动转移焦点，光靠 onActiveFocusChanged 收不起来；
    // 用 TapHandler 又会被子项 MouseArea 的独占 grab 取消（实测点歌曲行收不起来），
    // 所以历史展开时铺一层透明遮罩：点在搜索框内保持原样，点别处收起并吞掉这次点击。
    MouseArea {
        anchors.fill: parent
        z: 1000
        visible: root.historyOpen
        enabled: root.historyOpen
        preventStealing: true
        onClicked: function (mouse) {
            const p = searchBox.mapToItem(root, 0, 0)
            if (mouse.x >= p.x && mouse.x <= p.x + searchBox.width
                    && mouse.y >= p.y && mouse.y <= p.y + searchBox.height) {
                // 点在输入框内要让事件继续传给 TextField，否则光标定位/选词全被吞掉
                mouse.accepted = false
                return
            }
            root.historyOpen = false
            keyword.focus = false
        }
    }
}           // root
