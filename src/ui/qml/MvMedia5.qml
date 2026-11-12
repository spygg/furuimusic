import QtQuick 2.15
import QtMultimedia 5.15

// Qt5 媒体原语：MediaPlayer + VideoOutput。对外只暴露归一化接口，
// 让 MvPage 不接触任何 QtMultimedia 类型（Qt6 的同名 API 有差异）。
Item {
    id: root

    readonly property alias player: mp
    // 与 MvMedia6 完全一致的归一化音量接口
    property real volume: 1.0
    property bool muted: false
    readonly property bool playing: mp.playbackState === MediaPlayer.PlayingState
    readonly property bool busy: mp.mediaStatus === MediaPlayer.Loading
                                 || mp.mediaStatus === MediaPlayer.Buffering
    readonly property bool ended: mp.mediaStatus === MediaPlayer.EndOfMedia
    readonly property int position: mp.position
    readonly property int duration: mp.duration
    readonly property bool seekable: mp.seekable
    // 失败判定用 error 状态本身：Qt5 DirectShow 报错时 errorString 可能为空串
    readonly property bool failed: mp.error !== MediaPlayer.NoError
    readonly property string errorMessage: mp.errorString

    function open(url) {
        // 先卸载再赋源：错误态下重复赋同一 URL 不会触发重新加载
        mp.stop(); mp.source = ""; mp.source = url; mp.play()
    }
    function close() { mp.stop(); mp.source = "" }
    function seekTo(ms) { mp.seek(ms) }
    function togglePlay() {
        if (mp.playbackState === MediaPlayer.PlayingState) mp.pause()
        else mp.play()
    }
    function replay() { mp.seek(0); mp.play() }

    MediaPlayer {
        id: mp
        autoPlay: true
        volume: root.volume
        muted: root.muted
    }

    VideoOutput {
        anchors.fill: parent
        source: mp
        fillMode: VideoOutput.PreserveAspectFit
    }
}
