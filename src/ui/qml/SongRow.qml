import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

// 统一歌曲行：搜索结果 / 收藏 / 历史 / 队列都复用它
// 样式参照 AlgerMusicPlayer StandardSongItem：
//   无边框、悬停浅底、48px 圆角 12 封面、右侧操作胶囊 + 圆形播放键
Rectangle {
    id: root

    property int rowNumber: -1
    property string title: ""
    property string artist: ""
    property string album: ""
    property string cover: ""
    property string sourceName: ""
    property string sourceId: ""
    property color accent: "#ec4141"
    property int duration: 0
    property bool vip: false
    property bool favorite: false
    property bool playing: false
    property bool showAdd: true
    property bool showFavorite: true
    property bool showDownload: false
    property bool showRemove: false
    // 该行是否提供「播放 MV」入口（由使用处按平台的 MV 支持情况传入）
    property bool showMv: false
    // 交错入场序号（传 index % 30）；< 0 表示不参与交错
    property int stagger: -1
    // 触屏设备没有悬停：操作胶囊常驻，行内只留「更多」入口
    // 用 touchPrimary 而不是 isMobile：车机跑桌面外壳时同样是手指操作
    readonly property bool touchMode: platform.touchPrimary
    readonly property bool hasActions: showFavorite || showAdd || showDownload || showRemove

    // 「更多」菜单的数据源：与上面的 show* 开关保持一致
    readonly property var actionModel: {
        var m = []
        if (showFavorite) m.push("favorite")
        if (showAdd) m.push("add")
        if (showMv) m.push("mv")
        if (showDownload) m.push("download")
        if (showRemove) m.push("remove")
        return m
    }

    function triggerAction(kind) {
        if (kind === "favorite") favoriteRequested()
        else if (kind === "add") addRequested()
        else if (kind === "mv") mvRequested()
        else if (kind === "download") downloadRequested()
        else if (kind === "remove") removeRequested()
    }

    function actionLabel(kind) {
        if (kind === "favorite") return root.favorite ? "取消收藏" : "收藏"
        if (kind === "add") return "加入播放队列"
        if (kind === "mv") return "播放 MV"
        if (kind === "download") return "下载到本地"
        return "从播放队列移除"
    }

    signal playRequested()
    signal favoriteRequested()
    signal addRequested()
    signal mvRequested()
    signal downloadRequested()
    signal removeRequested()

    readonly property color cText: theme.text
    readonly property color cSub: theme.subText
    readonly property color cLine: theme.line
    readonly property color cHover: theme.cardHover
    readonly property color cChip: theme.dark ? Qt.rgba(1, 1, 1, 0.08)
                                              : Qt.rgba(0, 0, 0, 0.06)
    readonly property color cSoft: Qt.rgba(accent.r, accent.g, accent.b, 0.12)

    implicitHeight: 64
    radius: 16
    // 车机和手机没有 hover：按下必须有反馈，否则点一下像没反应。按下时叠一层主色底并
    // 轻微内收，松手由 Behavior 回弹。
    color: hover.pressed ? Qt.rgba(accent.r, accent.g, accent.b, 0.16)
                         : (hover.hovered ? cHover : "transparent")
    Behavior on color { ColorAnimation { duration: 140 } }
    scale: hover.pressed ? 0.985 : 1.0
    Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

    function mmss(ms) {
        if (isNaN(ms) || ms < 0)
            ms = 0
        var t = Math.floor(ms / 1000)
        var m = Math.floor(t / 60)
        var s = t % 60
        return (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s
    }

    // ---- 交错入场：fadeInUp，带兜底 Timer 防止动画中断卡在 0 ----
    opacity: 1
    Component.onCompleted: {
        if (stagger >= 0) {
            rowEntrance.restart()
            rowSettle.restart()
        }
    }
    SequentialAnimation {
        id: rowEntrance
        PauseAnimation { duration: Math.max(0, (root.stagger % 20) * 40) }
        NumberAnimation {
            target: root
            property: "opacity"
            from: 0
            to: 1
            duration: 460
            easing.type: Easing.OutCubic
        }
    }
    Timer {
        id: rowSettle
        interval: 1800
        onTriggered: root.opacity = 1
    }

    MouseArea {
        id: hover
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        onClicked: root.playRequested()
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 8
        spacing: 12

        Text {
            visible: root.rowNumber >= 0
            Layout.preferredWidth: 24
            text: root.playing ? "▶" : (root.rowNumber + 1 < 10
                                        ? "0" + (root.rowNumber + 1) : (root.rowNumber + 1))
            color: root.playing ? accent : cSub
            font.pixelSize: 13
            font.italic: true
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
        }

        // 封面：没有封面时用平台色兜底
        Rectangle {
            Layout.preferredWidth: 48
            Layout.preferredHeight: 48
            radius: 12
            color: cSoft
            clip: true
            Text {
                anchors.centerIn: parent
                text: "♪"
                color: accent
                font.pixelSize: 20
                visible: coverImage.status !== Image.Ready
            }
            Image {
                id: coverImage
                anchors.fill: parent
                anchors.margins: 1
                source: root.cover
                sourceSize.width: 96
                sourceSize.height: 96
                fillMode: Image.PreserveAspectCrop
                opacity: status === Image.Ready ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
                asynchronous: true
                cache: true
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Text {
                    Layout.fillWidth: true
                    text: root.title.length > 0 ? root.title : "未知曲目"
                    color: root.playing ? accent : cText
                    font.pixelSize: 14
                    elide: Text.ElideRight
                    Behavior on color { ColorAnimation { duration: 140 } }
                }
                Rectangle {
                    visible: root.vip
                    width: 30
                    height: 16
                    radius: 8
                    color: theme.vip
                    Text {
                        anchors.centerIn: parent
                        text: "VIP"
                        color: "#FFFFFF"
                        font.pixelSize: theme.fontXs
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Text {
                    Layout.fillWidth: true
                    text: root.artist + (root.album.length > 0 ? " · " + root.album : "")
                    color: cSub
                    font.pixelSize: theme.fontSm
                    elide: Text.ElideRight
                }
                Rectangle {
                    visible: root.sourceName.length > 0
                    width: sourceLabel.implicitWidth + 12
                    height: 18
                    radius: 9
                    color: cSoft
                    Text {
                        id: sourceLabel
                        anchors.centerIn: parent
                        text: root.sourceName
                        color: accent
                        font.pixelSize: theme.fontXs
                    }
                }
                Text {
                    visible: root.duration > 0 && !hover.hovered
                    text: root.mmss(root.duration)
                    color: cSub
                    font.pixelSize: theme.fontSm
                    Behavior on opacity { NumberAnimation { duration: 120 } }
                }
            }
        }

        // ---- 操作胶囊：悬停浮现（参考项目同款圆角胶囊容器）----
        Rectangle {
            id: capsule
            // 胶囊内每枚按钮都是触控目标，胶囊高度按 theme.touchMin 给足，
            // 行高 64 容得下（居中后上下各留 8），不会与相邻行重叠
            Layout.preferredHeight: theme.touchMin
            Layout.preferredWidth: capsuleRow.implicitWidth + 8
            radius: height / 2
            color: root.playing ? cSoft : theme.dark ? Qt.rgba(1, 1, 1, 0.05) : "#FFFFFF"
            border.color: root.playing ? Qt.rgba(accent.r, accent.g, accent.b, 0.4) : cLine
            border.width: 1
            // 触屏没有 hover 事件：胶囊必须常驻，否则整行所有操作都点不到
            opacity: root.touchMode || hover.hovered || root.playing ? 1 : 0
            // 隐藏时不能再吃掉点击：否则整行右侧有一块看不见但仍可点的热区
            enabled: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

            Row {
                id: capsuleRow
                anchors.centerIn: parent
                spacing: 2

                // 收藏（触屏端收进「更多」菜单）
                Item {
                    width: theme.touchMin; height: theme.touchMin
                    visible: root.showFavorite && !root.touchMode
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: root.favorite ? "heartFill" : "heart"
                        size: 17
                        color: root.favorite ? "#e64c4c" : root.cSub
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.favoriteRequested()
                    }
                }

                // 加入队列
                Item {
                    width: theme.touchMin; height: theme.touchMin
                    visible: root.showAdd && !root.touchMode
                    Text {
                        anchors.centerIn: parent
                        text: "＋"
                        color: root.cSub
                        font.pixelSize: 15
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.addRequested()
                    }
                }

                // 播放 MV（桌面端胶囊直达；触屏端没有 hover，走「更多」菜单）
                Item {
                    width: theme.touchMin; height: theme.touchMin
                    visible: root.showMv && !root.touchMode
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: "video"
                        size: 15
                        color: root.cSub
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.mvRequested()
                    }
                }

                // 下载
                Item {
                    width: theme.touchMin; height: theme.touchMin
                    visible: root.showDownload && !root.touchMode
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: "download"
                        size: 15
                        color: root.cSub
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.downloadRequested()
                    }
                }

                // 移除
                Item {
                    width: theme.touchMin; height: theme.touchMin
                    visible: root.showRemove && !root.touchMode
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: "close"
                        size: 15
                        color: root.cSub
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.removeRequested()
                    }
                }

                // 圆形播放键：悬停/播放中变主色
                AbstractButton {
                    id: rowPlay
                    width: theme.touchMin
                    height: theme.touchMin
                    scale: rowPlayHover.containsMouse ? 1.08 : 1.0
                    Behavior on scale { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
                    background: Rectangle {
                        radius: height / 2
                        color: root.playing || rowPlay.pressed
                               ? accent
                               : rowPlayHover.containsMouse
                                 ? Qt.rgba(accent.r, accent.g, accent.b, 0.16) : cChip
                        border.color: root.playing ? accent : cLine
                        border.width: root.playing ? 0 : 1
                        Behavior on color { ColorAnimation { duration: 140 } }
                    }
                    contentItem: Item {
                        GlyphIcon {
                            anchors.centerIn: parent
                            name: root.playing ? "pause" : "play"
                            size: 13
                            color: root.playing ? "#FFFFFF" : root.cText
                        }
                    }
                    MouseArea {
                        id: rowPlayHover
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.playRequested()
                    }
                }

                // 触屏端：行内操作全部收进这个「更多」菜单
                Item {
                    width: theme.touchMin
                    height: theme.touchMin
                    visible: root.touchMode && root.hasActions
                    GlyphIcon {
                        anchors.centerIn: parent
                        name: "more"
                        size: 16
                        color: root.cSub
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: actionMenu.popup()
                    }
                }
            }
        }

        // 触屏端的「更多」菜单：Instantiator + insertItem/removeItem 动态挂载，
        // 不用 MenuItem.visible（Qt Quick Controls 的菜单模型会忽略不可见项的行为不统一）
        Menu {
            id: actionMenu
            padding: 6
            background: Rectangle {
                implicitWidth: 200
                radius: 14
                color: theme.card
                border.color: cLine
                border.width: 1
            }
            Instantiator {
                model: root.actionModel
                delegate: MenuItem {
                    id: actionItem
                    height: Math.max(44, theme.touchMin)
                    contentItem: Text {
                        text: root.actionLabel(modelData)
                        color: cText
                        font.pixelSize: 14
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: 8
                    }
                    background: Rectangle {
                        radius: 10
                        color: actionItem.hovered ? cHover : "transparent"
                    }
                    onClicked: {
                        actionMenu.close()
                        root.triggerAction(modelData)
                    }
                }
                onObjectAdded: function(index, object) { actionMenu.insertItem(index, object) }
                onObjectRemoved: function(index, object) { actionMenu.removeItem(object) }
            }
        }

    }
}
