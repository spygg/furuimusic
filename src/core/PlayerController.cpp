#include "PlayerController.h"

#include "core/QtCompat.h"
#include <QRegularExpression>

#include <limits>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QMediaContent>
#endif
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QNetworkRequest>
#include <QThread>
#include <QTimer>

#include "HttpClient.h"
#include <QDebug>
#include "Settings.h"

namespace Furui {

// ---- 续播状态的 Song <-> JSON（只存续播需要的字段，extra 透传自定义音源）----
static QJsonObject songToJson(const Song &s)
{
    QJsonObject o;
    o[QStringLiteral("uid")] = s.uid;
    o[QStringLiteral("source")] = s.source;
    o[QStringLiteral("sourceId")] = s.sourceId;
    o[QStringLiteral("title")] = s.title;
    o[QStringLiteral("artist")] = s.artist;
    o[QStringLiteral("album")] = s.album;
    o[QStringLiteral("cover")] = s.cover;
    o[QStringLiteral("albumId")] = s.albumId;
    o[QStringLiteral("duration")] = static_cast<double>(s.duration);
    o[QStringLiteral("vip")] = s.vip;
    o[QStringLiteral("playable")] = s.playable;
    o[QStringLiteral("quality")] = s.quality;
    if (!s.extra.isEmpty())
        o[QStringLiteral("extra")] = QJsonObject::fromVariantMap(s.extra);
    return o;
}

static Song songFromJson(const QJsonObject &o)
{
    Song s;
    s.uid = o.value(QStringLiteral("uid")).toString();
    s.source = o.value(QStringLiteral("source")).toString();
    s.sourceId = o.value(QStringLiteral("sourceId")).toString();
    s.title = o.value(QStringLiteral("title")).toString();
    s.artist = o.value(QStringLiteral("artist")).toString();
    s.album = o.value(QStringLiteral("album")).toString();
    s.cover = o.value(QStringLiteral("cover")).toString();
    s.albumId = o.value(QStringLiteral("albumId")).toString();
    s.duration = static_cast<qint64>(o.value(QStringLiteral("duration")).toDouble());
    s.vip = o.value(QStringLiteral("vip")).toBool();
    s.playable = o.value(QStringLiteral("playable")).toBool(true);
    s.quality = o.value(QStringLiteral("quality")).toString();
    s.extra = o.value(QStringLiteral("extra")).toObject().toVariantMap();
    return s;
}

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

// 缓存内容校验：防盗链过期时 CDN 常回 200 + HTML/JSON 错误页，仅凭 size>0
// 无法区分，会把错误页当音频永久缓存（清缓存前这首歌都播不出来）。
// 策略：识别常见音频魔数；命中已知「坏内容」特征直接拒绝；其余 exotic
// 格式按疑似音频放行，避免误杀。
static bool cacheFileLooksLikeAudio(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(16);
    f.close();
    if (head.size() < 4) return false;
    const unsigned char b0 = static_cast<unsigned char>(head.at(0));
    const unsigned char b1 = static_cast<unsigned char>(head.at(1));
    if (head.startsWith("ID3")) return true;                       // MP3 (ID3v2)
    if (b0 == 0xFF && (b1 & 0xE0) == 0xE0) return true;            // MP3/AAC 帧同步 (ADTS/ADIF 前导)
    if (head.startsWith("fLaC") || head.startsWith("OggS") ||
        head.startsWith("RIFF") || head.startsWith("MAC "))        // FLAC / OGG / WAV / APE
        return true;
    if (head.mid(4, 4) == "ftyp") return true;                     // MP4 / M4A
    // 明确的坏内容：HTML 错误页 / JSON 报错 / 文本 403 页
    if (head.startsWith("<") || head.startsWith("{") ||
        head.startsWith("%PDF") || head.startsWith("GIF8") ||
        head.startsWith("\x89PNG"))
        return false;
    return true;
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
        // 看门狗读数：位置往前走即说明真的在播，清停滞记录
        m_lastPosition = pos;
        if (pos > 500) m_everPlayed = true;
        if (pos > 0) m_stallSince = 0;
        emit positionChanged();
        // 兜底自动连播：部分后端（尤其在线流）在末尾不派发 EndOfMedia，
        // 距结束 150ms 内且确实在播放时排队切下一首；m_advancing 保证只触发一次。
        if (m_mode == 2 || m_advancing || m_suppressEnd) return;
        // 时长优先取播放器上报；在线流报 0 时退回队列元数据长度（duration() 已兜底），
        // 否则 Android 上在线流 duration=0，这条末尾兜底永远不触发，播完就停住。
        const qint64 total = duration();
        if (total <= 0 || pos < total - 150 || !playing()) return;
        m_advancing = true;
        qInfo("进度兜底触发下一首：%s（%lld/%lld）",
              qUtf8Printable(current().title), pos, total);
        QTimer::singleShot(0, this, [this] {
            if (m_advancing) next();
        });
    });
    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64) { emit stateChanged(); });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
        // 续播恢复：媒体加载就绪后跳到上次停播位置（只应用一次，之后清零）
        if (m_pendingSeek > 0
            && (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia)) {
            m_player->setPosition(m_pendingSeek);
            m_pendingSeek = 0;
        }
        // 新歌加载完成前忽略旧曲残留的结束信号。在线流常停留在 BufferingMedia
        // （而非 BufferedMedia），只认 Loaded/Buffered 会让 suppressEnd 一直挂着，
        // 末尾的 EndOfMedia 与进度兜底都被挡掉，表现为播完停住。这里只要进入有效
        // 媒体状态就放开；Invalid/NoMedia 保留旧标记由 playUrl 重置。
        if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia
            || status == QMediaPlayer::BufferingMedia || status == QMediaPlayer::StalledMedia)
            m_suppressEnd = false;
        if (status != QMediaPlayer::EndOfMedia) return;
        // 到达结尾先放开 suppressEnd，避免它仍挂着把这次结束信号挡掉
        m_suppressEnd = false;
        m_ended = true;
        if (m_mode == 2) {
            // 单曲循环：原地重播，不重新联网解析地址
            seek(0);
            m_player->play();
            m_ended = false;
            return;
        }
        if (m_advancing) return;
        m_advancing = true;
        qInfo("播放结束，排队下一首：%s（模式 %d，队列 %d，index %d）",
              qUtf8Printable(current().title), m_mode, int(m_queue.size()), m_currentIndex);
        // 不要在 FFmpeg/Android 正在分发 EndOfMedia 的调用栈里同步 stop + setSource：
        // 旧实现这样做时新歌经常起不来，表现为「播完一首就停住」。
        QTimer::singleShot(0, this, [this] {
            if (m_advancing) next();
        });
    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    connect(m_player, &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState) { emit stateChanged(); });
    connect(m_player, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error error, const QString &msg) {
                if (error == QMediaPlayer::NoError) return;
                qWarning("[播放器] 错误：%s（pos %lld / dur %lld，状态 %d）",
                         qUtf8Printable(msg), position(), duration(),
                         int(m_player->mediaStatus()));
                // 位置已经在结尾：这是正常播完，不是故障。在线流在 EOF 处常报
                // "Internal error" / "Failed to read"，若交给上层换源，就会把刚播完的
                // 这首重新取链再放一遍（听感上「播完又播当前这首」，再报播放错误）。
                if (atTrackEnd()) { finishAsEnded(); return; }
                // 带防盗链头的直链：先直连试播，被拒后再带 headers 下载缓存重试一次。
                // 大多数直链其实不校验 Referer，这样可以避免每次播放都先下载整首。
                if (!m_headerRetry && !m_headers.isEmpty()
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
                if (error == QMediaPlayer::NoError) return;
                // Qt5 DirectShow 失败时 errorString 可能是空串，直接透传会弹一个空 toast
                const QString msg = m_player->errorString();
                qWarning("[播放器] 错误：%s（pos %lld / dur %lld）",
                         qUtf8Printable(msg), position(), duration());
                // 同 Qt6：位置已在结尾的报错算正常播完，别把这首重新取链再放一遍
                if (atTrackEnd()) { finishAsEnded(); return; }
                // Qt5 的 Windows 后端是 DirectShow，它根本不读 QNetworkRequest 的自定义头，
                // 所以带防盗链的直链一定失败；这里必须和 Qt6 一样退回「带头下载再本地播放」，
                // 否则酷狗/QQ 这类校验 Referer 的源在 Windows 上永远播不了。
                if (!m_headerRetry && !m_headers.isEmpty()
                    && m_currentUrl.scheme().startsWith(QStringLiteral("http"))) {
                    m_headerRetry = true;
                    fetchWithHeaders();
                    return;
                }
                emit errorOccurred(msg.isEmpty() ? QStringLiteral("播放失败") : msg);
            });
