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

#include "AuthManager.h"
#include "CustomSource.h"
#include "HttpClient.h"
#include "Library.h"
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
    case CoverRole:     return s.cover;
    case SourceRole:    return s.source;
    case SourceNameRole:return SourceConfig::instance()->nameOf(s.source);
    case VipRole:       return s.vip;
    case BadgeRole:     return s.vip ? QStringLiteral("VIP") : QString();
    case SourceIdRole:  return s.sourceId;
    case FavoriteRole:  return Library::instance()->isFavorite(s.uid);
    default:            return QVariant();
    }
}

QHash<int, QByteArray> SongListModel::roleNames() const
{
    return {
        {UidRole, "uid"}, {TitleRole, "title"}, {ArtistRole, "artist"}, {AlbumRole, "album"},
        {DurationRole, "duration"}, {CoverRole, "cover"}, {SourceRole, "source"},
        {SourceNameRole, "sourceName"}, {VipRole, "vip"}, {BadgeRole, "badge"},
        {SourceIdRole, "sourceId"}, {FavoriteRole, "favorite"}
    };
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
    case CoverRole:      return p.cover;
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

    d->quality = Settings::string(QStringLiteral("quality"), QStringLiteral("standard"));
    d->searchType = Settings::string(QStringLiteral("searchType"), QStringLiteral("song"));
    d->vipSubstitute = Settings::boolean(QStringLiteral("vipSubstitute"), true);

    OpenApiServer *api = OpenApiServer::instance();
    connect(api, &OpenApiServer::toast, this, [this](const QString &msg) { emit toast(msg); });
    connect(api, &OpenApiServer::stateChanged, this, [this] { emit settingsChanged(); });
    if (Settings::boolean(QStringLiteral("openApi"), false))
        api->start(Settings::integer(QStringLiteral("openApiPort"), 23330),
                   Settings::boolean(QStringLiteral("openApiLocalOnly"), true));

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
    connect(player, &PlayerController::positionChanged, this, [this] { emit positionChanged(); });
    connect(player, &PlayerController::volumeChanged, this, [this] { emit volumeChanged(); });
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
{ m_queue->setSongs(PlayerController::instance()->queue()); }

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
    if (id == QLatin1String("qishui"))  return QStringLiteral("抖音"); // 汽水二维码的扫码目标是抖音 App（同账号体系）
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
QString AppController::currentCover() const { return PlayerController::instance()->current().cover; }
QString AppController::currentSourceLabel() const { return d->currentSourceLabel; }
QString AppController::substituteBadge() const { return d->substituteBadge; }
bool AppController::playing() const { return PlayerController::instance()->playing(); }
qint64 AppController::position() const { return PlayerController::instance()->position(); }
qint64 AppController::duration() const { return PlayerController::instance()->duration(); }
int AppController::currentQueueIndex() const { return PlayerController::instance()->currentIndex(); }
bool AppController::currentFavorite() const { return isFavoriteCurrent(); }
int AppController::volume() const { return PlayerController::instance()->volume(); }
int AppController::playMode() const { return PlayerController::instance()->mode(); }
QString AppController::playModeName() const { return PlayerController::instance()->modeName(); }
QString AppController::lyricsText() const { return PlayerController::instance()->lyricsText(); }
QStringList AppController::lyricLines() const { return PlayerController::instance()->lyricLines(); }
QVariantList AppController::lyricTimes() const { return PlayerController::instance()->lyricTimes(); }
bool AppController::lyricsTimed() const { return PlayerController::instance()->lyricsTimed(); }
QString AppController::currentLyricLine() const { return PlayerController::instance()->currentLyricLine(); }
int AppController::currentLyricIndex() const { return PlayerController::instance()->currentLyricIndex(); }
QString AppController::statusMessage() const { return d->status; }
QString AppController::quality() const { return d->quality; }
bool AppController::vipSubstituteEnabled() const { return d->vipSubstitute; }
bool AppController::autoToggleSource() const
{ return Settings::boolean(QStringLiteral("autoToggleSource"), true); }
QString AppController::searchType() const { return d->searchType; }
bool AppController::openApiEnabled() const { return OpenApiServer::instance()->running(); }
QString AppController::openApiAddress() const { return OpenApiServer::instance()->address(); }
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
    if (target.uid.isEmpty()) { emit toast(QStringLiteral("未找到该榜单")); return; }

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

    // 登录同步时已把用户歌单的歌曲写入本地库，优先直接展示
    const QVector<Song> cached = Library::instance()->playlistSongs(target.uid);
    if (!cached.isEmpty()) {
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
    if (list.isEmpty() && !d->playlistFilter.isEmpty())
        loadRank(d->playlistFilter);
}

void AppController::refreshPlaylists()
{
    applyPlaylistFilter();
    emit playlistsLoaded();
}

void AppController::refreshAccounts() { syncAccountModel(); }

void AppController::resolveAndPlay(const Song &song)
{
    if (song.title.isEmpty()) return;
    // 开始一首新歌：重置播放失败的换源状态，记录当前歌（供失败换源复用）。
    d->currentSong = song;
    d->triedFallback.clear();
    d->inFallback = false;
    setStatus(QStringLiteral("正在解析「%1」的播放地址…").arg(song.title));

    // 歌词/封面在「切歌」这一刻就开始取，不等播放地址解析结果。
    // 原来这段写在 p.ok 分支里：VIP 歌曲取链失败直接 return，歌词请求根本
    // 没发出，界面就一直停在「暂无歌词」，封面也只剩占位图。
    const quint64 lyricGeneration = ++d->lyricGen;
    SourceManager::instance()->lyric(song, [this, lyricGeneration](const QString &lrc) {
        if (lyricGeneration != d->lyricGen) return;   // 已切歌，迟到结果丢弃
        PlayerController::instance()->setLyrics(lrc);
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
            setStatus(p.message);
            emit toast(p.message);
            return;
        }
        d->lastResolvedUrl = p.url.toString();
        d->lastPlayableHeaders = p.headers;
        d->substituteBadge = p.substituted
                ? QStringLiteral("VIP 替代 · %1").arg(p.substituteSource)
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
    // QMediaPlayer 的错误原文（"Error transferring http://... - server replied ..."）
    // 又长又没信息量：状态栏留给排查，提示只保留一句，免得糊满屏幕
    const QString brief = QStringLiteral("播放失败");
    if (song.title.isEmpty() || d->inFallback) {
        setStatus(msg);
        emit toast(brief);
        return;
    }
    d->inFallback = true;
    const quint64 gen = ++d->fallbackGen;
    // 跳过：原平台（它已失败）+ 本次换源流程里已经试过的候选源。
    const QStringList exclude = QStringList() << song.source << d->triedFallback.values();
    SourceManager::instance()->resolveAlternate(song, exclude, d->quality,
        [this, gen, msg, song, brief](const Playable &r) {
            if (gen != d->fallbackGen) return;     // 期间用户已切到别的歌，丢弃迟到结果
            if (r.ok) {
                d->triedFallback.insert(r.source);
                // 成功源置顶：custom 永远最前，内置平台按序排到第一。
                SourceManager::instance()->promoteSource(r.source);
                d->substituteBadge = r.substituted ? r.substituteSource
                                                  : (song.vip ? QStringLiteral("VIP") : QString());
                d->currentSourceLabel = SourceConfig::instance()->nameOf(song.source)
                        + (r.source == QLatin1String("custom")
                               ? QString() : QStringLiteral(" → %1").arg(r.substituteSource));
                // 放开 inFallback：若本次换源播放仍失败，errorOccurred 会再次进来试下一源；
                // 试过的源已记入 triedFallback，不会重复，也不会死循环。
                d->inFallback = false;
                PlayerController::instance()->playUrl(r.url, r.headers);
                Library::instance()->addHistory(song);
                setStatus(r.message.isEmpty()
                              ? QStringLiteral("正在播放：%1 - %2").arg(song.title, song.artist)
                              : r.message);
                emit notify(r.message.isEmpty() ? QStringLiteral("已自动换源继续播放")
                                              : r.message, QStringLiteral("info"));
                emit playerChanged();
                return;
            }
            // 所有候选源都失败：终止换源，原样提示播放失败。
            d->inFallback = false;
            setStatus(msg);
            emit toast(brief);
        });
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
    PlayerController::instance()->addQueue(QVector<Song>() << songs.at(index));
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
    else if (key == QLatin1String("openApi") || key == QLatin1String("openApiPort") ||
             key == QLatin1String("openApiLocalOnly")) {
        OpenApiServer *api = OpenApiServer::instance();
        if (Settings::boolean(QStringLiteral("openApi"), false))
            api->start(Settings::integer(QStringLiteral("openApiPort"), 23330),
                       Settings::boolean(QStringLiteral("openApiLocalOnly"), true));
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

} // namespace Furui
