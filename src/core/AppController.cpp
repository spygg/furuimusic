#include "AppController.h"

#include <QDesktopServices>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSslSocket>
#include <QSet>
#include <QTimer>
#include <QUrl>

#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#include <jni.h>
#elif defined(Q_OS_ANDROID)
#include <QAndroidJniObject>
#include <QtAndroid>
#endif

#include "AuthManager.h"
#include "CustomSource.h"
#include "HttpClient.h"
#include "Library.h"
#include "Logger.h"
#include "OpenApiServer.h"
#include "PlayerController.h"
#include "ScriptSource.h"
#include "UserApi.h"
#include "Settings.h"
#include "SourceConfig.h"
#include "SourceManager.h"

namespace Furui {

// 落盘后缀必须跟真实编码一致：flac/m4a 存成 .mp3 时，Windows 靠嗅探还能放，
// Android/GStreamer 一侧直接拒绝打开，库里也会按扩展名判断格式。
static QString extensionForUrl(const QUrl &url)
{
    static const QStringList known = {QStringLiteral("mp3"), QStringLiteral("flac"),
                                      QStringLiteral("m4a"), QStringLiteral("aac"),
                                      QStringLiteral("ogg"), QStringLiteral("opus"),
                                      QStringLiteral("wav"), QStringLiteral("wma")};
    const QString suffix = QFileInfo(url.path()).suffix().toLower();
    return known.contains(suffix) ? suffix : QStringLiteral("mp3");
}

// ---------------------------------------------------------------- 模型实现
// 历史数据里有把专辑名/歌名写进 cover 的旧记录（早期解析遗留，也会跟着续播
// 队列一起持久化）。原样交给 QML 会变成 "Cannot open: qrc:/qml/<歌名>" 的
// 重复告警，封面也永远是占位图 —— 这里只放行真正的图片地址。
static QString coverUrlOrEmpty(const QString &cover)
{
    const QString c = cover.trimmed();
    if (c.isEmpty()) return QString();
    if (c.startsWith(QLatin1String("http://")) || c.startsWith(QLatin1String("https://"))
        || c.startsWith(QLatin1String("file://")) || c.startsWith(QLatin1String("qrc:/"))
        || c.startsWith(QLatin1String("image://")) || c.startsWith(QLatin1String("data:")))
        return c;
    return QString();
}

SongListModel::SongListModel(QObject *parent) : QAbstractListModel(parent) {}

int SongListModel::rowCount(const QModelIndex &parent) const
{ return parent.isValid() ? 0 : m_songs.size(); }

QVariant SongListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_songs.size()) return QVariant();
    const Song &s = m_songs.at(index.row());
    switch (role) {
    case UidRole:       return s.uid;
    case TitleRole:     return s.title;
    case ArtistRole:    return s.artist;
    case AlbumRole:     return s.album;
    case DurationRole:  return s.duration;
    case CoverRole:     return coverUrlOrEmpty(s.cover);
    case SourceRole:    return s.source;
    case SourceNameRole:return SourceConfig::instance()->nameOf(s.source);
    case VipRole:       return s.vip;
    case BadgeRole:     return s.vip ? QStringLiteral("VIP") : QString();
    case SourceIdRole:  return s.sourceId;
    case FavoriteRole:  return Library::instance()->isFavorite(s.uid);
    case InQueueRole:   return m_queueUids.contains(s.uid);
    case RowRole:       return index.row();
    default:            return QVariant();
    }
}

QHash<int, QByteArray> SongListModel::roleNames() const
{
    return {
        {UidRole, "uid"}, {TitleRole, "title"}, {ArtistRole, "artist"}, {AlbumRole, "album"},
        {DurationRole, "duration"}, {CoverRole, "cover"}, {SourceRole, "source"},
        {SourceNameRole, "sourceName"}, {VipRole, "vip"}, {BadgeRole, "badge"},
        {SourceIdRole, "sourceId"}, {FavoriteRole, "favorite"}, {InQueueRole, "inQueue"},
        {RowRole, "row"}
    };
}

void SongListModel::setQueueUids(const QSet<QString> &uids)
{
    if (m_queueUids == uids) return;
    m_queueUids = uids;
    if (m_songs.isEmpty()) return;
    // 整表刷新 inQueue 列（uid→bool，无副作用），QML 侧 SongRow 的「＋/✓」据此切换
    emit dataChanged(index(0, 0), index(m_songs.size() - 1, 0), {InQueueRole});
}

void SongListModel::setSongs(const QVector<Song> &songs)
{
    beginResetModel();
    m_songs = songs;
    endResetModel();
}

void SongListModel::appendSongs(const QVector<Song> &songs)
{
    if (songs.isEmpty()) return;
    beginInsertRows(QModelIndex(), m_songs.size(), m_songs.size() + songs.size() - 1);
    m_songs.append(songs);
    endInsertRows();
}

void SongListModel::refreshRow(int row)
{
    if (row < 0 || row >= m_songs.size()) return;
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, {FavoriteRole});
}

void SongListModel::setCover(const QString &uid, const QString &cover)
{
    if (uid.isEmpty() || cover.isEmpty()) return;
    for (int i = 0; i < m_songs.size(); ++i) {
        if (m_songs.at(i).uid != uid || m_songs.at(i).cover == cover) continue;
        m_songs[i].cover = cover;
        const QModelIndex idx = index(i, 0);
        emit dataChanged(idx, idx, {CoverRole});
    }
}

void SongListModel::clear() { setSongs(QVector<Song>()); }

// ---------------------------------------------------------------- 歌单内搜索过滤
SongFilterModel::SongFilterModel(QObject *parent) : QSortFilterProxyModel(parent)
{
    // 大小写折叠由 filterAcceptsRow 自己做，关闭基类的正则/通配语义，
    // 避免歌名里的 "(", "*" 等字符被当成模式符号
    setFilterCaseSensitivity(Qt::CaseInsensitive);
    setFilterFixedString(QString());
}

void SongFilterModel::setFilter(const QString &f)
{
    if (m_filter == f) return;
    m_filter = f;
    // 空过滤也要整表刷新：beginFilterChange 语义下 invalidate 折叠成一次 reset
    invalidateFilter();
    emit filterChanged();
}

bool SongFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (m_filter.trimmed().isEmpty()) return true;
    const QString f = m_filter.trimmed();
    const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
    const QString title = idx.data(SongListModel::TitleRole).toString();
    const QString artist = idx.data(SongListModel::ArtistRole).toString();
    return title.contains(f, Qt::CaseInsensitive) || artist.contains(f, Qt::CaseInsensitive);
}

PlaylistListModel::PlaylistListModel(QObject *parent) : QAbstractListModel(parent) {}

int PlaylistListModel::rowCount(const QModelIndex &parent) const
{ return parent.isValid() ? 0 : m_list.size(); }

QVariant PlaylistListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_list.size()) return QVariant();
    const Playlist &p = m_list.at(index.row());
    switch (role) {
    case UidRole:        return p.uid;
    case TitleRole:      return p.title;
    case CoverRole:      return coverUrlOrEmpty(p.cover);
    case CountRole:      return p.count;
    case SourceRole:     return p.source;
    case SourceNameRole: return SourceConfig::instance()->nameOf(p.source);
    case TypeRole:       return p.type;
    case OwnerRole:      return p.owner;
    default:             return QVariant();
    }
}

QHash<int, QByteArray> PlaylistListModel::roleNames() const
{
    return {
        {UidRole, "uid"}, {TitleRole, "title"}, {CoverRole, "cover"}, {CountRole, "count"},
        {SourceRole, "source"}, {SourceNameRole, "sourceName"}, {TypeRole, "type"},
        {OwnerRole, "owner"}
    };
}

void PlaylistListModel::setPlaylists(const QVector<Playlist> &list)
{
    beginResetModel();
    m_list = list;
    endResetModel();
}

void PlaylistListModel::clear() { setPlaylists(QVector<Playlist>()); }

AccountListModel::AccountListModel(QObject *parent) : QAbstractListModel(parent) {}

int AccountListModel::rowCount(const QModelIndex &parent) const
{ return parent.isValid() ? 0 : m_list.size(); }

QVariant AccountListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_list.size()) return QVariant();
    const Account &a = m_list.at(index.row());
    switch (role) {
    case SourceRole:   return a.source;
    case NameRole:     return m_meta.value(a.source).name;
    case NicknameRole: return a.nickname;
    case AvatarRole:   return a.avatar;
    case LoggedRole:   return a.logged;
    case StatusRole:   return a.status.isEmpty() ? (a.logged ? QStringLiteral("已登录") : QStringLiteral("未登录")) : a.status;
    case MethodsRole:  return m_meta.value(a.source).methods;
    case ApiAvailableRole: return m_meta.value(a.source).apiAvailable;
    case ApiStatusRole:    return m_meta.value(a.source).apiStatus;
    case PlaylistCountRole:return m_meta.value(a.source).playlistCount;
    default:           return QVariant();
    }
}

QHash<int, QByteArray> AccountListModel::roleNames() const
{
    return {
        {SourceRole, "source"}, {NameRole, "name"}, {NicknameRole, "nickname"},
        {AvatarRole, "avatar"}, {LoggedRole, "logged"}, {StatusRole, "status"},
        {MethodsRole, "methods"}, {ApiAvailableRole, "apiAvailable"},
        {ApiStatusRole, "apiStatus"}, {PlaylistCountRole, "playlistCount"}
    };
}

void AccountListModel::setAccounts(const QVector<Account> &list, const QHash<QString, Meta> &meta)
{
    beginResetModel();
    m_list = list;
    m_meta = meta;
    endResetModel();
}