#endif

    // ---- 续播落盘 ----
    // 每 5 秒记一次位置（Android 常被系统直接杀进程，来不及走 aboutToQuit）；
    // 切歌/播放状态变化也立刻落盘，退出时再补一次。
    m_saveTimer = new QTimer(this);
    m_saveTimer->setInterval(5000);
    connect(m_saveTimer, &QTimer::timeout, this, &PlayerController::saveState);
    m_saveTimer->start();

    // 播放看门狗：每秒巡检起播超时 / 末尾停滞 / 中途卡死，兜底后端丢失结束与错误信号
    m_watchdog = new QTimer(this);
    m_watchdog->setInterval(1000);
    connect(m_watchdog, &QTimer::timeout, this, &PlayerController::onWatchdog);
    connect(this, &PlayerController::queueChanged, this, &PlayerController::saveState);
    connect(this, &PlayerController::stateChanged, this, &PlayerController::saveState);
    if (QCoreApplication::instance())
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                this, &PlayerController::saveState);
}

void PlayerController::playUrl(const QUrl &url, const QVariantMap &headers)
{
    if (!url.isValid()) {
        emit errorOccurred(QStringLiteral("无效的播放地址"));
        return;
    }
    // 网易云等平台返回的播放直链常为 http://（如 http://m?.music.126.net/...）。
    // Android 9+ 默认禁止明文流量、桌面 Qt 部分环境也会拒绝 http 拉流，表现为
    // "Error transferring http://..."。与封面统一策略（MusicSource::mvCoverThumb）对齐，
    // 在此把 http 升 https，避免明文直链整批播放失败。
    QUrl play = url;
    if (play.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) == 0)
        play.setScheme(QStringLiteral("https"));
    ++m_playGen;
    m_headerRetry = false;
    m_advancing = false;
    m_ended = false;
    m_switching = false;
    // 新源加载完成（LoadedMedia/BufferedMedia）前，忽略旧曲残留的结束信号
    m_suppressEnd = true;
    // 重置看门狗读数：本次源重新计起播超时与停滞
    m_lastPosition = -1;
    m_stallSince = 0;
    m_everPlayed = false;
    m_loadStarted = QDateTime::currentMSecsSinceEpoch();
    m_watchdog->start();
    m_headers = headers;
    m_currentUrl = play;
    // 平台的直链几乎都带防盗链：sources.json 里声明的 Referer/Cookie 必须随请求发出。
    // 原来只把 headers 存起来，播放器从来没用到，QQ/网易/酷狗的直链在非官方来源上会被 CDN 拒掉。
    QNetworkRequest req(play);
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        const QByteArray value = it.value().toString().toUtf8();
        if (!value.trimmed().isEmpty()) req.setRawHeader(it.key().toUtf8(), value);
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Qt 6 的 setSource 不接受 QNetworkRequest，也没有等价的自定义头入口。
    // 策略：先按地址直连（多数直链不需要头）；如果播放器报错，errorOccurred
    // 里会调用 fetchWithHeaders() 带 headers 下载到缓存后改播本地文件。
    Q_UNUSED(req);
    // 换源前先把旧流彻底停掉：setSource 是异步的，新源首帧就绪前部分后端仍在
    // 输出上一路音频，听感上就是「切歌了还在放老歌」。
    m_player->stop();
    m_player->setSource(play);
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
    // 缓存键必须覆盖完整 URL（scheme+host+path+query）。防盗链直链普遍形如
    // https://host/stream?id=xxx&vkey=yyy —— 只取 path 的话所有歌曲同键，
    // 会命中同一个缓存文件，表现为「点 A 播放 B」。
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(url.toString(QUrl::FullyEncoded).toUtf8(),
                                 QCryptographicHash::Sha1).toHex());
    const QString path = dir + QLatin1Char('/') + key + QLatin1Char('.') + cacheSuffixFor(url);

    auto startLocal = [this](const QString &p) {
        // 本地缓存就绪：重新起看门狗，按本地文件的起播/停滞重新计时
        m_lastPosition = -1;
        m_stallSince = 0;
        m_everPlayed = false;
        m_loadStarted = QDateTime::currentMSecsSinceEpoch();
        m_watchdog->start();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        m_player->setSource(QUrl::fromLocalFile(p));
#else
        m_player->setMedia(QMediaContent(QUrl::fromLocalFile(p)));
#endif
        m_player->play();
        emit stateChanged();
    };

    if (QFileInfo(path).size() > 0) {
        if (cacheFileLooksLikeAudio(path)) {
            startLocal(path);
            return;
        }
        // 历史遗留的坏缓存（如防盗链过期时的 200+HTML 错误页）删掉重下
        QFile::remove(path);
    }

    // 下载整首到缓存期间播放器处于 StoppedState：暂停看门狗，避免把「下载中」误判成
    // 起播超时；本地文件就绪后由 startLocal 重新起表。
    m_watchdog->stop();

    HttpClient::instance()->download(url, m_headers, path,
        [this, gen, startLocal, path](bool ok, const QString &error) {
            if (gen != m_playGen) return;          // 期间已经切歌
            if (!ok) {
                emit errorOccurred(QStringLiteral("播放失败：%1").arg(error));
                return;
            }
            // 防盗链过期时 CDN 常回 200 + HTML/JSON 错误页，size>0 但不是音频。
            // 不校验就会把错误页永久缓存，这首歌在清理缓存前永远播不出来。
            if (!cacheFileLooksLikeAudio(path)) {
                QFile::remove(path);
                emit errorOccurred(QStringLiteral("播放失败：取流被拒绝（链接已失效）"));
                return;
            }
            startLocal(path);
        }, 600000);
}

