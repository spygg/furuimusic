import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// 播放页 —— 版式参照 AlgerMusicPlayer 的沉浸式播放页：
//   1) 背景不是"深色遮罩"，而是封面自身重度模糊后的颜色，并且跟随主题走：
//      浅色主题刷一层白纱（整页是封面的淡彩），深色主题刷一层黑纱 —— 这样侧栏/播放栏
//      的底色和播放页不再一刀两断，浅色皮肤下也不会出现"白底怼着一张唱片"的突兀感；
//   2) 宽屏左列是唯一焦点：封面 → 标题/艺人 → 进度 → 控件 → 音量，垂直居中；
//      移动竖屏单独重排：封面(上) → 歌词(中，吃满剩余空间) → 进度/控件(贴底)，
//      音量收成一个按钮，点开垂直滑块（Android 有实体音量键，不必常驻横条）；
//   3) 歌词不套玻璃卡片，直接浮在背景上，靠遮罩上下渐隐，当前行放大变实（同参考项目）；
//   4) 切歌时封面交叉淡入，进页时整块内容上浮淡入，按钮按下有回弹。
// 桌面外壳隐藏底部 PlayBar 后由本页承担全部控件；移动端同一套内容竖排。
Item {
    id: root
    clip: true

    // 顶部按钮回调（由外壳接线：关闭播放页 / 打开设置 / 打开队列）
    signal requestClose()
    signal requestSettings()
    signal requestQueue()
    signal requestComments()

    // 兼容旧调用点：播放页现在始终自带控件，这个属性只为老外壳不报错
    property bool showControls: true
    // 分屏标记由外壳传入。分屏即使被拉到接近正方形，也保持横分屏的「封面/歌词二选一」
    // 版式，不因为底栏收起后内容区变高就误判成竖屏（竖屏才在封面下留歌词速览）。
    property bool inMultiWindow: false

    // 竖屏音量弹层：横条收成按钮，点一下才展开垂直滑块
    property bool volumePopupOpen: false

    // 宽屏/横屏版式判定 —— 由「自身可用宽度 + 设备方向」共同决定，保证车机三种形态自适应：
    //   1) 横屏(含车机整屏 / 横屏分屏)：方向明确，只要宽度够放下「左封面+右歌词」双栏即双栏，
    //      阈值 520（左列下限 300 + 间距 + 歌词最小可用宽度）；极窄分屏(<520)退回单栏，避免控件被挤；
    //   2) 竖屏(含大平板竖屏)：需更宽(>=760)才双栏，否则保持单栏(封面在上/歌词居中/控件贴底)，
    //      这样竖屏与横屏呈现明显不同的自适应布局，而非把竖屏布局强行拉伸。
    // 用窗口自身的形状而不是 Screen.orientation：车机分屏时设备仍是横屏，
    // 但窗口可能只剩一个竖长条，按屏幕方向会错判成横屏双栏。
    readonly property bool wide: root.width >= root.height
                                 ? (root.height < 420 ? root.width >= 520
                                                      : root.width >= 760)
                                 : root.width >= 760
    // 中等宽度的横屏窗口（车机分屏：约 560×500）：双栏会把封面和歌词各挤成一条，
    // 改走单栏 pager，一次只显示封面或歌词（与竖屏一致，左右滑动切换）。
    // 矮横屏(<420)没有可用单栏高度，仍保留双栏，否则 compactTiny 会把封面/歌词全部隐藏。
    readonly property bool splitSingle: !root.wide
                                        && (root.inMultiWindow || root.width >= root.height)
    // 宽窄形态切换时收起音量弹层：两个形态各有一个弹层实例，切换后旧实例已隐藏，
    // 不复位会让新形态的弹层莫名保持展开
    onWideChanged: volumePopupOpen = false
    // 竖屏分屏/窄矮窗口：页面高度只剩 250dp 上下时，封面+歌词会把进度和控件顶出可视区
    // （底部标签栏再把它们盖住）。这种高度下让封面/歌词让位，保证播放控件始终可点。
    readonly property bool compactTiny: !root.wide && root.height < 420
    // 再往下只剩 250dp 上下的竖屏分屏 / 车机窄条：顶部让位 72 + 元信息 ~50 + 进度 36 +
    // 控件 62 + 间距与边距 30 已经超过可用高度，ColumnLayout 会把控件压到首选高度以下，
    // 实测主按钮被底部标签栏切掉一半。这里按高度再收一档，保证主按钮始终完整可点。
    readonly property bool ultraTiny: !root.wide && root.height < 340
    // 页面自带封面氛围底。移动端外壳已在根层铺了一层覆盖整窗的 Backdrop（含侧栏区域），
    // 这里必须关掉，否则侧栏右侧会出现一条接缝、左右封面还可能错位。
    // 设置页「封面作为歌词背景」关掉后整页统一用纯主题底色。
    property bool ownBackdrop: theme.coverLyricBackground
    readonly property bool isDark: theme.dark

    // 本页专用文字色：浅色主题用深墨（压在白纱背景上），深色主题用浅墨
    readonly property color ink: isDark ? "#F3F5FA" : "#15171C"
    readonly property color inkSub: isDark ? Qt.rgba(1, 1, 1, 0.62) : Qt.rgba(0, 0, 0, 0.55)
    readonly property color inkFaint: isDark ? Qt.rgba(1, 1, 1, 0.16) : Qt.rgba(0, 0, 0, 0.11)
    readonly property color chipBg: isDark ? Qt.rgba(1, 1, 1, 0.14) : Qt.rgba(1, 1, 1, 0.62)
    readonly property color chipBgHover: isDark ? Qt.rgba(1, 1, 1, 0.22) : Qt.rgba(1, 1, 1, 0.92)
    readonly property color cPrimary: theme.accent

    // 歌词对齐：auto 沿用「宽屏左 / 竖屏居中」，其余由设置页强制指定。
    // 扫光层与译文行都跟着它走，避免亮字与底字错位。
    readonly property int lyricHAlign: {
        if (theme.lyricAlign === "left") return Text.AlignLeft
        if (theme.lyricAlign === "center") return Text.AlignHCenter
        if (theme.lyricAlign === "right") return Text.AlignRight
        return root.wide ? Text.AlignLeft : Text.AlignHCenter
    }

    readonly property string artSource: app.currentCover.length > 0
                                        ? app.currentCover
                                        : "qrc:/qml/art/default_cover.jpg"
    readonly property bool hasSong: app.currentTitle.length > 0

    // ---- 进场：只让封面「弹」一下 ----
    // 刻意不用「整页从 opacity 0 淡入」：一旦动画被打断（离屏截图、窗口刚显示等场景），
    // 整页会永久停在 0 而变成空白。这里只动封面缩放，最坏情况也只是小 3%，不会白屏。
    property real coverPop: 1
    function playEntrance() {
        coverPopAnim.restart()
    }
    NumberAnimation {
        id: coverPopAnim
        target: root
        property: "coverPop"
        from: 0.96
        to: 1
        duration: 380
        easing.type: Easing.OutCubic
    }

    function mmss(ms) {
        if (isNaN(ms) || ms < 0)
            ms = 0
        var t = Math.floor(ms / 1000)
        var m = Math.floor(t / 60)
        var s = t % 60
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    function modeIcon() {
        if (app.playMode === 0) return "sequential"
        if (app.playMode === 2) return "repeatOne"
        if (app.playMode === 3) return "shuffle"
        return "repeat"
    }

    // ================= 背景：封面模糊 + 主题纱 =================
    // 不用 QtGraphicalEffects（Qt 6 的旧模块名已不可用，Qt5Compat 也需要额外部署）。
    // 这里把封面以极低分辨率（96px）解码再拉伸铺满：等价于一次很重的模糊，
    // 但没有任何逐帧卷积开销，且 Qt 5 / Qt 6 行为一致。
    Item {
        id: bgLayer
        anchors.fill: parent
        scale: 1.06
        visible: root.ownBackdrop

        Image {
            id: bgOld
            anchors.fill: parent
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            sourceSize.width: 96
            sourceSize.height: 96
            smooth: true
            mipmap: true
            opacity: 0
        }
        Image {
            id: bgNew
            anchors.fill: parent
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            sourceSize.width: 96
            sourceSize.height: 96
            smooth: true
            mipmap: true
        }

        property string currentArt: ""
        function setArt(src) {
            if (!src || src === currentArt)
                return
            if (currentArt.length === 0) {
                currentArt = src
                bgNew.source = src
                return
            }
            bgOld.source = bgNew.source
            bgOld.opacity = 1
            bgNew.source = src
            currentArt = src
            artFade.restart()
        }
        NumberAnimation {
            id: artFade
            target: bgOld
            property: "opacity"
            to: 0
            duration: 520
            easing.type: Easing.InOutQuad
        }
    }
    Component.onCompleted: {
        if (visible)
            root.playEntrance()
        if (root.ownBackdrop)
            bgLayer.setArt(root.artSource)
    }
    onArtSourceChanged: {
        if (root.ownBackdrop)
            bgLayer.setArt(root.artSource)
    }
    onOwnBackdropChanged: {
        if (root.ownBackdrop)
            bgLayer.setArt(root.artSource)
    }

    // 主题纱：浅色 = 白纱（页面变成封面的淡彩），深色 = 黑纱
    Rectangle {
        anchors.fill: parent
        visible: root.ownBackdrop
        gradient: Gradient {
            GradientStop {
                position: 0.0
                color: root.isDark ? Qt.rgba(0.06, 0.06, 0.08, 0.52) : Qt.rgba(1, 1, 1, 0.40)
            }
            GradientStop {
                position: 0.55
                color: root.isDark ? Qt.rgba(0.05, 0.05, 0.07, 0.60) : Qt.rgba(1, 1, 1, 0.52)
            }
            GradientStop {
                position: 1.0
                color: root.isDark ? Qt.rgba(0.04, 0.04, 0.06, 0.70) : Qt.rgba(1, 1, 1, 0.64)
            }
        }
    }

    // ================= 内容（统一进场） =================
    Item {
        id: contentRoot
        anchors.fill: parent
        opacity: 1

        // ---------- 顶部按钮 ----------
        // z 必须高于下面的排版层：宽屏/窄屏排版都是本层里的 anchors.fill 兄弟项、
        // 声明在后，会把这两个按钮整块盖住——按钮看得见，点下去却全被排版层
        // （尤其左侧那个 Flickable）吃掉，表现为「播放页的关闭/队列/设置全部失灵」
        Item {
            id: topLeft
            z: 30
            anchors.left: parent.left
            anchors.top: parent.top
            // 只收顶部：安全区已由外壳内缩，22dp 的顶部下移让浅色背景下顶部显空（旧值 22）
            anchors.leftMargin: 22
            anchors.topMargin: 10
            width: theme.touchMin
            height: theme.touchMin

            Rectangle {
                anchors.fill: parent
                radius: 10
                color: closeArea.containsMouse ? root.chipBgHover : root.chipBg
                Behavior on color { ColorAnimation { duration: 160 } }
                GlyphIcon {
                    anchors.centerIn: parent
                    name: "chevronDown"
                    size: 17
                    color: root.ink
                }
            }
            MouseArea {
                id: closeArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.requestClose()
            }
        }

        Row {
            z: 30
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.rightMargin: 22
            anchors.topMargin: 10
            spacing: 8

            // 队列 / MV / 设置：与参考项目右上角一样是小的半透明方按钮
            Repeater {
                model: [
                    { icon: "comment", tip: "评论" },
                    { icon: "list", tip: "播放队列" },
                    { icon: "video", tip: "播放 MV" },
                    { icon: "settings", tip: "设置" }
                ]
                delegate: Item {
                    width: theme.touchMin
                    height: theme.touchMin
                    Rectangle {
                        anchors.fill: parent
                        radius: 10
                        color: tipArea.containsMouse ? root.chipBgHover : root.chipBg
                        Behavior on color { ColorAnimation { duration: 160 } }
                        GlyphIcon {
                            anchors.centerIn: parent
                            name: modelData.icon
                            size: 16
                            color: root.ink
                        }
                    }
                    MouseArea {
                        id: tipArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (index === 0) root.requestComments()
                            else if (index === 1) root.requestQueue()
                            else if (index === 2) app.playMvCurrent()
                            else root.requestSettings()
                        }
                    }
                }
            }
        }

        // ---------- 宽屏：左封面右歌词 ----------
        Item {
            id: widePane
            anchors.fill: parent
            // 横屏手机宽度只有 700dp 出头，左右各 32 会把双栏再挤掉 64；
            // 大屏（平板/车机）反而需要更宽的留白，按宽度分档。
            anchors.leftMargin: Math.max(16, Math.min(56, root.width * 0.045))
            anchors.rightMargin: anchors.leftMargin
            // 顶部避让：悬浮的「返回」按钮 topMargin 10 + 48 触控高 = 底部 58dp，
            // 双栏排版原来只留 26，封面上沿会压到返回按钮下方（窄一点的车机/横屏手机
            // 尤其明显，封面左边缘正好穿过按钮）。统一留到 62dp，与紧凑版式一致，
            // 顺带让右栏歌词也不再贴住右上角按钮。
            anchors.topMargin: root.height < 420 ? 14 : 62
            anchors.bottomMargin: root.height < 420 ? 8 : 14
            visible: root.wide

            // 封面边长：由「左列宽度」和「扣除信息区后的剩余高度」共同决定，避免小窗口把按钮挤出去。
            // 预留量必须按实际内容高度反推——以前写死 206，但「标题+艺人+两枚角标」实测约 89，
            // 加进度 34、控件 86 与两处间距共 229，车机横屏（可用高 ~500）时整列溢出，
            // 底部主播放按钮被裁掉一截。改成读 Loader 的真实高度，加载前后都有保守兜底。
            readonly property bool shortRow: height < 520
            readonly property real metaH: wideMetaLoader.item ? wideMetaLoader.item.implicitHeight : (widePane.shortRow ? 89 : 89)
            readonly property real ctrlH: wideControlsLoader.item ? wideControlsLoader.item.implicitHeight : (widePane.shortRow ? 72 : 86)
            readonly property real reserved: widePane.metaH + (widePane.shortRow ? 6 : 10) + 34
                                             + (widePane.shortRow ? 6 : 10) + widePane.ctrlH + 10
            // 短屏（手机横屏 336dp 高）里封面再占 84 会把最后的控件行挤出可视区，
            // 下限收到 60；其余高度按可用高度反推，保证整列放得下。
            readonly property real coverSide: Math.max(widePane.shortRow ? 60 : 84,
                                                       Math.min(width * 0.40, height - widePane.reserved, 390))

            RowLayout {
                anchors.fill: parent
                spacing: Math.max(32, Math.min(72, widePane.width * 0.06))

                Item {
                    id: wideLeft
                    // 左列至少留出控件行所需宽度(~296)，否则窄横屏下播放控件会被压成竖线
                    Layout.preferredWidth: Math.max(300, Math.min(widePane.coverSide + 24, widePane.width * 0.46))
                    Layout.fillHeight: true

                    // 横屏（尤其手机横屏高度有限）左侧内容可能超出可用高度，
                    // 用 Flickable 承载，放得下时垂直居中，放不下时可滚动 + 显示滚动条
                    Flickable {
                        id: leftFlick
                        anchors.fill: parent
                        contentWidth: width
                        contentHeight: leftCol.height
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                        Column {
                            id: leftCol
                            width: parent.width
                            y: (leftFlick.height > height) ? (leftFlick.height - height) / 2 : 0

                            Item {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: widePane.coverSide
                                height: widePane.coverSide
                                Loader { anchors.fill: parent; sourceComponent: coverComp }
                            }

                            Loader {
                                id: wideMetaLoader
                                width: parent.width
                                // 固定 56/72 装不下「标题 + 艺人 + 两枚角标」三行（实测约 89），
                                // 多出来的部分会直接压在下面的歌词/进度上；按内容实际高度给位
                                height: item ? item.implicitHeight : (widePane.shortRow ? 56 : 72)
                                sourceComponent: metaComp
                            }

                            Item { width: 1; height: widePane.shortRow ? 6 : 10 }

                            Loader {
                                width: parent.width
                                height: 34
                                sourceComponent: progressComp
                            }

                            Item { width: 1; height: widePane.shortRow ? 6 : 10 }

                            Loader {
                                id: wideControlsLoader
                                width: parent.width
                                // 控件行高度按内容实际给位；写死会把行挤出 Loader 被 Flickable 裁掉
                                height: item ? item.implicitHeight : (widePane.shortRow ? 72 : 86)
                                sourceComponent: controlsComp
                            }
                        }
                    }
                }

                Loader {
                    active: root.wide
                    sourceComponent: lyricComp
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }
            }

            // 弹层打开时点任意空白处收起（弹层自身 z 更高，不受影响）
            MouseArea {
                anchors.fill: parent
                z: 4
                visible: root.volumePopupOpen
                onClicked: root.volumePopupOpen = false
            }

            // 宽屏垂直音量弹层：与竖屏同一交互（点音量按钮弹出），
            // 浮在左列控件行正上方、水平居中于左列
            Loader {
                active: root.wide
                width: theme.touchMin
                height: 160
                z: 5
                sourceComponent: volPopupComp
                x: wideLeft.x + (wideLeft.width - width) / 2
                y: (leftCol.y - leftFlick.contentY + wideControlsLoader.y) - height - 8
            }
        }

        // ---------- 窄屏 / 移动竖屏：参考 lx-music-mobile 播放页 ----------
        // 封面页 ↔ 歌词页 SwipeView 左右滑动切换 + 页点指示器；
        // 标题/艺人、进度、控件、音量按钮固定贴底，不做整页滚动
        Item {
            id: compactPane
            anchors.fill: parent
            anchors.margins: root.ultraTiny ? 10 : (root.compactTiny ? 12 : 18)
            // 顶部留白：标题（元信息）落在工具条按钮下方一点即可。
            // 工具条 button topMargin 10 + 48 触控高 = 底部约 58dp，标题 62dp 留 4dp 间隙。
            // 旧值 72/74 是按工具条 margins 22 算的，工具条收上去后这里会空出一大块白
            // （浅色背景尤其明显）；现在同步收下来，标题仍不与「队列/视频/音效」重叠。
            // 矮窗（竖屏分屏、手机横屏单栏）再省 2dp。
            anchors.topMargin: (root.ultraTiny || root.compactTiny) ? 60 : 62
            visible: !root.wide

            ColumnLayout {
                id: compactCol
                anchors.fill: parent
                spacing: root.ultraTiny ? 4 : 6

                Loader {
                    Layout.fillWidth: true
                    // 同宽屏那处：三行元信息约 89 高，写死 64 会让角标压在歌词第一行上
                    Layout.preferredHeight: item ? Math.min(item.implicitHeight, root.ultraTiny ? 30 : 9999)
                                                : (root.ultraTiny ? 30 : 80)
                    sourceComponent: metaComp
                }

                // 极矮窗口里封面/歌词让位后，用这个占位把进度与控件压到底部
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: root.compactTiny
                }

                // 封面 ↔ 歌词：左右滑动切换（lx 的 PagerView 交互）
                SwipeView {
                    id: pager
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    // 最小高度不能给太大：竖屏分屏（约 400dp 高）里固定的元信息/进度/控件
                    // 已占满大部分高度，再把这里卡到 140 会把整行控件顶到可视区外。
                    Layout.minimumHeight: 80
                    visible: !root.compactTiny
                    clip: true

                    // 第 1 页：封面保持正方形，整体略上移，封面下方显示当前一两句歌词，
                    // 填掉高屏上封面与页底之间的空白（封面边长封顶 300）
                    Item {
                        id: compactCoverPage
                        // 封面边长：不超过页宽 92%、页高扣除歌词行，封顶 300，保持正方形。
                        // 分屏单栏没有下方歌词 peek，把预留高度还给封面。
                        // 竖屏封面下方显示「上一句/当前句/下一句」三行歌词，预留 106dp
                        //（旧版单行只留 64dp，封面下方大片空白浪费了）。
                        readonly property real peekH: root.splitSingle ? 0 : 106
                        readonly property real coverSide: Math.max(0, Math.min(parent.width * 0.92,
                                                                               parent.height - peekH, 300))
                        Item {
                            id: compactCoverSlot
                            anchors.horizontalCenter: parent.horizontalCenter
                            // 封面 + 歌词作为一组在页内垂直居中，避免封面顶天、歌词贴底
                            anchors.top: parent.top
                            anchors.topMargin: Math.max(0, (compactCoverPage.height
                                                            - compactCoverPage.coverSide
                                                            - compactCoverPage.peekH) / 2)
                            width: compactCoverPage.coverSide
                            height: compactCoverPage.coverSide
                            Loader { anchors.fill: parent; sourceComponent: coverComp }
                        }
                        // 封面下方歌词速览：跟随播放进度显示「上一句 / 当前句 / 下一句」三行。
                        // 数据直接取 app.lyricLines + app.currentLyricIndex（QML 侧索引），
                        // 当前行加粗高亮、上下行压暗渐隐；无歌词时留白不占位。
                        // 分屏单栏时封面页不再附歌词行，与歌词页真正「二选一」。
                        Item {
                            id: lyricPeek
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.top: compactCoverSlot.bottom
                            anchors.topMargin: 6
                            width: parent.width * 0.88
                            height: 96
                            visible: !root.splitSingle
                                     && (app.lyricsTimed || app.lyricsText.length > 0)

                            // 当前行索引，未定位（-1）时上/下行留空、只显示一行
                            readonly property int idx: app.currentLyricIndex
                            function peekLine(i) {
                                return (i >= 0 && i < app.lyricLines.length)
                                       ? app.lyricLines[i] : ""
                            }
                            Column {
                                anchors.top: parent.top
                                anchors.left: parent.left
                                anchors.right: parent.right
                                spacing: 6

                                // 上一句：压暗小字，单行省略
                                Text {
                                    width: parent.width
                                    visible: text.length > 0
                                    text: lyricPeek.idx > 0
                                          ? lyricPeek.peekLine(lyricPeek.idx - 1) : ""
                                    color: root.inkSub
                                    opacity: 0.55
                                    font.pixelSize: Math.round(13 * theme.lyricFontScale / 100)
                                    wrapMode: Text.NoWrap
                                    elide: Text.ElideRight
                                    horizontalAlignment: root.lyricHAlign
                                }
                                // 当前句：加粗高亮，最多两行
                                Text {
                                    width: parent.width
                                    text: app.currentLyricLine.length > 0
                                          ? app.currentLyricLine : "♪"
                                    color: theme.lyricHighlight
                                    font.pixelSize: Math.round(16 * theme.lyricFontScale / 100)
                                    font.bold: true
                                    wrapMode: Text.Wrap
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
                                    horizontalAlignment: root.lyricHAlign
                                }
                                // 下一句：压暗小字，单行省略
                                Text {
                                    width: parent.width
                                    visible: text.length > 0
                                    text: lyricPeek.idx >= 0
                                          ? lyricPeek.peekLine(lyricPeek.idx + 1) : ""
                                    color: root.inkSub
                                    opacity: 0.55
                                    font.pixelSize: Math.round(13 * theme.lyricFontScale / 100)
                                    wrapMode: Text.NoWrap
                                    elide: Text.ElideRight
                                    horizontalAlignment: root.lyricHAlign
                                }
                            }
                        }
                    }

                    // 第 2 页：歌词（页内自带纵向滚动）
                    Item {
                        Loader {
                            active: !root.wide
                            anchors.fill: parent
                            sourceComponent: lyricComp
                        }
                    }
                }

                // 页点指示器：当前页高亮主色
                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: root.compactTiny ? 0 : 14
                    visible: !root.compactTiny
                    Row {
                        anchors.centerIn: parent
                        spacing: 6
                        Repeater {
                            model: 2
                            Rectangle {
                                width: 6
                                height: 6
                                radius: 3
                                color: pager.currentIndex === index
                                       ? root.cPrimary
                                       : (root.isDark ? Qt.rgba(1, 1, 1, 0.25)
                                                      : Qt.rgba(0, 0, 0, 0.18))
                                Behavior on color { ColorAnimation { duration: 150 } }
                            }
                        }
                    }
                }

                Loader {
                    Layout.fillWidth: true
                    // 进度自身是 18(滑条) + 2 + 16(时间) 的定高，压小只会切掉时间文字，
                    // 高度由上面的元信息与下面的控件去省，这里保持原样
                    Layout.preferredHeight: 36
                    sourceComponent: progressComp
                }

                Loader {
                    Layout.fillWidth: true
                    Layout.preferredHeight: root.ultraTiny ? 50 : 62
                    sourceComponent: controlsComp
                }
            }

            // 弹层打开时点任意空白处收起（竖滑块本身 z 更高，不受影响）
            MouseArea {
                anchors.fill: parent
                z: 1
                visible: root.volumePopupOpen && !root.wide
                onClicked: root.volumePopupOpen = false
            }

            // 垂直音量滑块：浮在控制行（62 高）正上方，居中收放
            Loader {
                active: !root.wide
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 76
                width: theme.touchMin
                height: 160
                z: 2
                sourceComponent: volPopupComp
            }
        }
    }



    // ================= 可复用：垂直音量弹层（竖屏 / 宽屏共用） =================
    Component {
        id: volPopupComp
        Rectangle {
            // 宽度跟触控最小值：底部的静音按钮要整块可点，窄了会溢出圆角
            width: theme.touchMin
            height: 160
            radius: width / 2
            color: root.isDark ? Qt.rgba(0.13, 0.14, 0.17, 0.95)
                               : Qt.rgba(1, 1, 1, 0.95)
            border.color: root.isDark ? Qt.rgba(1, 1, 1, 0.14) : Qt.rgba(0, 0, 0, 0.10)
            border.width: 1
            opacity: root.volumePopupOpen ? 1 : 0
            visible: opacity > 0.01
            scale: 0.92 + 0.08 * opacity
            transformOrigin: Item.Bottom
            Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 7
                text: app.volume + "%"
                color: root.inkSub
                font.pixelSize: theme.fontXs
                font.bold: true
            }

            // 与 PlayBar.qml 的桌面竖滑块同一套定位逻辑。
            // 注意：Qt 竖向 Slider 的 visualPosition 是翻转的（1 - position），
            // 直接用它会让 80% 显示在下方 20%；这里统一用逻辑 position。
            Slider {
                id: compactVolSlider
                orientation: Qt.Vertical
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 24
                width: 26
                // 行程底部让给静音按钮（touchMin 高 + 8 间隙），否则两者热区重叠
                height: parent.height - 24 - theme.touchMin - 8
                padding: 0
                from: 0
                to: 100
                stepSize: 1
                value: app.volume
                onMoved: app.setVolume(value)
                onPressedChanged: {
                    if (!pressed)
                        value = Qt.binding(function () { return app.volume })
                }
                background: Rectangle {
                    x: (compactVolSlider.width - width) / 2
                    y: 0
                    width: 4
                    height: compactVolSlider.availableHeight
                    radius: 2
                    color: root.inkFaint
                    Rectangle {
                        width: parent.width
                        height: compactVolSlider.position * parent.height
                        radius: 2
                        color: root.cPrimary
                        anchors.bottom: parent.bottom
                    }
                }
                handle: Rectangle {
                    x: (compactVolSlider.width - width) / 2
                    y: Math.max(0, Math.min(compactVolSlider.availableHeight - height,
                                            (1 - compactVolSlider.position) * compactVolSlider.availableHeight - height / 2))
                    width: 12
                    height: 12
                    radius: 6
                    color: root.cPrimary
                }
            }

            // 底部一键静音 / 恢复
            Item {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                width: theme.touchMin
                height: theme.touchMin
                GlyphIcon {
                    anchors.centerIn: parent
                    name: app.volume > 0 ? "volume" : "volumeMute"
                    size: 13
                    color: root.inkSub
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: app.toggleMute()
                }
            }
        }
    }

    // ================= 可复用：封面 =================
    Component {
        id: coverComp
        Item {
            anchors.fill: parent

            // 封面投影：三层伪投影，内联实现抽成了 SoftShadow 全局共用
            SoftShadow { target: coverCard }

            Rectangle {
                id: coverCard
                anchors.fill: parent
                radius: Math.min(width, height) * 0.045
                color: root.isDark ? Qt.rgba(1, 1, 1, 0.06) : Qt.rgba(0, 0, 0, 0.04)
                scale: (coverHover.containsMouse ? 1.012 : 1.0) * root.coverPop
                Behavior on scale { NumberAnimation { duration: 240; easing.type: Easing.OutCubic } }

                Image {
                    anchors.fill: parent
                    source: root.artSource
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    // 封面显示最大约 390px；不限源尺寸会把 CDN 原图（数千像素）
                    // 整张解码进显存，宽/窄两份歌词页各一张，低端 Android 易 OOM。
                    sourceSize.width: 780
                    sourceSize.height: 780
                    visible: status === Image.Ready
                }

                // 悬停浮出的暗色罩层：纯装饰，让封面有质感反馈。
                // 不再放播放/暂停圆钮、也不响应点击——点封面不应触发暂停/播放，
                // 播放/暂停只用下方控件行的主按钮（真机误触反馈）。
                Rectangle {
                    anchors.fill: parent
                    radius: coverCard.radius
                    color: Qt.rgba(0, 0, 0, coverHover.containsMouse ? 0.12 : 0)
                    Behavior on color { ColorAnimation { duration: 220 } }
                }

                // 内描边：白封面上也能看出边界
                Rectangle {
                    anchors.fill: parent
                    radius: coverCard.radius
                    color: "transparent"
                    border.width: 1
                    border.color: root.isDark ? Qt.rgba(1, 1, 1, 0.10) : Qt.rgba(0, 0, 0, 0.06)
                }
            }

            MouseArea {
                id: coverHover
                anchors.fill: parent
                hoverEnabled: true
                // 封面只是展示，不再是可点击控件：去掉 onClicked 与手型光标，
                // 播放/暂停交给控件行的主按钮。保留 hover 仅为封面微缩放的视觉反馈。
            }
        }
    }

    // ================= 可复用：标题 / 艺人 =================
    Component {
        id: metaComp
        Column {
            anchors.fill: parent
            topPadding: 2
            spacing: 5

            Text {
                width: parent.width
                text: root.hasSong ? app.currentTitle : "还没有播放歌曲"
                color: root.ink
                // 与 PlayBar 标题同一套字号令牌，保持页面间层级一致（26/21 不变）
                font.pixelSize: root.wide ? theme.fontXl + 6 : theme.fontXl + 1
                font.bold: true
                font.letterSpacing: -0.3
                elide: Text.ElideRight
                wrapMode: Text.NoWrap
                horizontalAlignment: Text.AlignHCenter
            }

            Text {
                width: parent.width
                // 极矮窗口里省掉艺人行，把高度让给进度与控件
                visible: !root.compactTiny
                text: root.hasSong
                      ? (app.currentArtist.length > 0 ? app.currentArtist : "未知艺人")
                      : "在曲库或歌单里选一首开始"
                color: root.inkSub
                font.pixelSize: root.wide ? theme.fontLg - 1 : theme.fontMd
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 8
                // 极矮分屏里连角标一起省掉：一行 20dp 高度留给控件更值
                visible: root.hasSong && !root.ultraTiny
                topPadding: 2

                Rectangle {
                    visible: app.currentSourceLabel.length > 0
                    width: srcText.implicitWidth + 16
                    height: 20
                    radius: 10
                    color: root.chipBg
                    Text {
                        id: srcText
                        anchors.centerIn: parent
                        text: app.currentSourceLabel
                        color: root.inkSub
                        font.pixelSize: theme.fontXs
                        font.bold: true
                    }
                }
                Rectangle {
                    visible: app.substituteBadge.length > 0
                    width: subText.implicitWidth + 16
                    height: 20
                    radius: 10
                    // 浅色主题下 0.22 橙底 + 橙字几乎读不出来（背景是封面模糊层，越亮越糊）
                    color: Qt.rgba(theme.vip.r, theme.vip.g, theme.vip.b, root.isDark ? 0.26 : 0.14)
                    Text {
                        id: subText
                        anchors.centerIn: parent
                        text: app.substituteBadge
                        color: root.isDark ? theme.vip : Qt.darker(theme.vip, 1.45)
                        font.pixelSize: theme.fontXs
                        font.bold: true
                    }
                }
            }
        }
    }

    // ================= 可复用：进度 =================
    Component {
        id: progressComp
        Column {
            anchors.fill: parent
            spacing: 2

            Slider {
                id: seekBar
                width: parent.width
                height: 18
                padding: 0
                from: 0
                to: app.duration > 0 ? app.duration : 1
                value: app.position
                onMoved: app.seek(value)
                onPressedChanged: {
                    if (!pressed)
                        value = Qt.binding(function () { return app.position })
                }

                background: Rectangle {
                    x: 0
                    y: (seekBar.height - height) / 2
                    width: seekBar.availableWidth
                    height: seekBar.hovered || seekBar.pressed ? 6 : 4
                    radius: height / 2
                    color: root.inkFaint
                    Behavior on height { NumberAnimation { duration: 150 } }

                    Rectangle {
                        width: seekBar.visualPosition * parent.width
                        height: parent.height
                        radius: parent.radius
                        color: root.ink
                        opacity: 0.92
                    }
                }
                handle: Rectangle {
                    x: Math.max(0, Math.min(seekBar.availableWidth - width,
                                             seekBar.visualPosition * seekBar.availableWidth - width / 2))
                    y: (seekBar.height - height) / 2
                    width: 12
                    height: 12
                    radius: 6
                    color: root.ink
                    opacity: seekBar.pressed ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: 140 } }
                }
            }

            Item {
                width: parent.width
                height: 16
                Text {
                    anchors.left: parent.left
                    text: root.mmss(app.position)
                    color: root.inkSub
                    font.pixelSize: theme.fontSm
                }
                Text {
                    anchors.right: parent.right
                    text: root.mmss(app.duration)
                    color: root.inkSub
                    font.pixelSize: theme.fontSm
                }
            }
        }
    }

    // ================= 可复用：控件 =================
    Component {
        id: controlsComp
        Column {
            id: controlsCol
            anchors.fill: parent
            spacing: 10

            // 控制行是 5 枚触控按钮 + 1 个主按钮。360dp 手机按 48dp 触控位 + 12 间距
            // 需要 358dp，扣掉页面左右留白后放不下，最右的音量按钮会被裁掉。
            // 这里按可用宽度反推按钮尺寸与间距，窄屏自动收小，宽屏保持 48dp。
            readonly property int ctrlPlayW: root.wide ? 64 : (root.ultraTiny ? 46 : 58)
            readonly property int ctrlBtn: {
                var avail = Math.max(0, width - ctrlPlayW)
                var gapTarget = root.wide ? 18 : 12
                var byWidth = Math.floor((avail - gapTarget * 5) / 5)
                // 触屏优先的车机/手机不能为了塞下 5 枚按钮缩到 34dp，
                // 间距会自动收到 2dp；桌面鼠标保持旧行为。
                var floorSize = platform.touchPrimary ? theme.touchMin : 34
                return Math.max(floorSize, Math.min(theme.touchMin, byWidth))
            }
            readonly property int ctrlGap: {
                var avail = Math.max(0, width - ctrlPlayW - ctrlBtn * 5)
                return Math.max(2, Math.min(root.wide ? 18 : 12, Math.floor(avail / 5)))
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: controlsCol.ctrlGap

                // 收藏
                Item {
                    width: controlsCol.ctrlBtn; height: controlsCol.ctrlBtn
                    Rectangle {
                        anchors.fill: parent
                        radius: height / 2
                        color: likeArea.containsMouse ? root.chipBg : "transparent"
                        Behavior on color { ColorAnimation { duration: 150 } }
                        GlyphIcon {
                            anchors.centerIn: parent
                            name: app.currentFavorite ? "heartFill" : "heart"
                            size: 18
                            color: app.currentFavorite ? root.cPrimary : root.ink
                            scale: likeArea.pressed ? 0.85 : 1.0
                            Behavior on scale { NumberAnimation { duration: 120 } }
                        }
                    }
                    MouseArea {
                        id: likeArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: app.toggleFavoriteCurrent()
                    }
                }

                Item {
                    width: controlsCol.ctrlBtn; height: controlsCol.ctrlBtn
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: "prev"
                        size: 19
                        color: root.ink
                        opacity: prevArea.pressed ? 0.5 : 0.92
                    }
                    MouseArea {
                        id: prevArea
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: app.previous()
                    }
                }

                // 主按钮：单色圆盘（深色主题为白盘），按下回弹
                Rectangle {
                    width: controlsCol.ctrlPlayW
                    height: width
                    radius: width / 2
                    color: root.ink
                    scale: playArea.pressed ? 0.93 : (playArea.containsMouse ? 1.04 : 1.0)
                    Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                    Behavior on color { ColorAnimation { duration: 250 } }
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: app.playing ? "pause" : "play"
                        size: 22
                        color: root.isDark ? "#101216" : "#FFFFFF"
                    }
                    MouseArea {
                        id: playArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: app.togglePause()
                    }
                }

                Item {
                    width: controlsCol.ctrlBtn; height: controlsCol.ctrlBtn
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: "next"
                        size: 19
                        color: root.ink
                        opacity: nextArea.pressed ? 0.5 : 0.92
                    }
                    MouseArea {
                        id: nextArea
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: app.next()
                    }
                }

                // 播放模式：图标随模式变化（顺序 / 循环 / 单曲 / 随机）
                Item {
                    width: controlsCol.ctrlBtn; height: controlsCol.ctrlBtn
                    Rectangle {
                        anchors.fill: parent
                        radius: height / 2
                        color: modeArea.containsMouse ? root.chipBg : "transparent"
                        Behavior on color { ColorAnimation { duration: 150 } }
                        GlyphIcon {
                            anchors.centerIn: parent
                            name: root.modeIcon()
                            size: 17
                            color: root.ink
                            opacity: 0.85
                        }
                    }
                    MouseArea {
                        id: modeArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: app.cyclePlayMode()
                    }
                }

                // 音量：点开垂直滑块弹层（竖屏浮在控制行上方、宽屏浮在控件块上方，
                // 同一个 volPopupComp；不再用宽屏常驻横条）
                Item {
                    width: controlsCol.ctrlBtn; height: controlsCol.ctrlBtn
                    Rectangle {
                        anchors.fill: parent
                        radius: height / 2
                        color: (compactVolArea.containsMouse || root.volumePopupOpen)
                               ? root.chipBg : "transparent"
                        Behavior on color { ColorAnimation { duration: 150 } }
                        GlyphIcon {
                            anchors.centerIn: parent
                            name: app.volume > 0 ? "volume" : "volumeMute"
                            size: 17
                            color: root.ink
                            opacity: 0.85
                        }
                    }
                    MouseArea {
                        id: compactVolArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.volumePopupOpen = !root.volumePopupOpen
                    }
                }
            }
        }
    }

    // ================= 可复用：歌词 =================
    Component {
        id: lyricComp
        Item {
            id: lyricRoot
            anchors.fill: parent
            // 上下羽化比例：按视口高度取百分比。竖屏视口矮，比例稍大就会吃掉第一行，
            // 所以宽屏给足、竖屏收敛（也是按像素上限夹一层，超高视口不至于拖着长尾）。
            readonly property real fadeRatio: {
                const px = root.wide ? 96 : 58
                return Math.min(root.wide ? 0.34 : 0.22, px / Math.max(1, height))
            }

            // 上下羽化不盖色带，直接按「行中心在视口里的位置」压行的 alpha：越贴近上下
            // 边缘越淡，到边缘正好归零，于是歌词是真的淡出到背景。
            // 试过两条路都不行：①叠两条「近似页面色」的渐变矩形——背景是模糊封面（彩色
            // 渐变）时那两块纯色带会露出硬边，浅色主题下整个歌词区像扣了个白方框；
            // ②ShaderEffect 做 alpha 遮罩——桌面版是 Qt 5（内联 GLSL 能跑），Android 是
            // Qt 6（fragmentShader 只认预编译 .qsb，内联源码报 "Failed to deserialize
            // QShader" 后整层不绘制），一套 QML 没法同时满足，所以改成纯 QML 逐行衰减。
            Item {
                id: lyricViewport
                anchors.fill: parent

                ListView {
                    id: lyricView
                    anchors.fill: parent
                    // 预渲染两屏：默认只多备 100px，长歌词自动滚动时委托反复重建
                    cacheBuffer: visible ? height * 2 : 100
                    model: app.lyricLines
                    // 时间轴只在换行时（lyricsChanged）重新取一次，不要让每次
                    // positionChanged 都回到 C++ 重建一整个 list
                    property var times: app.lyricTimes
                    property bool timed: app.lyricsTimed
                    // 播放后端的进度回调频率差别很大（Windows DirectShow 约 250ms、
                    // Qt6/FFmpeg 更密、Android 可能到 1s）。直接拿 app.position 做扫光
                    // 会一顿一顿，所以记住「上一拍的位置 + 墙钟」，在拍与拍之间插值。
                    property int tickMs: 260
                    property real anchorPos: 0
                    property real anchorStamp: 0
                    property real shownPos: 0
                    readonly property int posWatch: app.position
                    onPosWatchChanged: {
                        const now = Date.now()
                        if (anchorStamp > 0) {
                            const dt = now - anchorStamp
                            if (dt >= 30 && dt <= 1000) tickMs = dt
                        }
                        anchorPos = app.position
                        anchorStamp = now
                        shownPos = app.position
                    }
                    Timer {
                        interval: 60
                        repeat: true
                        triggeredOnStart: true
                        // 两个窗格各有一份歌词组件，页面被 StackLayout 隐藏时
                        // visible 会变 false；不判断可见性的话只要音乐在放，
                        // 两个 60ms 定时器就会在后台一直刷新整棵歌词绑定。
                        running: lyricView.timed && app.playing
                                 && root.visible && lyricView.visible
                        onTriggered: {
                            // anchorStamp 尚未由 position 回报初始化时（初始 0），
                            // ahead 会是 Date.now() 的巨大值被截到 tickMs*2，导致首帧闪跳。
                            if (lyricView.anchorStamp <= 0) return
                            const ahead = Date.now() - lyricView.anchorStamp
                            lyricView.shownPos = lyricView.anchorPos
                                    + Math.min(Math.max(0, ahead), lyricView.tickMs * 2)
                        }
                    }
                    spacing: theme.lyricLineGap
                    topMargin: lyricRoot.height * (root.wide ? 0.34 : 0.24)
                    bottomMargin: lyricRoot.height * (root.wide ? 0.34 : 0.24)
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    property bool userScrolling: false
                    property int followIndex: app.currentLyricIndex
                    // 拖动后停留的那一行（歌词定位播放）：-1 表示没有待播放的选中行。
                    // 用户拖停 → pickDragRow 记录中心行 → 浮出「从此播放」按钮；
                    // 点按钮才 app.seek(该行时间)，避免一拖动就跳转打断当前播放。
                    property int dragRow: -1
                    // 取视口正中间那一行作为拖动选中的行。indexAt 用内容坐标
                    //（不含 contentY 的视口坐标取法在这里是错的），所以 y 要加 contentY。
                    function pickDragRow() {
                        const r = indexAt(width / 2, contentY + height / 2)
                        if (r >= 0 && app.lyricsTimed && r < app.lyricTimes.length)
                            dragRow = r
                        else
                            dragRow = -1
                    }
                    function playFromDragRow() {
                        if (dragRow < 0 || dragRow >= app.lyricTimes.length) return
                        app.seek(Number(app.lyricTimes[dragRow]))
                        dragRow = -1
                        userScrolling = false
                        if (app.currentLyricIndex >= 0) centerOn(app.currentLyricIndex)
                    }

                    delegate: Item {
                        id: lyricRow
                        width: ListView.view.width
                        // 有翻译的行在原文下方多占一小行；高度全部由内容撑出
                        height: Math.max(34, lineBox.height + 16)
                        readonly property string transLine:
                            index < app.lyricTransLines.length ? app.lyricTransLines[index] : ""
                        readonly property bool hasTrans: transLine.length > 0
                        readonly property bool active: index === app.currentLyricIndex
                        readonly property bool timed: app.currentLyricIndex >= 0
                        readonly property int dist: Math.abs(index - app.currentLyricIndex)
                        // 逐行扫光的进度：本行起点 → 下一行起点。末行没有下一行，退回歌曲
                        // 总长；总长也没拿到就用 4s，否则扫光条要么瞬间走完要么不出现。
                        readonly property int startMs: lyricView.timed ? Number(lyricView.times[index]) : 0
                        readonly property int endMs: {
                            const next = Number(lyricView.times[index + 1])
                            if (!isNaN(next) && next > startMs) return next
                            return app.duration > startMs ? app.duration : startMs + 4000
                        }
                        readonly property real karaoke: (active && lyricView.timed)
                            ? Math.min(1, Math.max(0,
                                  (lyricView.shownPos - startMs) / Math.max(400, endMs - startMs)))
                            : 0

                        // 上下边缘羽化：行中心越贴近视口边缘 alpha 越低，到边缘正好归零，
                        // clip 的硬边因此看不出来，也不需要往歌词上盖任何色带。
                        // 这里不加 Behavior —— 滚动时它每帧都在变，补间只会拖出残影。
                        opacity: {
                            const vh = lyricView.height
                            if (vh <= 0) return 1
                            const c = (y + height / 2 - lyricView.contentY) / vh
                            const f = lyricRoot.fadeRatio
                            let k = 1.0
                            if (c < f) k = c / f
                            else if (c > 1 - f) k = (1 - c) / f
                            k = Math.min(1, Math.max(0, k))
                            return k * k * (3 - 2 * k)   // smoothstep，避免看出折角
                        }

                        // 缩放和透明度放在容器上：transform 只作用于自身，两层文字必须
                        // 一起变换才能继续保持重合。
                        Item {
                            id: lineBox
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.topMargin: 8
                            height: lineText.implicitHeight
                                    + (lyricRow.hasTrans ? transText.implicitHeight + 2 : 0)
                            // 文字实际占位的宽度。不能用 lineText.contentWidth：这一层是
                            // anchors.fill 定宽定高的 Text，contentWidth 在这种排版下量出来是 0，
                            // 扫光条于是永远 0 宽（截图上完全看不见）。
                            readonly property real textW: Math.min(lineText.implicitWidth, lineText.width)
                            transformOrigin: Item.Left
                            scale: lyricRow.active ? 1.04 : 1.0
                            Behavior on scale { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }
                            opacity: lyricRow.active ? 1.0
                                    : (!lyricRow.timed ? 0.62
                                                       : (lyricRow.dist <= 1 ? 0.52
                                                                              : (lyricRow.dist <= 3 ? 0.34 : 0.18)))
                            Behavior on opacity { NumberAnimation { duration: 300 } }

                            Text {
                                id: lineText
                                anchors.top: parent.top
                                anchors.left: parent.left
                                anchors.right: parent.right
                                text: modelData.length > 0 ? modelData : "♪"
                                // 未唱到的部分留暗底，亮色交给上面的扫光层逐字露出。
                                // 只能压暗「当前行」：这里误用全局的 timed 会把整屏几十行
                                // 一起乘上 0.42，再叠上各自的透明度，结果全都没了。
                                color: lyricRow.active && lyricView.timed
                                       ? Qt.rgba(root.ink.r, root.ink.g, root.ink.b, 0.45) : root.ink
                                // 左对齐、当前行加粗放大：与参考项目一致。
                                // 字号按设置页的歌词缩放取，百分比作用在两档视口基准上。
                                font.pixelSize: Math.round((root.wide ? 21 : 17)
                                                           * theme.lyricFontScale / 100)
                                font.bold: lyricRow.active
                                wrapMode: Text.Wrap
                                verticalAlignment: Text.AlignVCenter
                                horizontalAlignment: root.lyricHAlign
                            }

                            // 扫光条：外层 clip 矩形只露出「已唱到的那一段」，内层文字按整行
                            // 宽度排版后整体左移，保证与底字逐像素对齐。居中排版的行必须从
                            // 文字真正的左边缘开始扫，否则每行开头几拍先扫一段空白。
                            // 只覆盖原文高度：翻译行在下方，不参与逐字扫光。
                            Item {
                                id: sweepZone
                                readonly property real inset: {
                                    if (lineText.horizontalAlignment === Text.AlignHCenter)
                                        return Math.max(0, (lineText.width - lineBox.textW) / 2)
                                    if (lineText.horizontalAlignment === Text.AlignRight)
                                        return Math.max(0, lineText.width - lineBox.textW)
                                    return 0
                                }
                                x: inset
                                y: 0
                                height: lineText.implicitHeight
                                // 宽度不加 Behavior：推进速度已经由上面 60ms 的墙钟插值给平了，
                                // 再叠一层缓动只会让亮色落后于唱词。
                                width: Math.max(0, Math.min(lineBox.textW,
                                                            lineText.width - 2 * inset) * lyricRow.karaoke)
                                clip: true
                                Text {
                                    x: -sweepZone.inset
                                    width: lineText.width
                                    height: lineText.implicitHeight
                                    text: lineText.text
                                    font.family: lineText.font.family
                                    font.pixelSize: lineText.font.pixelSize
                                    font.bold: lineText.font.bold
                                    // 蓝轴高亮：红绿色弱也能与灰色未唱部分区分
                                    color: theme.lyricHighlight
                                    wrapMode: Text.Wrap
                                    verticalAlignment: Text.AlignVCenter
                                    horizontalAlignment: lineText.horizontalAlignment
                                }
                            }

                            // 翻译行：平台歌词接口带了翻译轨（网易云 tlyric / QQ trans / 咪咕
                            // translateLyric / 脚本 tlrc）时默认显示，随本行一起淡入淡出。
                            Text {
                                id: transText
                                anchors.top: lineText.bottom
                                anchors.topMargin: 2
                                anchors.left: parent.left
                                anchors.right: parent.right
                                visible: lyricRow.hasTrans
                                text: lyricRow.transLine
                                color: lyricRow.active && lyricView.timed
                                       ? Qt.rgba(root.ink.r, root.ink.g, root.ink.b, 0.45) : root.ink
                                opacity: 0.78
                                font.pixelSize: Math.round((root.wide ? 14 : 12)
                                                           * theme.lyricFontScale / 100
                                                           * theme.lyricTransScale / 100)
                                wrapMode: Text.Wrap
                                horizontalAlignment: lineText.horizontalAlignment
                            }
                        }

                        // 单击歌词行直接跳到该行时间（参考洛雪 PlayLine 的点行定位）。
                        // 用按下/松开的位移区分「点击」与「拖动列表」，滚动歌词时不会误触 seek。
                        MouseArea {
                            z: 2
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            property real pressY: -1
                            onPressed: pressY = mouseY
                            onReleased: {
                                if (pressY < 0 || Math.abs(mouseY - pressY) > 10)
                                    return
                                if (!lyricView.timed || index >= app.lyricTimes.length)
                                    return
                                const t = Number(app.lyricTimes[index])
                                if (!isFinite(t)) return
                                lyricView.userScrolling = false
                                lyricView.dragRow = -1
                                app.seek(t)
                                lyricView.centerOn(index)
                            }
                        }
                    }

                    Behavior on contentY {
                        enabled: !lyricView.dragging && !lyricView.userScrolling
                        NumberAnimation { duration: 460; easing.type: Easing.OutCubic }
                    }
                    // 居中自己算，不用 positionViewAtIndex(..., Center)：那条路径不认
                    // topMargin/bottomMargin，实测在唱的那行会被顶到可视区上沿、半行裁掉。
                    // 委托的 y 已经在 contentItem 里含了上边距，所以行中心减半个视口高
                    // 就是 contentY；再按 StopAtBounds 的可用区间夹住。
                    function centerOn(row) {
                        const item = itemAtIndex(row)
                        if (!item) {
                            positionViewAtIndex(row, ListView.Center)
                            return
                        }
                        const target = item.y + item.height / 2 - height / 2
                        contentY = Math.max(0, Math.min(target, Math.max(0, contentHeight - height)))
                    }
                    onFollowIndexChanged: {
                        if (userScrolling || followIndex < 0)
                            return
                        centerOn(followIndex)
                    }
                    // 视口尺寸会变（宽屏封面列出现、车机分屏切换），委托高度跟着重排，
                    // contentY 却还停在按旧视口算出来的值上——实测当前行会贴到顶边。
                    // 排版要等一轮事件循环才稳，所以延后一次重新居中。
                    Timer {
                        id: refollow
                        interval: 80
                        onTriggered: {
                            if (lyricView.dragRow >= 0) return   // 保留用户待播放的选中行
                            if (!lyricView.userScrolling && app.currentLyricIndex >= 0)
                                lyricView.centerOn(app.currentLyricIndex)
                        }
                    }
                    onHeightChanged: refollow.restart()
                    onWidthChanged: refollow.restart()
                    onModelChanged: {
                        userScrolling = false
                        dragRow = -1
                        positionViewAtBeginning()
                    }
                    onMovementStarted: {
                        userScrolling = true
                        dragRow = -1   // 重新拖动，撤掉上一次的「从此播放」按钮
                        resumeFollow.stop()
                    }
                    // 拖停后不立刻弹回跟随：先记下中心行并显示「从此播放」按钮，
                    // 给用户一个确认的机会（其它播放器也是这个交互）。resumeFollow
                    // 的自动归位延后到按钮逻辑里处理。
                    onMovementEnded: {
                        pickDragRow()
                        if (dragRow < 0) resumeFollow.restart()
                    }

                    Timer {
                        id: resumeFollow
                        interval: 5000
                        onTriggered: {
                            if (lyricView.dragRow >= 0) return   // 用户正看着待播放行，不弹回
                            lyricView.userScrolling = false
                            if (app.currentLyricIndex >= 0)
                                lyricView.centerOn(app.currentLyricIndex)
                        }
                    }
                }

                Column {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 40, 420)
                    spacing: 8
                    visible: app.lyricLines.length === 0

                    Text {
                        width: parent.width
                        text: app.lyricsText.length > 0 ? app.lyricsText
                              : (root.hasSong ? "当前音源没有提供歌词" : "播放歌曲后会在这里显示歌词")
                        color: root.inkSub
                        font.pixelSize: 14
                        lineHeight: 1.5
                        wrapMode: Text.Wrap
                        horizontalAlignment: Text.AlignHCenter
                    }
                }

                // 拖动歌词后浮出的「从此播放」按钮：点它才真正跳转。
                // 位置固定在视口正中（与 pickDragRow 取的中心行一致），
                // 半透明胶囊避免挡住歌词；无待跳转行（dragRow<0）时淡出。
                Rectangle {
                    z: 5
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.verticalCenter: parent.verticalCenter
                    width: playPill.implicitWidth + 32
                    height: 40
                    radius: 20
                    color: root.isDark ? Qt.rgba(0.14, 0.15, 0.19, 0.96)
                                       : Qt.rgba(1, 1, 1, 0.96)
                    border.color: root.isDark ? Qt.rgba(1, 1, 1, 0.18) : Qt.rgba(0, 0, 0, 0.12)
                    border.width: 1
                    opacity: lyricView.dragRow >= 0 ? 1 : 0
                    visible: opacity > 0.01
                    scale: lyricView.dragRow >= 0 ? 1 : 0.9
                    Behavior on opacity { NumberAnimation { duration: 160 } }
                    Behavior on scale { NumberAnimation { duration: 200; easing.type: Easing.OutBack } }
                    Row {
                        id: playPill
                        anchors.centerIn: parent
                        spacing: 8
                        GlyphIcon {
                            anchors.verticalCenter: parent.verticalCenter
                            name: "play"
                            size: 14
                            color: theme.accent
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: {
                                if (lyricView.dragRow < 0) return ""
                                const t = Number(app.lyricTimes[lyricView.dragRow])
                                return root.mmss(t) + " 从此播放"
                            }
                            color: root.ink
                            font.pixelSize: theme.fontSm
                            font.bold: true
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: lyricView.playFromDragRow()
                    }
                }

            }

        }
    }
}
