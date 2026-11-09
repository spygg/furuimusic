#pragma once
// 福瑞音乐 - 播放核心（Qt 5 / Qt 6 双 API 兼容）
#include <QMediaPlayer>
#include <QObject>
#include <QVariantMap>
#include <QVector>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QAudioOutput>
#endif

#include "Types.h"

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
    int volume() const { return m_volume; }

    void setQueue(const QVector<Song> &songs, int index);
    void addQueue(const QVector<Song> &songs);
    void removeQueue(int index);
    void clearQueue();
    void setCurrentIndex(int index);
    const QVector<Song> &queue() const { return m_queue; }
    int currentIndex() const { return m_currentIndex; }
    Song current() const;

    bool playing() const;
    qint64 position() const;
    qint64 duration() const;

    int mode() const { return m_mode; }
    void setMode(int mode);
    void cycleMode();
    QString modeName() const;

    void setLyrics(const QString &lrc);
    QString lyricsText() const { return m_lyricsText; }
    QString currentLyricLine() const;
    int currentLyricIndex() const { return m_lyricIndex; }
    QVariantMap playbackHeaders() const { return m_headers; }

signals:
    void stateChanged();
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

    QMediaPlayer *m_player = nullptr;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QAudioOutput *m_audio = nullptr;
#endif
    QVector<Song> m_queue;
    int m_currentIndex = -1;
    int m_mode = 1;          // 默认列表循环
    int m_volume = 80;
    QVariantMap m_headers;
    QVector<LyricLine> m_lyrics;
    QString m_lyricsText;
    int m_lyricIndex = -1;
    QUrl m_currentUrl;
};

} // namespace Furui