void PlayerController::togglePause()
{
    if (!m_currentUrl.isValid()) {
        // 启动时恢复了「暂停」态（还没加载音频）：按下播放即开始解析，从上次位置继续
        if (m_currentIndex >= 0 && m_currentIndex < m_queue.size())
            emitRequest(m_currentIndex);
        return;
    }
    if (playing()) {
        m_player->pause();
        emit stateChanged();
        return;
    }
    // 切歌取链的空窗期：新地址马上就到。此时 m_currentUrl 还指着上一首，
    // 直接 play() 会把刚播完（或刚要切走）的那首从头再放一遍，随后新地址到达又打断它，
    // 听感/界面就是「播完一首又去播当前这首，然后报播放错误」。
    if (m_switching) {
        qInfo("忽略播放请求：正在切换音源（%s）", qUtf8Printable(current().title));
        return;
    }
    // 已经播到结尾：这里的「播放」应该是下一首。Android 后端 play() 会从 0 重放当前曲，
    // 单曲循环除外（那是用户明确要的原地重播）。
    if (m_ended && m_mode != 2) {
        // 媒体键是从 JNI 线程同步进来的（见 AndroidMediaSession 的说明），
        // 切歌要改队列、发一堆信号，必须回主线程做
        if (QThread::currentThread() != thread()) {
            qInfo("播完后收到播放请求（媒体键线程），排队切下一首");
            QMetaObject::invokeMethod(this, [this] { next(); }, Qt::QueuedConnection);
            return;
        }
        qInfo("播完后收到播放请求，按下一首处理：%s", qUtf8Printable(current().title));
        next();
        return;
    }
    m_player->play();
    m_ended = false;
    emit stateChanged();
}