// ---------------------------------------------------------------- 门面实现
struct AppController::Impl {
    QString status = QStringLiteral("就绪");
    QString substituteBadge;
    QString currentSourceLabel;
    QString quality;
    QString searchType = QStringLiteral("song");
    bool vipSubstitute = true;
    QString lastResolvedUrl;
    QVariantMap lastPlayableHeaders;
    QStringList customIds;
    QStringList customNames;
    // 空串：显示全部平台；否则只显示该平台的本地歌单
    QString playlistFilter;
    // 每次切歌递增：歌词是异步回来的，旧请求后到会把上一首的歌词盖到当前这首上
    quint64 lyricGen = 0;
    // 封面补齐同样是异步的，切歌后迟到的结果不能盖到新歌上
    quint64 coverGen = 0;
    // 异步搜索/榜单/歌单请求的内容代次：慢请求返回时若用户已发起新操作就丢弃，
    // 避免旧结果覆盖新结果（与 lyricGen 同一模式）
    // searchRequestId：任何会接管结果区/搜索状态的操作都会 +1（歌曲搜索、歌单搜索、
    // 打开歌单、收藏/历史、榜单），旧回调据此整条链路作废。
    quint64 searchRequestId = 0;
    // 挂起中的搜索类型（"" / "song" / "playlist"），用于判断该不该补发终止信号
    QString searchKind;
    bool searching = false;
    // 只服务于歌单模型：本地库刷新（applyPlaylistFilter）时作废挂起的歌单/榜单请求，
    // 但不应误伤正在进行的歌曲搜索
    quint64 playlistGen = 0;
    bool discoveryLoading = false;
    // 平台切换时作废过期的 discovery 请求，避免旧平台结果覆盖新平台
    quint64 discoveryGen = 0;
    // 取链是异步的：用户连点两首歌时，慢的那首不能把当前播放替换掉
    quint64 resolveGen = 0;
    // 播放失败换源：记录当前正在播的歌，以及已尝试过的源（避免重复/死循环）；
    // inFallback 在换源解析期间置真，换源成功开始播放后清掉，以便本次播放再失败时能试下一源。
    Song currentSong;
    QSet<QString> triedFallback;
    bool inFallback = false;
    quint64 fallbackGen = 0;
    // 连续「所有音源都换过仍播不出来」的次数：达到队列长度就停下，避免列表循环下
    // 一首首失败歌被自动跳过N后绕回起点、无限重播。某首歌真正开始播放（进度>2s）后清零。
    int consecutiveFailures = 0;
    // 同一首歌反复「取到直链但播不出来」的次数：脚本音源偶尔返回坏地址（过期/防盗链
    // /格式不对），会形成 取链→播放失败→再取链 的死循环，屏幕上表现为反复换源但一直没声。
    // 同一首连续 3 次就放弃并跳下一首，把决定权交回队列。
    QString guardSongUid;
    int guardSongTries = 0;
    // 脚本音源刚取到链、尚未确认能播的（engineId, platformKey）。进度走过 2 秒补记成功，
    // 播放报错记重度失败 —— 「取到地址」和「能播出声音」是两件事，成绩表只认后者。
    QString pendingEngine;
    QString pendingKey;
    // 推荐请求在途：连点下一首时只允许有一个 /luna/track/related 请求
    bool recommendPending = false;
    // 曲库页搜索框是否有内容：外壳据此隐藏播放栏，给搜索结果/软键盘让位
    bool searchActive = false;
};

AppController *AppController::s_instance = nullptr;

AppController *AppController::instance()
{
    if (!s_instance) s_instance = new AppController();
    return s_instance;
}

AppController::AppController(QObject *parent)
    : QObject(parent), d(new Impl)
{
    s_instance = this;
    m_results   = new SongListModel(this);
    m_queue     = new SongListModel(this);
    m_playlists = new PlaylistListModel(this);
    m_discovery = new PlaylistListModel(this);
    m_accounts  = new AccountListModel(this);
    // 歌单详情的过滤代理：源永远是 m_results（歌单/榜单/收藏/搜索共用同一结果模型）
    m_detailFilter = new SongFilterModel(this);
    m_detailFilter->setSourceModel(m_results);

    d->quality = Settings::string(QStringLiteral("quality"), QStringLiteral("standard"));
    d->searchType = Settings::string(QStringLiteral("searchType"), QStringLiteral("song"));
    d->vipSubstitute = Settings::boolean(QStringLiteral("vipSubstitute"), true);

    OpenApiServer *api = OpenApiServer::instance();
    connect(api, &OpenApiServer::toast, this, [this](const QString &msg) { emit toast(msg); });
    connect(api, &OpenApiServer::stateChanged, this, [this] { emit settingsChanged(); });
    Logger::instance()->setFileEnabled(Settings::boolean(QStringLiteral("logToFile"), false));
    if (Settings::boolean(QStringLiteral("openApi"), false))
        api->start(Settings::integer(QStringLiteral("openApiPort"), 23330), false);

    Library::instance();
    SourceConfig::instance();
    CustomSourceManager::instance();
    SourceManager::instance();
    AuthManager::instance();
    PlayerController *player = PlayerController::instance();

    connect(player, &PlayerController::stateChanged, this, [this] {
        emit playingChanged();
        emit playerChanged();
    });
    connect(player, &PlayerController::mediaChanged, this, [this] { emit playerChanged(); });
    connect(player, &PlayerController::positionChanged, this, [this, player] {
        // 进度走过 2 秒说明这首歌真的在播：清零「连续播放失败」计数。
        // 这样自动跳过失败歌的过程中，中间任何一首正常播放的歌都会把额度恢复，
        // 不会因为前面跳过几首而在后面误停。
        if (d->consecutiveFailures > 0 && player->position() > 2000)
            d->consecutiveFailures = 0;
        // 真的播起来了 → 这首歌不是坏歌。同一次会话里反复播放同一首歌不该被
        // 「连续失败 3 次」的闸门误伤（早先的计数在进 resolveAndPlay 时就 +1，
        // 手动重播四次就会被判成坏歌）。
        if (d->guardSongTries > 0 && player->position() > 2000)
            d->guardSongTries = 0;
        // 进度走过 2 秒 = 这条直链真的能播：给提供它的「脚本 × 平台」补记成功分
        if (!d->pendingEngine.isEmpty() && player->position() > 2000) {
            ScriptSourceManager::instance()->reportResult(d->pendingEngine, d->pendingKey, true, 2);
            ScriptSourceManager::instance()->promoteSource(d->pendingEngine);
            qInfo().noquote() << QStringLiteral("[换源] 确认可播放，加分并置顶：%1 [%2]")
                                     .arg(d->pendingEngine, d->pendingKey);
            d->pendingEngine.clear();
            d->pendingKey.clear();
        }
        emit positionChanged();
    });
    connect(player, &PlayerController::volumeChanged, this, [this] { emit volumeChanged(); });
    connect(player, &PlayerController::duckedChanged, this, [this] { emit duckedChanged(); });
    connect(player, &PlayerController::lyricsChanged, this, [this] { emit lyricsChanged(); });
    connect(player, &PlayerController::queueChanged, this, [this] {
        syncQueueModel();
        emit playerChanged();
    });
    connect(player, &PlayerController::errorOccurred, this, [this](const QString &msg) {
        onPlayerError(msg);
    });
    connect(player, &PlayerController::requestPlay, this,
            [this](const Song &song) { resolveAndPlay(song); });
    connect(player, &PlayerController::recommendationNeeded, this,
            [this] { onRecommendationNeeded(); });

    AuthManager *auth = AuthManager::instance();
    connect(auth, &AuthManager::toast, this, [this](const QString &msg) { emit toast(msg); });
    connect(auth, &AuthManager::qrChanged, this, [this] { emit qrChanged(); });
    connect(auth, &AuthManager::captchaChanged, this, [this] { emit captchaChanged(); });
    connect(auth, &AuthManager::accountsChanged, this, [this] { syncAccountModel(); });
    connect(auth, &AuthManager::loginStateChanged, this,
            [this](const QString &id, bool ok, const QString &msg) {
                setStatus(msg);
                emit loginStateChanged(id, ok, msg);
            });
    connect(auth, &AuthManager::libraryUpdated, this, [this] {
        refreshPlaylists();
        syncAccountModel();
        emit playlistsLoaded();
    });
    connect(auth, &AuthManager::librarySynced, this,
            [this](const QString &id, bool ok, const QString &msg) {
                syncAccountModel();
                emit syncFinished(id, ok, msg);
            });

    connect(CustomSourceManager::instance(), &CustomSourceManager::changed,
            this, [this] { syncCustomModel(); });

    connect(ScriptSourceManager::instance(), &ScriptSourceManager::changed,
            this, [this] { emit scriptSourcesChanged(); });
    connect(ScriptSourceManager::instance(), &ScriptSourceManager::message,
            this, [this](const QString &id, bool ok, const QString &text) {
                emit scriptSourceMessage(id, ok, text);
                Logger::write(ok ? QStringLiteral("I") : QStringLiteral("W"),
                              QStringLiteral("在线音源[%1] %2").arg(id, text));
                // 成功走普通提示、失败走红条：一眼分得清，不靠读长句子
                emit notify(text, ok ? QStringLiteral("info") : QStringLiteral("error"));
            });

    syncQueueModel();
    syncAccountModel();
    syncCustomModel();
    refreshPlaylists();

    // 启动续播：信号已全部接好（requestPlay -> resolveAndPlay），此时恢复上次的
    // 播放列表与位置；若上次在播，会自动续播并跳到原位置。
    player->restoreState();

    // 从 build/cmake 之类没有 OpenSSL DLL 的目录直接启动时，Qt 会在所有 HTTPS
    // 请求上返回「TLS initialization failed」，表现就是「音源导入了却取不到链接」。
    // 这里主动自检并说清楚原因，避免用户误以为是 VIP 判定或音源失效。
    // 注意：只有 Windows 才依赖随包分发的 OpenSSL DLL，而且 Qt5 要 1.1、Qt6 要 3.x，
    // 文案必须按当前构建实际期望的库名给，否则会把用户引向错误方向。
    if (!QSslSocket::supportsSsl()) {
        const QString buildVersion = QSslSocket::sslLibraryBuildVersionString();
#if defined(Q_OS_WIN)
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QString sslMsg = QStringLiteral(
            "未检测到 OpenSSL 运行库（libssl-3-x64.dll / libcrypto-3-x64.dll）："
            "HTTPS 音源与在线脚本将无法取链。当前构建期望 %1，"
            "请把程序 bin/ 目录加入 PATH，或从 bin/ 启动").arg(buildVersion);
#else
        const QString sslMsg = QStringLiteral(
            "未检测到 OpenSSL 运行库（libssl-1_1-x64.dll / libcrypto-1_1-x64.dll）："
            "HTTPS 音源与在线脚本将无法取链。当前构建期望 %1，"
            "请把程序 bin/ 目录加入 PATH，或从 bin/ 启动").arg(buildVersion);
#endif
#elif defined(Q_OS_ANDROID)
        const QString sslMsg = QStringLiteral(
            "当前安装包缺少 OpenSSL 运行库：HTTPS 音源与在线脚本将无法取链。"
            "请更新到包含 TLS 组件的安装包");
#else
        const QString sslMsg = QStringLiteral(
            "当前环境缺少 TLS/SSL 运行库：HTTPS 音源与在线脚本将无法取链。"
            "当前构建期望 %1，请安装后重新启动").arg(buildVersion);
#endif
        setStatus(sslMsg);
        QTimer::singleShot(1500, this, [this, sslMsg] { emit toast(sslMsg); });
    }
}

