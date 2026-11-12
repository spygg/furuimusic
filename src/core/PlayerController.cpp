#include "PlayerController.h"

#include "core/QtCompat.h"
#include <QRegularExpression>

#include <limits>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QMediaContent>
#endif
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QNetworkRequest>

#include "HttpClient.h"
#include "Settings.h"

namespace Furui {

// 缓存文件后缀：播放器后端（Qt6 FFmpeg/Android、Qt5 DirectShow）都靠扩展名判断容器
static QString cacheSuffixFor(const QUrl &url)
{
    static const QStringList known = {QStringLiteral("mp3"), QStringLiteral("flac"),
                                      QStringLiteral("m4a"), QStringLiteral("aac"),
                                      QStringLiteral("ogg"), QStringLiteral("opus"),
                                      QStringLiteral("wav"), QStringLiteral("wma")};
    const QString suffix = QFileInfo(url.path()).suffix().toLower();
    return known.contains(suffix) ? suffix : QStringLiteral("mp3");
}

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
        if (status != QMediaPlayer::EndOfMedia) return;
        if (m_mode == 2) {
            // 单曲循环：原地重播，不重新联网解析地址
            seek(0);
            m_player->play();
            return;
        }
        next();
    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    connect(m_player, &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState) { emit stateChanged(); });
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error error, const QString &msg) {
                // 带防盗链头的直链：先直连试播，被拒后再带 headers 下载缓存重试一次。
                // 大多数直链其实不校验 Referer，这样可以避免每次播放都先下载整首。
                if (error != QMediaPlayer::NoError && !m_headerRetry && !m_headers.isEmpty()
                    && m_currentUrl.scheme().startsWith(QStringLiteral("http"))) {
                    m_headerRetry = true;
                    fetchWithHeaders();
                    return;
                }
                emit errorOccurred(msg.isEmpty() ? QStringLiteral("播放失败") : msg);
            });
#else
    connect(m_player, &QMediaPlayer::stateChanged, this,
            [this](QMediaPlayer::State) { emit stateChanged(); });
    connect(m_player, static_cast<void (QMediaPlayer::*)(QMediaPlayer::Error)>(&QMediaPlayer::error),
            this, [this](QMediaPlayer::Error error) {
                // Qt5 的 Windows 后端是 DirectShow，它根本不读 QNetworkRequest 的自定义头，
                // 所以带防盗链的直链一定失败；这里必须和 Qt6 一样退回「带头下载再本地播放」，
                // 否则酷狗/QQ 这类校验 Referer 的源在 Windows 上永远播不了。
                if (error != QMediaPlayer::NoError && !m_headerRetry && !m_headers.isEmpty()
                    && m_currentUrl.scheme().startsWith(QStringLiteral("http"))) {
                    m_headerRetry = true;
                    fetchWithHeaders();
                    return;
                }
                // Qt5 DirectShow 失败时 errorString 可能是空串，直接透传会弹一个空 toast
                const QString msg = m_player->errorString();
                emit errorOccurred(msg.isEmpty() ? QStringLiteral("播放失败") : msg);
            });
#endif
}

void PlayerController::playUrl(const QUrl &url, const QVariantMap &headers)
{
    if (!url.isValid()) {
        emit errorOccurred(QStringLiteral("无效的播放地址"));
        return;
    }
    ++m_playGen;
    m_headerRetry = false;
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
    // Qt 6 的 setSource 不接受 QNetworkRequest，也没有等价的自定义头入口。
    // 策略：先按地址直连（多数直链不需要头）；如果播放器报错，errorOccurred
    // 里会调用 fetchWithHeaders() 带 headers 下载到缓存后改播本地文件。
    Q_UNUSED(req);
    m_player->setSource(url);
#else
    m_player->setMedia(QMediaContent(req));
#endif
    m_player->play();
    emit stateChanged();
}

void PlayerController::fetchWithHeaders()
{
    if (!m_currentUrl.isValid() || m_headers.isEmpty()) return;
    const quint64 gen = m_playGen;
    const QUrl url = m_currentUrl;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                        + QStringLiteral("/playback");
    QDir().mkpath(dir);
    // 缓存目录只服务于「带防盗链头的直链」这一条兜底路径：每次启动只保留最近 20 首，
    // 否则长期使用会把手机存储慢慢吃满（系统清理缓存前不一定会通知到我们）。
    static bool cachePruned = false;
    if (!cachePruned) {
        cachePruned = true;
        const QFileInfoList cached = QDir(dir).entryInfoList(
            QDir::Files | QDir::NoDotAndDotDot, QDir::Time);
        for (int i = 20; i < cached.size(); ++i)
            QFile::remove(cached.at(i).absoluteFilePath());
    }
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(url.path().toUtf8(), QCryptographicHash::Sha1).toHex());
    const QString path = dir + QLatin1Char('/') + key + QLatin1Char('.') + cacheSuffixFor(url);

    auto startLocal = [this](const QString &p) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        m_player->setSource(QUrl::fromLocalFile(p));
#else
        m_player->setMedia(QMediaContent(QUrl::fromLocalFile(p)));
#endif
        m_player->play();
        emit stateChanged();
    };

    if (QFileInfo(path).size() > 0) {
        startLocal(path);
        return;
    }

    HttpClient::instance()->download(url, m_headers, path,
        [this, gen, startLocal, path](bool ok, const QString &error) {
            if (gen != m_playGen) return;          // 期间已经切歌
            if (!ok) {
                emit errorOccurred(QStringLiteral("播放失败：%1").arg(error));
                return;
            }
            startLocal(path);
        }, 600000);
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