void PlayerController::stop()
{
    m_watchdog->stop();
    m_lastPosition = -1;
    m_stallSince = 0;
    m_everPlayed = false;
    m_ended = false;
    m_switching = false;
    m_player->stop();
    emit stateChanged();
}

void PlayerController::beginSwitch()
{
    stop();              // stop() 会清掉 m_switching，所以在它之后再置位
    m_switching = true;
}

// 这次「错误」是否其实已经播到结尾。在线流的 FFmpeg/Android 后端在 EOF 处
// 经常派发 errorOccurred（"Internal error" / "Failed to read"），若位置已到结尾附近
// 就不能当故障处理 —— 否则上层会把同一首歌重新取链再放一遍。
bool PlayerController::atTrackEnd() const
{
    if (m_ended) return true;
    const qint64 total = duration();
    const qint64 pos = position();
    if (!m_everPlayed || total <= 0) return false;
    // 5 秒余量：替代音源的真实时长与平台元数据常有几秒出入，太紧会把「正常播完」
    // 当成故障，又回到「重新取链放同一首」的老路上。
    return pos >= total - 5000;
}

void PlayerController::finishAsEnded()
{
    m_ended = true;
    if (m_mode == 2) {
        qInfo("结尾报错，按单曲循环原地重播：%s", qUtf8Printable(current().title));
        seek(0);
        m_player->play();
        m_ended = false;
        return;
    }
    if (m_advancing) return;
    m_advancing = true;
    qInfo("结尾报错，按播完处理，切下一首：%s（%lld/%lld）",
          qUtf8Printable(current().title), position(), duration());
    QTimer::singleShot(0, this, [this] {
        if (m_advancing) next();
    });
}

