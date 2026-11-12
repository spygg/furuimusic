#pragma once
// 福瑞音乐 - Qt 5 自编译 FFmpeg 播放后端
//
// Qt 5 桌面只有 DirectShow 媒体服务（无 MP4/H.264 分离器），Qt 6 才有官方
// FFmpeg 插件。这个组件让 Qt 5 也具备应用内 MV 播放能力：
//   FFmpeg 解码 -> QQuickPaintedItem 画视频帧 + QAudioOutput 放声音
// 对外暴露与 MvMedia5.qml / MvMedia6.qml 完全一致的归一化接口，MvPage
// 不需要知道底层是哪个后端。只在 FURUI_HAVE_FFMPEG 定义时参与编译
//（由 CMake / qmake 在 Qt5 且检测到 FFmpeg 静态库时定义）。
#ifdef FURUI_HAVE_FFMPEG

#include <QQuickPaintedItem>
#include <QAudioFormat>
#include <QAudioOutput>
#include <QAudioDeviceInfo>
#include <QImage>
#include <QMutex>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace Furui {

class FfmpegVideoPlayer : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(qreal volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool ended READ ended NOTIFY stateChanged)
    Q_PROPERTY(bool seekable READ seekable NOTIFY stateChanged)
    Q_PROPERTY(bool failed READ failed NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(int position READ position NOTIFY positionChanged)
    Q_PROPERTY(int duration READ duration NOTIFY positionChanged)

public:
    explicit FfmpegVideoPlayer(QQuickItem *parent = nullptr);
    ~FfmpegVideoPlayer() override;

    void paint(QPainter *painter) override;

    qreal volume() const { return m_volume; }
    void setVolume(qreal v);
    bool muted() const { return m_muted; }
    void setMuted(bool m);

    // GUI 线程缓存；由 syncState() 从解码线程的原子状态同步过来
    bool playing() const { return m_propPlaying; }
    bool busy() const { return m_propBusy; }
    bool ended() const { return m_propEnded; }
    bool seekable() const { return m_propSeekable; }
    bool failed() const { return m_propFailed; }
    QString errorMessage() const { return m_propError; }
    int position() const { return m_propPosition; }
    int duration() const { return m_propDuration; }

    Q_INVOKABLE void open(const QUrl &url);
    Q_INVOKABLE void close();
    Q_INVOKABLE void seekTo(int ms);
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void replay();

signals:
    void volumeChanged();
    void mutedChanged();
    void stateChanged();
    void positionChanged();

private slots:
    void requestUpdate();
    void syncState();

private:
    static int interruptCallback(void *opaque);
    void stopDecode();
    void decodeLoop(const QString &url);
    void publishError(const QString &msg);
    void refreshAtomicVolume();

    // ---- GUI 线程状态 ----
    qreal m_volume = 1.0;
    bool m_muted = false;
    bool m_propPlaying = false;
    bool m_propBusy = false;
    bool m_propEnded = false;
    bool m_propSeekable = false;
    bool m_propFailed = false;
    QString m_propError;
    int m_propPosition = 0;
    int m_propDuration = 0;
    QTimer *m_syncTimer = nullptr;

    QImage m_frame;                 // 待绘制帧，paint() 只读
    QMutex m_frameMutex;
    QString m_error;                // 解码线程写、GUI 线程读
    QMutex m_errorMutex;

    // ---- 解码线程状态（全部原子，避免跨线程数据竞争）----
    std::atomic<bool> m_stop{true};
    std::atomic<bool> m_pause{false};
    std::atomic<int> m_seekTarget{-1};
    std::atomic<int> m_vol{1000};   // 0..1000，已合并 muted
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_ended{false};
    std::atomic<bool> m_failed{false};
    std::atomic<bool> m_seekable{false};
    std::atomic<qint64> m_pos{0};
    std::atomic<qint64> m_dur{0};

    std::thread m_thread;
    std::mutex m_waitMutex;
    std::condition_variable m_waitCv;
};

} // namespace Furui

#endif // FURUI_HAVE_FFMPEG
