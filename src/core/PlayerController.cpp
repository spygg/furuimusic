#include "PlayerController.h"

#include <QRandomGenerator>

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QMediaContent>
#endif
#include <QNetworkRequest>

#include "Settings.h"

namespace Furui {

PlayerController *PlayerController::instance()
{
    static PlayerController *p = new PlayerController();
    return p;
}

PlayerController::PlayerController(QObject *parent)
    : QObject(parent)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    m_player = new QMediaPlayer(this);
    m_audio = new QAudioOutput(this);
    m_player->setAudioOutput(m_audio);
#else
    m_player = new QMediaPlayer(this, QMediaPlayer::StreamPlayback);
#endif
    m_volume = Settings::integer(QStringLiteral("volume"), 80);
    m_mode = Settings::integer(QStringLiteral("playMode"), 1);
    setVolume(m_volume);

    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 pos) {
        updateLyric(pos);
        emit positionChanged();
    });
    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64) { emit stateChanged(); });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) next();
    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    connect(m_player, &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState) { emit stateChanged(); });
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString &msg) {
                emit errorOccurred(msg.isEmpty() ? QStringLiteral("播放失败") : msg);
            });
#else
    connect(m_player, &QMediaPlayer::stateChanged, this,
            [this](QMediaPlayer::State) { emit stateChanged(); });
    connect(m_player, static_cast<void (QMediaPlayer::*)(QMediaPlayer::Error)>(&QMediaPlayer::error),
            this, [this](QMediaPlayer::Error) { emit errorOccurred(m_player->errorString()); });
#endif
}

void PlayerController::playUrl(const QUrl &url, const QVariantMap &headers)
{
    if (!url.isValid()) {
        emit errorOccurred(QStringLiteral("无效的播放地址"));
        return;
    }
    m_headers = headers;
    m_currentUrl = url;
    // 平台的直链几乎都带防盗链：sources.json 里声明的 Referer/Cookie 必须随请求发出。
    // 原来只把 headers 存起来，播放器从来没用到，QQ/网易/酷狗的直链在非官方来源上会被 CDN 拒掉。
    QNetworkRequest req(url);
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        const QByteArray value = it.value().toString().toUtf8();
        if (!value.trimmed().isEmpty()) req.setRawHeader(it.key().toUtf8(), value);
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Qt 6 的 setSource 不接受 QNetworkRequest，也没有等价的自定义头入口；
    // 因此 Qt 6 构建下先按地址直接播放（windeployqt/macdeployqt 平台暂不依赖防盗链头）。
    Q_UNUSED(req);
    m_player->setSource(url);
#else
    m_player->setMedia(QMediaContent(req));
#endif
    m_player->play();
    emit stateChanged();
}

void PlayerController::togglePause()
{
    if (!m_currentUrl.isValid()) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (m_player->playbackState() == QMediaPlayer::PlayingState) m_player->pause();
    else m_player->play();
#else
    if (m_player->state() == QMediaPlayer::PlayingState) m_player->pause();
    else m_player->play();
#endif
    emit stateChanged();
}

void PlayerController::stop()
{
    m_player->stop();
    emit stateChanged();
}

bool PlayerController::playing() const
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return m_player->playbackState() == QMediaPlayer::PlayingState;
#else
    return m_player->state() == QMediaPlayer::PlayingState;
#endif
}

qint64 PlayerController::position() const { return m_player->position(); }
qint64 PlayerController::duration() const { return m_player->duration(); }

void PlayerController::seek(qint64 ms)
{
    if (ms < 0) ms = 0;
    m_player->setPosition(ms);
    updateLyric(ms);
}

void PlayerController::setVolume(int value)
{
    m_volume = qBound(0, value, 100);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (m_audio) m_audio->setVolume(qreal(m_volume) / 100.0);
#else
    m_player->setVolume(m_volume);
#endif
    Settings::set(QStringLiteral("volume"), m_volume);
    emit stateChanged();
}