AppController::~AppController() { delete d; }

void AppController::setStatus(const QString &text)
{
    d->status = text;
    emit statusChanged();
}

void AppController::syncQueueModel()
{
    const QVector<Song> songs = PlayerController::instance()->queue();
    m_queue->setSongs(songs);
    // 让搜索结果等列表知道哪些歌已在队列里，「＋」按钮据此切成「✓」
    QSet<QString> uids;
    uids.reserve(songs.size());
    for (const Song &s : songs)
        if (!s.uid.isEmpty()) uids.insert(s.uid);
    m_results->setQueueUids(uids);
}

void AppController::syncAccountModel()
{
    QVector<Account> list;
    QHash<QString, AccountListModel::Meta> meta;
    const QVector<Account> stored = Library::instance()->accounts();
    const QVector<Playlist> playlists = Library::instance()->playlists();
    for (const QString &id : SourceConfig::instance()->ids()) {
        Account a;
        a.source = id;
        a.status = QStringLiteral("未登录");
        for (const Account &s : stored)
            if (s.source == id) { a = s; break; }
        list << a;
        // 聚合视图需要的平台元数据：是否已登录、能否同步歌单、该平台已有多少用户歌单
        AccountListModel::Meta m;
        m.name = SourceConfig::instance()->nameOf(id);
        m.methods = AuthManager::instance()->methods(id);
        m.apiAvailable = UserApi::instance()->available(id);
        m.apiStatus = UserApi::instance()->statusOf(id);
        for (const Playlist &p : playlists) {
            if (p.source != id) continue;
            if (p.type == QLatin1String("user") || p.type == QLatin1String("favorite"))
                ++m.playlistCount;
        }
        meta.insert(id, m);
    }
    m_accounts->setAccounts(list, meta);
}

void AppController::syncCustomModel()
{
    d->customIds = CustomSourceManager::instance()->ids();
    d->customNames = CustomSourceManager::instance()->names();
    emit customSourcesChanged();
}

QString AppController::sourceName(const QString &id) const
{ return SourceConfig::instance()->nameOf(id); }

// 扫码时该提示用户打开哪个 App：QQ 音乐要用「QQ」扫，写平台全名会让人去开错应用
QString AppController::loginAppName(const QString &id) const
{
    if (id == QLatin1String("netease")) return QStringLiteral("网易云音乐");
    if (id == QLatin1String("qq"))      return QStringLiteral("手机 QQ");
    if (id == QLatin1String("kugou"))   return QStringLiteral("酷狗音乐");
    if (id == QLatin1String("migu"))    return QStringLiteral("咪咕音乐");
    // 酷我的内置扫码是「微信扫码」（走开放平台 snsapi_login + i.kuwo.cn 回调），
    // 提示文案必须说微信，不能写酷我 App —— 拿酷我 App 扫这张码是扫不动的
    if (id == QLatin1String("kuwo"))    return QStringLiteral("微信");
    // 汽水出的是汽水域名的扫码 SDK 链接（bff-pc.qishui.com/...uc_sdk=scan-auth），
    // 抖音 App 不认这个域名会直接提示失败，必须用汽水音乐 App 扫
    if (id == QLatin1String("qishui"))  return QStringLiteral("汽水音乐");
    const QString n = SourceConfig::instance()->nameOf(id);
    return n.isEmpty() ? QStringLiteral("官方 App") : n;
}

int AppController::accountTotal() const
{ return SourceConfig::instance()->ids().size(); }

int AppController::loggedAccountCount() const
{
    int n = 0;
    for (const Account &a : AuthManager::instance()->accounts())
        if (a.logged) ++n;
    return n;
}

bool AppController::accountLogged(const QString &sourceId) const
{ return AuthManager::instance()->account(sourceId).logged; }

QString AppController::currentTitle() const { return PlayerController::instance()->current().title; }
QString AppController::currentArtist() const { return PlayerController::instance()->current().artist; }
QString AppController::currentCover() const
{ return coverUrlOrEmpty(PlayerController::instance()->current().cover); }
QString AppController::currentSourceLabel() const { return d->currentSourceLabel; }
QString AppController::substituteBadge() const { return d->substituteBadge; }
bool AppController::playing() const { return PlayerController::instance()->playing(); }
qint64 AppController::position() const { return PlayerController::instance()->position(); }
qint64 AppController::duration() const { return PlayerController::instance()->duration(); }
int AppController::currentQueueIndex() const { return PlayerController::instance()->currentIndex(); }

// 队列空时 current() 可能返回无效 Song，uid 自然为空串，QML 绑定安全
QString AppController::currentSongUid() const
{
    return PlayerController::instance()->current().uid;
}

// 当前播放歌曲在歌单详情过滤视图里的行号：先在源模型里按 uid 找，再经代理映射。
// 被过滤掉时 mapFromSource 无效，返回 -1（调用方不滚动）。
int AppController::playingDetailRow() const
{
    const QString uid = currentSongUid();
    if (uid.isEmpty()) return -1;
    const QVector<Song> &songs = m_results->songs();
    int srcRow = -1;
    for (int i = 0; i < songs.size(); ++i) {
        if (songs.at(i).uid == uid) { srcRow = i; break; }
    }
    if (srcRow < 0) return -1;
    const QModelIndex proxyIdx = m_detailFilter->mapFromSource(m_results->index(srcRow, 0));
    return proxyIdx.isValid() ? proxyIdx.row() : -1;
}

bool AppController::currentFavorite() const { return isFavoriteCurrent(); }
int AppController::volume() const { return PlayerController::instance()->volume(); }
bool AppController::ducked() const { return PlayerController::instance()->ducked(); }
int AppController::playMode() const { return PlayerController::instance()->mode(); }
QString AppController::playModeName() const { return PlayerController::instance()->modeName(); }
QString AppController::lyricsText() const { return PlayerController::instance()->lyricsText(); }
QStringList AppController::lyricLines() const { return PlayerController::instance()->lyricLines(); }
QStringList AppController::lyricTransLines() const { return PlayerController::instance()->lyricTransLines(); }
QVariantList AppController::lyricTimes() const { return PlayerController::instance()->lyricTimes(); }
bool AppController::lyricsTimed() const { return PlayerController::instance()->lyricsTimed(); }
QString AppController::currentLyricLine() const { return PlayerController::instance()->currentLyricLine(); }
int AppController::currentLyricIndex() const { return PlayerController::instance()->currentLyricIndex(); }
QString AppController::statusMessage() const { return d->status; }
QString AppController::quality() const { return d->quality; }
bool AppController::vipSubstituteEnabled() const { return d->vipSubstitute; }
bool AppController::autoToggleSource() const
{ return Settings::boolean(QStringLiteral("autoToggleSource"), true); }
QString AppController::sourceFirstOrder() const
{ return Settings::string(QStringLiteral("sourceFirst"), QStringLiteral("vip")); }
bool AppController::qishuiRoam() const
{ return Settings::boolean(QStringLiteral("qishuiRoam"), false); }
bool AppController::keepScreenOn() const
{ return Settings::boolean(QStringLiteral("keepScreenOn"), true); }
QString AppController::searchType() const { return d->searchType; }
bool AppController::openApiEnabled() const { return OpenApiServer::instance()->running(); }
QString AppController::openApiAddress() const { return OpenApiServer::instance()->address(); }
bool AppController::logToFile() const { return Logger::instance()->fileEnabled(); }
QString AppController::logFilePath() const { return Logger::instance()->filePath(); }
QString AppController::recentLog(int lines)
{
    return Logger::instance()->recentText(lines > 0 ? lines : 300);
}
QStringList AppController::sourceIds() const { return SourceConfig::instance()->ids(); }
QStringList AppController::sourceNames() const { return SourceConfig::instance()->names(); }
QStringList AppController::customSourceIds() const { return d->customIds; }
QStringList AppController::customSourceNames() const { return d->customNames; }
QVariantList AppController::scriptSources() const { return ScriptSourceManager::instance()->items(); }
QString AppController::customLocalDirectory() const { return CustomSourceManager::instance()->localDirectory(); }
QString AppController::qrImagePath() const { return AuthManager::instance()->qrImagePath(); }
QString AppController::qrHint() const { return AuthManager::instance()->qrHint(); }
QString AppController::qrStatus() const { return AuthManager::instance()->qrStatus(); }
QString AppController::qrSource() const { return AuthManager::instance()->qrSource(); }
bool AppController::qrActive() const { return AuthManager::instance()->qrActive(); }
QString AppController::captchaImagePath() const { return AuthManager::instance()->captchaImagePath(); }
QString AppController::accountApiBase() const
{ return Settings::string(QStringLiteral("accountApiBase")); }
void AppController::setAccountApiBase(const QString &value)
{ setSetting(QStringLiteral("accountApiBase"), value.trimmed()); }
QString AppController::accountApiStatus(const QString &sourceId) const
{ return UserApi::instance()->statusOf(sourceId); }

