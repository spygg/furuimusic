import QtQuick 2.15
import QtMultimedia

// Qt6 媒体原语：Qt6 的 MediaPlayer 没有 volume/seek，且必须显式挂 AudioOutput
// 才出声（Qt5 不能声明 AudioOutput，该类型不存在——所以两份变体文件）。
// 对外暴露与 MvMedia5 完全一致的归一化接口。
Item {
    id: root

    readonly property alias player: mp
    // 归一化音量接口：Qt5 挂在 MediaPlayer 上、Qt6 挂在 AudioOutput 上，
    // MvPage 只认这两个属性，不接触任何 QtMultimedia 类型
    property real volume: 1.0
    property bool muted: false
    readonly property bool playing: mp.playbackState === MediaPlayer.PlayingState
    readonly property bool busy: mp.mediaStatus === MediaPlayer.Loading
                                 || mp.mediaStatus === MediaPlayer.Buffering
    readonly property bool ended: mp.mediaStatus === MediaPlayer.EndOfMedia
    readonly property int position: mp.position
    readonly property int duration: mp.duration
    readonly property bool seekable: mp.seekable
    // 与 MvMedia5 一致：失败判定用 error 状态本身，不依赖 errorString 非空
    readonly property bool failed: mp.error !== MediaPlayer.NoError
    readonly property string errorMessage: mp.errorString

    function open(url) {
        // 先卸载再赋源：错误态下重复赋同一 URL 不会触发重新加载
        mp.stop(); mp.source = ""; mp.source = url; mp.play()
    }
    function close() { mp.stop(); mp.source = "" }
    function seekTo(ms) { mp.setPosition(ms) }
    function togglePlay() {
        if (mp.playbackState === MediaPlayer.PlayingState) mp.pause()
        else mp.play()
    }
    function replay() { mp.setPosition(0); mp.play() }

    // Qt6 的 VideoOutput 没有 source 属性：必须由 MediaPlayer.videoOutput
    // 指向 VideoOutput（或 QVideoSink）。沿用 Qt5 的 source: mp 会报
    // "Cannot assign to non-existent property source"，音频照走但整页黑屏。
    MediaPlayer {
        id: mp
        autoPlay: true
        audioOutput: AudioOutput { id: audioOut; volume: root.volume; muted: root.muted }
        videoOutput: videoOut
    }

    VideoOutput {
        id: videoOut
        anchors.fill: parent
        fillMode: VideoOutput.PreserveAspectFit
    }
}
