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

    // 兼容旧调用点：播放页现在始终自带控件，这个属性只为老外壳不报错
    property bool showControls: true

    // 竖屏音量弹层：横条收成按钮，点一下才展开垂直滑块
    property bool volumePopupOpen: false

    // 宽屏/横屏版式判定 —— 由「自身可用宽度 + 设备方向」共同决定，保证车机三种形态自适应：
    //   1) 横屏(含车机整屏 / 横屏分屏)：方向明确，只要宽度够放下「左封面+右歌词」双栏即双栏，
    //      阈值 520（左列下限 300 + 间距 + 歌词最小可用宽度）；极窄分屏(<520)退回单栏，避免控件被挤；
    //   2) 竖屏(含大平板竖屏)：需更宽(>=760)才双栏，否则保持单栏(封面在上/歌词居中/控件贴底)，
    //      这样竖屏与横屏呈现明显不同的自适应布局，而非把竖屏布局强行拉伸。
    // 用窗口自身的形状而不是 Screen.orientation：车机分屏时设备仍是横屏，
    // 但窗口可能只剩一个竖长条，按屏幕方向会错判成横屏双栏。
    readonly property bool wide: (root.width >= root.height ? root.width >= 520
                                                            : root.width >= 760)
    readonly property bool isDark: theme.dark

    // 本页专用文字色：浅色主题用深墨（压在白纱背景上），深色主题用浅墨
    readonly property color ink: isDark ? "#F3F5FA" : "#15171C"
    readonly property color inkSub: isDark ? Qt.rgba(1, 1, 1, 0.62) : Qt.rgba(0, 0, 0, 0.55)
    readonly property color inkFaint: isDark ? Qt.rgba(1, 1, 1, 0.16) : Qt.rgba(0, 0, 0, 0.11)
    readonly property color chipBg: isDark ? Qt.rgba(1, 1, 1, 0.14) : Qt.rgba(1, 1, 1, 0.62)
    readonly property color chipBgHover: isDark ? Qt.rgba(1, 1, 1, 0.22) : Qt.rgba(1, 1, 1, 0.92)
    readonly property color cPrimary: theme.accent

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
        bgLayer.setArt(root.artSource)
    }
    onArtSourceChanged: bgLayer.setArt(root.artSource)

    // 主题纱：浅色 = 白纱（页面变成封面的淡彩），深色 = 黑纱
    Rectangle {
        anchors.fill: parent
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
            anchors.margins: 22
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
            anchors.margins: 22
            spacing: 8

            // 队列 / MV / 设置：与参考项目右上角一样是小的半透明方按钮
            Repeater {
                model: [
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
                            if (index === 0) root.requestQueue()
                            else if (index === 1) app.playMvCurrent()
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
            anchors.margins: 32
            anchors.topMargin: 26
            anchors.bottomMargin: 14
            visible: root.wide

            // 封面边长：由「左列宽度」和「扣除信息区后的剩余高度」共同决定，避免小窗口把按钮挤出去
            // 预留 216 = 标题区 56 + 进度 34 + 控件 72 + 两段间距，使矮横屏（车机分屏常只有 300~400 高）
            // 也能整列放下，而不是把播放按钮推出可视区
            readonly property bool shortRow: height < 420
            readonly property real coverSide: Math.max(84, Math.min(width * 0.40,
                                                                  height - 216, 390))

            RowLayout {
                anchors.fill: parent
                spacing: Math.max(32, Math.min(72, widePane.width * 0.06))

                Item {
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
                                width: parent.width
                                height: widePane.shortRow ? 72 : 86
                                sourceComponent: controlsComp
                            }
                        }
                    }
                }

                Loader {
                    sourceComponent: lyricComp
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }
            }
        }

        // ---------- 窄屏 / 移动竖屏：参考 lx-music-mobile 播放页 ----------
        // 封面页 ↔ 歌词页 SwipeView 左右滑动切换 + 页点指示器；
        // 标题/艺人、进度、控件、音量按钮固定贴底，不做整页滚动
        Item {
            id: compactPane
            anchors.fill: parent
            anchors.margins: 18
            anchors.topMargin: 58
            visible: !root.wide

            ColumnLayout {
                id: compactCol
                anchors.fill: parent
                spacing: 6

                Loader {
                    Layout.fillWidth: true
                    // 同宽屏那处：三行元信息约 89 高，写死 64 会让角标压在歌词第一行上
                    Layout.preferredHeight: item ? item.implicitHeight : 80
                    sourceComponent: metaComp
                }

                // 封面 ↔ 歌词：左右滑动切换（lx 的 PagerView 交互）
                SwipeView {
                    id: pager
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 140
                    clip: true

                    // 第 1 页：封面居中，边长随页高自适应
                    Item {
                        Item {
                            anchors.centerIn: parent
                            width: Math.min(parent.width * 0.92, parent.height * 0.94, 300)
                            height: width
                            Loader { anchors.fill: parent; sourceComponent: coverComp }
                        }
                    }

                    // 第 2 页：歌词（页内自带纵向滚动）
                    Item {
                        Loader {
                            anchors.fill: parent
                            sourceComponent: lyricComp
                        }
                    }
                }

                // 页点指示器：当前页高亮主色
                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 14
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
                    Layout.preferredHeight: 36
                    sourceComponent: progressComp
                }

                Loader {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 62
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
            Rectangle {
                id: compactVolPopup
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 76
                // 宽度跟触控最小值：底部的静音按钮要整块可点，窄了会溢出圆角
                width: theme.touchMin
                height: 160
                radius: width / 2
                z: 2
                color: root.isDark ? Qt.rgba(0.13, 0.14, 0.17, 0.95)
                                   : Qt.rgba(1, 1, 1, 0.95)
                border.color: root.isDark ? Qt.rgba(1, 1, 1, 0.14) : Qt.rgba(0, 0, 0, 0.10)
                border.width: 1
                opacity: root.volumePopupOpen ? 1 : 0
                visible: opacity > 0.01 && !root.wide
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
                    visible: status === Image.Ready
                }

                // 悬停浮出的播放/暂停圆钮
                Rectangle {
                    anchors.fill: parent
                    radius: coverCard.radius
                    color: Qt.rgba(0, 0, 0, coverHover.containsMouse ? 0.22 : 0)
                    Behavior on color { ColorAnimation { duration: 220 } }

                    Rectangle {
                        width: 60
                        height: 60
                        radius: 30
                        anchors.centerIn: parent
                        color: Qt.rgba(1, 1, 1, 0.95)
                        opacity: coverHover.containsMouse ? 1 : 0
                        scale: coverHover.containsMouse ? 1 : 0.82
                        Behavior on opacity { NumberAnimation { duration: 180 } }
                        Behavior on scale { NumberAnimation { duration: 240; easing.type: Easing.OutBack } }
                        GlyphIcon {
                            anchors.centerIn: parent
                            name: app.playing ? "pause" : "play"
                            size: 20
                            color: "#15171C"
                        }
                    }
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
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    // 竖屏改成「封面在上 + 歌词居中」后不再有封面/歌词切换，
                    // 点封面与宽屏保持一致：播放 / 暂停
                    app.togglePause()
                }
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
                font.pixelSize: root.wide ? 26 : 21
                font.bold: true
                font.letterSpacing: -0.3
                elide: Text.ElideRight
                wrapMode: Text.NoWrap
                horizontalAlignment: Text.AlignHCenter
            }

            Text {
                width: parent.width
                text: root.hasSong
                      ? (app.currentArtist.length > 0 ? app.currentArtist : "未知艺人")
                      : "在曲库或歌单里选一首开始"
                color: root.inkSub
                font.pixelSize: root.wide ? 15 : 13
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 8
                visible: root.hasSong
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
                    color: Qt.rgba(theme.vip.r, theme.vip.g, theme.vip.b, 0.22)
                    Text {
                        id: subText
                        anchors.centerIn: parent
                        text: app.substituteBadge
                        color: theme.vip
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
            anchors.fill: parent
            spacing: 10

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: root.wide ? 18 : 12

                // 收藏
                Item {
                    width: theme.touchMin; height: theme.touchMin
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
                    width: theme.touchMin; height: theme.touchMin
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
                    width: root.wide ? 64 : 58
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
                    width: theme.touchMin; height: theme.touchMin
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
                    width: theme.touchMin; height: theme.touchMin
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

                // 音量：窄屏收成控制行里的一枚按钮，点开垂直滑块弹层；
                // 宽屏下方已有常驻音量滑条，这里不再重复显示
                Item {
                    width: theme.touchMin; height: theme.touchMin
                    visible: !root.wide
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

            // 音量：宽屏仍保留常驻横条；竖屏收成按钮 + 垂直弹层（见 compactVolPopup）
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 8
                topPadding: 2
                height: 20
                visible: root.wide

                GlyphIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    name: app.volume > 0 ? "volume" : "volumeMute"
                    size: 15
                    color: root.inkSub
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: app.toggleMute()
                    }
                }

                Slider {
                    id: vol
                    width: root.wide ? 132 : 160
                    height: 16
                    padding: 0
                    anchors.verticalCenter: parent.verticalCenter
                    from: 0; to: 100; stepSize: 1
                    value: app.volume
                    onMoved: app.setVolume(value)
                    onPressedChanged: {
                        if (!pressed)
                            value = Qt.binding(function () { return app.volume })
                    }
                    background: Rectangle {
                        x: 0
                        y: (vol.height - height) / 2
                        width: vol.availableWidth
                        height: 3
                        radius: 1.5
                        color: root.inkFaint
                        Rectangle {
                            width: vol.visualPosition * parent.width
                            height: parent.height
                            radius: 1.5
                            color: root.ink
                            opacity: 0.55
                        }
                    }
                    handle: Rectangle {
                        x: Math.max(0, Math.min(vol.availableWidth - width,
                                                 vol.visualPosition * vol.availableWidth - width / 2))
                        y: (vol.height - height) / 2
                        width: 10; height: 10; radius: 5
                        color: root.ink
                        opacity: vol.pressed ? 1 : 0
                    }
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: app.volume + "%"
                    color: root.inkSub
                    font.pixelSize: theme.fontXs
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
                    cacheBuffer: height * 2
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
                        running: lyricView.timed && app.playing
                        onTriggered: {
                            const ahead = Date.now() - lyricView.anchorStamp
                            lyricView.shownPos = lyricView.anchorPos
                                    + Math.min(Math.max(0, ahead), lyricView.tickMs * 2)
                        }
                    }
                    spacing: 2
                    topMargin: lyricRoot.height * (root.wide ? 0.34 : 0.24)
                    bottomMargin: lyricRoot.height * (root.wide ? 0.34 : 0.24)
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    property bool userScrolling: false
                    property int followIndex: app.currentLyricIndex

                    delegate: Item {
                        id: lyricRow
                        width: ListView.view.width
                        height: Math.max(34, lineText.implicitHeight + 16)
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
                            anchors.verticalCenter: parent.verticalCenter
                            height: lineText.implicitHeight
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
                                anchors.fill: parent
                                text: modelData.length > 0 ? modelData : "♪"
                                // 未唱到的部分留暗底，亮色交给上面的扫光层逐字露出。
                                // 只能压暗「当前行」：这里误用全局的 timed 会把整屏几十行
                                // 一起乘上 0.42，再叠上各自的透明度，结果全都没了。
                                color: lyricRow.active && lyricView.timed
                                       ? Qt.rgba(root.ink.r, root.ink.g, root.ink.b, 0.45) : root.ink
                                // 左对齐、当前行加粗放大：与参考项目一致
                                font.pixelSize: root.wide ? 21 : 17
                                font.bold: lyricRow.active
                                wrapMode: Text.Wrap
                                verticalAlignment: Text.AlignVCenter
                                // 宽屏歌词贴左栏排版；竖屏区域窄，居中更接近手机播放器习惯
                                horizontalAlignment: root.wide ? Text.AlignLeft : Text.AlignHCenter
                            }

                            // 扫光条：外层 clip 矩形只露出「已唱到的那一段」，内层文字按整行
                            // 宽度排版后整体左移，保证与底字逐像素对齐。居中排版的行必须从
                            // 文字真正的左边缘开始扫，否则每行开头几拍先扫一段空白。
                            Item {
                                id: sweepZone
                                readonly property real inset: lineText.horizontalAlignment === Text.AlignHCenter
                                    ? Math.max(0, (lineText.width - lineBox.textW) / 2) : 0
                                x: inset
                                y: 0
                                height: lineBox.height
                                // 宽度不加 Behavior：推进速度已经由上面 60ms 的墙钟插值给平了，
                                // 再叠一层缓动只会让亮色落后于唱词。
                                width: Math.max(0, Math.min(lineBox.textW,
                                                            lineText.width - 2 * inset) * lyricRow.karaoke)
                                clip: true
                                Text {
                                    x: -sweepZone.inset
                                    width: lineText.width
                                    height: lineBox.height
                                    text: lineText.text
                                    font.family: lineText.font.family
                                    font.pixelSize: lineText.font.pixelSize
                                    font.bold: lineText.font.bold
                                    color: theme.accent
                                    wrapMode: Text.Wrap
                                    verticalAlignment: Text.AlignVCenter
                                    horizontalAlignment: lineText.horizontalAlignment
                                }
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
                            if (!lyricView.userScrolling && app.currentLyricIndex >= 0)
                                lyricView.centerOn(app.currentLyricIndex)
                        }
                    }
                    onHeightChanged: refollow.restart()
                    onWidthChanged: refollow.restart()
                    onModelChanged: {
                        userScrolling = false
                        positionViewAtBeginning()
                    }
                    onMovementStarted: userScrolling = true
                    onMovementEnded: resumeFollow.restart()

                    Timer {
                        id: resumeFollow
                        interval: 5000
                        onTriggered: {
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
            }

        }
    }
}