// 看门狗巡检：兜底 Android MediaPlayer 等后端丢失 EndOfMedia / errorOccurred 的情况。
// 不直接换源（换源策略在上层 AppController），只把「起播超时 / 末尾停住 / 中途卡死」
// 翻译成 errorOccurred 或一次下一首推进，与现有 EndOfMedia、进度兜底链路汇合。
void PlayerController::onWatchdog()
{
    if (m_currentUrl.isEmpty() || m_advancing) return;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const auto state = m_player->playbackState();
#else
    const auto state = m_player->state();
#endif
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 pos = position();
    const qint64 total = duration();

    if (state == QMediaPlayer::StoppedState) {
        if (pos > 0) {
            // 出过声音后自己停住：结束信号丢失，兜底切下一首
            m_advancing = true;
            m_ended = true;
            qInfo("看门狗：结束信号丢失，兜底下一首：%s（pos %lld）",
                  qUtf8Printable(current().title), pos);
            QTimer::singleShot(0, this, [this] { if (m_advancing) next(); });
        } else {
            // 一直没起播：超过 15s 判加载失败，交上层逐源换源
            const qint64 elapsed = m_loadStarted > 0 ? now - m_loadStarted : 0;
            if (elapsed >= 15000) {
                m_watchdog->stop();
                qWarning("看门狗：起播超时（%lldms）：%s", elapsed,
                         qUtf8Printable(current().title));
                emit errorOccurred(QStringLiteral("加载超时，无法播放"));
            }
        }
        return;
    }

    // 暂停 / 缓冲中：不干预，等回到播放态再看位置
    if (state != QMediaPlayer::PlayingState) return;

    // 位置往前走：播放正常，清停滞记录
    if (pos > m_lastPosition) {
        m_lastPosition = pos;
        m_stallSince = 0;
        m_everPlayed = true;
        return;
    }

    // 位置停滞：距结尾 3s 内视为播完，兜底切歌
    if (total > 0 && pos >= total - 3000) {
        m_advancing = true;
        m_ended = true;
        qInfo("看门狗：末尾停滞，兜底下一首：%s（%lld/%lld）",
              qUtf8Printable(current().title), pos, total);
        QTimer::singleShot(0, this, [this] { if (m_advancing) next(); });
        return;
    }

    if (m_stallSince == 0) {
        m_stallSince = now;
        return;
    }
    // 替代音源的实际长度常短于平台元数据（VIP 替代/跨平台匹配到的音源时长不同），
    // 于是「已经放完」时 pos 可能永远到不了 total-3000，只会停在那里不动。
    // 停滞 4 秒且已在末尾 2%（至少 8 秒）以内，同样按播完处理 —— 否则要等 15 秒
    // 才判「播放中断」，再把这件已经播完的歌重新取链放一遍（听感上就是「播完又播当前这首」）。
    if (total > 0 && pos >= total - qMax<qint64>(8000, total / 50)) {
        if (now - m_stallSince >= 4000) {
            m_advancing = true;
            m_ended = true;
            qInfo("看门狗：已到末尾停滞 %lldms，按播完切下一首：%s（%lld/%lld）",
                  now - m_stallSince, qUtf8Printable(current().title), pos, total);
            QTimer::singleShot(0, this, [this] { if (m_advancing) next(); });
        }
        return;
    }
    // 中途停滞超过 15s 仍不动：判播放失败，交上层换源 / 跳下一首
    if (now - m_stallSince >= 15000) {
        m_watchdog->stop();
        qWarning("看门狗：播放停滞 %lldms：%s", now - m_stallSince,
                 qUtf8Printable(current().title));
        emit errorOccurred(QStringLiteral("播放中断，正在尝试其它音源"));
    }
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

void PlayerController::saveState()
{
    if (m_queue.isEmpty()) {
        Settings::set(QStringLiteral("resumeQueue"), QString());
        Settings::set(QStringLiteral("resumeIndex"), -1);
        Settings::set(QStringLiteral("resumePosition"), 0);
        Settings::set(QStringLiteral("resumePlaying"), false);
        return;
    }
    QJsonArray arr;
    for (const Song &s : m_queue)
        arr.append(songToJson(s));
    Settings::set(QStringLiteral("resumeQueue"),
                  QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    Settings::set(QStringLiteral("resumeIndex"), m_currentIndex);
    // 恢复位置还没真正应用（媒体未加载完成）前，保留待跳转点，别被 0 覆盖掉
    const qint64 pos = m_pendingSeek > 0 ? m_pendingSeek : position();
    Settings::set(QStringLiteral("resumePosition"), pos);
    Settings::set(QStringLiteral("resumePlaying"), playing());
}

// 修复历史坏数据：Qt5 构建曾把 Qt6 写的裸 UTF-8 ini 按 Latin-1 读花再存回，
// resumeQueue 里的标题/歌手已变成 Latin-1 化的乱码（层数 = 经过的 Qt5 会话数）。
// 修复 = 把这种「Latin-1 字符串」重新按 UTF-8 解码，最多 4 轮（乱码每多经过
// 一次旧 Qt5 会话加深一层，4 轮足够兜住历史会话）。仅当整段内容每个字符
// ≤ 0xFF、字节序列是合法 UTF-8（往返无损）时才修——正常中文（> 0xFF）和
// 真实拉丁文（如 é 的 Latin-1 字节不是合法 UTF-8）都会在此止步，零误伤。
static bool unmojibake(QString &text)
{
    bool changed = false;
    for (int round = 0; round < 4; ++round) {
        QByteArray bytes(int(text.size()), Qt::Uninitialized);
        bool latin1 = true;
        for (int i = 0; i < text.size(); ++i) {
            const ushort u = text.at(i).unicode();
            if (u > 0xFF) { latin1 = false; break; }
            bytes[i] = char(u);
        }
        if (!latin1) break;
        const QString decoded = QString::fromUtf8(bytes);
        if (decoded == text) break;            // 纯 ASCII 或已修干净
        if (decoded.toUtf8() != bytes) break;  // 不是合法 UTF-8，不能修
        text = decoded;
        changed = true;
    }
    return changed;
}

void PlayerController::restoreState()
{
    const QString raw = Settings::string(QStringLiteral("resumeQueue"));
    QString json = raw;
    if (!json.isEmpty() && unmojibake(json)) {
        // 损坏历史数据自愈：立即写回修好的文本（不动 index/position/playing）
        const QJsonDocument probe = QJsonDocument::fromJson(json.toUtf8());
        if (probe.isArray())
            Settings::set(QStringLiteral("resumeQueue"), json);
        else
            json = raw;   // 转码后反而不成 JSON：放弃修复，按原文解析
    }
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isArray())
        return;
    QVector<Song> restored;
    for (const QJsonValue &v : doc.array())
        if (v.isObject())
            restored.append(songFromJson(v.toObject()));
    if (restored.isEmpty())
        return;

    m_queue = restored;
    const int idx = Settings::integer(QStringLiteral("resumeIndex"), 0);
    m_currentIndex = qBound(-1, idx, m_queue.size() - 1);
    m_pendingSeek = static_cast<qint64>(Settings::integer(QStringLiteral("resumePosition"), 0));
    const bool wasPlaying = Settings::boolean(QStringLiteral("resumePlaying"), false);

    // 先让队列/播放条立即显示上次的播放列表与曲目（此时 pendingSeek 已就位，
    // 上面 saveState 不会被 0 覆盖）
    emit queueChanged();
    emit mediaChanged();
    if (wasPlaying && m_currentIndex >= 0) {
        // 上次正在播 → 自动续播；具体位置在媒体加载完成后由 m_pendingSeek 应用
        emitRequest(m_currentIndex);
    }
}

void PlayerController::seek(qint64 ms)
{
    if (ms < 0) ms = 0;
    // 往回拖过结尾：这首重新算「没播完」，之后再按播放是继续播而不是跳下一首
    const qint64 total = duration();
    if (total <= 0 || ms < total - 2000) {
        m_ended = false;
        m_stallSince = 0;
        m_lastPosition = ms;
    }
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
    m_pendingSeek = 0;   // 用户显式换歌/开新列表，不再套用续播位置
    m_advancing = false;
    m_suppressEnd = false;
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
    m_pendingSeek = 0;
    m_advancing = false;
    m_suppressEnd = false;
    m_ended = false;
    m_switching = false;
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
    // 媒体键（JNI 线程）也会走到这里：切歌要改队列并发一串信号，必须回主线程
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this] { next(); }, Qt::QueuedConnection);
        return;
    }
    advance(true);
}

