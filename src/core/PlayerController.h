#pragma once
// 福瑞音乐 - 播放核心（Qt 5 / Qt 6 双 API 兼容）
#include <QMediaPlayer>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioOutput>
#endif

#include "Types.h"

class QTimer;

namespace Furui {

class PlayerController : public QObject {
    Q_OBJECT
public:
    static PlayerController *instance();

    void playUrl(const QUrl &url, const QVariantMap &headers = QVariantMap());
    void togglePause();
    void stop();
    void next();
    void previous();
    void seek(qint64 ms);
    void setVolume(int value);       // 0..100
    void toggleMute();               // 静音并在再点一次时回到静音前的档位
    int volume() const { return m_volume; }

    void setQueue(const QVector<Song> &songs, int index);
    void addQueue(const QVector<Song> &songs);
    void removeQueue(int index);
    void clearQueue();
    void setCurrentIndex(int index);
    const QVector<Song> &queue() const { return m_queue; }
    int currentIndex() const { return m_currentIndex; }
    Song current() const;
    // 搜索接口常常不返回封面（网易云新版只给 album.picId），播放时异步补齐后
    // 写回队列里的当前歌曲，让播放页/播放栏/列表同时刷新。
    void setCurrentCover(const QString &cover);

    bool playing() const;
    qint64 position() const;
    qint64 duration() const;

    // ---- 启动续播 ----
    // 把队列 / 当前曲目 / 播放位置 / 是否在播写入本地设置；每 5 秒、切歌、
    // 播放状态变化与退出时都会落盘，Android 被系统杀进程也能续上。
    void saveState();
    // 启动时从本地设置恢复上一次的队列与位置；若上次正在播则自动续播并跳到原位置。
    // 由 AppController 在信号连接完成后调用（此时 requestPlay 已被接住）。
    void restoreState();

    int mode() const { return m_mode; }
    void setMode(int mode);
    void cycleMode();
    QString modeName() const;

    void setLyrics(const QString &lrc);
    QString lyricsText() const { return m_lyricsText; }
    // 逐行歌词（含无时间轴的纯文本），供播放页做逐行高亮与自动滚动
    QStringList lyricLines() const { return m_lyricLines; }
    // 与 lyricLines 同序的起始毫秒表：界面做逐行扫光（karaoke）时还需要下一行的
    // 起点算出本行的持续时间，所以表里存的是「本行起点」，末行由界面用歌曲长度兜底。
    QVariantList lyricTimes() const;
    // 是否为带时间轴的 LRC（决定是否自动跟随进度滚动）
    bool lyricsTimed() const { return !m_lyrics.isEmpty(); }
    QString currentLyricLine() const;
    int currentLyricIndex() const { return m_lyricIndex; }
    QVariantMap playbackHeaders() const { return m_headers; }

signals:
    void stateChanged();
    // 音量单独走一个信号：拖动滑条时不应该让 UI 把整棵播放器状态重新绑定一遍
    void volumeChanged();
    void positionChanged();
    void mediaChanged();
    void queueChanged();
    void lyricsChanged();
    void errorOccurred(const QString &message);
    // 换歌时由 AppController 负责解析新的播放地址（含 VIP 替代链）后回调 playUrl
    void requestPlay(const Song &song);

private:
    explicit PlayerController(QObject *parent = nullptr);
    void updateLyric(qint64 pos);
    void emitRequest(int index);
    // Qt 6 无法给 QMediaPlayer 注入请求头：防盗链直链在直连失败后，
    // 带 headers 流式下载到缓存再播放本地文件。
    void fetchWithHeaders();

    QMediaPlayer *m_player = nullptr;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QAudioOutput *m_audio = nullptr;
#endif
    QVector<Song> m_queue;
    int m_currentIndex = -1;
    int m_mode = 1;          // 默认列表循环
    int m_volume = 80;
    int m_volumeBeforeMute = 0;
    QVariantMap m_headers;
    QVector<LyricLine> m_lyrics;
    QString m_lyricsText;
    QStringList m_lyricLines;
    int m_lyricIndex = -1;
    QUrl m_currentUrl;
    // 每次换歌递增：迟到的带 headers 下载结果不能盖掉新歌
    quint64 m_playGen = 0;
    bool m_headerRetry = false;
    // 恢复续播时要跳到的时间点（毫秒）；媒体加载完成（LoadedMedia）后应用一次并清零。
    // 只在 restoreState 设置，用户切歌时会清掉，避免影响后续歌曲。
    qint64 m_pendingSeek = 0;
    QTimer *m_saveTimer = nullptr;
};

} // namespace Furui
