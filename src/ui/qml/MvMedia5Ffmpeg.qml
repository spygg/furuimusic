import QtQuick 2.15
import FuruiMusic 1.0

// Qt5 + 自编译 FFmpeg 播放后端（build_ffmpeg_desktop.sh 产出静态库）。
// C++ 的 FfmpegVideoPlayer 自己解封装/解码/出声，完全绕开 Qt5 的
// DirectShow mediaservice，因此 Windows / Linux / macOS 的 Qt5 构建都能
// 在应用内播放 MP4(H.264 + AAC) MV。
//
// 对外接口与 MvMedia5.qml / MvMedia6.qml 完全一致，MvPage 无需区分后端。
Item {
    id: root

    readonly property alias player: fp
    property real volume: 1.0
    property bool muted: false
    readonly property bool playing: fp.playing
    readonly property bool busy: fp.busy
    readonly property bool ended: fp.ended
    readonly property int position: fp.position
    readonly property int duration: fp.duration
    readonly property bool seekable: fp.seekable
    readonly property bool failed: fp.failed
    readonly property string errorMessage: fp.errorMessage

    function open(url) { fp.open(url) }
    function close() { fp.close() }
    function seekTo(ms) { fp.seekTo(ms) }
    function togglePlay() { fp.togglePlay() }
    function replay() { fp.replay() }

    FfmpegVideoPlayer {
        id: fp
        anchors.fill: parent
        volume: root.volume
        muted: root.muted
    }
}