void PlayerController::playNextInQueue()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this] { playNextInQueue(); }, Qt::QueuedConnection);
        return;
    }
    advance(false);
}

// allowRecommend=true（用户点下一首 / 播完自动切歌）时：
//   1) 汽水漫游开启且当前是汽水果 → 交给 AppController 按当前歌取推荐；
//   2) 顺序播放走到队列末尾 → 也交给 AppController 取推荐续上。
// allowRecommend=false 只用于「推荐已经插好，继续推进」和「推荐失败后的兜底」，
// 避免同一次切歌反复请求推荐。
void PlayerController::advance(bool allowRecommend)
{
    if (m_queue.isEmpty()) return;
    m_advancing = false;   // 本次推进开始；排队回调看到 false 就不会重复切歌
    m_ended = false;       // 换歌后这首的「已播完」状态作废

    const Song cur = current();
    if (allowRecommend && cur.source == QLatin1String("qishui") && !cur.sourceId.isEmpty()) {
        if (Settings::boolean(QStringLiteral("qishuiRoam"), false)) {
            emit recommendationNeeded();
            return;
        }
    }

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
        if (m_currentIndex + 1 >= m_queue.size()) {
            // 队列播完：先问上层要一首推荐续上，取不到再停
            if (allowRecommend) {
                emit recommendationNeeded();
                return;
            }
            stop();
            return;
        }
        nextIndex = m_currentIndex + 1;
        break;
    default: // 列表循环
        nextIndex = (m_currentIndex + 1) % m_queue.size();
        break;
    }
    m_currentIndex = nextIndex;
    m_pendingSeek = 0;
    qInfo("切到下一首：index %d / %d「%s」", nextIndex, int(m_queue.size()),
          qUtf8Printable(m_queue.at(nextIndex).title));
    emit mediaChanged();
    emitRequest(nextIndex);
}