QString AppController::accountApiBaseFor(const QString &sourceId) const
{ return Settings::string(QStringLiteral("accountApiBase/") + sourceId); }

void AppController::setAccountApiBaseFor(const QString &sourceId, const QString &value)
{
    setSetting(QStringLiteral("accountApiBase/") + sourceId, value.trimmed());
    refreshAccounts();
}

bool AppController::userApiAvailable(const QString &sourceId) const
{ return UserApi::instance()->available(sourceId); }

QString AppController::qrUnavailableReason(const QString &sourceId) const
{ return UserApi::instance()->qrUnavailableReason(sourceId); }

QString AppController::loginUnavailableReason(const QString &sourceId) const
{ return UserApi::instance()->loginUnavailableReason(sourceId); }

void AppController::setSearchType(const QString &type)
{ setSetting(QStringLiteral("searchType"), type); }

bool AppController::searchActive() const
{ return d->searchActive; }

void AppController::setSearchActive(bool value)
{
    if (d->searchActive == value) return;
    d->searchActive = value;
    emit searchActiveChanged();
}

void AppController::cancelPendingSearch()
{
    if (!d->searching) return;
    // 只是丢弃挂起请求、收起 loading；count=-1 让界面不弹「搜索完成」
    d->searching = false;
    d->searchKind.clear();
    emit searchFinished(-1, QString());
}

void AppController::search(const QString &keyword, const QString &sourceId)
{
    if (keyword.trimmed().isEmpty()) return;
    Library::instance()->addSearchHistory(keyword.trimmed());
    emit searchStarted();
    // 任何新搜索都接管搜索状态：上一条（无论歌曲还是歌单）的迟到结果全部作废
    const quint64 req = ++d->searchRequestId;
    d->searching = true;
    setStatus(QStringLiteral("正在聚合搜索「%1」…").arg(keyword));
    const int limit = Settings::integer(QStringLiteral("pageSize"), 30);

    // 歌单模式（对标洛雪搜索页的「歌曲 / 歌单」类型切换）
    if (d->searchType == QLatin1String("songlist")) {
        d->searchKind = QStringLiteral("playlist");
        const quint64 gen = ++d->playlistGen;
        SourceManager::instance()->searchPlaylists(keyword.trimmed(), sourceId, limit,
            [this, keyword, req, gen](const QVector<Playlist> &list) {
                if (req != d->searchRequestId || gen != d->playlistGen) return;
                // 只更新内存模型：搜索出来的歌单不属于本地音乐库，不能写进 playlists 表，
                // 否则会污染「音乐库 → 歌单」，同名 uid 还会覆盖真实用户歌单的类型。
                m_playlists->setPlaylists(list);
                d->searching = false;
                d->searchKind.clear();
                setStatus(QStringLiteral("歌单搜索「%1」完成，共 %2 个歌单").arg(keyword).arg(list.size()));
                emit playlistsLoaded();
                emit searchFinished(list.size(), QStringLiteral("playlist"));
            });
        return;
    }

    d->searchKind = QStringLiteral("song");
    SourceManager::instance()->search(keyword.trimmed(), sourceId, limit,
        [this, keyword, req](const QVector<Song> &songs) {
            if (req != d->searchRequestId) return;
            m_results->setSongs(songs);
            d->searching = false;
            d->searchKind.clear();
            setStatus(QStringLiteral("搜索「%1」完成，共 %2 条结果").arg(keyword).arg(songs.size()));
            emit searchFinished(songs.size(), QStringLiteral("song"));
        });
}

bool AppController::discoveryLoading() const { return d->discoveryLoading; }

int AppController::playlistCount() const { return m_playlists->rowCount(); }

void AppController::loadDiscovery(const QString &sourceId)
{
    d->discoveryLoading = true;
    emit discoveryChanged();
    setStatus(sourceId.isEmpty()
                  ? QStringLiteral("正在加载各平台公开榜单（无需登录）…")
                  : QStringLiteral("正在加载%1公开榜单（无需登录）…")
                        .arg(SourceConfig::instance()->nameOf(sourceId)));
    // 平台切换时作废过期请求：旧平台的后到结果不能覆盖新平台
    const quint64 gen = ++d->discoveryGen;
    // sourceId 为空 = 全部平台；指定平台只拉该平台默认榜单（未配置 rank 端点或接口
    // 失效的音源会回退到配置里的静态 defaultBoards，保证未登录也有数据）
    SourceManager::instance()->rank(sourceId, 30,
        [this, sourceId, gen](const QVector<Playlist> &list) {
            if (gen != d->discoveryGen) return;
            d->discoveryLoading = false;
            m_discovery->setPlaylists(list);
            setStatus(list.isEmpty()
                          ? QStringLiteral("公开榜单暂不可用，请检查网络后点「刷新榜单」重试")
                          : QStringLiteral("已加载 %1 个公开榜单/歌单（无需登录）").arg(list.size()));
            emit discoveryChanged();
        });
}

void AppController::openDiscoveryPlaylist(const QString &playlistUid)
{
    Playlist target;
    for (const Playlist &p : m_discovery->playlists())
        if (p.uid == playlistUid) { target = p; break; }
    if (target.uid.isEmpty()) {
        qWarning().noquote() << "[discovery] playlist not found:" << playlistUid;
        emit toast(QStringLiteral("未找到该榜单"));
        return;
    }

    ++d->searchRequestId;
    cancelPendingSearch();
    const quint64 gen = d->searchRequestId;
    setStatus(QStringLiteral("正在加载榜单「%1」…").arg(target.title));
    SourceManager::instance()->playlistDetail(target,
        [this, target, gen](const QVector<Song> &songs) {
            if (gen != d->searchRequestId) return;
            m_results->setSongs(songs);
            setStatus(songs.isEmpty()
                          ? QStringLiteral("榜单「%1」暂未读到歌曲（该平台公共接口可能不可用）")
                                .arg(target.title)
                          : QStringLiteral("榜单「%1」共 %2 首").arg(target.title).arg(songs.size()));
            emit searchFinished(songs.size(), QString());
        });
}

void AppController::loadRank(const QString &sourceId)
{
    ++d->searchRequestId;
    cancelPendingSearch();
    setStatus(QStringLiteral("正在加载榜单…"));
    const quint64 gen = ++d->playlistGen;
    SourceManager::instance()->rank(sourceId, 100, [this, gen](const QVector<Playlist> &list) {
        if (gen != d->playlistGen) return;
        // 不再写进本地歌单库：榜单是「每次拉取的公开数据」，落库后会污染
        // 「我的歌单」（历史版本会把酷狗/QQ 榜单当成用户歌单显示，且数量为 0）
        m_playlists->setPlaylists(list);
        setStatus(QStringLiteral("已加载 %1 个榜单/歌单").arg(list.size()));
        emit playlistsLoaded();
    });
}

