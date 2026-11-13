#include "PlayerController.h"

#include "core/QtCompat.h"
#include <QRegularExpression>

#include <limits>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QMediaContent>
#endif
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QNetworkRequest>
#include <QTimer>

#include "HttpClient.h"
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
        emit positionChanged();
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

    // ---- 续播落盘 ----
    // 每 5 秒记一次位置（Android 常被系统直接杀进程，来不及走 aboutToQuit）；
    // 切歌/播放状态变化也立刻落盘，退出时再补一次。
    m_saveTimer = new QTimer(this);
    m_saveTimer->setInterval(5000);
    connect(m_saveTimer, &QTimer::timeout, this, &PlayerController::saveState);
    m_saveTimer->start();
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
    m_pendingSeek = 0;
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
    m_pendingSeek = 0;
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