void PlayerController::insertAfterCurrent(const QVector<Song> &songs)
{
    if (songs.isEmpty()) return;
    const int at = m_currentIndex < 0
            ? m_queue.size() : qMin(m_currentIndex + 1, m_queue.size());
    for (int i = 0; i < songs.size(); ++i) m_queue.insert(at + i, songs.at(i));
    emit queueChanged();
}

void PlayerController::previous()
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this] { previous(); }, Qt::QueuedConnection);
        return;
    }
    if (m_queue.isEmpty()) return;
    m_advancing = false;
    m_ended = false;
    // 上一首就是上一首：不再走「已播 5 秒就先重头播本曲」的行业惯例。
    // 那样第一次点击只是 restart、第二次才真换歌，真机上会被当成按钮失灵。
    int prev = m_currentIndex - 1;
    if (prev < 0) {
        if (m_mode == 0) {
            // 顺序播放已在队首：没有上一首，保持现状（也不重头播当前这首）
            qInfo("上一首：已在队首，忽略");
            return;
        }
        prev = m_queue.size() - 1;
    }
    if (prev == m_currentIndex) return;
    m_currentIndex = prev;
    m_pendingSeek = 0;
    qInfo("切到上一首：index %d / %d「%s」", prev, int(m_queue.size()),
          qUtf8Printable(m_queue.at(prev).title));
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

