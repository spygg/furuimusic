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

    property bool searching: false
    // 0 = 全部平台，其余对应 app.sourceIds[index - 1]
    property int selectedPlatform: 0
    // 封面卡片墙 / 传统列表，一键切换；默认网格更接近现代音乐产品
    property bool gridView: true
    readonly property int gridColumns: Math.max(2, Math.min(6,
        Math.floor(Math.max(340, resultsArea.width) / 170)))
    readonly property real gridCellWidth: Math.max(120, resultsArea.width / gridColumns)
    readonly property real gridCellHeight: gridCellWidth + 76

    function selectedSourceId() {
        if (selectedPlatform <= 0)
            return ""
        if (selectedPlatform - 1 < app.sourceIds.length)
            return app.sourceIds[selectedPlatform - 1]
        return ""
    }

    function doSearch() {
        app.search(keyword.text, root.selectedSourceId())
    }

    // 打开歌单后切回歌曲视图，直接展示该歌单曲目
    function openPlaylist(uid) {
        app.loadPlaylist(uid)
        app.setSearchType("song")
    }

    Connections {
        target: app
        onSearchStarted: root.searching = true
        onSearchFinished: root.searching = false
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        // 顶部主视觉：搜索入口 + 聚合平台说明
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 148
            radius: theme.radiusLg
            clip: true
            gradient: Gradient {
                GradientStop { position: 0.0; color: theme.heroStart }
                GradientStop { position: 1.0; color: theme.heroEnd }
            }

            Rectangle {
                width: 180
                height: 180
                radius: 90
                x: parent.width - 120
                y: -70
                color: "#FFFFFF"
                opacity: 0.08
            }

            Rectangle {
                width: 110
                height: 110
                radius: 55
                x: parent.width - 210
                y: 78
                color: "#FFFFFF"
                opacity: 0.06
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: theme.spaceLg
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    ColumnLayout {
                        spacing: 2
                        Text {
                            text: "发现音乐"
                            color: "#FFFFFF"
                            font.pixelSize: theme.fontXl
                            font.bold: true
                        }
                        Text {
                            text: "一次搜索聚合 " + Math.max(0, app.sourceNames.length) + " 个音乐平台"
                            color: Qt.rgba(1, 1, 1, 0.82)
                            font.pixelSize: theme.fontSm
                        }
                    }
                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    TextField {
                        id: keyword
                        Layout.fillWidth: true
                        Layout.preferredHeight: 40
                        placeholderText: "输入歌名、歌手或专辑"
                        color: cText
                        placeholderTextColor: theme.mutedText
                        font.pixelSize: 14
                        leftPadding: theme.spaceMd
                        rightPadding: theme.spaceMd
                        background: Rectangle {
                            radius: theme.radiusSm
                            color: theme.surface
                            border.color: theme.surface
                        }
                        onAccepted: root.doSearch()
                    }

                    // 歌曲 / 歌单 二选一
                    Rectangle {
                        Layout.preferredWidth: 100
                        Layout.preferredHeight: 40
                        radius: theme.radiusSm
                        color: theme.surface
                        Row {
                            anchors.centerIn: parent
                            spacing: 2
                            Repeater {
                                model: ["歌曲", "歌单"]
                                delegate: Rectangle {
                                    width: 48
                                    height: 32
                                    radius: theme.radiusXs
                                    color: (index === 0 ? app.searchType !== "songlist"
                                                        : app.searchType === "songlist")
                                           ? theme.accent : "transparent"
                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData
                                        color: (index === 0 ? app.searchType !== "songlist"
                                                             : app.searchType === "songlist")
                                               ? "#FFFFFF" : cSub
                                        font.pixelSize: theme.fontSm
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        onClicked: app.setSearchType(index === 0 ? "song" : "songlist")
                                    }
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.preferredWidth: 68
                        Layout.preferredHeight: 40
                        radius: theme.radiusSm
                        color: searchArea.pressed ? Qt.rgba(0, 0, 0, 0.34) : Qt.rgba(0, 0, 0, 0.24)
                        Text {
                            anchors.centerIn: parent
                            text: "搜索"
                            color: "#FFFFFF"
                            font.pixelSize: 14
                            font.bold: true
                        }
                        MouseArea {
                            id: searchArea
                            anchors.fill: parent
                            onClicked: root.doSearch()
                        }
                    }
                }
            }
        }

        // 平台筛选：全部 / 各平台主色胶囊
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Text {
                text: "平台"
                color: cSub
                font.pixelSize: 12
            }
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
                        onClicked: root.selectedPlatform = 0
                    }
                    Repeater {
                        model: app.sourceIds.length
                        delegate: PlatformChip {
                            anchors.verticalCenter: parent.verticalCenter
                            text: index < app.sourceNames.length ? app.sourceNames[index] : ""
                            accent: app.sourceColor(app.sourceIds[index])
                            active: root.selectedPlatform === index + 1
                            onClicked: root.selectedPlatform = index + 1
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Text {
                text: app.searchType === "songlist" ? "歌单结果" : "歌曲结果"
                color: cText
                font.pixelSize: 14
                font.bold: true
            }
            Text {
                text: "共 " + (app.searchType === "songlist" ? playlistList.count : resultList.count) + " 条"
                color: cSub
                font.pixelSize: 11
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
                color: "transparent"
                border.color: cLine
                Text {
                    anchors.centerIn: parent
                    text: "加载榜单"
                    color: cSub
                    font.pixelSize: 11
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        var sid = root.selectedSourceId()
                        if (sid.length === 0 && app.sourceIds.length > 0)
                            sid = app.sourceIds[0]
                        if (sid.length > 0) {
                            app.setSearchType("songlist")
                            app.loadRank(sid)
                        }
                    }
                }
            }
        }

        Item {
            id: resultsArea
            Layout.fillWidth: true
            Layout.fillHeight: true

            // 歌曲结果（列表）
            ListView {
                id: resultList
                anchors.fill: parent
                clip: true
                spacing: 8
                model: app.results
                visible: !root.gridView && app.searchType !== "songlist" && count > 0

                delegate: SongRow {
                    width: resultList.width
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
                    showDownload: true
                    onPlayRequested: app.playResult(index)
                    onFavoriteRequested: app.toggleFavoriteResult(index)
                    onAddRequested: app.addResultToQueue(index)
                    onDownloadRequested: app.downloadResult(index)
                }
            }

            // 歌曲结果（封面卡片墙）
            GridView {
                id: resultGrid
                anchors.fill: parent
                clip: true
                model: app.results
                cellWidth: root.gridCellWidth
                cellHeight: root.gridCellHeight
                visible: root.gridView && app.searchType !== "songlist" && count > 0

                delegate: AlbumCard {
                    width: resultGrid.cellWidth
                    height: resultGrid.cellHeight
                    title: model.title
                    subtitle: model.artist + (model.album.length > 0 ? " · " + model.album : "")
                    cover: model.cover
                    badge: model.sourceName
                    accent: app.sourceColor(model.source)
                    onClicked: app.playResult(index)
                }
            }

            // 歌单结果（歌单模式下 app.playlists 承载搜索结果）
            ListView {
                id: playlistList
                anchors.fill: parent
                clip: true
                spacing: 8
                model: app.playlists
                visible: !root.gridView && app.searchType === "songlist" && count > 0

                delegate: Rectangle {
                    id: playDelegate
                    width: playlistList.width
                    height: 68
                    radius: theme.radiusMd
                    color: plHover.hovered ? theme.cardHover : root.cCard
                    border.color: root.cLine

                    // 封面兜底色与标签跟随平台主色
                    readonly property color accent: app.sourceColor(model.source)

                    MouseArea {
                        id: plHover
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: root.openPlaylist(model.uid)
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 10

                        Rectangle {
                            Layout.preferredWidth: 46
                            Layout.preferredHeight: 46
                            radius: 10
                            color: Qt.rgba(playDelegate.accent.r, playDelegate.accent.g, playDelegate.accent.b, 0.12)
                            Text {
                                anchors.centerIn: parent
                                text: "♪"
                                color: playDelegate.accent
                                font.pixelSize: 18
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3
                            Text {
                                Layout.fillWidth: true
                                text: model.title
                                color: root.cText
                                font.pixelSize: 14
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: model.count + " 首 · " + model.sourceName
                                      + (model.owner.length > 0 ? " · " + model.owner : "")
                                color: root.cSub
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }

                        Rectangle {
                            Layout.preferredWidth: 62
                            Layout.preferredHeight: 30
                            radius: 15
                            color: Qt.rgba(playDelegate.accent.r, playDelegate.accent.g, playDelegate.accent.b, 0.12)
                            Text {
                                anchors.centerIn: parent
                                text: "查看"
                                color: playDelegate.accent
                                font.pixelSize: theme.fontSm
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: root.openPlaylist(model.uid)
                            }
                        }
                    }
                }
            }

            // 歌单 / 榜单（封面卡片墙）
            GridView {
                id: playlistGrid
                anchors.fill: parent
                clip: true
                model: app.playlists
                cellWidth: root.gridCellWidth
                cellHeight: root.gridCellHeight
                visible: root.gridView && app.searchType === "songlist" && count > 0

                delegate: AlbumCard {
                    width: playlistGrid.cellWidth
                    height: playlistGrid.cellHeight
                    title: model.title
                    subtitle: model.count + " 首 · " + model.sourceName
                              + (model.owner.length > 0 ? " · " + model.owner : "")
                    cover: model.cover
                    badge: model.sourceName
                    accent: app.sourceColor(model.source)
                    onClicked: root.openPlaylist(model.uid)
                }
            }

            EmptyState {
                anchors.centerIn: parent
                visible: !root.searching
                         && (app.searchType === "songlist" ? playlistList.count === 0
                                                          : resultList.count === 0)
                iconName: "search"
                title: app.searchType === "songlist" ? "还没有歌单结果" : "还没有歌曲结果"
                hint: "输入关键词搜索，或点「加载榜单」看看各平台热门"
            }

            BusyIndicator {
                anchors.centerIn: parent
                running: root.searching
                visible: root.searching
            }
        }

        Text {
            Layout.fillWidth: true
            visible: app.statusMessage.length > 0
            text: app.statusMessage
            color: cSub
            font.pixelSize: 11
            elide: Text.ElideRight
        }
    }
}