void AppController::loadPlaylist(const QString &playlistUid)
{
    Playlist target;
    for (const Playlist &p : m_playlists->playlists())
        if (p.uid == playlistUid) { target = p; break; }
    if (target.uid.isEmpty()) {
        for (const Playlist &p : Library::instance()->playlists())
            if (p.uid == playlistUid) { target = p; break; }
    }
    if (target.uid.isEmpty()) { emit toast(QStringLiteral("未找到该歌单")); return; }

    // 本次歌单打开作废旧请求：用户可能连续点开多个歌单，慢的那个不能覆盖最后点的
    ++d->searchRequestId;
    cancelPendingSearch();
    const quint64 gen = d->searchRequestId;

    // 登录同步时已把用户歌单的歌曲写入本地库，缓存够多就直接展示。
    // 注意旧版本的同步结果只有预览曲目（网易云歌单接口只回前 10 首），
    // 这种「看着有缓存、其实缺了九成」的情况必须继续走网络补全，否则永远看不到完整歌单。
    const QVector<Song> cached = Library::instance()->playlistSongs(target.uid);
    const bool cacheComplete = !cached.isEmpty()
        && (target.count > 0 ? cached.size() >= target.count : cached.size() >= 100);
    if (cacheComplete) {
        m_results->setSongs(cached);
        setStatus(QStringLiteral("歌单「%1」共 %2 首（本地已同步）").arg(target.title).arg(cached.size()));
        emit searchFinished(cached.size(), QString());
        return;
    }

    // 用户歌单尚未同步：用账号 API 实时拉取，而不是去请求公共歌单的示例端点
    if (target.type == QLatin1String("user") || target.type == QLatin1String("favorite")) {
        QString cookie;
        for (const Account &a : Library::instance()->accounts()) {
            if (a.source == target.source && a.logged) { cookie = a.cookie; break; }
        }
        if (!cookie.isEmpty()) {
            setStatus(QStringLiteral("正在从账号读取歌单「%1」…").arg(target.title));
            UserApi::instance()->fetchPlaylistSongs(target.source, cookie, target.sourceId,
                [this, target, gen](const QVector<Song> &songs) {
                    if (gen != d->searchRequestId) return;
                    if (!songs.isEmpty()) {
                        Library::instance()->saveSongs(songs);
                        Library::instance()->setPlaylistSongs(target.uid, songs);
                    }
                    m_results->setSongs(songs);
                    setStatus(songs.isEmpty()
                        ? QStringLiteral("歌单「%1」未读到歌曲，请检查 Cookie 或先同步账号歌单").arg(target.title)
                        : QStringLiteral("歌单「%1」共 %2 首").arg(target.title).arg(songs.size()));
                    emit searchFinished(songs.size(), QString());
                });
            return;
        }
        emit toast(QStringLiteral("该歌单属于账号数据，请先登录并同步该平台"));
        return;
    }
    setStatus(QStringLiteral("正在加载歌单「%1」…").arg(target.title));
    SourceManager::instance()->playlistDetail(target, [this, target, gen](const QVector<Song> &songs) {
        if (gen != d->searchRequestId) return;
        m_results->setSongs(songs);
        setStatus(QStringLiteral("歌单「%1」共 %2 首").arg(target.title).arg(songs.size()));
        emit searchFinished(songs.size(), QString());
    });
}

void AppController::loadFavorites() { loadFavoritesBySource(QString()); }

void AppController::loadHistory() { loadHistoryBySource(QString()); }

void AppController::loadFavoritesBySource(const QString &sourceId)
{
    // 同步结果直接落地，同时作废所有挂起的异步请求（否则迟到的搜索结果会覆盖收藏列表）
    ++d->searchRequestId;
    cancelPendingSearch();
    const QString id = sourceId == QLatin1String("all") ? QString() : sourceId;
    QVector<Song> list;
    for (const Song &s : Library::instance()->favorites())
        if (id.isEmpty() || s.source == id) list << s;
    m_results->setSongs(list);
    setStatus(id.isEmpty()
        ? QStringLiteral("全部平台收藏共 %1 首").arg(list.size())
        : QStringLiteral("%1 的收藏共 %2 首")
              .arg(SourceConfig::instance()->nameOf(id)).arg(list.size()));
}

void AppController::loadHistoryBySource(const QString &sourceId)
{
    ++d->searchRequestId;
    cancelPendingSearch();
    const QString id = sourceId == QLatin1String("all") ? QString() : sourceId;
    QVector<Song> list;
    for (const Song &s : Library::instance()->history())
        if (id.isEmpty() || s.source == id) list << s;
    m_results->setSongs(list);
    setStatus(id.isEmpty()
        ? QStringLiteral("全部平台播放历史共 %1 首").arg(list.size())
        : QStringLiteral("%1 的播放历史共 %2 首")
              .arg(SourceConfig::instance()->nameOf(id)).arg(list.size()));
}

void AppController::filterPlaylists(const QString &sourceId)
{
    d->playlistFilter = sourceId == QLatin1String("all") ? QString() : sourceId;
    applyPlaylistFilter();
    emit playlistsLoaded();
}

QString AppController::sourceColor(const QString &sourceId) const
{ return SourceConfig::instance()->source(sourceId).color; }



QStringList AppController::searchHistory() const
{ return Library::instance()->searchHistory(); }

void AppController::applyPlaylistFilter()
{
    ++d->playlistGen;   // 同步重设歌单列表，作废挂起的榜单/歌单搜索回调
    // 只收起「歌单搜索」的 loading；正在进行的歌曲搜索不受影响
    if (d->searching && d->searchKind == QLatin1String("playlist")) {
        d->searching = false;
        d->searchKind.clear();
        emit searchFinished(-1, QString());
    }
    QVector<Playlist> list = Library::instance()->playlists();
    if (!d->playlistFilter.isEmpty()) {
        QVector<Playlist> filtered;
        for (const Playlist &p : list)
            if (p.source == d->playlistFilter) filtered << p;
        list = filtered;
    }
    m_playlists->setPlaylists(list);
    // 未登录 / 该平台无本地歌单时，回退展示其公开默认榜单（洛雪式：未登录也按平台给默认歌单）。
    // 点开榜单走 rankDetail/playlistDetail 公共端点，不依赖账号。
    if (list.isEmpty() && !d->playlistFilter.isEmpty()) {
        loadRank(d->playlistFilter);
    } else if (!d->playlistFilter.isEmpty()) {
        // 已登录该平台：本地歌单照常显示，再把默认榜单追加在后面（用户要求两者共存）
        appendDefaultBoards(d->playlistFilter, list);
    }
}

// 追加公开默认榜单到当前平台歌单视图。数据源与未登录回退（loadRank）一致：
// rank 端点被反爬/失效时由 MusicSource::rank 回退静态 defaultBoards，保证总有内容。
// 榜单保持「每次拉取的公开数据」不落库（理由同 loadRank 注释：落库会污染「我的歌单」）。
void AppController::appendDefaultBoards(const QString &sourceId,
                                        const QVector<Playlist> &local)
{
    // applyPlaylistFilter 进入时已 ++playlistGen；用户再切平台/刷新即作废本回调
    const quint64 gen = d->playlistGen;
    SourceManager::instance()->rank(sourceId, 100,
        [this, gen, local](const QVector<Playlist> &rankList) {
            if (gen != d->playlistGen || rankList.isEmpty()) return;
            QVector<Playlist> merged = local;
            for (const Playlist &rp : rankList) {
                bool dup = false;
                for (const Playlist &lp : merged)
                    if (lp.uid == rp.uid) { dup = true; break; }
                if (!dup) merged << rp;
            }
            if (merged.size() == local.size()) return;
            m_playlists->setPlaylists(merged);
            emit playlistsLoaded();
        });
}

void AppController::refreshPlaylists()
{
    applyPlaylistFilter();
    emit playlistsLoaded();
}

void AppController::refreshAccounts() { syncAccountModel(); }