void PlayerController::setLyrics(const QString &lrc, const QString &trans)
{
    m_lyrics = parseLrc(lrc);
    // 翻译轨按时间戳对齐到原文行：两边都升序，双指针就近合并（差 ≤1.2s 才认），
    // 平台翻译轨偶尔会多/少几行（纯音乐段落），对不上的行直接丢弃。
    if (!trans.trimmed().isEmpty() && !m_lyrics.isEmpty()) {
        const QVector<LyricLine> tl = parseLrc(trans);
        int j = 0;
        int merged = 0;
        for (int i = 0; i < m_lyrics.size() && j < tl.size(); ++i) {
            // 丢弃远早于当前原文行的翻译行（原文开头的 Credit 行没有翻译，
            // 不能因为「这行没对上」就消耗掉后面的翻译行，否则整体错位）。
            while (j < tl.size() && tl.at(j).time + 1200 < m_lyrics.at(i).time) ++j;
            if (j >= tl.size()) break;
            const qint64 diff = qAbs(tl.at(j).time - m_lyrics.at(i).time);
            if (diff <= 1200) { m_lyrics[i].trans = tl.at(j).text; ++merged; ++j; }
            // diff 过大时不消耗 j：tl[j] 可能对应后面的原文行
        }
        qWarning().noquote() << "[lyrics] setLyrics: lrc=" << lrc.size()
                             << "trans=" << trans.size()
                             << "tl=" << tl.size() << "merged=" << merged;
    }
    m_lyricsText.clear();
    m_lyricLines.clear();
    m_lyricTransLines.clear();
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
                m_lyricTransLines << QString();
                m_lyricsText += line + QLatin1Char('\n');
            }
        }
    } else {
        for (const LyricLine &l : m_lyrics) {
            m_lyricLines << l.text;
            m_lyricTransLines << l.trans;
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
