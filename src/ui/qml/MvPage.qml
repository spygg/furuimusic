import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15

// MV 沉浸式播放器（交互参考 AlgerMusicPlayer 的 MvPlayer.vue）：
//   全屏黑底 + 视频 contain + 上下渐变控制条自动隐藏 + 进度/音量/静音/
//   循环模式/全屏/上下曲 + 单击暂停 + 双击左右快退快进、中间全屏。
// 纯 UI 层不 import QtMultimedia —— Qt5/Qt6 的 MediaPlayer API 有差异，
// 媒体原语由 MvMedia5.qml / MvMedia5Ffmpeg.qml / MvMedia6.qml 提供，
// 按 platform.qtMajor 与 platform.ffmpegBackend 动态加载。
// 外壳监听 app.mvOpened 后切到本页（桌面/移动外壳都挂在 index 4）。
Rectangle {
    id: root
    color: "#000000"

    // MV 全屏恒黑：亮色主题下状态栏是深色图标，压在黑底上时间/电量全都看不见。
    // 外壳用 opacity+enabled 控制 MV 显隐（不能动 visible，Android 会销毁 video
    // sink），所以监听 enabled：进 MV 强制浅色图标，退出恢复跟随主题。
    // MV 期间临时预览深色主题：ColorOS 在浅色主题下会持续强制
    // LIGHT_STATUS_BARS（白底深字），App 侧 setSystemBarsAppearance 会被系统改回，
    // 导致白状态栏压在黑色 MV 上割裂。MV 页本来全黑，切深色用户无感；
    // 退出时用 previewMode 恢复原主题（不写持久化设置）。
    property string savedThemeMode: ""

    function syncScreenState(on) {
        if (on) {
            savedThemeMode = theme.mode
            if (theme.mode !== "dark")
                theme.previewMode("dark")
        } else if (savedThemeMode.length > 0) {
            theme.previewMode(savedThemeMode)
            savedThemeMode = ""
        }
        theme.statusBarOverride = on ? 1 : -1
        // 进 MV 会暂停音乐，屏幕常亮不能只跟着音乐播放态；单独告知原生侧。
        app.mvScreenOn = on
    }
    onEnabledChanged: syncScreenState(enabled)
    // 外壳重建/页面销毁时可能来不及走 enabled→false，留下全局 override；
    // 只在仍由本页置位时才复位，避免误伤另一个 MV 实例。
    Component.onDestruction: {
        if (theme.statusBarOverride === 1)
            syncScreenState(false)
    }
    onVisibleChanged: {
        if (!visible && theme.statusBarOverride === 1)
            syncScreenState(false)
    }

    signal requestClose()

    property string mvTitle: ""
    property string mvArtist: ""
    property string mvCover: ""
    property url currentUrl
    property bool hasPrev: false
    property bool hasNext: false

    readonly property var _media: videoLoader.item
    readonly property bool compact: root.width < 700
    readonly property bool touchLike: !platform.isDesktop
    // 触屏优先的车机/平板按触控最小值给按钮，桌面鼠标维持紧凑尺寸
    readonly property int ctrlSize: platform.touchPrimary ? theme.touchMin : 40
    property bool controlsVisible: true
    property bool modeSingle: false
    property bool isFullscreen: false
    property real volume: 1.0
    property bool muted: false
    property string seekHint: ""
    property string modeHint: ""
    property bool lastEnded: false
    property double lastDoubleTap: 0

    readonly property color ink: "#FFFFFF"
    readonly property color inkSoft: Qt.rgba(1, 1, 1, 0.66)
    readonly property color chipBg: Qt.rgba(1, 1, 1, 0.14)

    // 暗色页固定浅色文字，不随主题明暗变化
    function open(mv) {
        releaseTimer.stop()
        if (!videoLoader.active) videoLoader.active = true
        mvTitle = mv.title || ""
        mvArtist = mv.artist || ""
        mvCover = mv.cover || ""
        hasPrev = !!mv.hasPrev
        hasNext = !!mv.hasNext
        lastEnded = false
        seekHint = ""
        controlsVisible = true
        idleTimer.restart()
        openOrQueue(mv.url)
    }

    function close() {
        singleTap.stop()
        idleTimer.stop()
        if (videoLoader.item) videoLoader.item.close()
        if (root.isFullscreen) root.setFullscreen(false)
        requestClose()
        // 桌面端页面淡出后销毁媒体项，真正释放解码器和最后一帧纹理
        if (Qt.platform.os !== "android") releaseTimer.restart()
    }

    function wake() {
        controlsVisible = true
        idleTimer.restart()
    }

    function togglePlay() {
        if (root._media) root._media.togglePlay()
        root.wake()
    }

    function setFullscreen(on) {
        // Android：锁定横屏 + 沉浸式隐藏系统栏（native 实现），退出时恢复自动方向；
        // 窗口本身始终占满屏幕，不需要再 showFullScreen。
        // 桌面：保持窗口级 showFullScreen/showNormal。
        if (Qt.platform.os === "android")
            app.setMvFullscreen(on)
        else {
            const w = root.Window.window
            if (w) {
                if (on) w.showFullScreen()
                else w.showNormal()
            }
        }
        root.isFullscreen = on
        root.wake()
    }

    function skipBy(ms) {
        const m = root._media
        if (!m) return
        const end = m.duration > 0 ? m.duration : m.position + Math.abs(ms)
        m.seekTo(Math.max(0, Math.min(end, m.position + ms)))
        root.seekHint = ms > 0 ? "+10s" : "-10s"
        hintTimer.restart()
        root.wake()
    }

    function toggleMode() {
        modeSingle = !modeSingle
        modeHint = modeSingle ? "单曲循环" : "列表自动"
        modeHintTimer.restart()
        root.wake()
    }

    function handleKey(key) {
        root.wake()
        if (key === "play") root.togglePlay()
        else if (key === "prev" && root.hasPrev) app.playMvPrev()
        else if (key === "next" && root.hasNext) app.playMvNext()
        else if (key === "mute") root.muted = !root.muted
        else if (key === "mode") root.toggleMode()
        else if (key === "full") root.setFullscreen(!root.isFullscreen)
    }

    // 视频铺满整个页面，由 VideoOutput 的 PreserveAspectFit 居中留黑边，
    // 竖屏/横屏/车机分屏都不用再按 16:9 手工算高度
    // Qt6 官方 FFmpeg 插件 / Qt5 自编译 FFmpeg / Qt5 DirectShow（仅兜底）
    Loader {
        id: videoLoader
        anchors.fill: parent
        source: platform.qtMajor >= 6 ? "MvMedia6.qml"
              : platform.ffmpegBackend ? "MvMedia5Ffmpeg.qml"
              : "MvMedia5.qml"
        onItemChanged: {
            if (!item) return
            // Loader.item 的属性不能在声明式里直接绑定，用 Qt.binding 补上。
            // app.ducked：Android 音频焦点 CAN_DUCK（导航播报）时把 MV 音量也压到 30%
            item.volume = Qt.binding(function() { return root.volume * (app.ducked ? 0.3 : 1.0) })
            item.muted = Qt.binding(function() { return root.muted })
            if (root.pendingUrl.length > 0) {
                item.open(root.pendingUrl)
                root.pendingUrl = ""
            }
        }
    }

    // 桌面端退出 MV 后销毁媒体项：close() 只做 pause（Android 上销毁正在渲染的
    // sink 会 SIGABRT，见 MvMedia6.close 注释），解码器、帧队列和 VideoOutput
    // 的最后一帧会一直占着内存，1080p/4K 动辄几十上百 MB。桌面没有这个崩溃问题，
    // 等页面彻底隐藏后把 Loader 置为 inactive 即释放；再次播放时 open() 重新加载。
    Timer {
        id: releaseTimer
        interval: 800
        onTriggered: {
            if (Qt.platform.os === "android") return
            if (root.opacity > 0.05) { restart(); return }
            videoLoader.active = false
        }
    }

    // Android 音频焦点被其它应用抢走时（LOSS/LOSS_TRANSIENT），音乐播放器
    // 与 MV 沉浸页一起暂停——不能只停音乐、MV 还在响
    Connections {
        target: app
        function onMvPauseRequested() {
            if (root._media && root._media.playing)
                root.togglePlay()
        }
    }

    // 加载期间用封面做淡化底图，避免开头一段死黑
    Image {
        anchors.fill: parent
        z: 2
        source: root.mvCover
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        // 只是半透明背景，按显示容器缩到 320x180 就够，别解整张高清封面
        sourceSize.width: 320
        sourceSize.height: 180
        opacity: root._media && root._media.busy && root.mvCover.length > 0 ? 0.26 : 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: 320 } }
    }

    // 交互层放在 Loader 外面：Loader 的静态子项会被加载进来的媒体项压住
    // （绘制能靠 z 顶上来，鼠标事件却到不了）
    MouseArea {
        id: stageArea
        anchors.fill: parent
        z: 5
        hoverEnabled: true
        cursorShape: root.controlsVisible ? Qt.ArrowCursor : Qt.BlankCursor
        onPositionChanged: if (!root.touchLike) root.wake()
        onClicked: {
            // 工具条隐藏时，第一次点击只负责把它唤出来，不改变播放状态；
            // 工具条已显示时再点才是播放/暂停。
            if (!root.controlsVisible) { root.wake(); return }
            root.wake()
            // 双击的第二次 click 不能又触发一次播放/暂停
            if (Date.now() - root.lastDoubleTap < 350) return
            // 单击画面切换播放/暂停：桌面鼠标、触屏点按都走这条。
            // 延时 230ms 由 singleTap 等双击；真双击时 onDoubleClicked 会 stop 掉它。
            // 旧版只给「桌面宽屏鼠标」开了这条，触屏点画面毫无反应（只有暂停时的
            // 中央大播放按钮能点），移动端补上。
            singleTap.restart()
        }
        onDoubleClicked: function (mouse) {
            root.lastDoubleTap = Date.now()
            singleTap.stop()
            if (mouse.x < root.width * 0.33) root.skipBy(-10000)
            else if (mouse.x > root.width * 0.67) root.skipBy(10000)
            else root.setFullscreen(!root.isFullscreen)
        }
    }

    // 加载 / 缓冲
    BusyIndicator {
        anchors.centerIn: parent
        z: 12
        running: root._media ? root._media.busy : false
        visible: running
    }

    // 移动端：暂停时给一枚大的中央播放按钮（参考项目的中心播放键）
    Rectangle {
        anchors.centerIn: parent
        z: 12
        width: 72
        height: 72
        radius: 36
        visible: root.touchLike && root.compact && root._media
                 && !root._media.playing && !root._media.busy && !root._media.failed
        color: Qt.rgba(0, 0, 0, 0.42)
        border.color: Qt.rgba(1, 1, 1, 0.28)
        border.width: 1
        GlyphIcon {
            anchors.centerIn: parent
            name: "play"
            size: 26
            color: root.ink
        }
        MouseArea {
            anchors.fill: parent
            onClicked: root.togglePlay()
        }
    }

    // 渲染失败：区分「有 FFmpeg 后端但解码失败」与「Qt5 根本没编 FFmpeg」。
    // 给准确说明 + 系统播放器出口，而不是死黑屏。
    Column {
        anchors.centerIn: parent
        z: 12
        spacing: theme.spaceMd
        visible: root._media && root._media.failed
        width: Math.min(parent.width - 48, 520)

        Text {
            width: parent.width
            text: platform.videoBackend !== "directshow"
                  ? ("MV 播放失败：" + (root._media && root._media.errorMessage.length > 0
                                       ? root._media.errorMessage : "未知错误"))
                  : "当前为 Qt 5 桌面构建且未启用自编译 FFmpeg 后端（DirectShow 无法解 MP4）。运行 src/scripts/build_ffmpeg_desktop.bat 后重新构建，或改用 Qt 6 构建，也可用系统播放器打开。"
            color: root.ink
            font.pixelSize: theme.fontMd
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: theme.spaceSm

            Rectangle {
                width: sysRow.implicitWidth + 32
                height: theme.touchMin
                radius: height / 2
                color: sysMa.containsMouse ? Qt.rgba(1, 1, 1, 0.22) : root.chipBg
                Row {
                    id: sysRow
                    anchors.centerIn: parent
                    spacing: 6
                    GlyphIcon { name: "expand"; size: 15; color: root.ink }
                    Text { text: "用系统播放器打开"; color: root.ink; font.pixelSize: theme.fontSm
                           anchors.verticalCenter: parent.verticalCenter }
                }
                MouseArea {
                    id: sysMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (root.currentUrl.toString().length > 0)
                                   Qt.openUrlExternally(root.currentUrl)
                }
            }

            Rectangle {
                width: retryRow.implicitWidth + 32
                height: theme.touchMin
                radius: height / 2
                color: retryMa.containsMouse ? Qt.rgba(1, 1, 1, 0.22) : root.chipBg
                Row {
                    id: retryRow
                    anchors.centerIn: parent
                    spacing: 6
                    GlyphIcon { name: "refresh"; size: 15; color: root.ink }
                    Text { text: "重试"; color: root.ink; font.pixelSize: theme.fontSm
                           anchors.verticalCenter: parent.verticalCenter }
                }
                MouseArea {
                    id: retryMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (root.currentUrl.toString().length > 0)
                                   root.openOrQueue(root.currentUrl)
                }
            }
        }
    }

    // 播完给「重播 / 下一个」，不然画面停在最后一帧像卡死
    Column {
        anchors.centerIn: parent
        z: 12
        spacing: theme.spaceSm
        visible: root._media && root._media.ended && !root.modeSingle

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: theme.spaceSm

            Rectangle {
                width: replayRow.implicitWidth + 28
                height: theme.touchMin
                radius: height / 2
                color: root.chipBg
                Row {
                    id: replayRow
                    anchors.centerIn: parent
                    spacing: 6
                    GlyphIcon { name: "refresh"; size: 15; color: root.ink }
                    Text { text: "重新播放"; color: root.ink; font.pixelSize: theme.fontSm
                           anchors.verticalCenter: parent.verticalCenter }
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: { root.lastEnded = false; root._media.replay(); root.wake() }
                }
            }

            Rectangle {
                width: nextRow.implicitWidth + 28
                height: theme.touchMin
                radius: height / 2
                color: root.chipBg
                visible: root.hasNext
                Row {
                    id: nextRow
                    anchors.centerIn: parent
                    spacing: 6
                    GlyphIcon { name: "next"; size: 15; color: root.ink }
                    Text { text: "下一个 MV"; color: root.ink; font.pixelSize: theme.fontSm
                           anchors.verticalCenter: parent.verticalCenter }
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: app.playMvNext()
                }
            }
        }
    }

    // 模式切换 / 双击快进的中央短提示
    Rectangle {
        anchors.centerIn: parent
        z: 13
        width: Math.max(112, hintLabel.implicitWidth + 36)
        height: 48
        radius: theme.radiusMd
        color: Qt.rgba(0, 0, 0, 0.72)
        visible: root.modeHint.length > 0 || root.seekHint.length > 0
        Text {
            id: hintLabel
            anchors.centerIn: parent
            text: root.seekHint.length > 0 ? root.seekHint : root.modeHint
            color: root.ink
            font.pixelSize: theme.fontMd
            font.bold: true
        }
    }

    // 顶部栏：返回 + 标题/艺人 + 解码后端徽标
    Rectangle {
        id: topBar
        z: 14
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: Math.max(76, theme.touchMin + 28)
        gradient: Gradient {
            orientation: Gradient.Vertical
            GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0.80) }
            GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0) }
        }
        opacity: root.controlsVisible ? 1 : 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: 220 } }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: theme.spaceMd
            anchors.rightMargin: theme.spaceMd
            spacing: theme.spaceMd

            Rectangle {
                Layout.preferredWidth: theme.touchMin
                Layout.preferredHeight: theme.touchMin
                Layout.alignment: Qt.AlignVCenter
                radius: 10
                color: closeArea.containsMouse ? Qt.rgba(1, 1, 1, 0.24) : root.chipBg
                Behavior on color { ColorAnimation { duration: 160 } }
                GlyphIcon {
                    anchors.centerIn: parent
                    name: "chevronDown"
                    size: 17
                    color: root.ink
                }
                MouseArea {
                    id: closeArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.close()
                }
            }

            Column {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                spacing: 2
                Text {
                    width: parent.width
                    text: root.mvTitle
                    color: root.ink
                    font.pixelSize: theme.fontLg
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: root.mvArtist
                    color: root.inkSoft
                    font.pixelSize: theme.fontSm
                    elide: Text.ElideRight
                    visible: text.length > 0
                }
            }

            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                visible: !root.compact
                // RowLayout 的直接子项：raw width/height 会被布局清零
                Layout.preferredWidth: backendText.implicitWidth + 20
                Layout.preferredHeight: 26
                radius: 13
                color: root.chipBg
                Text {
                    id: backendText
                    anchors.centerIn: parent
                    text: platform.videoBackend === "ffmpeg" ? "FFmpeg 解码"
                        : platform.videoBackend === "ffmpeg-qt5" ? "FFmpeg 自编译" : "DirectShow"
                    color: root.inkSoft
                    font.pixelSize: theme.fontXs
                }
            }
        }
    }

    // 底部控制条：进度 + 播放控制 + 音量 + 模式 + 全屏
    Rectangle {
        id: controlBar
        z: 14
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: root.compact ? 104 : 96
        gradient: Gradient {
            orientation: Gradient.Vertical
            GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0) }
            GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, 0.88) }
        }
        opacity: root.controlsVisible ? 1 : 0
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: 220 } }

        ColumnLayout {
            anchors.fill: parent
            anchors.leftMargin: theme.spaceMd
            anchors.rightMargin: theme.spaceMd
            anchors.bottomMargin: theme.spaceSm
            spacing: root.compact ? 2 : 6

            Slider {
                id: progress
                Layout.fillWidth: true
                Layout.preferredHeight: 22
                from: 0
                to: Math.max(1, root._media ? root._media.duration : 0)
                value: root._media ? root._media.position : 0
                enabled: root._media && root._media.seekable
                onMoved: {
                    if (root._media) root._media.seekTo(value)
                    root.wake()
                }
                // 拖动时 Slider 会写 value 打断绑定，松手后用 Qt.binding 恢复，
                // 与 PlayBar/PlayerPage 的进度条处理方式一致。
                onPressedChanged: {
                    if (!pressed)
                        value = Qt.binding(function () {
                            return root._media ? root._media.position : 0
                        })
                }
                background: Rectangle {
                    x: progress.leftPadding
                    y: progress.topPadding + progress.availableHeight / 2 - height / 2
                    width: progress.availableWidth
                    height: 4
                    radius: 2
                    color: Qt.rgba(1, 1, 1, 0.24)
                    Rectangle {
                        // 水平 Slider 的 position === visualPosition；统一用逻辑 position，
                        // 与 PlayBar 的竖滑块/顶边进度条同一套口径。
                        width: progress.position * parent.width
                        height: parent.height
                        radius: 2
                        color: root.ink
                    }
                }
                handle: Rectangle {
                    x: progress.leftPadding + progress.position * (progress.availableWidth - width)
                    y: progress.topPadding + progress.availableHeight / 2 - height / 2
                    width: 12
                    height: 12
                    radius: 6
                    color: root.ink
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: root.compact ? 2 : 6

                Repeater {
                    model: ["prev", "play", "next", "mute", "mode", "full"]
                    delegate: Rectangle {
                        readonly property string key: modelData
                        readonly property string glyph:
                            key === "play" ? (root._media && root._media.playing ? "pause" : "play")
                          : key === "mute" ? ((root.muted || root.volume < 0.02) ? "volumeMute" : "volume")
                          : key === "mode" ? (root.modeSingle ? "repeatOne" : "repeat")
                          : key === "full" ? (root.isFullscreen ? "close" : "expand")
                          : key === "prev" ? "prev" : "next"
                        readonly property bool usable:
                            (key !== "prev" || root.hasPrev) && (key !== "next" || root.hasNext)
                        readonly property bool shown: !root.compact
                            || (key !== "prev" && key !== "next")

                        Layout.preferredWidth: root.ctrlSize
                        Layout.preferredHeight: root.ctrlSize
                        Layout.alignment: Qt.AlignVCenter
                        visible: shown
                        radius: height / 2
                        color: btnArea.containsMouse && usable ? Qt.rgba(1, 1, 1, 0.20)
                             : (key === "play" ? root.chipBg : "transparent")
                        opacity: usable ? 1 : 0.32

                        GlyphIcon {
                            anchors.centerIn: parent
                            name: parent.glyph
                            size: parent.key === "play" ? 15 : 17
                            color: root.ink
                        }
                        MouseArea {
                            id: btnArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: if (parent.usable) root.handleKey(parent.key)
                        }
                    }
                }

                Text {
                    Layout.alignment: Qt.AlignVCenter
                    text: root.mmss(root._media ? root._media.position : 0)
                          + " / " + root.mmss(root._media ? root._media.duration : 0)
                    color: root.inkSoft
                    font.pixelSize: theme.fontXs
                }

                Item { Layout.fillWidth: true }

                Slider {
                    id: volumeSlider
                    Layout.preferredWidth: 76
                    Layout.preferredHeight: platform.touchPrimary ? theme.touchMin : 26
                    Layout.alignment: Qt.AlignVCenter
                    visible: !root.compact
                    from: 0
                    to: 1
                    value: root.muted ? 0 : root.volume
                    onMoved: {
                        root.volume = value
                        root.muted = false
                        root.wake()
                    }
                    onPressedChanged: {
                        if (!pressed)
                            value = Qt.binding(function () {
                                return root.muted ? 0 : root.volume
                            })
                    }
                    background: Rectangle {
                        x: volumeSlider.leftPadding
                        y: volumeSlider.topPadding + volumeSlider.availableHeight / 2 - height / 2
                        width: volumeSlider.availableWidth
                        height: 3
                        radius: 2
                        color: Qt.rgba(1, 1, 1, 0.24)
                        Rectangle {
                            width: volumeSlider.position * parent.width
                            height: parent.height
                            radius: 2
                            color: root.ink
                        }
                    }
                    handle: Rectangle {
                        x: volumeSlider.leftPadding
                           + volumeSlider.position * (volumeSlider.availableWidth - width)
                        y: volumeSlider.topPadding + volumeSlider.availableHeight / 2 - height / 2
                        width: 10
                        height: 10
                        radius: 5
                        color: root.ink
                    }
                }
            }
        }
    }

    // 控制条自动隐藏：播放中静止 3 秒收起；暂停 / 拖动进度 / 缓冲时保持
    Timer {
        id: idleTimer
        interval: 3000
        repeat: false
        onTriggered: {
            if (root._media && root._media.playing && !root._media.busy && !progress.pressed)
                root.controlsVisible = false
        }
    }
    Timer {
        id: singleTap
        interval: 230
        repeat: false
        onTriggered: root.togglePlay()
    }
    Timer {
        id: hintTimer
        interval: 700
        repeat: false
        onTriggered: root.seekHint = ""
    }
    Timer {
        id: modeHintTimer
        interval: 1400
        repeat: false
        onTriggered: root.modeHint = ""
    }
    // 单曲循环：靠轮询识别「刚播完」这一拍，不依赖 var 属性的 changed 信号
    Timer {
        id: endWatch
        interval: 260
        repeat: true
        // 外壳用 opacity 而不是 visible 隐藏 MV（保住 video sink，ColorOS 上
        // visible=false 会 SIGABRT），所以光看 visible 会以为它一直开着；
        // 不判断 opacity 的话这个 260ms 定时器从启动起就一直跑。
        running: root.visible && root.opacity > 0.01 && root._media
        onTriggered: {
            const m = root._media
            if (!m) return
            if (m.ended && !root.lastEnded) {
                root.lastEnded = true
                root.controlsVisible = true
                if (root.modeSingle) m.replay()
            } else if (!m.ended) {
                root.lastEnded = false
            }
        }
    }

    // 外壳的 onMvOpened 可能在 Loader 完成前到达：先存住 URL，Loader 就绪后补开
    property string pendingUrl: ""
    onPendingUrlChanged: {
        if (pendingUrl.length > 0 && videoLoader.item) {
            videoLoader.item.open(pendingUrl)
            pendingUrl = ""
        }
    }

    function openOrQueue(url) {
        currentUrl = url
        if (videoLoader.item) { videoLoader.item.open(url); return }
        pendingUrl = url
    }

    function mmss(ms) {
        if (isNaN(ms) || ms < 0) ms = 0
        var t = Math.floor(ms / 1000)
        var m = Math.floor(t / 60)
        var s = t % 60
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }
}
