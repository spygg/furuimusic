#include "AppController.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
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
    // 异步搜索/榜单/歌单请求的内容代次：慢请求返回时若用户已发起新操作就丢弃，
    // 避免旧结果覆盖新结果（与 lyricGen 同一模式）
    quint64 resultGen = 0;
    quint64 playlistGen = 0;
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
    connect(player, &PlayerController::lyricsChanged, this, [this] { emit lyricsChanged(); });
    connect(player, &PlayerController::queueChanged, this, [this] {
        syncQueueModel();
        emit playerChanged();
    });
    connect(player, &PlayerController::errorOccurred, this, [this](const QString &msg) {
        setStatus(msg);
        emit toast(msg);
    });
    connect(player, &PlayerController::requestPlay, this,
            [this](const Song &song) { resolveAndPlay(song); });

    AuthManager *auth = AuthManager::instance();
    connect(auth, &AuthManager::toast, this, [this](const QString &msg) { emit toast(msg); });
    connect(auth, &AuthManager::qrChanged, this, [this] { emit qrChanged(); });
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
                emit toast(text);
            });

    syncQueueModel();
    syncAccountModel();
    syncCustomModel();
    refreshPlaylists();
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

QString AppController::cookieLoginHint(const QString &sourceId) const
{ return UserApi::instance()->cookieLoginHint(sourceId); }

void AppController::setSearchType(const QString &type)
{ setSetting(QStringLiteral("searchType"), type); }

void AppController::search(const QString &keyword, const QString &sourceId)
{
    if (keyword.trimmed().isEmpty()) return;
    Library::instance()->addSearchHistory(keyword.trimmed());
    emit searchStarted();
    setStatus(QStringLiteral("正在聚合搜索「%1」…").arg(keyword));
    const int limit = Settings::integer(QStringLiteral("pageSize"), 30);

    // 歌单模式（对标洛雪搜索页的「歌曲 / 歌单」类型切换）
    if (d->searchType == QLatin1String("songlist")) {
        const quint64 gen = ++d->playlistGen;
        SourceManager::instance()->searchPlaylists(keyword.trimmed(), sourceId, limit,
            [this, keyword, gen](const QVector<Playlist> &list) {
                if (gen != d->playlistGen) return;   // 已有更新的歌单操作，丢弃这次迟到结果
                Library::instance()->savePlaylists(list);
                m_playlists->setPlaylists(list);
                setStatus(QStringLiteral("歌单搜索「%1」完成，共 %2 个歌单").arg(keyword).arg(list.size()));
                emit playlistsLoaded();
                emit searchFinished(list.size());
            });
        return;
    }

    const quint64 gen = ++d->resultGen;
    SourceManager::instance()->search(keyword.trimmed(), sourceId, limit,
        [this, keyword, gen](const QVector<Song> &songs) {
            if (gen != d->resultGen) return;         // 已有更新的结果操作，丢弃这次迟到结果
            m_results->setSongs(songs);
            setStatus(QStringLiteral("搜索「%1」完成，共 %2 条结果").arg(keyword).arg(songs.size()));
            emit searchFinished(songs.size());
        });
}

void AppController::loadRank(const QString &sourceId)
{
    setStatus(QStringLiteral("正在加载榜单…"));
    const quint64 gen = ++d->playlistGen;
    SourceManager::instance()->rank(sourceId, 100, [this, gen](const QVector<Playlist> &list) {
        if (gen != d->playlistGen) return;
        Library::instance()->savePlaylists(list);
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
    const quint64 gen = ++d->resultGen;

    // 登录同步时已把用户歌单的歌曲写入本地库，优先直接展示
    const QVector<Song> cached = Library::instance()->playlistSongs(target.uid);
    if (!cached.isEmpty()) {
        m_results->setSongs(cached);
        setStatus(QStringLiteral("歌单「%1」共 %2 首（本地已同步）").arg(target.title).arg(cached.size()));
        emit searchFinished(cached.size());
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
                    if (gen != d->resultGen) return;
                    if (!songs.isEmpty()) {
                        Library::instance()->saveSongs(songs);
                        Library::instance()->setPlaylistSongs(target.uid, songs);
                    }
                    m_results->setSongs(songs);
                    setStatus(songs.isEmpty()
                        ? QStringLiteral("歌单「%1」未读到歌曲，请检查 Cookie 或先同步账号歌单").arg(target.title)
                        : QStringLiteral("歌单「%1」共 %2 首").arg(target.title).arg(songs.size()));
                    emit searchFinished(songs.size());
                });
            return;
        }
        emit toast(QStringLiteral("该歌单属于账号数据，请先登录并同步该平台"));
        return;
    }
    setStatus(QStringLiteral("正在加载歌单「%1」…").arg(target.title));
    SourceManager::instance()->playlistDetail(target, [this, target, gen](const QVector<Song> &songs) {
        if (gen != d->resultGen) return;
        m_results->setSongs(songs);
        setStatus(QStringLiteral("歌单「%1」共 %2 首").arg(target.title).arg(songs.size()));
        emit searchFinished(songs.size());
    });
}

void AppController::loadFavorites() { loadFavoritesBySource(QString()); }

void AppController::loadHistory() { loadHistoryBySource(QString()); }