Song PlayerController::current() const
{
    if (m_currentIndex >= 0 && m_currentIndex < m_queue.size()) return m_queue.at(m_currentIndex);
    return Song();
}

void PlayerController::setQueue(const QVector<Song> &songs, int index)
{
    m_queue = songs;
    m_currentIndex = qBound(-1, index, m_queue.size() - 1);
    emit queueChanged();
    emit mediaChanged();
}

void PlayerController::addQueue(const QVector<Song> &songs)
{
    m_queue.append(songs);
    emit queueChanged();
}

void PlayerController::removeQueue(int index)
{
    if (index < 0 || index >= m_queue.size()) return;
    m_queue.removeAt(index);
    if (m_currentIndex == index) m_currentIndex = qMin(m_currentIndex, m_queue.size() - 1);
    else if (m_currentIndex > index) --m_currentIndex;
    emit queueChanged();
}

void PlayerController::clearQueue()
{
    m_queue.clear();
    m_currentIndex = -1;
    emit queueChanged();
}

void PlayerController::setCurrentIndex(int index)
{
    if (index < 0 || index >= m_queue.size()) return;
    m_currentIndex = index;
    emit mediaChanged();
    emitRequest(index);
}

void PlayerController::emitRequest(int index)
{
    if (index < 0 || index >= m_queue.size()) return;
    emit requestPlay(m_queue.at(index));
}

void PlayerController::next()
{
    if (m_queue.isEmpty()) return;
    int nextIndex = m_currentIndex;
    switch (m_mode) {
    case 2:  // 单曲循环
        break;
    case 3:  // 随机
        nextIndex = QRandomGenerator::global()->bounded(m_queue.size());
        break;
    case 0:  // 顺序播放
        if (m_currentIndex + 1 >= m_queue.size()) { stop(); return; }
        nextIndex = m_currentIndex + 1;
        break;
    default: // 列表循环
        nextIndex = (m_currentIndex + 1) % m_queue.size();
        break;
    }
    m_currentIndex = nextIndex;
    emit mediaChanged();
    emitRequest(nextIndex);
}

void PlayerController::previous()
{
    if (m_queue.isEmpty()) return;
    if (position() > 5000) { seek(0); return; }
    int prev = m_currentIndex - 1;
    if (prev < 0) prev = m_mode == 0 ? 0 : m_queue.size() - 1;
    m_currentIndex = prev;
    emit mediaChanged();
    emitRequest(prev);
}

void PlayerController::setMode(int mode)
{
    m_mode = qBound(0, mode, 3);
    Settings::set(QStringLiteral("playMode"), m_mode);
    emit stateChanged();
}

void PlayerController::cycleMode() { setMode((m_mode + 1) % 4); }

QString PlayerController::modeName() const
{
    switch (m_mode) {
    case 0: return QStringLiteral("顺序播放");
    case 1: return QStringLiteral("列表循环");
    case 2: return QStringLiteral("单曲循环");
    case 3: return QStringLiteral("随机播放");
    }
    return QStringLiteral("列表循环");
}

void PlayerController::setLyrics(const QString &lrc)
{
    m_lyrics = parseLrc(lrc);
    m_lyricsText.clear();
    for (const LyricLine &l : m_lyrics) m_lyricsText += l.text + QLatin1Char('\n');
    m_lyricIndex = -1;
    emit lyricsChanged();
}

QString PlayerController::currentLyricLine() const
{
    if (m_lyricIndex >= 0 && m_lyricIndex < m_lyrics.size()) return m_lyrics.at(m_lyricIndex).text;
    if (m_lyrics.isEmpty()) return QStringLiteral("暂无歌词");
    return QString();
}

void PlayerController::updateLyric(qint64 pos)
{
    if (m_lyrics.isEmpty()) return;
    int idx = -1;
    for (int i = 0; i < m_lyrics.size(); ++i) {
        if (m_lyrics.at(i).time <= pos) idx = i;
        else break;
    }
    if (idx != m_lyricIndex) {
        m_lyricIndex = idx;
        emit lyricsChanged();
    }
}

} // namespace Furui
