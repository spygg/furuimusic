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
    readonly property color cLine: theme.line
    readonly property color cSoft: theme.accentSoft

    property string hint: ""
    property int tabIndex: 0
    // 0 = 全部平台
    property int selectedPlatform: 0
    // 点开歌单后显示曲目详情，空串表示停留在歌单列表
    property string openPlaylistTitle: ""
    // 歌单封面卡片墙 / 列表
    property bool gridView: true
    readonly property int gridColumns: Math.max(2, Math.min(6,
        Math.floor(Math.max(340, libraryArea.width) / 170)))
    readonly property real gridCellWidth: Math.max(120, libraryArea.width / gridColumns)
    readonly property real gridCellHeight: gridCellWidth + 76

    readonly property var tabTitles: ["我的歌单", "收藏", "播放历史", "播放队列"]

    function selectedSourceId() {
        if (selectedPlatform <= 0)
            return ""
        if (selectedPlatform - 1 < app.sourceIds.length)
            return app.sourceIds[selectedPlatform - 1]
        return ""
    }

    function reloadSongs() {
        root.openPlaylistTitle = ""
        var sid = root.selectedSourceId()
        if (root.tabIndex === 1)
            app.loadFavoritesBySource(sid)
        else if (root.tabIndex === 2)
            app.loadHistoryBySource(sid)
    }

    function openPlaylist(title, uid) {
        app.loadPlaylist(uid)
        root.openPlaylistTitle = title
    }

    Connections {
        target: app
        onPlaylistsLoaded: root.hint = "歌单列表已刷新"
        onSyncFinished: root.hint = (ok ? "同步完成：" : "同步失败：") + message
    }

    Component.onCompleted: app.refreshPlaylists()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        // 页头
        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Rectangle {
                Layout.preferredWidth: 42
                Layout.preferredHeight: 42
                radius: 12
                gradient: Gradient {
                    GradientStop { position: 0.0; color: theme.heroStart }
                    GradientStop { position: 1.0; color: theme.heroEnd }
                }
                Text {
                    anchors.centerIn: parent
                    text: "♫"
                    color: "#FFFFFF"
                    font.pixelSize: 20
                }
            }

            ColumnLayout {
                spacing: 2
                Text {
                    text: "我的音乐"
                    color: cText
                    font.pixelSize: 18
                    font.bold: true
                }
                Text {
                    text: "各平台歌单、收藏与播放历史在这里聚合"
                    color: cSub
                    font.pixelSize: 11
                }
            }

            Item { Layout.fillWidth: true }

            Rectangle {
                width: 82
                height: 30
                radius: 15
                color: root.gridView
                       ? Qt.rgba(cPrimary.r, cPrimary.g, cPrimary.b, 0.14)
                       : "transparent"
                border.color: root.gridView ? "transparent" : cLine
                Text {
                    anchors.centerIn: parent
                    text: root.gridView ? "▦ 网格" : "☰ 列表"
                    color: root.gridView ? cPrimary : cSub
                    font.pixelSize: 11
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: root.gridView = !root.gridView
                }
            }

            Rectangle {
                width: 88
                height: 30
                radius: 15
                color: cSoft
                Text {
                    anchors.centerIn: parent
                    text: "刷新歌单"
                    color: cPrimary
                    font.pixelSize: 11
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: { app.refreshPlaylists(); root.hint = "正在刷新歌单" }
                }
            }
            Rectangle {
                width: 104
                height: 30
                radius: 15
                color: "transparent"
                border.color: cLine
                Text {
                    anchors.centerIn: parent
                    text: "同步全部账号"
                    color: cSub
                    font.pixelSize: 11
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: { app.syncAllUserLibraries(); root.hint = "已开始同步全部账号" }
                }
            }
        }

        // 平台筛选
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Text { text: "平台"; color: cSub; font.pixelSize: 12 }
            Flickable {
                id: chipScroll
                Layout.fillWidth: true
                Layout.preferredHeight: 32
                contentWidth: chipRow.implicitWidth
                contentHeight: height
                clip: true
                flickableDirection: Flickable.HorizontalFlick
                Row {
                    id: chipRow
                    height: chipScroll.height
                    spacing: 8
                    PlatformChip {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "全部平台"
                        active: root.selectedPlatform === 0
                        accent: cPrimary
                        onClicked: {
                            root.selectedPlatform = 0
                            app.filterPlaylists("")
                            root.reloadSongs()
                        }
                    }
                    Repeater {
                        model: app.sourceIds.length
                        delegate: PlatformChip {
                            anchors.verticalCenter: parent.verticalCenter
                            text: index < app.sourceNames.length ? app.sourceNames[index] : ""
                            accent: app.sourceColor(app.sourceIds[index])
                            active: root.selectedPlatform === index + 1
                            onClicked: {
                                root.selectedPlatform = index + 1
                                app.filterPlaylists(app.sourceIds[index])
                                root.reloadSongs()
                            }
                        }
                    }
                }
            }
        }

        // 分段标签
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            radius: 20
            color: cCard
            border.color: cLine

            Row {
                id: tabRow
                anchors.fill: parent
                anchors.margins: 4
                spacing: 4

                Repeater {
                    model: root.tabTitles
                    delegate: Rectangle {
                        width: (tabRow.width - 12) / 4
                        height: tabRow.height
                        radius: 16
                        color: root.tabIndex === index ? cPrimary : "transparent"
                        Text {
                            anchors.centerIn: parent
                            text: modelData
                            color: root.tabIndex === index ? "#FFFFFF" : cSub
                            font.pixelSize: 12
                            font.bold: root.tabIndex === index
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                root.tabIndex = index
                                root.reloadSongs()
                            }
                        }
                    }
                }
            }
        }

        Item {
            id: libraryArea
            Layout.fillWidth: true
            Layout.fillHeight: true

            StackLayout {
                anchors.fill: parent
                currentIndex: root.tabIndex

                // 我的歌单
                Item {
                    ListView {
                        id: playlistList
                        anchors.fill: parent
                        clip: true
                        spacing: 8
                        model: app.playlists
                        visible: !root.gridView && root.openPlaylistTitle === ""

                        delegate: Rectangle {
                            width: playlistList.width
                            height: 68
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
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                }

                                Rectangle {
                                    Layout.preferredWidth: 76
                                    Layout.preferredHeight: 30
                                    radius: 15
                                    color: Qt.rgba(accent.r, accent.g, accent.b, 0.12)
                                    Text {
                                        anchors.centerIn: parent
                                        text: "加入队列"
                                        color: accent
                                        font.pixelSize: 11
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
                                height: 30
                                radius: 15
                                color: "transparent"
                                border.color: cLine
                                Text {
                                    anchors.centerIn: parent
                                    text: "← 返回歌单"
                                    color: cSub
                                    font.pixelSize: 11
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
                                font.pixelSize: 11
                            }
                        }

                        ListView {
                            id: detailList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
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
                                onPlayRequested: app.playResult(index)
                                onFavoriteRequested: app.toggleFavoriteResult(index)
                                onAddRequested: app.addResultToQueue(index)
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
                        anchors.fill: parent
                        clip: true
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
                            onPlayRequested: app.playResult(index)
                            onFavoriteRequested: app.toggleFavoriteResult(index)
                            onAddRequested: app.addResultToQueue(index)
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
                        anchors.fill: parent
                        clip: true
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
                            onPlayRequested: app.playResult(index)
                            onFavoriteRequested: app.toggleFavoriteResult(index)
                            onAddRequested: app.addResultToQueue(index)
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
                        anchors.fill: parent
                        clip: true
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
                        height: 30
                        radius: 15
                        visible: queueList.count > 0
                        color: cSoft
                        Text {
                            anchors.centerIn: parent
                            text: "清空队列"
                            color: cPrimary
                            font.pixelSize: 11
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: app.clearQueue()
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
            font.pixelSize: 11
            elide: Text.ElideRight
        }
    }
}
