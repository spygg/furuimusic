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

    // resumeMs >= 0：换源/断流续播时指定从哪个毫秒继续（媒体加载完成后由
    // m_pendingSeek 应用一次）。默认 -1 表示从头播，保持原有调用点不变。
    void playUrl(const QUrl &url, const QVariantMap &headers = QVariantMap(),
                 qint64 resumeMs = -1);
    void togglePause();
    void stop();
    // 切歌取链的空窗期专用：先停掉旧流并标记「正在切换」。
    // 这段时间里任何播放命令（用户点播放、车机/通知栏送来的 play）都必须被忽略，
    // 否则 m_currentUrl 还指着上一首，play() 会把刚播完的那首从头再放一遍。
    void beginSwitch();
    // 换源解析期（脚本取链可达 15s）暂停看门狗起播计时：这段时间没有新流在加载，
    // 继续计时会把「解析慢」误判成「加载超时」。playUrl 会重新拉起看门狗。
    void pauseWatchdog();
    void next();
    void previous();
    // 队列推进（不做推荐）：AppController 取到推荐歌并插入队列后调用，
    // 或在推荐取不到时作为兜底继续走原队列。
    void playNextInQueue();
    // 把推荐歌插到当前歌后面（当前 index+1），保持正在播放的这首歌不中断。
    void insertAfterCurrent(const QVector<Song> &songs);
    void seek(qint64 ms);
    void setVolume(int value);       // 0..100
    void toggleMute();               // 静音并在再点一次时回到静音前的档位
    int volume() const { return m_volume; }
    // Android 音频焦点 CAN_DUCK（导航播报、提醒音等）：临时把实际输出压到用户档位的
    // 30%，不改 m_volume、不写设置持久化；焦点恢复后 setDucked(false)。
    // QML（MV 音量）按「方法 + duckedChanged 信号」约定直接绑 app.ducked。
    void setDucked(bool on);
    bool ducked() const { return m_ducked; }

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
    // 最近一次有效播放位置（>1s）。error 到达瞬间部分后端把 position 清 0，
    // 上层记 failedPos 时用它兜底，避免续播又从头来。
    qint64 lastGoodPosition() const { return m_lastGoodPosition; }

    // ---- 启动续播 ----
    // 把队列 / 当前曲目 / 播放位置 / 是否在播写入本地设置；每 5 秒、切歌、
    // 播放状态变化（1.2s 防抖）与退出时都会落盘，Android 被系统杀进程也能续上。
    void saveState();
    // 启动时从本地设置恢复上一次的队列与位置；若上次正在播则自动续播并跳到原位置。
    // 由 AppController 在信号连接完成后调用（此时 requestPlay 已被接住）。
    void restoreState();

    int mode() const { return m_mode; }
    void setMode(int mode);
    void cycleMode();
    QString modeName() const;

    void setLyrics(const QString &lrc, const QString &trans = QString());
    QString lyricsText() const { return m_lyricsText; }
    // 逐行歌词（含无时间轴的纯文本），供播放页做逐行高亮与自动滚动
    QStringList lyricLines() const { return m_lyricLines; }
    // 与 lyricLines 同序的翻译行表：有翻译轨时该行非空，QML 把它渲染成原文下方的小字
    QStringList lyricTransLines() const { return m_lyricTransLines; }
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
    // Android 音频焦点 duck 状态变化：MV 的 AudioOutput 绑定 app.ducked 乘系数
    void duckedChanged();
    void positionChanged();
    void mediaChanged();
    void queueChanged();
    void lyricsChanged();
    void errorOccurred(const QString &message);
    // 换歌时由 AppController 负责解析新的播放地址（含 VIP 替代链）后回调 playUrl
    void requestPlay(const Song &song);
    // 需要一首「推荐续播」：汽水漫游开启，或顺序播放走到队列末尾时发出。
    // AppController 取到推荐歌后 insertAfterCurrent + playNextInQueue；取不到就
    // playNextInQueue 兜底（顺序播放末尾则停止）。
    void recommendationNeeded();