qint64 PlayerController::duration() const
{
    const qint64 real = m_player->duration();
    if (real > 0)
        return real;
    // Windows 的 DirectShow 后端放 HTTP 流时经常报不出总长（实测界面显示
    // 01:05 / 00:00）：进度条、剩余时间、末行歌词的时长一起失效。队列里这首歌
    // 的元数据长度来自平台接口，够兜住界面。
    if (m_currentIndex >= 0 && m_currentIndex < m_queue.size()) {
        const qint64 meta = m_queue.at(m_currentIndex).duration;
        if (meta > 0)
            return meta;
    }
    return real;
}

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
    emit volumeChanged();
}

// 静音必须回到「静音前的档位」。以前界面各写一个常数（常驻栏 62、播放页 60），
// 既会覆盖用户调好的音量，两个入口还会跳出不同的结果。
void PlayerController::toggleMute()
{
    if (m_volume > 0) {
        m_volumeBeforeMute = m_volume;
        setVolume(0);
        return;
    }
    setVolume(m_volumeBeforeMute > 0 ? m_volumeBeforeMute : 80);
}

Song PlayerController::current() const
{
    if (m_currentIndex >= 0 && m_currentIndex < m_queue.size()) return m_queue.at(m_currentIndex);
    return Song();
}

void PlayerController::setCurrentCover(const QString &cover)
{
    if (cover.isEmpty() || m_currentIndex < 0 || m_currentIndex >= m_queue.size()) return;
    if (m_queue[m_currentIndex].cover == cover) return;
    m_queue[m_currentIndex].cover = cover;
    // 只刷新当前歌曲的展示数据。原来这里还发 queueChanged，会让整个队列模型
    // beginResetModel：队列滚动位置回到顶部、所有行重放入场动画。
    // 队列模型里的封面由 AppController 用 setCover() 做行级 dataChanged。
    emit mediaChanged();
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
    const bool wasCurrent = (index == m_currentIndex);
    m_queue.removeAt(index);

    if (m_queue.isEmpty()) {
        // 清空后必须同时停掉音频，否则会出现「队列空了/播放栏空白但还在响」
        m_currentIndex = -1;
        m_currentUrl = QUrl();
        m_headers.clear();
        m_player->stop();
        emit queueChanged();
        emit mediaChanged();
        emit stateChanged();
        return;
    }

    if (wasCurrent) {
        // 删掉正在播的歌：顺位接上下一首（删的是最后一首则停下并播剩的最后一首），
        // 避免 currentIndex 指向新歌、音频还停在已被删除的那首上。
        m_currentIndex = qMin(index, m_queue.size() - 1);
        emit queueChanged();
        emit mediaChanged();
        emitRequest(m_currentIndex);
        return;
    }

    if (m_currentIndex > index) --m_currentIndex;
    emit queueChanged();
}

void PlayerController::clearQueue()
{
    m_queue.clear();
    m_currentIndex = -1;
    m_currentUrl = QUrl();
    m_headers.clear();
    m_player->stop();
    emit queueChanged();
    emit mediaChanged();
    emit stateChanged();
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
    case 2:  // 单曲循环：用户主动点下一首仍然切歌，循环由 EndOfMedia 处理
        nextIndex = (m_currentIndex + 1) % m_queue.size();
        break;
    case 3:  // 随机
        nextIndex = QRandomGenerator::global()->bounded(m_queue.size());
        // 随机不应抽到自己：否则等于把当前歌重新解析一遍
        if (m_queue.size() > 1) {
            for (int guard = 0; nextIndex == m_currentIndex && guard < 8; ++guard)
                nextIndex = QRandomGenerator::global()->bounded(m_queue.size());
            if (nextIndex == m_currentIndex)
                nextIndex = (m_currentIndex + 1) % m_queue.size();
        }
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
    m_lyricLines.clear();
    if (m_lyrics.isEmpty()) {
        // 有些音源返回的是无时间轴的纯文本歌词，parseLrc 会返回空，
        // 原来会把整段文本丢掉，界面就一直显示「暂无歌词」。
        static const QRegularExpression metaTag(QStringLiteral("\\[[^\\]]*\\]"));
        const QStringList rawLines = lrc.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (QString line : rawLines) {
            line.remove(QLatin1Char('\r'));
            line.remove(metaTag);
            line = line.trimmed();
            if (!line.isEmpty()) {
                m_lyricLines << line;
                m_lyricsText += line + QLatin1Char('\n');
            }
        }
    } else {
        for (const LyricLine &l : m_lyrics) {
            m_lyricLines << l.text;
            m_lyricsText += l.text + QLatin1Char('\n');
        }
    }
    m_lyricIndex = -1;
    emit lyricsChanged();
}

QString PlayerController::currentLyricLine() const
{
    if (m_lyricIndex >= 0 && m_lyricIndex < m_lyrics.size()) return m_lyrics.at(m_lyricIndex).text;
    if (m_lyrics.isEmpty()) return QStringLiteral("暂无歌词");
    return QString();
}

QVariantList PlayerController::lyricTimes() const
{
    // 无时间轴歌词时返回空表，界面据此决定要不要做逐行扫光。
    QVariantList out;
    for (const LyricLine &l : m_lyrics)
        out.append(static_cast<long long>(l.time));
    return out;
}

void PlayerController::updateLyric(qint64 pos)
{
    if (m_lyrics.isEmpty()) return;
    // 绝大多数 positionChanged 落在当前行内部：命中就免掉一次整表线性扫描
    if (m_lyricIndex >= 0 && m_lyricIndex < m_lyrics.size()) {
        const qint64 start = m_lyrics.at(m_lyricIndex).time;
        const qint64 end = (m_lyricIndex + 1 < m_lyrics.size())
                               ? m_lyrics.at(m_lyricIndex + 1).time
                               : std::numeric_limits<qint64>::max();
        if (pos >= start && pos < end) return;
    }
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