void AppController::loadFavoritesBySource(const QString &sourceId)
{
    ++d->resultGen;   // 同步结果直接落地，同时作废旧异步请求
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
    ++d->resultGen;
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
    QVector<Playlist> list = Library::instance()->playlists();
    if (!d->playlistFilter.isEmpty()) {
        QVector<Playlist> filtered;
        for (const Playlist &p : list)
            if (p.source == d->playlistFilter) filtered << p;
        list = filtered;
    }
    m_playlists->setPlaylists(list);
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
    setStatus(QStringLiteral("正在解析「%1」的播放地址…").arg(song.title));
    SourceManager::instance()->resolve(song, d->quality, [this, song](const Playable &p) {
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
        PlayerController::instance()->playUrl(p.url, p.headers);
        Library::instance()->addHistory(song);
        setStatus(p.substituted ? p.message : QStringLiteral("正在播放：%1 - %2").arg(song.title, song.artist));
        if (p.substituted) emit substituteInfo(p.message);
        const quint64 gen = ++d->lyricGen;      // 本次播放的歌词代次
        SourceManager::instance()->lyric(song, [this, gen](const QString &lrc) {
            if (gen != d->lyricGen) return;     // 已经切歌，迟到的歌词丢掉，别盖到新一首上
            PlayerController::instance()->setLyrics(lrc);
        });
        emit playerChanged();
    });
}

void AppController::playResult(int index)
{
    const QVector<Song> &songs = m_results->songs();
    if (index < 0 || index >= songs.size()) return;
    PlayerController::instance()->setQueue(songs, index);
    syncQueueModel();
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
    syncQueueModel();
    emit toast(QStringLiteral("已加入播放队列"));
}

void AppController::addPlaylistToQueue(const QString &playlistUid)
{
    const QVector<Song> songs = Library::instance()->playlistSongs(playlistUid);
    if (songs.isEmpty()) { emit toast(QStringLiteral("该歌单暂无本地缓存，请先打开一次")); return; }
    PlayerController::instance()->addQueue(songs);
    syncQueueModel();
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
{ PlayerController::instance()->removeQueue(index); syncQueueModel(); }

void AppController::clearQueue()
{ PlayerController::instance()->clearQueue(); syncQueueModel(); }

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
        HttpClient::instance()->request(QStringLiteral("GET"), p.url, p.headers, QByteArray(),
            [this, path](const HttpClient::Reply &r) {
                if (!r.ok || r.body.isEmpty()) {
                    emit toast(QStringLiteral("下载失败：") + r.error);
                    return;
                }
                QFile f(path);
                if (f.open(QIODevice::WriteOnly)) {
                    f.write(r.body);
                    f.close();
                    setStatus(QStringLiteral("已下载到 %1").arg(path));
                    emit downloadFinished(path);
                    emit toast(QStringLiteral("下载完成"));
                } else {
                    emit toast(QStringLiteral("无法写入下载文件"));
                }
            }, 120000);
    });
}

void AppController::togglePause() { PlayerController::instance()->togglePause(); }
void AppController::next() { PlayerController::instance()->next(); }
void AppController::previous() { PlayerController::instance()->previous(); }
void AppController::seek(qint64 positionMs) { PlayerController::instance()->seek(positionMs); }
void AppController::setVolume(int value) { PlayerController::instance()->setVolume(value); emit settingsChanged(); }
void AppController::setPlayMode(int mode) { PlayerController::instance()->setMode(mode); emit settingsChanged(); }
void AppController::cyclePlayMode() { PlayerController::instance()->cycleMode(); emit settingsChanged(); }

QStringList AppController::loginMethods(const QString &sourceId) const
{ return AuthManager::instance()->methods(sourceId); }
void AppController::startQrLogin(const QString &sourceId) { AuthManager::instance()->startQrLogin(sourceId); }
void AppController::cancelQrLogin() { AuthManager::instance()->cancelQrLogin(); }
void AppController::loginByCookie(const QString &sourceId, const QString &cookie)
{ AuthManager::instance()->loginByCookie(sourceId, cookie); }
void AppController::loginByPassword(const QString &sourceId, const QString &account, const QString &password)
{ AuthManager::instance()->loginByPassword(sourceId, account, password); }
void AppController::sendPhoneCode(const QString &sourceId, const QString &phone)
{ AuthManager::instance()->sendPhoneCode(sourceId, phone); }
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

void AppController::importScriptSource(const QString &url)
{
    ScriptSourceManager::instance()->importUrl(url, [this](bool ok, const QString &message) {
        emit scriptSourcesChanged();
        emit toast(ok ? QStringLiteral("在线音源：") + message
                      : QStringLiteral("在线音源导入失败：") + message);
    });
}

void AppController::updateScriptSource(const QString &id)
{
    ScriptSourceManager::instance()->update(id, [this](bool ok, const QString &message) {
        emit scriptSourcesChanged();
        emit toast((ok ? QStringLiteral("音源已更新：") : QStringLiteral("更新失败：")) + message);
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
        emit toast((ok ? QStringLiteral("检查通过：") : QStringLiteral("检查失败：")) + message);
    });
}

void AppController::checkAllScriptSources()
{
    ScriptSourceManager::instance()->checkAll();
    emit toast(QStringLiteral("正在逐个检查已启用的在线音源…"));
}

} // namespace Furui