private:
    explicit PlayerController(QObject *parent = nullptr);
    void advance(bool allowRecommend);
    void updateLyric(qint64 pos);
    void emitRequest(int index);
    // 位置已到结尾（或已判定播完）：在线流在 EOF 处常报 "Internal error / Failed to read"，
    // 那不是故障，按正常播完处理，否则上层会把刚播完的这首重新取链再放一遍。
    bool atTrackEnd() const;
    // 切源过渡期内 QMediaPlayer 可能迟到投递上一路被 stop 掉的流的 error
    //（实测：换源成功后 3ms 到达的 "Demuxing failed"，pos 0 且状态 StalledMedia）。
    // 把它当新源失败上报会让刚换上的好源被误扣分/拉黑。
    bool isStaleSwitchError() const;
    // 把用户档位 × duck 系数落到实际输出（Qt6 走 QAudioOutput，Qt5 走 QMediaPlayer）
    void applyVolume();
    void finishAsEnded();
    // 校验式断点续播：反复尝试 setPosition 直到真正到位（或超时放弃）。
    // 由媒体状态变化与 m_resumeSeekTimer 共同驱动，成功后清 m_pendingSeek。
    void applyPendingSeek();
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
    // Android 音频焦点 CAN_DUCK 状态：仅压低实际输出，不改变用户档位
    bool m_ducked = false;
    QVariantMap m_headers;
    QVector<LyricLine> m_lyrics;
    QString m_lyricsText;
    QStringList m_lyricLines;
    QStringList m_lyricTransLines;   // 与 m_lyricLines 同长度；无翻译行为空串
    // lyricTimes() 的缓存：QML 绑定在每次换行（lyricsChanged）时都会重新读取，
    // 预生成可避免每次重新遍历整份歌词。
    QVariantList m_lyricTimes;
    int m_lyricIndex = -1;
    QUrl m_currentUrl;
    // 每次换歌递增：迟到的带 headers 下载结果不能盖掉新歌
    quint64 m_playGen = 0;
    bool m_headerRetry = false;
    // 恢复续播时要跳到的时间点（毫秒）；媒体加载完成（LoadedMedia）后应用一次并清零。
    // 只在 restoreState 设置，用户切歌时会清掉，避免影响后续歌曲。
    qint64 m_pendingSeek = 0;
    // 断点续播校验：在线流刚 Loaded/Buffered 时 setPosition 常常静默失败（流还不可
    // seek），旧实现 setPosition 一次就把 m_pendingSeek 清零 → 从 0 重播（"没播完又
    // 重来"）。现在保留目标，用定时器反复 setPosition 直到 position 真正接近目标
    // 或超时放弃，确保续播真的生效。
    QTimer *m_resumeSeekTimer = nullptr;
    int m_resumeSeekTries = 0;
    // 最近一次有效播放位置（position>1s 时刷新）。error 到达的瞬间部分后端会把
    // position 清成 0，上层据此记的 failedPos 就变成 0，续播又从头来。用这个值兜底。
    qint64 m_lastGoodPosition = 0;
    // 自动连播防重入：EndOfMedia / 进度兜底只允许排队一次下一首；
    // 新歌加载完成前（m_suppressEnd）忽略旧曲残留的结束信号。
    bool m_advancing = false;
    bool m_suppressEnd = false;
    // 已播到结尾：此后「播放」在语义上是「下一首」，而不是把这首从头再放一遍。
    bool m_ended = false;
    // 正在切歌取链：旧源已停止但新地址还没到，期间的播放命令一律忽略。
    bool m_switching = false;
    QTimer *m_saveTimer = nullptr;
    // 队列/状态变化的落盘防抖：整队列序列化成 JSON 在千首队列上并不便宜，
    // 连续 addQueue/removeQueue 时没必要每次都做，攒一小段再写。
    QTimer *m_saveDebounce = nullptr;
    // 播放看门狗：部分后端（尤其 Android MediaPlayer）在线流播完/出错时既不派发
    // EndOfMedia 也不派发 errorOccurred，表现为「播完一首停住」或「失败卡住不换源」。
    // 每秒巡检一次：起播超时判失败换源、播完停滞判结束切歌、中途停滞超时判失败换源。
    QTimer *m_watchdog = nullptr;
    qint64 m_lastPosition = -1;     // 上次巡检读到的播放位置
    qint64 m_stallSince = 0;        // 位置开始停滞的时刻（ms），0 = 未停滞
    qint64 m_loadStarted = 0;       // 本次 playUrl 起播时刻（ms），用于加载超时判定
    bool m_everPlayed = false;      // 本次源是否真正出过声音（position 越过 0.5s）
    void onWatchdog();
};

} // namespace Furui