void AppController::backgroundApp()
{
    Logger::write(QStringLiteral("I"), QStringLiteral("[返回] 首页返回键 → moveTaskToBack 退到后台（进程保留）"));
#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Activity.moveTaskToBack(true) 只把整个 task 切到后台：播放/媒体会话继续存活，
    // 与主流播放器「首页按返回=退到后台」一致。必须在 Android 主线程调用。
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([] {
        const QJniObject activity = QJniObject::callStaticObjectMethod(
            "org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
        if (activity.isValid())
            activity.callMethod<jboolean>("moveTaskToBack", "(Z)Z", JNI_TRUE);
    });
#elif defined(Q_OS_ANDROID)
    QtAndroid::runOnAndroidThreadSync([] {
        const QAndroidJniObject activity = QtAndroid::androidActivity();
        if (activity.isValid())
            activity.callMethod<jboolean>("moveTaskToBack", "(Z)Z", true);
    });
#else
    // 桌面无「置于后台」语义，返回键由窗口管理器处理
#endif
}

void AppController::resolveAndPlay(const Song &song)
{
    if (song.title.isEmpty()) return;
    // 死循环闸门：同一首歌已经连续三次「取到链但播不出来」，再换源只是把同一件事重做一遍。
    // 计数只在真的失败时 +1（见 autoAdvanceAfterFailure），播起来就清零；
    // 不能在进函数时 +1 —— 那样同一首歌手动重播四次就会被误判成坏歌。
    const QString guardUid = song.uid.isEmpty()
            ? song.title + QLatin1Char('|') + song.artist : song.uid;
    if (guardUid != d->guardSongUid) {
        d->guardSongUid = guardUid;
        d->guardSongTries = 0;
    }
    if (d->guardSongTries >= 3) {
        qWarning().noquote() << QStringLiteral("同一首歌连续 %1 次取链后仍无法播放，跳过：%2 - %3")
                                    .arg(d->guardSongTries).arg(song.title, song.artist);
        d->guardSongTries = 0;
        d->guardSongUid.clear();
        autoAdvanceAfterFailure(song.title + QLatin1String(" - ") + song.artist,
                                QStringLiteral("该歌曲的音源地址无法播放"), QStringLiteral("播放失败"));
        return;
    }
    // 开始一首新歌：先停掉上一首。取链是异步的（多源轮询 + 失败换源），这一小段
    // 如果不停，上一首会继续响，用户看到的就是「点了下一首、界面已经切了，声音
    // 还是老歌」；停住后新直链回来再 playUrl，宁可短暂静音也不留旧歌的尾音。
    // beginSwitch 额外标记「正在切换」：这段空窗期里收到的播放命令（用户点播放、
    // 通知栏/车机送来的 play）一律忽略，否则会拿上一首的旧地址从头再放一遍。
    PlayerController::instance()->beginSwitch();
    // 重置播放失败的换源状态，记录当前歌（供失败换源复用）。
    d->currentSong = song;
    d->triedFallback.clear();
    d->inFallback = false;
    setStatus(QStringLiteral("正在解析「%1」的播放地址…").arg(song.title));

    // 歌词/封面在「切歌」这一刻就开始取，不等播放地址解析结果。
    // 原来这段写在 p.ok 分支里：VIP 歌曲取链失败直接 return，歌词请求根本
    // 没发出，界面就一直停在「暂无歌词」，封面也只剩占位图。
    const quint64 lyricGeneration = ++d->lyricGen;
    SourceManager::instance()->lyric(song, [this, lyricGeneration](const QString &lrc,
                                                                  const QString &trans) {
        if (lyricGeneration != d->lyricGen) return;   // 已切歌，迟到结果丢弃
        PlayerController::instance()->setLyrics(lrc, trans);
    });

    const quint64 coverGeneration = ++d->coverGen;
    if (song.cover.trimmed().isEmpty() && !song.sourceId.isEmpty()) {
        const QString uid = song.uid.isEmpty()
            ? song.source + QLatin1Char(':') + song.sourceId : song.uid;
        SourceManager::instance()->cover(song, [this, coverGeneration, uid](const QString &url) {
            if (coverGeneration != d->coverGen || url.isEmpty()) return;
            PlayerController::instance()->setCurrentCover(url);
            m_results->setCover(uid, url);
            m_queue->setCover(uid, url);
            emit playerChanged();
        });
    }

    const quint64 resolveGeneration = ++d->resolveGen;
    SourceManager::instance()->resolve(song, d->quality, [this, song, resolveGeneration](const Playable &p) {
        if (resolveGeneration != d->resolveGen) return;   // 已切到别的歌，丢弃迟到的取链结果
        if (!p.ok) {
            // 取链阶段所有音源都失败（含 VIP 替代/跨源都试过）：同样自动跳下一首。
            autoAdvanceAfterFailure(song.title + QLatin1String(" - ") + song.artist,
                                    p.message, p.message);
            return;
        }
        d->lastResolvedUrl = p.url.toString();
        d->lastPlayableHeaders = p.headers;
        d->pendingEngine = p.engineId;
        d->pendingKey = p.platformKey;
        // 非 VIP 歌走「全部自定义」时也被标了 substituted，但写「VIP 替代」会误导：
        // 直接用它自己的来源名（脚本层已带「在线音源 · 」前缀）
        d->substituteBadge = p.substituted
                ? (song.vip ? QStringLiteral("VIP 替代 · %1").arg(p.substituteSource)
                            : p.substituteSource)
                : (song.vip ? QStringLiteral("VIP") : QString());
        d->currentSourceLabel = SourceConfig::instance()->nameOf(song.source);
        // 原平台取链成功：把成功源置顶，让后续换源优先尝试它（"成功源放到第一位"）。
        SourceManager::instance()->promoteSource(song.source);
        PlayerController::instance()->playUrl(p.url, p.headers);
        Library::instance()->addHistory(song);
        setStatus(p.substituted ? p.message : QStringLiteral("正在播放：%1 - %2").arg(song.title, song.artist));
        if (p.substituted) emit substituteInfo(p.message);
        emit playerChanged();
    });
}

void AppController::onPlayerError(const QString &msg)
{
    // 播放失败（已取到直链但播不出来）：尝试自动换源。
    // d->inFallback 为 true 表示正处于上一次换源的解析过程中或已成功换源正在播放，
    // 不再二次触发；只有用户主动切歌（resolveAndPlay）才会清掉它。
    Song song = d->currentSong;
    // 断流前的位置：在线直链（咪咕/网易云临时链）常在曲中断掉，换源时要把进度带过去，
    // 否则听感上就是「播到一半突然从头再来」。position() 在 error 到达时仍是断流点。
    const qint64 failedPos = PlayerController::instance()->position();
    // QMediaPlayer 的错误原文（"Error transferring http://... - server replied ..."）
    // 又长又没信息量：状态栏留给排查，提示只保留一句，免得糊满屏幕
    const QString brief = QStringLiteral("播放失败");
    // 这条直链是脚本音源给的却没播出来：重度扣分（-4），让它尽快跌出优先位置。
    // 不这么做的话「取到地址就 +2」会把返回坏地址的源越推越前，形成死循环。
    if (!d->pendingEngine.isEmpty()) {
        ScriptSourceManager::instance()->reportResult(d->pendingEngine, d->pendingKey, false, -4);
        // 记进「本首坏候选」：下次取链直接跳过它，把这一轮让给下一个音源
        ScriptSourceManager::instance()->notePlaybackFailure(
            d->pendingEngine, d->pendingKey,
            song.title + QLatin1Char('|') + song.artist);
        qWarning().noquote() << QStringLiteral("[换源] 直链播不出来，扣分：%1 [%2]")
                                    .arg(d->pendingEngine, d->pendingKey);
        d->pendingEngine.clear();
        d->pendingKey.clear();
    }
    Logger::write(QStringLiteral("W"),
                  QStringLiteral("播放失败：%1 - %2（%3）").arg(song.title, song.artist, msg));
    if (song.title.isEmpty() || d->inFallback) {
        setStatus(msg);
        // 换源解析/播放期间，被 stop 掉的旧流会持续投递迟到的 error（真机日志里
        // 同一秒刷了 20+ 条 "Demuxing failed"）。此时弹「播放失败」是误导——换源
        // 很可能马上成功。真正的失败由 resolveAlternate 的失败回调 autoAdvanceAfterFailure
        // 统一提示；这里只更新状态栏，不弹 toast。
        return;
    }
    d->inFallback = true;
    // 换源解析期间暂停看门狗：脚本逐源取链每个可达 15s，若继续按旧源的起播时刻
    // 计时，会在换源尚未完成时误报「加载超时」（实测 16s 假超时）。
    // 新源 playUrl() 会重新拉起看门狗并按新源重置基准。
    PlayerController::instance()->pauseWatchdog();
    // 动态权重：已取到直链却播不出来的原平台先降权，下次优先选更稳的源
    SourceManager::instance()->demoteSource(song.source);
    const quint64 gen = ++d->fallbackGen;
    // 跳过：原平台（它已失败）+ 本次换源流程里已经试过的候选源。
    const QStringList exclude = QStringList() << song.source << d->triedFallback.values();
    qWarning().noquote() << QStringLiteral("[换源] 播放出错，开始换源：%1 - %2（源=%3 inFallback=%4 已试=%5）")
                                .arg(song.title, song.artist, song.source)
                                .arg(d->inFallback ? 1 : 0)
                                .arg(d->triedFallback.values().join(QStringLiteral(",")));
    SourceManager::instance()->resolveAlternate(song, exclude, d->quality,
        [this, gen, msg, song, brief, failedPos](const Playable &r) {
            if (gen != d->fallbackGen) return;     // 期间用户已切到别的歌，丢弃迟到结果
            if (r.ok) {
                d->consecutiveFailures = 0;        // 换源后能播了，失败链结束
                // custom 不能在这里记「已试」：脚本层内部有十几个候选，这一条播不出来
                // 还要继续试下一个；真正去重由 ScriptSource 的坏候选名单负责。
                // 内置平台没有候选链，试过就不再试。
                if (r.source != QLatin1String("custom"))
                    d->triedFallback.insert(r.source);
                // 成功源置顶：custom 永远最前，内置平台按序排到第一。
                SourceManager::instance()->promoteSource(r.source);
                d->substituteBadge = r.substituted
                        ? (song.vip ? QStringLiteral("VIP 替代 · %1").arg(r.substituteSource)
                                    : r.substituteSource)
                        : (song.vip ? QStringLiteral("VIP") : QString());
                d->currentSourceLabel = SourceConfig::instance()->nameOf(song.source)
                        + (r.source == QLatin1String("custom")
                               ? QString() : QStringLiteral(" → %1").arg(r.substituteSource));
                // 换源（含脚本层多候选）成功后同样要记提供者：真的播出来才补记成功分
                d->pendingEngine = r.engineId;
                d->pendingKey = r.platformKey;
                // 放开 inFallback：若本次换源播放仍失败，errorOccurred 会再次进来试下一源；
                // 试过的源已记入 triedFallback，不会重复，也不会死循环。
                d->inFallback = false;
                // 从断流位置续播，而不是从头开始；新源正在加载，Loaded/Buffered 后
                // 由 PlayerController 的 m_pendingSeek 一次性 setPosition 到 failedPos
                PlayerController::instance()->playUrl(r.url, r.headers, failedPos);
                Library::instance()->addHistory(song);
                setStatus(r.message.isEmpty()
                              ? QStringLiteral("正在播放：%1 - %2").arg(song.title, song.artist)
                              : r.message);
                // 换源成功只写状态栏 / 日志，不再弹提示：像「已由自定义音源「X」替代播放」
                // 这类信息属于正常链路，弹出来只是噪音（播放条上的替代徽标已经说明了）
                Logger::write(QStringLiteral("I"),
                              QStringLiteral("换源成功：%1 - %2 → %3")
                                  .arg(song.title, song.artist, r.source));
                emit playerChanged();
                return;
            }
            d->inFallback = false;
            autoAdvanceAfterFailure(song.title + QLatin1String(" - ") + song.artist, msg, brief);
        });
}

void AppController::autoAdvanceAfterFailure(const QString &songLabel, const QString &msg,
                                            const QString &brief)
{
    // 所有音源都换过仍播不出来：自动播放下一首。
    // 用连续失败计数 + 队列长度兜底，防止「列表循环里全是失败歌」时
    // 一首首跳过又绕回起点、无限重播；中间只要有歌正常播起来（进度>2s）
    // 就会由 positionChanged 把计数清零。
    PlayerController *p = PlayerController::instance();
    const int queueSize = p->queue().size();
    // 这一首彻底放不出来（所有音源都试完了）：给「同曲闸门」计一笔，
    // 攒到 3 次说明不是偶发网络抖动，再点它就别再耗一整轮取链。
    ++d->guardSongTries;
    if (++d->consecutiveFailures < queueSize) {
        qWarning().noquote() << QStringLiteral("所有音源播放失败，自动跳到下一首：%1（连续失败 %2）")
                                .arg(songLabel).arg(d->consecutiveFailures);
        setStatus(QStringLiteral("切换音源后仍无法播放，正在播放下一首…"));
        // 与 EndOfMedia 一致：不要在播放/换源回调栈里同步 stop/setSource，
        // 否则新歌经常起不来，表现成「跳到下一首却没声音」。
        QTimer::singleShot(0, this, [this] { PlayerController::instance()->next(); });
        return;
    }
    // 已经把队列里的歌都跳了一遍还全失败：停下并提示，避免无限循环。
    d->consecutiveFailures = 0;
    // 明确收尾：清掉「正在切换」标记，否则播放按钮会一直处于被忽略状态
    p->stop();
    setStatus(msg);
    emit toast(brief);
}

void AppController::playResult(int index)
{
    const QVector<Song> &songs = m_results->songs();
    if (index < 0 || index >= songs.size()) return;
    // setQueue 已经发 queueChanged，队列模型会自动同步，无需再手动 syncQueueModel
    PlayerController::instance()->setQueue(songs, index);
    resolveAndPlay(songs.at(index));
}

void AppController::playQueue(int index)
{
    const QVector<Song> &songs = m_queue->songs();
    if (index < 0 || index >= songs.size()) return;
    PlayerController::instance()->setCurrentIndex(index);  // 触发 requestPlay -> resolveAndPlay
}

void AppController::addResultToQueue(int index)
{
    const QVector<Song> &songs = m_results->songs();
    if (index < 0 || index >= songs.size()) return;
    const Song &s = songs.at(index);
    PlayerController *p = PlayerController::instance();
    // 按 uid 去重：已在队列里就移除（按钮变回「＋」），否则追加。
    // 这样 SongRow 的「＋/✓」是真正的开关，连点也不会堆出多份同一首歌。
    const QVector<Song> &q = p->queue();
    for (int i = 0; i < q.size(); ++i) {
        if (!s.uid.isEmpty() && q.at(i).uid == s.uid) {
            p->removeQueue(i);
            emit toast(QStringLiteral("已从播放队列移除"));
            return;
        }
    }
    p->addQueue(QVector<Song>() << s);
    emit toast(QStringLiteral("已加入播放队列"));
}

void AppController::addPlaylistToQueue(const QString &playlistUid)
{
    const QVector<Song> songs = Library::instance()->playlistSongs(playlistUid);
    if (songs.isEmpty()) { emit toast(QStringLiteral("该歌单暂无本地缓存，请先打开一次")); return; }
    PlayerController::instance()->addQueue(songs);
    emit toast(QStringLiteral("已把歌单加入播放队列"));
}

void AppController::toggleFavoriteResult(int index)
{
    const QVector<Song> &songs = m_results->songs();
    if (index < 0 || index >= songs.size()) return;
    const Song &s = songs.at(index);
    if (Library::instance()->isFavorite(s.uid)) {
        Library::instance()->removeFavorite(s.uid);
        emit toast(QStringLiteral("已取消收藏"));
    } else {
        Library::instance()->addFavorite(s);
        emit toast(QStringLiteral("已收藏"));
    }
    m_results->refreshRow(index);
}

void AppController::toggleFavoriteQueue(int index)
{
    const QVector<Song> &songs = m_queue->songs();
    if (index < 0 || index >= songs.size()) return;
    const Song &s = songs.at(index);
    if (Library::instance()->isFavorite(s.uid)) Library::instance()->removeFavorite(s.uid);
    else Library::instance()->addFavorite(s);
    m_queue->refreshRow(index);
    emit toast(QStringLiteral("收藏状态已更新"));
}

bool AppController::isFavoriteCurrent() const
{
    const Song s = PlayerController::instance()->current();
    return !s.uid.isEmpty() && Library::instance()->isFavorite(s.uid);
}

void AppController::toggleFavoriteCurrent()
{
    const Song s = PlayerController::instance()->current();
    if (s.uid.isEmpty()) {
        emit toast(QStringLiteral("没有正在播放的歌曲"));
        return;
    }
    if (Library::instance()->isFavorite(s.uid)) {
        Library::instance()->removeFavorite(s.uid);
        emit toast(QStringLiteral("已取消收藏"));
    } else {
        Library::instance()->addFavorite(s);
        emit toast(QStringLiteral("已收藏"));
    }
    emit playerChanged();
}

void AppController::removeQueue(int index)
{ PlayerController::instance()->removeQueue(index); }

void AppController::clearQueue()
{ PlayerController::instance()->clearQueue(); }

void AppController::downloadResult(int index)
{
    const QVector<Song> &songs = m_results->songs();
    if (index < 0 || index >= songs.size()) return;
    const Song song = songs.at(index);
    setStatus(QStringLiteral("正在准备下载「%1」…").arg(song.title));
    SourceManager::instance()->resolve(song, d->quality, [this, song](const Playable &p) {
        if (!p.ok) { emit toast(p.message); return; }
        QString dir = Settings::string(QStringLiteral("downloadDir"));
        if (dir.isEmpty()) dir = Settings::filePath(QStringLiteral("downloads"));
        QDir().mkpath(dir);
        QString name = (song.artist.isEmpty() ? song.title : song.artist + QStringLiteral(" - ") + song.title);
        name.remove(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")));
        const QString path = dir + QLatin1Char('/') + name + QLatin1Char('.') + extensionForUrl(p.url);
        // 流式写盘：整首 FLAC 动辄几十 MB，原来走 Reply::body 会把这些字节全留在内存里
        HttpClient::instance()->download(p.url, p.headers, path,
            [this, path](bool ok, const QString &error) {
                if (!ok) {
                    emit toast(QStringLiteral("下载失败：") + error);
                    return;
                }
                setStatus(QStringLiteral("已下载到 %1").arg(path));
                emit downloadFinished(path);
                emit toast(QStringLiteral("下载完成"));
            }, 600000);
    });
}

// ---- MV 播放 ----
void AppController::openMv(const Song &song)
{
    // MV 的画面与声音独立于音频播放器，音乐继续放会两路声音重叠
    if (playing()) togglePause();
    setStatus(QStringLiteral("正在解析「%1」的 MV…").arg(song.title));
    SourceManager::instance()->mv(song, [this, song](bool ok, const QUrl &url, const QString &error) {
        if (!ok) {
            setStatus(error);
            emit notify(error, QStringLiteral("error"));
            return;
        }
        emit mvOpened(QVariantMap{
            {QStringLiteral("url"), url.toString()},
            {QStringLiteral("title"), song.title},
            {QStringLiteral("artist"), song.artist},
            {QStringLiteral("cover"), song.cover},
            {QStringLiteral("source"), song.source},
            {QStringLiteral("hasPrev"), mvHasPrev()},
            {QStringLiteral("hasNext"), mvHasNext()}});
        setStatus(QStringLiteral("正在播放 MV：%1 - %2").arg(song.title, song.artist));
    });
}

void AppController::playMv(int index)
{
    const QVector<Song> &songs = m_results->songs();
    if (index < 0 || index >= songs.size()) return;
    // 从歌曲行单点进入：没有榜单上下文，禁用上一个/下一个
    m_mvIndex = -1;
    emit mvContextChanged();
    openMv(songs.at(index));
}

void AppController::playMvCurrent()
{
    const Song cur = PlayerController::instance()->current();
    if (cur.title.isEmpty()) { emit notify(QStringLiteral("当前没有正在播放的歌曲"), QStringLiteral("warning")); return; }
    m_mvIndex = -1;
    emit mvContextChanged();
    openMv(cur);
}

void AppController::loadMvList(const QString &area, bool append)
{
    if (m_mvLoading || (append && !m_mvHasMore)) return;
    if (!append) {
        m_mvList.clear();
        m_mvOffset = 0;
        m_mvHasMore = true;
        m_mvIndex = -1;
    }
    m_mvArea = area.isEmpty() ? QStringLiteral("全部") : area;
    m_mvLoading = true;
    emit mvListChanged();
    emit mvContextChanged();
    const int pageSize = 24;
    SourceManager::instance()->mvList(QStringLiteral("netease"), m_mvArea, pageSize,
                                      m_mvOffset,
        [this](const QVector<QVariantMap> &items, bool hasMore) {
            for (const QVariantMap &m : items)
                m_mvList.append(m);
            m_mvOffset += items.size();
            m_mvHasMore = hasMore;
            m_mvLoading = false;
            emit mvListChanged();
            if (m_mvList.isEmpty())
                emit notify(QStringLiteral("MV 榜单暂时拉不到数据，检查网络后重试"),
                            QStringLiteral("warning"));
        });
}

void AppController::playMvItem(int index)
{
    if (index < 0 || index >= m_mvList.size()) return;
    m_mvIndex = index;
    const QVariantMap item = m_mvList.at(index).toMap();
    Song s;
    s.source = item.value(QStringLiteral("source")).toString();
    if (s.source.isEmpty()) s.source = QStringLiteral("netease");
    s.sourceId = item.value(QStringLiteral("id")).toString();
    s.title = item.value(QStringLiteral("title")).toString();
    s.artist = item.value(QStringLiteral("artist")).toString();
    s.cover = item.value(QStringLiteral("cover")).toString();
    // 榜单条目本身就是 MV：id 即 mvid，塞进 extra 可跳过 mvInfo 那一次额外请求
    s.extra.insert(QStringLiteral("mvid"), s.sourceId);
    emit mvContextChanged();
    openMv(s);
}

void AppController::playMvNext()
{
    if (mvHasNext()) playMvItem(m_mvIndex + 1);
}

void AppController::playMvPrev()
{
    if (mvHasPrev()) playMvItem(m_mvIndex - 1);
}

void AppController::togglePause() { PlayerController::instance()->togglePause(); }
void AppController::next() { PlayerController::instance()->next(); }
void AppController::previous() { PlayerController::instance()->previous(); }

// 推荐续播：PlayerController 只在「汽水漫游已开且当前是汽水果」或「顺序播放走到
// 队列末尾」时发出这个请求，所以这里不需要再判断播放模式。
// 取到推荐后插在当前歌后面再推进，保证当前这首不会被重复解析/打断。
void AppController::onRecommendationNeeded()
{
    PlayerController *player = PlayerController::instance();
    const Song seed = player->current();
    if (d->recommendPending) return;   // 上一个推荐请求还没回来，别叠加成多个请求

    // 种子必须是汽水果：其他平台没有免签名的相似歌曲接口，直接走原队列逻辑。
    if (seed.source != QLatin1String("qishui") || seed.sourceId.isEmpty()) {
        player->playNextInQueue();
        return;
    }

    d->recommendPending = true;
    setStatus(QStringLiteral("正在按「%1」获取汽水推荐…").arg(seed.title));
    SourceManager::instance()->related(seed, 12, [this, seed](const QVector<Song> &list) {
        d->recommendPending = false;
        PlayerController *p = PlayerController::instance();
        // 请求期间用户可能已经手动切歌：种子已经不是当前歌时丢弃这批推荐，
        // 不能把「上一首的相似歌」塞进用户正在听的歌后面。
        if (p->current().uid != seed.uid) return;

        // 去重：跳过种子本身和队列里已有的歌（uid = "source:sourceId"），最多插 10 首。
        QSet<QString> known;
        known.insert(seed.uid);
        for (const Song &s : p->queue()) known.insert(s.uid);
        QVector<Song> picked;
        for (const Song &s : list) {
            if (picked.size() >= 10) break;
            if (s.uid.isEmpty() || s.title.isEmpty() || known.contains(s.uid)) continue;
            known.insert(s.uid);
            picked << s;
        }

        if (picked.isEmpty()) {
            // 没拿到推荐（未登录 / 网络失败 / 全被去重掉）：退回原队列。
            // 顺序播放的末尾会在这里自然停住，不会卡死也不会重播当前歌。
            setStatus(QStringLiteral("汽水推荐不可用，继续按队列播放"));
            p->playNextInQueue();
            return;
        }

        Logger::write(QStringLiteral("I"),
                      QStringLiteral("汽水漫游：按「%1」取到 %2 首推荐")
                              .arg(seed.title).arg(picked.size()));
        p->insertAfterCurrent(picked);
        setStatus(QStringLiteral("汽水漫游：已加入 %1 首推荐").arg(picked.size()));
        // 等队列变更信号处理完再推进，界面先看到队列里多出来的推荐歌
        QTimer::singleShot(0, this, [p] { p->playNextInQueue(); });
    });
}
void AppController::seek(qint64 positionMs) { PlayerController::instance()->seek(positionMs); }
void AppController::setVolume(int value) { PlayerController::instance()->setVolume(value); }
void AppController::toggleMute() { PlayerController::instance()->toggleMute(); }
void AppController::setPlayMode(int mode) { PlayerController::instance()->setMode(mode); emit settingsChanged(); }
void AppController::cyclePlayMode() { PlayerController::instance()->cycleMode(); emit settingsChanged(); }

QStringList AppController::loginMethods(const QString &sourceId) const
{ return AuthManager::instance()->methods(sourceId); }
void AppController::startQrLogin(const QString &sourceId) { AuthManager::instance()->startQrLogin(sourceId); }
void AppController::cancelQrLogin() { AuthManager::instance()->cancelQrLogin(); }
void AppController::loginByPassword(const QString &sourceId, const QString &account,
                                    const QString &password, const QString &captcha)
{ AuthManager::instance()->loginByPassword(sourceId, account, password, captcha); }
void AppController::loginByCookie(const QString &sourceId, const QString &cookie)
{ AuthManager::instance()->loginByCookie(sourceId, cookie); }
void AppController::sendPhoneCode(const QString &sourceId, const QString &phone, const QString &captcha)
{ AuthManager::instance()->sendPhoneCode(sourceId, phone, captcha); }
void AppController::refreshCaptcha(const QString &sourceId)
{
    if (sourceId == QLatin1String("kuwo")) AuthManager::instance()->refreshKuwoCaptcha();
}
void AppController::loginByPhone(const QString &sourceId, const QString &phone, const QString &code)
{ AuthManager::instance()->loginByPhone(sourceId, phone, code); }
void AppController::logout(const QString &sourceId) { AuthManager::instance()->logout(sourceId); }
void AppController::syncUserLibrary(const QString &sourceId) { AuthManager::instance()->syncUserLibrary(sourceId); }
void AppController::syncAllUserLibraries() { AuthManager::instance()->syncAll(); }

QString AppController::theme() const
{
    return setting(QStringLiteral("theme"), QStringLiteral("light")).toString();
}

QVariant AppController::setting(const QString &key, const QVariant &fallback) const
{ return Settings::get(key, fallback); }

void AppController::setSetting(const QString &key, const QVariant &value)
{
    Settings::set(key, value);
    if (key == QLatin1String("quality")) d->quality = value.toString();
    else if (key == QLatin1String("vipSubstitute")) d->vipSubstitute = value.toBool();
    else if (key == QLatin1String("searchType")) d->searchType = value.toString();
    else if (key == QLatin1String("proxy")) HttpClient::instance()->setProxyFromSettings();
    else if (key == QLatin1String("logToFile")) Logger::instance()->setFileEnabled(value.toBool());
    else if (key == QLatin1String("openApi") || key == QLatin1String("openApiPort")) {
        OpenApiServer *api = OpenApiServer::instance();
        if (Settings::boolean(QStringLiteral("openApi"), false))
            api->start(Settings::integer(QStringLiteral("openApiPort"), 23330), false);
        else api->stop();
    }
    emit settingsChanged();
}

void AppController::addCustomHttpSource(const QString &name, const QString &baseUrl)
{
    CustomSourceManager::instance()->addHttp(name, baseUrl);
    syncCustomModel();
    emit toast(QStringLiteral("自定义音源已添加"));
}

void AppController::setCustomLocalDirectory(const QString &path)
{
    CustomSourceManager::instance()->setLocalDirectory(path);
    syncCustomModel();
    emit toast(QStringLiteral("本地音源目录已更新"));
}

void AppController::removeCustomSource(const QString &id)
{ CustomSourceManager::instance()->remove(id); syncCustomModel(); }

void AppController::refreshCustomSources()
{ CustomSourceManager::instance()->refresh(); syncCustomModel(); }

void AppController::openExternal(const QString &url)
{ QDesktopServices::openUrl(QUrl(url)); }

void AppController::quitApp()
{
    // Android 等系统没有返回键默认行为时，通过 QML 侧「退出」入口直接收尾整个应用
    QCoreApplication::exit(0);
}

void AppController::importScriptSource(const QString &url)
{
    ScriptSourceManager::instance()->importUrl(url, [this](bool ok, const QString &message) {
        emit scriptSourcesChanged();
        // message 已经是「音源「X」可用 / 不可用（原因）」的短句，别再套前缀
        emit notify(message, ok ? QStringLiteral("success") : QStringLiteral("error"));
    });
}

void AppController::updateScriptSource(const QString &id)
{
    ScriptSourceManager::instance()->update(id, [this](bool ok, const QString &message) {
        emit scriptSourcesChanged();
        emit notify(message, ok ? QStringLiteral("success") : QStringLiteral("error"));
    });
}

void AppController::removeScriptSource(const QString &id)
{
    ScriptSourceManager::instance()->remove(id);
    emit toast(QStringLiteral("已删除该在线音源"));
}

void AppController::setScriptSourceEnabled(const QString &id, bool enabled)
{
    ScriptSourceManager::instance()->setEnabled(id, enabled);
    emit toast(enabled ? QStringLiteral("已启用该在线音源") : QStringLiteral("已禁用该在线音源"));
}

void AppController::checkScriptSource(const QString &id)
{
    ScriptSourceManager::instance()->check(id, [this](bool ok, const QString &message) {
        emit scriptSourcesChanged();
        emit notify(message, ok ? QStringLiteral("success") : QStringLiteral("error"));
    });
}

void AppController::checkAllScriptSources()
{
    ScriptSourceManager::instance()->checkAll();
    emit toast(QStringLiteral("正在检查音源…"));
}

void AppController::rescanScriptSources()
{
    ScriptSourceManager::instance()->rescanRepository();
    emit toast(QStringLiteral("正在从仓库页面发现音源…"));
}

} // namespace Furui
