#include "AppController.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPair>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUuid>
#include <QSslSocket>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <memory>

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
#include "Pinyin.h"
#include "PlayerController.h"
#include "ScriptSource.h"
#include "UserApi.h"
#include "Settings.h"
#include "SourceConfig.h"
#include "SourceManager.h"

namespace Furui {

// 找到某音源已登录账号的 Cookie（写回收藏需要）
static QString cookieForSource(const QString &sourceId, QString *userId = nullptr)
{
    for (const Account &a : Library::instance()->accounts()) {
        if (a.source == sourceId && a.logged && !a.cookie.isEmpty()) {
            if (userId) *userId = a.uid;
            return a.cookie;
        }
    }
    return QString();
}

// 把收藏态写回云端（目前仅网易云内置实现）。失败只提示、不回滚本地收藏，
// 避免一次网络失败让用户以为本地操作也没生效。
static void syncFavoriteToCloud(const Song &s, bool favorite)
{
    Logger::write(QStringLiteral("I"),
                  QStringLiteral("[收藏同步] 入口：source=%1 sourceId=%2 favorite=%3")
                      .arg(s.source, s.sourceId, favorite ? QStringLiteral("true") : QStringLiteral("false")));
    if (!UserApi::canSetFavorite(s.source)) {
        Logger::write(QStringLiteral("I"), QStringLiteral("[收藏同步] 该音源不支持写回，跳过"));
        return;
    }
    QString userId;
    const QString cookie = cookieForSource(s.source, &userId);
    if (cookie.isEmpty() || s.sourceId.isEmpty()) {
        Logger::write(QStringLiteral("W"),
                      QStringLiteral("[收藏同步] 缺少 cookie(%1) 或 sourceId(%2)，跳过")
                          .arg(cookie.isEmpty() ? QStringLiteral("空") : QStringLiteral("有"),
                               s.sourceId.isEmpty() ? QStringLiteral("空") : s.sourceId));
        return;
    }
    Logger::write(QStringLiteral("I"),
                  QStringLiteral("[收藏同步] 发起 weapi 请求 sourceId=%1 userid=%2")
                      .arg(s.sourceId, userId));
    UserApi::instance()->setFavorite(s.source, cookie, userId, s.sourceId, favorite,
        [favorite](bool ok, const QString &err) {
            Logger::write(ok ? QStringLiteral("I") : QStringLiteral("W"),
                          QStringLiteral("[收藏同步] 结果=%1 err=%2")
                              .arg(ok ? QStringLiteral("成功") : QStringLiteral("失败"), err));
            if (!ok)
                AppController::instance()->notify(
                    QStringLiteral("本地已%1，但云端同步失败：%2")
                        .arg(favorite ? QStringLiteral("收藏") : QStringLiteral("取消收藏"), err),
                    QStringLiteral("warning"));
        });
}

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

// 建歌单时一次性生成检索键：原文 + 全拼 + 首字母，过滤时只做 contains。
// 汉字转拼音走静态二分表（6763 常用字），千首歌单也只有几毫秒。
static QVector<QString> buildSearchKeys(const QVector<Song> &songs)
{
    QVector<QString> keys;
    keys.reserve(songs.size());
    for (const Song &s : songs) {
        const QString raw = Pinyin::normalizedQuery(s.title) + QLatin1Char('|')
                          + Pinyin::normalizedQuery(s.artist);
        const QString full = Pinyin::text(s.title) + QLatin1Char('|')
                           + Pinyin::text(s.artist);
        const QString ini = Pinyin::initials(s.title) + QLatin1Char('|')
                          + Pinyin::initials(s.artist);
        keys << (raw + QLatin1Char('|') + full + QLatin1Char('|') + ini);
    }
    return keys;
}

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
    case SearchKeyRole: return m_searchKeys.value(index.row());
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
        {RowRole, "row"}, {SearchKeyRole, "searchKey"}
    };
}

void SongListModel::setQueueUids(const QSet<QString> &uids)
{
    if (m_queueUids == uids) return;
    const QSet<QString> previous = m_queueUids;
    m_queueUids = uids;
    if (m_songs.isEmpty()) return;
    // 只刷新 inQueue 真正变化的行：搜索结果动辄几百行，整表 dataChanged 会让
    // QML 在每次「加入队列」时重算所有委托的 InQueueRole。
    for (int i = 0; i < m_songs.size(); ++i) {
        const QString &uid = m_songs.at(i).uid;
        const bool before = !uid.isEmpty() && previous.contains(uid);
        const bool now = !uid.isEmpty() && uids.contains(uid);
        if (before == now) continue;
        const QModelIndex idx = index(i, 0);
        emit dataChanged(idx, idx, {InQueueRole});
    }
}

void SongListModel::setSongs(const QVector<Song> &songs)
{
    beginResetModel();
    m_songs = songs;
    m_searchKeys = buildSearchKeys(songs);
    endResetModel();
}

void SongListModel::appendSongs(const QVector<Song> &songs)
{
    if (songs.isEmpty()) return;
    beginInsertRows(QModelIndex(), m_songs.size(), m_songs.size() + songs.size() - 1);
    m_songs.append(songs);
    m_searchKeys.append(buildSearchKeys(songs));
    endInsertRows();
}

void SongListModel::syncSongs(const QVector<Song> &songs)
{
    if (songs.isEmpty()) { setSongs(songs); return; }

    // 1) 结构完全一致（常见于 setQueue 传入同一列表）：只把变化的行标脏
    if (songs.size() == m_songs.size()) {
        bool sameOrder = true;
        for (int i = 0; i < songs.size(); ++i) {
            if (m_songs.at(i).uid != songs.at(i).uid) { sameOrder = false; break; }
        }
        if (sameOrder) {
            for (int i = 0; i < songs.size(); ++i) {
                const Song &a = m_songs.at(i);
                const Song &b = songs.at(i);
                if (a.title == b.title && a.artist == b.artist && a.album == b.album
                    && a.cover == b.cover && a.duration == b.duration && a.vip == b.vip
                    && a.source == b.source && a.sourceId == b.sourceId)
                    continue;
                m_songs[i] = b;
                m_searchKeys[i] = buildSearchKeys(QVector<Song>() << b).value(0);
                const QModelIndex idx = index(i, 0);
                emit dataChanged(idx, idx);
            }
            return;
        }
    }

    // 2) 只在尾部追加（「加入队列」最常见路径）：保留已有行与滚动位置
    if (songs.size() > m_songs.size()) {
        bool prefix = true;
        for (int i = 0; i < m_songs.size(); ++i) {
            if (m_songs.at(i).uid != songs.at(i).uid) { prefix = false; break; }
        }
        if (prefix) {
            beginInsertRows(QModelIndex(), m_songs.size(), songs.size() - 1);
            m_songs = songs;
            m_searchKeys = buildSearchKeys(songs);
            endInsertRows();
            return;
        }
    }

    // 3) 删除/中间插入/重排：退回整表重置
    setSongs(songs);
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
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    // Qt 6.10+ 推荐成对 API；invalidateFilter() 在 6.13 起废弃。
    // 只对行做自定义过滤，故用 Direction::Rows，避免连列一起重算。
    beginFilterChange();
    m_filter = f;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    m_filter = f;
    // 空过滤也要整表刷新：旧 API 下 invalidate 折叠成一次 reset
    invalidateFilter();
#endif
    emit filterChanged();
}

bool SongFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (m_filter.trimmed().isEmpty()) return true;
    const QString f = m_filter.trimmed();
    const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
    const QString title = idx.data(SongListModel::TitleRole).toString();
    const QString artist = idx.data(SongListModel::ArtistRole).toString();
    if (title.contains(f, Qt::CaseInsensitive) || artist.contains(f, Qt::CaseInsensitive))
        return true;
    // 预生成索引：原文 + 全拼 + 首字母，支持 zjl / zhoujielun / 周杰伦
    const QString key = idx.data(SongListModel::SearchKeyRole).toString();
    return key.contains(Pinyin::normalizedQuery(f));
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
// 预取缓存键：同一首歌在不同音质下直链不同，必须带上音质
static QString preloadKey(const Song &song, const QString &quality)
{
    const QString id = song.uid.isEmpty()
            ? song.source + QLatin1Char('|') + song.sourceId + QLatin1Char('|') + song.title
            : song.uid;
    return id + QLatin1Char('|') + quality;
}

struct AppController::Impl {
    QString status = QStringLiteral("就绪");
    QString substituteBadge;
    QString currentSourceLabel;
    QString quality;
    // 默认歌单优先：与构造函数里 Settings 默认值保持一致
    QString searchType = QStringLiteral("songlist");
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
    // 下一首预取：直链与歌词缓存。key = song.uid|quality；设置项变化或列表重排时清空。
    QHash<QString, Playable> preloaded;
    QHash<QString, qint64> preloadedAt;
    QHash<QString, QPair<QString, QString>> preloadedLyrics;
    QSet<QString> preloading;
    quint64 preloadGen = 0;
    bool preloadTriggered = false;
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
    // 当前正在播放的直链提供者。pending* 在「确认能播」后即被清空，曲中断流时
    // 已经拿不到了（旧实现只罚「秒失败」，播了一半才断的源反而逍遥法外）；
    // playing* 保留到下一次 playUrl 覆盖，断流重罚靠它定位元凶。
    QString playingEngine;
    QString playingKey;
    QString playingProviderSource;   // 内置平台 id；脚本提供时为空，custom 脚本层为 "custom"
    // 曲中断流恢复阶段：原链接重连 → 同音源重新取链 → 换源。
    // 前两级不降权，避免一次网络抖动就把好源打下去。
    enum class RecoverStage { None, SameUrl, ReResolve, Alternate };
    RecoverStage recoverStage = RecoverStage::None;
    qint64 recoverPos = 0;
    // 推荐请求在途：连点下一首时只允许有一个 /luna/track/related 请求
    bool recommendPending = false;
    // 结果区上下文：playlist（歌单/榜单/收藏/历史）→ playResult 入队时按歌单播；
    // 空或 single（搜索/汽水推荐）→ 漫游模式下播完走推荐续播。
    QString resultsContext;
    // 曲库页搜索框是否有内容：外壳据此隐藏播放栏，给搜索结果/软键盘让位
    bool searchActive = false;
    // 汽水推荐面板当前选择（QML 侧 ComboBox 与这里双向同步）
    QString qishuiRecommendMode = QStringLiteral("default");
    QString qishuiRecommendCategory;
    quint64 qishuiGen = 0;
    // 冷启动是否直接进播放页：有历史播放或续播状态时为真（由构造函数计算一次）
    bool showPlayerOnStart = false;
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
    m_searchPlaylists = new PlaylistListModel(this);
    m_discovery = new PlaylistListModel(this);
    m_accounts  = new AccountListModel(this);
    // 歌单详情的过滤代理：源永远是 m_results（歌单/榜单/收藏/搜索共用同一结果模型）
    m_detailFilter = new SongFilterModel(this);
    m_detailFilter->setSourceModel(m_results);

    d->quality = Settings::string(QStringLiteral("quality"), QStringLiteral("standard"));
    // 全局搜索默认「歌单」优先：搜索结果先列匹配歌单，点进去即可浏览该歌单全部曲目，
    // 替代原来每个歌单内的独立搜索框（歌单内搜索框已移除）。
    d->searchType = Settings::string(QStringLiteral("searchType"), QStringLiteral("songlist"));

    OpenApiServer *api = OpenApiServer::instance();
    connect(api, &OpenApiServer::toast, this, [this](const QString &msg) { emit toast(msg); });
    connect(api, &OpenApiServer::stateChanged, this, [this] { emit settingsChanged(); });
    Logger::instance()->setFileEnabled(Settings::boolean(QStringLiteral("logToFile"), false));
    if (Settings::boolean(QStringLiteral("openApi"), false))
        // 默认仅本机；显式设置 openApiLocalOnly=false 且有令牌时才允许局域网访问
        api->start(Settings::integer(QStringLiteral("openApiPort"), 23330),
                   Settings::boolean(QStringLiteral("openApiLocalOnly"), true));

    Library::instance();
    SourceConfig::instance();
    CustomSourceManager::instance();
    SourceManager::instance();
    AuthManager::instance();
    // 空闲预热在线音源脚本引擎：把首次 QuickJS eval 的耗时挪到启动后 1.5s，
    // 用户第一次切歌时引擎已就绪（失败也不打扰用户）
    QTimer::singleShot(1500, this, [] {
        // FURUI_NO_SCRIPT_WARMUP=1 时跳过预热（用于对比预热占用的内存）
        if (qEnvironmentVariableIsEmpty("FURUI_NO_SCRIPT_WARMUP"))
            ScriptSourceManager::instance()->warmUp();
    });
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
        // 断流恢复成功：进度已越过断点 2 秒以上，说明原链接重连/重新取链生效，
        // 撤销恢复阶梯；下次再断流又从「原链接重连」开始，而不是直接换源。
        if (d->recoverStage != Impl::RecoverStage::None
            && player->position() > d->recoverPos + 2000) {
            d->recoverStage = Impl::RecoverStage::None;
            d->recoverPos = 0;
        }
        // 进度走过 2 秒 = 这条直链真的能播：给提供它的「脚本 × 平台」补记成功分
        if (!d->pendingEngine.isEmpty() && player->position() > 2000) {
            ScriptSourceManager::instance()->reportResult(d->pendingEngine, d->pendingKey, true, 2);
            ScriptSourceManager::instance()->promoteSource(d->pendingEngine);
            qInfo().noquote() << QStringLiteral("[换源] 确认可播放，加分并置顶：%1 [%2]")
                                     .arg(d->pendingEngine, d->pendingKey);
            d->pendingEngine.clear();
            d->pendingKey.clear();
        }
        // 真正播起来后才预取下一首：与当前播放错开，避免抢占取链/带宽。
        if (!d->preloadTriggered && player->position() > 1500) {
            d->preloadTriggered = true;
            QTimer::singleShot(0, this, [this] { preloadNext(); });
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

    // 老用户冷启动直接落播放页：有播放历史、或上次留了续播队列/正在播放。
    // 在 restoreState() 之后算，续播队列此时已就位。
    d->showPlayerOnStart = !Library::instance()->history(1).isEmpty()
        || !Settings::string(QStringLiteral("resumeQueue")).isEmpty()
        || Settings::boolean(QStringLiteral("resumePlaying"), false);

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
    m_queue->syncSongs(songs);
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
// 「VIP 替代音源」「自动跨源换源」开关已下线，行为固定为开启（与旧版勾选态一致）
bool AppController::vipSubstituteEnabled() const { return true; }
bool AppController::autoToggleSource() const { return true; }
bool AppController::sourceRaceEnabled() const
{ return Settings::boolean(QStringLiteral("sourceRace"), true); }
QString AppController::sourceFirstOrder() const
{ return Settings::sourceFirstOrder(); }
bool AppController::qishuiRoam() const
{ return Settings::boolean(QStringLiteral("qishuiRoam"), true); }   // 设置页已隐藏该开关，默认保持开启
bool AppController::roamMode() const
{ return Settings::boolean(QStringLiteral("roamMode"), false); }   // 默认关闭：开启后非歌单队列播完自动按汽水推荐续播
// 「播放时屏幕常亮」开关已下线：前台播放时自动保持，恒返回 true 供旧 UI 兼容
bool AppController::keepScreenOn() const { return true; }

void AppController::setMvScreenOn(bool on)
{
    if (m_mvScreenOn == on)
        return;
    m_mvScreenOn = on;
    Logger::write(QStringLiteral("I"),
                  QStringLiteral("[MV] 屏幕常亮 %1（与音乐播放态取或）").arg(on ? "开" : "关"));
    emit mvScreenOnChanged();
}

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
bool AppController::onlineSourcesUnlocked() const
{ return Settings::get(QStringLiteral("onlineSourcesUnlocked")).toBool(); }
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

bool AppController::showPlayerOnStart() const
{ return d->showPlayerOnStart; }

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
    ++m_searchHistoryEpoch;
    emit searchHistoryChanged();
    emit searchStarted();
    // 任何新搜索都接管搜索状态：上一条（无论歌曲还是歌单）的迟到结果全部作废
    const quint64 req = ++d->searchRequestId;
    ++d->qishuiGen;   // 作废旧汽水推荐回包：它只校验 qishuiGen，不认 searchRequestId
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
                // 只更新搜索专用模型：搜索出来的歌单不属于本地音乐库，不能写进 playlists 表，
                // 否则会污染「音乐库 → 歌单」（同名 uid 覆盖真实用户歌单），且搜索后
                // 用户自己的歌单列表会整段消失。退出搜索视图后本地歌单仍是原样。
                m_searchPlaylists->setPlaylists(list);
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
            d->resultsContext.clear();   // 搜索结果：单曲上下文，漫游模式播完走推荐
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
    // 平台切换时作废过期请求：旧平台的后到结果不能覆盖新平台。
    // 榜单页与汽水推荐页共用 m_discovery/discoveryLoading，两路必须互相当废——
    // 同时递增 qishuiGen，避免从汽水推荐切到榜单后，汽水的旧榜单回调覆盖新榜单。
    const quint64 gen = ++d->discoveryGen;
    ++d->qishuiGen;
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

QStringList AppController::qishuiModes() const
{
    // 对应汽水官方「默认模式 / 熟悉模式 / 新鲜模式」
    return QStringList() << QStringLiteral("default")
                         << QStringLiteral("familiar")
                         << QStringLiteral("fresh");
}

QStringList AppController::qishuiCategories() const
{
    // 官方车机版的场景子领域；通过现有 web 搜索接口按场景词取歌/歌单
    return QStringList() << QStringLiteral("儿歌") << QStringLiteral("乡村")
                         << QStringLiteral("雨天") << QStringLiteral("KTV必点")
                         << QStringLiteral("快乐时光") << QStringLiteral("动感健身")
                         << QStringLiteral("电子") << QStringLiteral("爵士")
                         << QStringLiteral("古风") << QStringLiteral("摇滚")
                         << QStringLiteral("轻音乐") << QStringLiteral("睡前")
                         << QStringLiteral("学习专注") << QStringLiteral("运动")
                         << QStringLiteral("咖啡馆") << QStringLiteral("车载");
}

void AppController::loadQishuiRecommend(const QString &mode, const QString &category)
{
    if (!mode.isEmpty()) d->qishuiRecommendMode = mode;
    d->qishuiRecommendCategory = category;
    const quint64 gen = ++d->qishuiGen;
    ++d->discoveryGen;   // 与榜单页互相当废：两路共用 m_discovery/discoveryLoading
    ++d->searchRequestId;   // 作废挂起的搜索/榜单请求
    cancelPendingSearch();  // 收起挂起搜索的 loading（否则 search 回调早退后 searching 永久卡 true）
    d->discoveryLoading = true;
    emit discoveryChanged();
    setStatus(QStringLiteral("正在加载汽水推荐…"));

    const auto done = [this, gen](const QVector<Song> &songs, const QString &label) {
        if (gen != d->qishuiGen) return;
        d->discoveryLoading = false;
        d->resultsContext.clear();   // 汽水推荐页：单曲上下文，本身就是推荐流
        m_results->setSongs(songs);
        setStatus(songs.isEmpty()
                      ? QStringLiteral("%1暂无可用内容，请检查网络后重试").arg(label)
                      : QStringLiteral("%1：%2 首").arg(label).arg(songs.size()));
        emit discoveryChanged();
        emit playerChanged();
        // kind 用 "qishui" 而非 "song"：车机语音点歌的单次连接只认 kind=="song"，
        // 这里若发 "song"，点歌 20s 窗口内加载汽水推荐会被误当点歌结果播放（互踩）
        emit searchFinished(songs.size(), QStringLiteral("qishui"));
    };

    // 任何模式下「推荐歌单」都不能空：rank 因网络/登录态/限流返回空时，
    // 用配置里的静态默认榜单兜底（qishuiRank 内部已有兜底，这里是双保险）。
    const auto boards = [this, gen](const QVector<Playlist> &list) {
        if (gen != d->qishuiGen) return QVector<Playlist>();
        const QVector<Playlist> out = list.isEmpty()
            ? SourceManager::instance()->defaultBoards(QStringLiteral("qishui")) : list;
        if (!out.isEmpty()) {
            m_discovery->setPlaylists(out);
            emit discoveryChanged();
        }
        return out;
    };

    // 场景子领域：用场景词直接搜歌（web 端点可用，官方私有 feed API 需设备签名）
    if (!category.trimmed().isEmpty()) {
        const QString kw = category.trimmed();
        // 实测 /luna/pc/search/track 匿名返回 HTTP 200 空响应（不是报错），
        // 必须带登录态 Cookie——未登录时退回推荐歌单的歌，并给出明确提示
        const Account qsAcct = AuthManager::instance()->account(QStringLiteral("qishui"));
        if (!qsAcct.logged || qsAcct.cookie.isEmpty()) {
            // 只发一次 rank：回调里同时刷新榜单 UI 与取第一份歌单详情，
            // 避免原先「刷榜单一次 + 取详情一次」的重复请求。
            SourceManager::instance()->rank(QStringLiteral("qishui"), 30,
                [this, gen, boards, done, kw](const QVector<Playlist> &list) {
                    if (gen != d->qishuiGen) return;
                    setStatus(QStringLiteral("「%1」需要汽水登录才有多场景歌曲；已显示推荐内容")
                                  .arg(kw));
                    const QVector<Playlist> final = boards(list);
                    if (final.isEmpty()) { done(QVector<Song>(), kw); return; }
                    SourceManager::instance()->playlistDetail(final.first(),
                        [done, kw](const QVector<Song> &songs) { done(songs, kw); });
                });
            return;
        }
        // 已登录：榜单照常加载（一次），场景词走搜索
        SourceManager::instance()->rank(QStringLiteral("qishui"), 30,
            [boards](const QVector<Playlist> &list) { boards(list); });
        SourceManager::instance()->search(kw, QStringLiteral("qishui"), 60,
            [done, kw](const QVector<Song> &songs) { done(songs, kw); });
        return;
    }

    const QString m = d->qishuiRecommendMode;
    if (m == QLatin1String("familiar") || m == QLatin1String("fresh")) {
        // 熟悉/新鲜：以收藏/历史/队列为「口味种子」做汽水站内相似推荐。
        // 关键：related() 按种子自身的 source 路由，而只有汽水驱动实现 related，
        // 所以必须先把本地种子（多来自网易/QQ/酷狗）映射成汽水曲库的歌，
        // 否则 related 全部返空、两种模式退化为默认榜单。
        const QString label = m == QLatin1String("familiar")
                            ? QStringLiteral("熟悉模式") : QStringLiteral("新鲜模式");
        QVector<Song> localSeeds;
        QSet<QString> seedUids;
        const QVector<Song> hist = Library::instance()->history(8);
        const QVector<Song> fav = Library::instance()->favorites();
        const QVector<Song> queue = PlayerController::instance()->queue();
        // max 表示「本次调用最多新增几首」，不是累计上限——否则第一路填满后
        // 后续历史/队列种子会被整段跳过（收藏 ≥3 首时熟悉模式只用收藏做种子）。
        auto addSeeds = [&localSeeds, &seedUids](const QVector<Song> &src, int max) {
            int added = 0;
            for (int i = 0; i < src.size() && added < max; ++i) {
                if (seedUids.contains(src.at(i).uid)) continue;
                seedUids.insert(src.at(i).uid);
                localSeeds.append(src.at(i));
                ++added;
            }
        };
        if (m == QLatin1String("familiar")) {
            addSeeds(fav, 3);
            addSeeds(hist, 3);
        } else {
            for (int i = hist.size() - 1; i >= 0 && localSeeds.size() < 2; --i)
                addSeeds(QVector<Song>() << hist.at(i), 2);
        }
        addSeeds(queue, 3);

        // 榜单与歌曲分开加载：任何模式都要有「推荐歌单」可看
        SourceManager::instance()->rank(QStringLiteral("qishui"), 30,
            [boards](const QVector<Playlist> &list) { boards(list); });

        // 无种子 / 相似推荐全空时的兜底：取推荐歌单第一份的歌
        auto fallbackToRank = [this, gen, boards, done, label] {
            SourceManager::instance()->rank(QStringLiteral("qishui"), 30,
                [this, gen, boards, done, label](const QVector<Playlist> &list) {
                    if (gen != d->qishuiGen) return;
                    const QVector<Playlist> final = boards(list);
                    if (final.isEmpty()) { done(QVector<Song>(), label); return; }
                    SourceManager::instance()->playlistDetail(final.first(),
                        [done, label](const QVector<Song> &songs) { done(songs, label); });
                });
        };

        if (localSeeds.isEmpty()) { fallbackToRank(); return; }

        // 阶段一：把本地种子映射到汽水曲库（汽水 search 已按相关度排序，取首个即可）。
        // 取到的汽水歌才带 qishui 的 sourceId，能正确路由到 qishuiRelated。
        auto qishuiSeeds = std::make_shared<QVector<Song>>();
        auto mapLeft = std::make_shared<int>(localSeeds.size());
        auto mapFinished = std::make_shared<bool>(false);
        auto doRelated = [this, gen, done, label, qishuiSeeds, mapFinished, fallbackToRank] {
            if (*mapFinished) return;
            *mapFinished = true;
            if (gen != d->qishuiGen) return;
            if (qishuiSeeds->isEmpty()) { fallbackToRank(); return; }
            // 阶段二：用汽水种子取站内相似歌曲，去重后汇总
            auto out = std::make_shared<QVector<Song>>();
            auto left = std::make_shared<int>(qishuiSeeds->size());
            auto finished = std::make_shared<bool>(false);
            for (const Song &seed : *qishuiSeeds) {
                SourceManager::instance()->related(seed,
                    label == QStringLiteral("新鲜模式") ? 40 : 25,
                    [this, gen, done, out, left, label, finished, fallbackToRank](const QVector<Song> &list) {
                        if (gen != d->qishuiGen || *finished) return;
                        QSet<QString> known;
                        for (const Song &s : *out) known.insert(s.uid);
                        for (const Song &s : list) {
                            if (known.contains(s.uid)) continue;
                            known.insert(s.uid);
                            out->append(s);
                        }
                        if (--(*left) == 0) {
                            *finished = true;
                            if (out->isEmpty()) { fallbackToRank(); return; }
                            done(*out, label);
                        }
                    });
            }
        };
        for (const Song &seed : localSeeds) {
            const QString kw = (seed.title + QLatin1Char(' ') + seed.artist).trimmed();
            SourceManager::instance()->search(kw, QStringLiteral("qishui"), 5,
                [this, gen, qishuiSeeds, mapLeft, mapFinished, doRelated](const QVector<Song> &songs) {
                    if (*mapFinished || gen != d->qishuiGen) return;
                    // search 已按相关度排序；取首个匹配作为汽水种子
                    if (!songs.isEmpty()) qishuiSeeds->append(songs.first());
                    if (--(*mapLeft) == 0) doRelated();
                });
        }
        return;
    }

    // 默认模式：官方推荐歌单 → 第一份歌单详情
    SourceManager::instance()->rank(QStringLiteral("qishui"), 30,
        [this, gen, boards, done](const QVector<Playlist> &list) {
            if (gen != d->qishuiGen) return;
            const QVector<Playlist> final = boards(list);
            if (final.isEmpty()) { done(QVector<Song>(), QStringLiteral("默认模式")); return; }
            SourceManager::instance()->playlistDetail(final.first(),
                [done](const QVector<Song> &songs) { done(songs, QStringLiteral("默认模式")); });
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
    ++d->qishuiGen;   // 作废在途汽水推荐回包（单向：汽水侧已作废搜索，这里补对称侧）
    cancelPendingSearch();
    const quint64 gen = d->searchRequestId;
    setStatus(QStringLiteral("正在加载榜单「%1」…").arg(target.title));
    SourceManager::instance()->playlistDetail(target,
        [this, target, gen](const QVector<Song> &songs) {
            if (gen != d->searchRequestId) return;
            d->resultsContext = QStringLiteral("playlist");
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
    ++d->qishuiGen;   // 作废在途汽水推荐回包
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
    // 搜索结果里的歌单（如搜歌手名出来的精选歌单）不在「我的歌单/本地库」，
    // 也必须能点开：从搜索歌单模型里补一次查找，否则永远弹「未找到该歌单」。
    if (target.uid.isEmpty() && m_searchPlaylists) {
        for (const Playlist &p : m_searchPlaylists->playlists())
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
        d->resultsContext = QStringLiteral("playlist");
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
                    d->resultsContext = QStringLiteral("playlist");
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
        d->resultsContext = QStringLiteral("playlist");
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
    d->resultsContext = QStringLiteral("playlist");
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
    d->resultsContext = QStringLiteral("playlist");
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

void AppController::clearSearchHistory()
{
    Library::instance()->clearSearchHistory();
    ++m_searchHistoryEpoch;
    emit searchHistoryChanged();
    emit toast(QStringLiteral("已清空搜索历史"));
}

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
            // 用户自建歌单（source=local）不属于任何平台，任何筛选下都保留
            if (p.source == d->playlistFilter || p.source == QLatin1String("local"))
                filtered << p;
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
    ++m_playlistsEpoch;
    applyPlaylistFilter();
    emit playlistsLoaded();
}

bool AppController::isPlaylistCollected(const QString &playlistUid) const
{
    for (const Playlist &p : Library::instance()->playlists())
        if (p.uid == playlistUid) return true;
    return false;
}

QString AppController::resultTitle(int index) const
{
    const QVector<Song> &songs = m_results->songs();
    if (index < 0 || index >= songs.size()) return QString();
    return songs.at(index).title;
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

void AppController::setMvFullscreen(bool on)
{
#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    Logger::write(QStringLiteral("I"),
                  QStringLiteral("[MV] 全屏 %1 → 横屏锁定 + 沉浸式系统栏").arg(on ? "进入" : "退出"));
    // 不请求固定横屏：本机（MTK + TrebleDroid GSI / Android 14）的物理屏不随
    // orientation 请求旋转，setRequestedOrientation(LANDSCAPE/SENSOR_LANDSCAPE)
    // 都会触发 Android 的 letterbox：Activity 配置变 land，但窗口被压到 720x322
    // （letterboxReason=FIXED_ORIENTATION），视频缩成顶部一小条、控制条被挤出可视区。
    // 保持 UNSPECIFIED 让窗口吃满 720x1612，视频 PreserveAspectFit 按宽度铺满居中，
    // 沉浸式隐藏系统栏照常执行。桌面仍由 QML showFullScreen 处理。
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([on] {
        const QJniObject activity = QJniObject::callStaticObjectMethod(
            "org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
        if (!activity.isValid())
            return;
        activity.callMethod<void>("setRequestedOrientation", "(I)V", jint(-1));

        // 沉浸式：隐藏状态栏 + 导航栏，边缘滑动只临时浮现（BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE）
        const QJniObject window = activity.callObjectMethod(
            "getWindow", "()Landroid/view/Window;");
        if (!window.isValid())
            return;
        // WindowInsets.Type.systemBars()：statusBars | navigationBars 的合集
        const jint barTypes = QJniObject::callStaticMethod<jint>(
            "android/view/WindowInsets$Type", "systemBars", "()I");
        const QJniObject insets = window.callObjectMethod(
            "getInsetsController", "()Landroid/view/WindowInsetsController;");
        if (insets.isValid()) {
            // API 30+ 统一走 WindowInsetsController（minSdk 为 Qt 6 默认 26，老设备降级 legacy 标志）
            if (on) {
                insets.callMethod<void>("setSystemBarsBehavior", "(I)V", jint(2));
                insets.callMethod<void>("hide", "(I)V", barTypes);
            } else {
                insets.callMethod<void>("show", "(I)V", barTypes);
            }
        } else {
            // API < 30 legacy SystemUiVisibility 沉浸式
            const QJniObject decor = window.callObjectMethod(
                "getDecorView", "()Landroid/view/View;");
            if (!decor.isValid())
                return;
            // FULLSCREEN|HIDE_NAVIGATION|STABLE|LAYOUT_FULLSCREEN|LAYOUT_HIDE_NAVIGATION|IMMERSIVE_STICKY
            const jint flags = on ? jint(0x4 | 0x2 | 0x100 | 0x400 | 0x200 | 0x1000)
                                  : jint(0);
            decor.callMethod<void>("setSystemUiVisibility", "(I)V", flags);
        }
    });
#else
    Q_UNUSED(on)   // 桌面全屏由 QML 侧 showFullScreen()/showNormal() 处理
#endif
}

void AppController::resolveAndPlay(const Song &song)
{
    if (song.title.isEmpty()) return;
    // 新歌取链开始：清掉上一首的直链提供者记录，断流重罚只对真正在播的歌生效
    d->playingEngine.clear();
    d->playingKey.clear();
    d->playingProviderSource.clear();
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
    // 任何新歌都递增取链代次：缓存命中时也要作废上一首仍在途的 resolve，
    // 否则旧结果会在新歌已经开始播放后把播放地址换回去。
    const quint64 resolveGeneration = ++d->resolveGen;
    d->inFallback = false;
    d->recoverStage = Impl::RecoverStage::None;   // 新歌重置断流恢复阶梯
    d->recoverPos = 0;
    d->preloadTriggered = false;   // 新歌从 0 开始计时，播起来后再预取下一首
    // 切歌作废在途评论请求；抽屉可保持打开，新歌必须能重新拉第一页
    ++m_commentsGen;
    m_commentsLoading = false;
    m_comments.clear();
    m_commentPage = 1;
    m_commentTotal = -1;
    emit commentsChanged();
    setStatus(QStringLiteral("正在解析「%1」的播放地址…").arg(song.title));

    // 歌词/封面在「切歌」这一刻就开始取，不等播放地址解析结果。
    // 原来这段写在 p.ok 分支里：VIP 歌曲取链失败直接 return，歌词请求根本
    // 没发出，界面就一直停在「暂无歌词」，封面也只剩占位图。
    const QString key = preloadKey(song, d->quality);
    const quint64 lyricGeneration = ++d->lyricGen;
    const auto cachedLyrics = d->preloadedLyrics.constFind(key);
    if (cachedLyrics != d->preloadedLyrics.constEnd()) {
        // 上一首播放时已预取过歌词：直接上屏，不再发一轮 lyric 请求
        PlayerController::instance()->setLyrics(cachedLyrics->first, cachedLyrics->second);
    } else {
        SourceManager::instance()->lyric(song, [this, key, lyricGeneration](const QString &lrc,
                                                                              const QString &trans) {
            if (lyricGeneration != d->lyricGen) return;   // 已切歌，迟到结果丢弃
            if (d->preloadedLyrics.size() >= 64) d->preloadedLyrics.clear();
            d->preloadedLyrics.insert(key, qMakePair(lrc, trans));
            PlayerController::instance()->setLyrics(lrc, trans);
        });
    }

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

    // 风险确认闸门：未签署「我自己承担后果」时，内置平台取链全部被拦。
    // 若照常发起 resolve，会一首首失败并自动跳下一首，把整个队列跳空
    // （实测日志出现过「连续失败 99」）。歌词/封面预取放在闸门之前——
    // 它们是只读文本/图片元数据（走 callMeta 放行），锁定态仍照常可用。
    // 音频取链在这里截断：不发起请求、不跳歌，只提示先完成风险确认。
    if (!onlineSourcesUnlocked()) {
        // beginSwitch() 置的 m_switching 必须清掉，否则用户后续点播放会被吞
        PlayerController::instance()->stop();
        const QString hint = QStringLiteral("请先导入音源：在设置页输入「我自己承担后果」并连点「导入」7 次完成风险确认");
        setStatus(hint);
        qWarning().noquote() << QStringLiteral("[音源] 未完成风险确认，停止取链（不跳歌）：%1 - %2")
                                    .arg(song.title, song.artist);
        emit notify(hint, QStringLiteral("warning"));
        return;
    }

    // 预取缓存命中：直链已在手，跳过整轮网络/脚本取链，切歌近乎即时。
    // 超过 5 分钟的缓存视为过期（网易/咪咕临时链会失效），重新取链。
    const auto cached = d->preloaded.constFind(key);
    if (cached != d->preloaded.constEnd()
        && QDateTime::currentMSecsSinceEpoch() - d->preloadedAt.value(key, 0) < 5 * 60 * 1000) {
        const Playable hit = cached.value();
        d->preloaded.remove(key);
        d->preloadedAt.remove(key);
        setStatus(QStringLiteral("正在播放：%1 - %2").arg(song.title, song.artist));
        applyResolveResult(song, hit);
        return;
    }

    SourceManager::instance()->resolve(song, d->quality,
        [this, song, key, resolveGeneration](const Playable &p) {
            if (resolveGeneration != d->resolveGen) return;   // 已切到别的歌，丢弃迟到的取链结果
            if (!p.ok) {
                // 取链阶段所有音源都失败（含 VIP 替代/跨源都试过）：同样自动跳下一首。
                autoAdvanceAfterFailure(song.title + QLatin1String(" - ") + song.artist,
                                        p.message, p.message);
                return;
            }
            // 缓存成功的直链：下次再播/预取命中时直接用（上限防止长会话无限增长）
            if (d->preloaded.size() >= 32) {
                d->preloaded.clear();
                d->preloadedAt.clear();
            }
            d->preloaded.insert(key, p);
            d->preloadedAt.insert(key, QDateTime::currentMSecsSinceEpoch());
            applyResolveResult(song, p);
        });
}

void AppController::applyResolveResult(const Song &song, const Playable &p)
{
    d->lastResolvedUrl = p.url.toString();
    d->lastPlayableHeaders = p.headers;
    d->pendingEngine = p.engineId;
    d->pendingKey = p.platformKey;
    // 同步记录本条直链的提供者（曲中断流重罚用；pending* 确认能播后就清了）
    d->playingEngine = p.engineId;
    d->playingKey = p.platformKey;
    d->playingProviderSource = p.engineId.isEmpty()
            ? (p.source.isEmpty() ? song.source : p.source) : QString();
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
}

void AppController::preloadNext()
{
    PlayerController *player = PlayerController::instance();
    const QVector<Song> queue = player->queue();
    const int cur = player->currentIndex();
    if (queue.isEmpty() || cur < 0 || cur >= queue.size()) return;

    int next = -1;
    switch (player->mode()) {
    case 3:   // 随机：不抽到自己
        if (queue.size() > 1) {
            for (int guard = 0; guard < 8 && next == cur; ++guard)
                next = QRandomGenerator::global()->bounded(queue.size());
            if (next == cur) next = (cur + 1) % queue.size();
        }
        break;
    case 0:   // 顺序播放：末尾不预取（留给推荐续播）
        if (cur + 1 < queue.size()) next = cur + 1;
        break;
    case 2:   // 单曲循环：缓存当前这首，重播时直接命中
        next = cur;
        break;
    default:  // 列表循环
        next = (cur + 1) % queue.size();
        break;
    }
    if (next < 0 || next >= queue.size()) return;
    preloadSong(queue.at(next));
}

void AppController::preloadSong(const Song &song)
{
    if (song.title.isEmpty()) return;
    const QString key = preloadKey(song, d->quality);
    if (d->preloading.contains(key)) return;
    d->preloading.insert(key);

    const quint64 gen = d->preloadGen;
    if (!d->preloadedLyrics.contains(key)) {
        SourceManager::instance()->lyric(song, [this, key, gen](const QString &lrc,
                                                                const QString &trans) {
            if (gen != d->preloadGen) return;
            if (d->preloadedLyrics.size() >= 64) d->preloadedLyrics.clear();
            d->preloadedLyrics.insert(key, qMakePair(lrc, trans));
        });
    }
    // quiet=true：预取早于真正播放，不能提前给音源降权/记「需要替代」
    SourceManager::instance()->resolve(song, d->quality, [this, key, song, gen](const Playable &p) {
        if (gen != d->preloadGen) return;
        d->preloading.remove(key);
        if (!p.ok) return;
        if (d->preloaded.size() >= 32) {
            d->preloaded.clear();
            d->preloadedAt.clear();
        }
        d->preloaded.insert(key, p);
        d->preloadedAt.insert(key, QDateTime::currentMSecsSinceEpoch());
        qInfo().noquote() << QStringLiteral("[预取] 下一首直链已缓存：%1 - %2")
                                 .arg(song.title, song.artist);
    }, true);
}

void AppController::clearPreloadCache()
{
    ++d->preloadGen;   // 在途预取结果全部作废
    d->preloaded.clear();
    d->preloadedAt.clear();
    d->preloadedLyrics.clear();
    d->preloading.clear();
    d->preloadTriggered = false;
}

void AppController::onPlayerError(const QString &msg)
{
    Song song = d->currentSong;
    // 断流前的位置：在线直链（咪咕/网易云临时链）常在曲中断掉，换源时要把进度带过去，
    // 否则听感上就是「播到一半突然从头再来」。position() 在 error 到达时仍是断流点；
    // 但部分后端出错瞬间把 position 清成 0，这时退回 PlayerController 记录的最后一个
    // 有效位置（>1s），避免 failedPos=0 导致换源后从头播。
    qint64 failedPos = PlayerController::instance()->position();
    if (failedPos < 1000) {
        const qint64 good = PlayerController::instance()->lastGoodPosition();
        if (good > failedPos) failedPos = good;
    }
    const QString brief = QStringLiteral("播放失败");
    // 播过 10 秒以上才按「曲中断流」处理；秒失败多半是地址过期/防盗链。
    // 注意量纲：failedPos 是「播放进度」，看门狗的 10s 是「停滞时长」。歌播到 3~5s
    // 卡住 10s 时看门狗已报「播放中断」，但 failedPos 仍 <10000；若只看进度会误判
    // 非曲中而跳过两级自愈直接扣分换源。看门狗消息带固定标记，据此一并纳入曲中断流。
    const bool watchdogStall = msg.contains(QStringLiteral("播放中断"));
    const bool midStream = failedPos >= 10000 || watchdogStall;

    Logger::write(QStringLiteral("W"),
                  QStringLiteral("播放失败：%1 - %2（%3）").arg(song.title, song.artist, msg));
    if (song.title.isEmpty() || d->inFallback) {
        setStatus(msg);
        // 换源解析/播放期间，被 stop 掉的旧流会持续投递迟到的 error（真机日志里
        // 同一秒刷了 20+ 条 "Demuxing failed"）。此时弹「播放失败」是误导——换源
        // 很可能马上成功。真正的失败由 resolveAlternate 的失败回调统一提示。
        // 关键：必须在早退之后才清 pendingEngine/pendingKey——换源成功后这两个字段
        // 属于「新源」，迟到 error 若把它们清掉，新源播过 2s 就拿不到加分/置顶（:512-518）。
        return;
    }
    // 只有确认是本次播放的 error 才清计分标记；早退路径保留新源的标记。
    d->pendingEngine.clear();
    d->pendingKey.clear();

    // ---- 曲中断流恢复阶梯：先自愈，最后才换源 ----
    // 1) 同一 URL 强制重连并从断点续播。绝大多数「放着放着停住」是后端缓冲/网络
    //    瞬时卡顿，重连即可恢复，不需要换源、也不该扣分。
    if (midStream && d->recoverStage == Impl::RecoverStage::None) {
        d->recoverStage = Impl::RecoverStage::SameUrl;
        d->recoverPos = failedPos;
        ++d->fallbackGen;   // 作废旧代次，避免上一次恢复的迟到回调插进来
        PlayerController::instance()->pauseWatchdog();
        PlayerController::instance()->restartCurrent(failedPos);
        setStatus(QStringLiteral("正在恢复播放…"));
        Logger::write(QStringLiteral("I"),
                      QStringLiteral("[恢复] 同一链接从 %1ms 重连（不换源）").arg(failedPos));
        return;
    }

    // 2) 重连仍失败 → 同一音源重新解析一条新直链（临时链过期的情况），依旧不换源。
    if (midStream && d->recoverStage == Impl::RecoverStage::SameUrl) {
        d->recoverStage = Impl::RecoverStage::ReResolve;
        const quint64 gen = ++d->fallbackGen;
        PlayerController::instance()->pauseWatchdog();
        setStatus(QStringLiteral("正在重新获取播放地址…"));
        SourceManager::instance()->resolve(song, d->quality,
            [this, gen, song, failedPos, msg](const Playable &p) {
                if (gen != d->fallbackGen) return;
                if (p.ok) {
                    PlayerController::instance()->playUrl(p.url, p.headers, failedPos);
                    setStatus(QStringLiteral("正在恢复播放…"));
                    Logger::write(QStringLiteral("I"),
                                  QStringLiteral("[恢复] 同音源新直链从 %1ms 续播").arg(failedPos));
                } else {
                    d->recoverStage = Impl::RecoverStage::Alternate;
                    startAlternateFallback(song, failedPos, msg);
                }
            });
        return;
    }

    // 3) 新直链还不行（或非曲中失败）：走原来的换源流程，并按真实提供者扣分。
    d->recoverStage = Impl::RecoverStage::Alternate;
    startAlternateFallback(song, failedPos, msg);
}

void AppController::startAlternateFallback(const Song &song, qint64 failedPos, const QString &msg)
{
    // 与 onPlayerError 的曲中断流门槛一致：播放进度 10s 或看门狗停滞标记（量纲不同，
    // 短进度卡死 10s 也应算曲中断流），保证扣分力度判定统一。
    const bool midStream = failedPos >= 10000 || msg.contains(QStringLiteral("播放中断"));
    const QString brief = QStringLiteral("播放失败");
    if (!d->playingEngine.isEmpty()) {
        // 直链由「脚本 × 平台」提供：重度扣分并记本首坏候选，让换源优先选别的
        ScriptSourceManager::instance()->reportResult(
            d->playingEngine, d->playingKey, false, midStream ? -8 : -4);
        ScriptSourceManager::instance()->notePlaybackFailure(
            d->playingEngine, d->playingKey,
            song.title + QLatin1Char('|') + song.artist);
        if (midStream)
            ScriptSourceManager::instance()->demoteSource(d->playingEngine);
        qWarning().noquote() << QStringLiteral("[换源] 直链播不出来%1，扣分：%2 [%3]")
                                    .arg(midStream ? QStringLiteral("（曲中断流）") : QString())
                                    .arg(d->playingEngine, d->playingKey);
    } else if (!d->playingProviderSource.isEmpty()
               && d->playingProviderSource != QLatin1String("custom")) {
        if (midStream)
            SourceManager::instance()->penalizeSource(d->playingProviderSource);
        else
            SourceManager::instance()->demoteSource(d->playingProviderSource);
    }
    d->inFallback = true;
    // 换源解析期间暂停看门狗：脚本逐源取链每个可达 15s，若继续按旧源的起播时刻
    // 计时，会在换源尚未完成时误报「加载超时」（实测 16s 假超时）。
    PlayerController::instance()->pauseWatchdog();
    const quint64 gen = ++d->fallbackGen;
    const QStringList exclude = QStringList() << song.source << d->triedFallback.values();
    qWarning().noquote() << QStringLiteral("[换源] 播放出错，开始换源：%1 - %2（源=%3 inFallback=%4 已试=%5）")
                                .arg(song.title, song.artist, song.source)
                                .arg(d->inFallback ? 1 : 0)
                                .arg(d->triedFallback.values().join(QStringLiteral(",")));
    SourceManager::instance()->resolveAlternate(song, exclude, d->quality,
        [this, gen, msg, song, brief, failedPos](const Playable &r) {
            if (gen != d->fallbackGen) return;     // 期间用户已切到别的歌，丢弃迟到结果
            if (r.ok) {
                d->consecutiveFailures = 0;
                if (r.source != QLatin1String("custom"))
                    d->triedFallback.insert(r.source);
                SourceManager::instance()->promoteSource(r.source);
                d->substituteBadge = r.substituted
                        ? (song.vip ? QStringLiteral("VIP 替代 · %1").arg(r.substituteSource)
                                    : r.substituteSource)
                        : (song.vip ? QStringLiteral("VIP") : QString());
                d->currentSourceLabel = SourceConfig::instance()->nameOf(song.source)
                        + (r.source == QLatin1String("custom")
                               ? QString() : QStringLiteral(" → %1").arg(r.substituteSource));
                d->pendingEngine = r.engineId;
                d->pendingKey = r.platformKey;
                d->playingEngine = r.engineId;
                d->playingKey = r.platformKey;
                d->playingProviderSource = r.engineId.isEmpty()
                        ? (r.source.isEmpty() ? song.source : r.source) : QString();
                d->inFallback = false;
                PlayerController::instance()->playUrl(r.url, r.headers, failedPos);
                Library::instance()->addHistory(song);
                setStatus(r.message.isEmpty()
                              ? QStringLiteral("正在播放：%1 - %2").arg(song.title, song.artist)
                              : r.message);
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
    // 未解锁风险确认时取链全被拦，这种情况下自动跳歌只会把队列跳空：
    // 直接停住并提示，绝不在锁定态下跳下一首（主闸门在 resolveAndPlay）。
    // 这里兜底拦的是闸门生效前已在途的 resolve/onPlayerError 迟到回调。
    if (!onlineSourcesUnlocked()) {
        d->consecutiveFailures = 0;
        d->inFallback = false;
        PlayerController *player = PlayerController::instance();
        player->stop();
        const QString hint = QStringLiteral("请先导入音源：在设置页输入「我自己承担后果」并连点「导入」7 次完成风险确认");
        setStatus(hint);
        emit notify(hint, QStringLiteral("warning"));
        return;
    }
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
    // 带上结果区上下文：歌单/榜单/收藏/历史按歌单播完，搜索/推荐由漫游模式接管续播。
    PlayerController::instance()->setQueue(songs, index, d->resultsContext);
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

// 搜索结果的歌曲加入「我的歌单」（歌单选择弹层选中后调用）。
// 歌追加到本地缓存歌曲表并更新歌单 count；账号同步的歌单下次同步仍以云端为准。
void AppController::addResultToPlaylist(int index, const QString &playlistUid)
{
    const QVector<Song> &songs = m_results->songs();
    if (index < 0 || index >= songs.size()) return;
    const Song s = songs.at(index);

    Playlist target;
    for (const Playlist &p : m_playlists->playlists())
        if (p.uid == playlistUid) { target = p; break; }
    if (target.uid.isEmpty()) {
        for (const Playlist &p : Library::instance()->playlists())
            if (p.uid == playlistUid) { target = p; break; }
    }
    if (target.uid.isEmpty()) { emit toast(QStringLiteral("未找到该歌单")); return; }

    QVector<Song> cur = Library::instance()->playlistSongs(target.uid);
    for (const Song &e : cur) {
        if (!s.uid.isEmpty() && e.uid == s.uid) {
            emit toast(QStringLiteral("已在歌单「%1」里").arg(target.title));
            return;
        }
    }
    cur.append(s);
    Library::instance()->saveSongs(QVector<Song>() << s);
    Library::instance()->setPlaylistSongs(target.uid, cur);
    if (target.count < cur.size()) {
        target.count = cur.size();
        Library::instance()->savePlaylist(target);
    }
    refreshPlaylists();
    emit toast(QStringLiteral("已加入歌单「%1」").arg(target.title));
}

// 收藏搜索结果里的歌单到「我的歌单」（lx-music-mobile 式）：歌单元数据立刻落库，
// 歌曲后台拉取缓存——离线也能打开；拉取失败不打扰，打开时会再走网络补全。
void AppController::collectSearchPlaylist(int index)
{
    if (!m_searchPlaylists) return;
    const QVector<Playlist> &list = m_searchPlaylists->playlists();
    if (index < 0 || index >= list.size()) return;
    collectPlaylist(list.at(index));
}

void AppController::collectPlaylist(const Playlist &playlist)
{
    for (const Playlist &e : Library::instance()->playlists()) {
        if (e.uid == playlist.uid) {
            emit toast(QStringLiteral("歌单「%1」已在「我的歌单」里").arg(playlist.title));
            return;
        }
    }
    Playlist p = playlist;
    p.type = QStringLiteral("custom");
    Library::instance()->savePlaylist(p);
    refreshPlaylists();
    emit toast(QStringLiteral("已收藏歌单「%1」").arg(p.title));

    SourceManager::instance()->playlistDetail(p, [this, p](const QVector<Song> &songs) {
        if (songs.isEmpty()) return;
        Library::instance()->saveSongs(songs);
        Library::instance()->setPlaylistSongs(p.uid, songs);
    });
}

void AppController::collectPlaylistByUid(const QString &uid)
{
    if (uid.isEmpty()) return;
    Playlist found;
    bool ok = false;
    const auto findIn = [&uid, &found, &ok](const QVector<Playlist> &list) {
        for (const Playlist &p : list) {
            if (p.uid == uid) { found = p; ok = true; return; }
        }
    };
    findIn(m_searchPlaylists->playlists());
    if (!ok) findIn(m_playlists->playlists());
    if (!ok) findIn(m_discovery->playlists());
    if (!ok) {
        emit toast(QStringLiteral("未找到该歌单"));
        return;
    }
    collectPlaylist(found);
}

void AppController::uncollectPlaylist(const QString &uid)
{
    if (uid.isEmpty()) return;
    const QVector<Playlist> all = Library::instance()->playlists();
    for (const Playlist &p : all) {
        if (p.uid != uid) continue;
        if (p.source == QLatin1String("local")) {
            emit toast(QStringLiteral("本地歌单不能取消收藏"));
            return;
        }
        Library::instance()->removePlaylist(uid);
        refreshPlaylists();
        emit toast(QStringLiteral("已取消收藏「%1」").arg(p.title));
        return;
    }
    emit toast(QStringLiteral("该歌单未收藏"));
}

// 新建空歌单：uid 用 randomUUID 防撞，type=custom 走本地歌单路径。
QString AppController::createMyPlaylist(const QString &title)
{
    const QString t = title.trimmed();
    if (t.isEmpty()) { emit toast(QStringLiteral("歌单名不能为空")); return QString(); }

    Playlist p;
    p.uid = QStringLiteral("local:%1")
                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    p.source = QStringLiteral("local");
    p.title = t;
    p.type = QStringLiteral("custom");
    p.count = 0;
    Library::instance()->savePlaylist(p);
    refreshPlaylists();
    emit toast(QStringLiteral("已创建歌单「%1」").arg(t));
    return p.uid;
}

void AppController::toggleFavoriteResult(int index)
{
    const QVector<Song> &songs = m_results->songs();
    if (index < 0 || index >= songs.size()) return;
    const Song &s = songs.at(index);
    if (Library::instance()->isFavorite(s.uid)) {
        Library::instance()->removeFavorite(s.uid);
        syncFavoriteToCloud(s, false);
        emit toast(QStringLiteral("已取消收藏"));
    } else {
        Library::instance()->addFavorite(s);
        syncFavoriteToCloud(s, true);
        emit toast(QStringLiteral("已收藏"));
    }
    m_results->refreshRow(index);
}

void AppController::toggleFavoriteQueue(int index)
{
    const QVector<Song> &songs = m_queue->songs();
    if (index < 0 || index >= songs.size()) return;
    const Song &s = songs.at(index);
    if (Library::instance()->isFavorite(s.uid)) {
        Library::instance()->removeFavorite(s.uid);
        syncFavoriteToCloud(s, false);
    } else {
        Library::instance()->addFavorite(s);
        syncFavoriteToCloud(s, true);
    }
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
        syncFavoriteToCloud(s, false);
        emit toast(QStringLiteral("已取消收藏"));
    } else {
        Library::instance()->addFavorite(s);
        syncFavoriteToCloud(s, true);
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
    const quint64 gen = d->resolveGen;   // 切歌后 resolveGen 递增，迟到的取链回调不再下载旧歌
    setStatus(QStringLiteral("正在准备下载「%1」…").arg(song.title));
    SourceManager::instance()->resolve(song, d->quality, [this, song, gen](const Playable &p) {
        if (gen != d->resolveGen) return;
        if (!p.ok) { emit toast(p.message); return; }
        QString dir = Settings::string(QStringLiteral("downloadDir"));
        if (dir.isEmpty()) dir = Settings::defaultDownloadsDir();
        QDir().mkpath(dir);
        QString name = (song.artist.isEmpty() ? song.title : song.artist + QStringLiteral(" - ") + song.title);
        name.remove(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")));
        name = name.trimmed();
        if (name.isEmpty()) name = QStringLiteral("furui-music");
        // 与 OpenApiServer::downloadFileName() 一致：超长文件名在部分系统/浏览器上会保存失败
        if (name.size() > 120) name = name.left(120);
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
    // MV 的画面与声音独立于音频播放器，音乐继续放会两路声音重叠。
    // 不能在解析前就暂停：解析失败（无 MV）时音乐会一直停着。改为记住
    // 进入前的播放状态，解析成功后才暂停；关闭 MV 时由 mvClosed() 恢复。
    setStatus(QStringLiteral("正在解析「%1」的 MV…").arg(song.title));
    const quint64 gen = ++m_mvOpenGen;
    const bool hasPrev = mvHasPrev();
    const bool hasNext = mvHasNext();
    SourceManager::instance()->mv(song, [this, song, gen, hasPrev, hasNext](bool ok, const QUrl &url, const QString &error) {
        if (gen != m_mvOpenGen) return;   // 连点/连切时先发后到的旧回包直接丢弃
        if (!ok) {
            setStatus(error);
            emit notify(error, QStringLiteral("error"));
            return;
        }
        if (playing()) {
            m_mvResumeOnClose = true;
            togglePause();
        }
        emit mvOpened(QVariantMap{
            {QStringLiteral("url"), url.toString()},
            {QStringLiteral("title"), song.title},
            {QStringLiteral("artist"), song.artist},
            {QStringLiteral("cover"), song.cover},
            {QStringLiteral("source"), song.source},
            {QStringLiteral("hasPrev"), hasPrev},
            {QStringLiteral("hasNext"), hasNext}});
        setStatus(QStringLiteral("正在播放 MV：%1 - %2").arg(song.title, song.artist));
    });
}

void AppController::mvClosed()
{
    ++m_mvOpenGen;   // 关闭时在途的 openMv 回包全部作废，避免关掉后又弹出 MV
    if (!m_mvResumeOnClose) return;
    m_mvResumeOnClose = false;
    if (!playing()) togglePause();
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
    // 切地区/刷新（!append）不是翻页：必须允许打断在途请求，否则 QML 已把 mvArea
    // 改成新地区、C++ 却被 loading 守卫吞掉，胶囊高亮与列表数据不一致。
    if ((m_mvLoading && append) || (append && !m_mvHasMore)) return;
    const quint64 gen = ++m_mvListGen;   // 每次请求（含切地区）作废旧回包
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
        [this, gen](const QVector<QVariantMap> &items, bool hasMore) {
            if (gen != m_mvListGen) return;   // 旧地区/旧请求的迟到回包直接丢弃
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

void AppController::loadComments(int page)
{
    if (page < 1) return;
    const Song song = d->currentSong;
    if (song.sourceId.isEmpty() || song.title.isEmpty()) {
        emit notify(QStringLiteral("请先播放一首歌曲再查看评论"), QStringLiteral("info"));
        return;
    }
    // 切歌后必须允许新歌重新拉第一页：旧的 in-flight 请求由 generation 作废，
    // 不再用 m_commentsLoading 守卫挡住新请求（否则新歌评论一直空）。
    if (m_commentsLoading && page > 1) return;
    const quint64 gen = ++m_commentsGen;
    m_commentsLoading = true;
    const bool append = (page > 1);
    if (!append) {
        m_comments.clear();
        m_commentTotal = -1;
    }
    m_commentSourceName = sourceName(song.source);
    emit commentsChanged();

    SourceManager::instance()->comments(song, page,
        [this, page, append, song, gen](const QVector<Comment> &list, int total) {
            if (gen != m_commentsGen) return;   // 切歌/重开评论后丢弃旧歌回包
            m_commentsLoading = false;
            if (list.isEmpty()) {
                // 失败/空页：不推进页码，避免 QML 反复请求不存在的下一页
                m_commentTotal = total >= 0 ? total : m_commentTotal;
                emit commentsChanged();
                if (page == 1)
                    emit notify(QStringLiteral("「%1」暂无评论或该平台未开放评论").arg(song.title),
                                QStringLiteral("info"));
                return;
            }
            if (append) {
                for (const Comment &c : list) m_comments.append(c.toVariant());
            } else {
                m_comments.clear();
                for (const Comment &c : list) m_comments.append(c.toVariant());
            }
            m_commentPage = page;   // 只在成功拿到数据时推进
            if (total >= 0) m_commentTotal = total;
            emit commentsChanged();
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

    const bool roaming = Settings::boolean(QStringLiteral("roamMode"), false);
    const bool qishuiSeed = seed.source == QLatin1String("qishui") && !seed.sourceId.isEmpty();
    // 漫游模式只接管非歌单上下文；歌单上下文即使开着漫游也按队列播完。
    // 但汽水歌例外：汽水官方 App 在歌单播完后会按最后一首继续推荐，
    // 这里不能被漫游的「歌单不接管」短路掉。
    if (!qishuiSeed && roaming
        && seed.extra.value(QStringLiteral("__roamContext")).toString()
                       == QLatin1String("playlist")) {
        player->playNextInQueue();
        return;
    }

    // 汽水 related 只认汽水 track_id。种子是其它平台时先用「歌名+歌手」在汽水
    // 搜一个匹配曲目当跳板（与 loadQishuiRecommend 熟悉/新鲜模式的映射同一思路），
    // 搜不到就退回原队列，不硬凑。
    const auto relatedOf = [this, seed, roaming](const Song &qishuiSeed) {
        SourceManager::instance()->related(qishuiSeed, 12,
            [this, seed, roaming](const QVector<Song> &list) {
                d->recommendPending = false;
                PlayerController *p = PlayerController::instance();
                // 请求期间用户可能已手动切歌：种子不是当前歌时丢弃，不能把上一首的
                // 相似歌塞进用户正在听的歌后面。
                if (p->current().uid != seed.uid) return;

                // 去重：跳过种子和队列里已有的歌，最多插 10 首。
                QSet<QString> known;
                known.insert(seed.uid);
                for (const Song &s : p->queue()) known.insert(s.uid);
                QVector<Song> picked;
                for (Song s : list) {
                    if (picked.size() >= 10) break;
                    if (s.uid.isEmpty() || s.title.isEmpty() || known.contains(s.uid)) continue;
                    known.insert(s.uid);
                    // 继承漫游上下文，让后续切歌继续按推荐流走
                    if (roaming) s.extra.insert(QStringLiteral("__roamContext"),
                                                seed.extra.value(QStringLiteral("__roamContext")));
                    picked << s;
                }

                if (picked.isEmpty()) {
                    setStatus(QStringLiteral("汽水推荐不可用，继续按队列播放"));
                    p->playNextInQueue();
                    return;
                }

                Logger::write(QStringLiteral("I"),
                              QStringLiteral("%1：按「%2」取到 %3 首推荐")
                                      .arg(roaming ? QStringLiteral("漫游模式")
                                                   : QStringLiteral("汽水漫游"))
                                      .arg(seed.title).arg(picked.size()));
                p->insertAfterCurrent(picked);
                setStatus(QStringLiteral("%1：已加入 %2 首推荐")
                              .arg(roaming ? QStringLiteral("漫游模式") : QStringLiteral("汽水漫游"))
                              .arg(picked.size()));
                QTimer::singleShot(0, this, [p] { p->playNextInQueue(); });
            });
    };

    d->recommendPending = true;
    setStatus(QStringLiteral("正在按「%1」获取推荐…").arg(seed.title));
    if (seed.source == QLatin1String("qishui") && !seed.sourceId.isEmpty()) {
        relatedOf(seed);
        return;
    }
    // 非汽水果子：汽水站内搜同名同歌手，取首个匹配映射成汽水 track
    const QString kw = (seed.title + QLatin1Char(' ') + seed.artist).trimmed();
    SourceManager::instance()->search(kw, QStringLiteral("qishui"), 5,
        [this, seed, relatedOf](const QVector<Song> &mapped) {
            // 请求期间用户已切到别的歌则丢弃
            if (PlayerController::instance()->current().uid != seed.uid) {
                d->recommendPending = false;
                return;
            }
            if (mapped.isEmpty()) {
                d->recommendPending = false;
                setStatus(QStringLiteral("未在汽水找到「%1」的匹配曲目，继续按队列播放")
                              .arg(seed.title));
                PlayerController::instance()->playNextInQueue();
                return;
            }
            relatedOf(mapped.first());
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
    // 这两个开关已从设置页移除、行为固定开启：忽略外部/旧 UI 的写入，
    // 也不让历史 settings.ini 里的 false 影响实际取链。
    if (key == QLatin1String("vipSubstitute") || key == QLatin1String("autoToggleSource")
        || key == QLatin1String("keepScreenOn"))
        return;
    // 「平台优先」选项已下线：外部脚本/旧配置若仍写入 platform，直接归一到默认值
    Settings::set(key, key == QLatin1String("sourceFirst")
                           && value.toString() == QLatin1String("platform")
                       ? QVariant(QStringLiteral("vip")) : value);
    if (key == QLatin1String("quality")) d->quality = value.toString();
    else if (key == QLatin1String("searchType")) d->searchType = value.toString();
    else if (key == QLatin1String("proxy")) HttpClient::instance()->setProxyFromSettings();
    else if (key == QLatin1String("logToFile")) Logger::instance()->setFileEnabled(value.toBool());
    else if (key == QLatin1String("openApi") || key == QLatin1String("openApiPort")) {
        OpenApiServer *api = OpenApiServer::instance();
        // 默认仅本机；显式设置 openApiLocalOnly=false 且有令牌时才允许局域网访问
        if (Settings::boolean(QStringLiteral("openApi"), false))
            api->start(Settings::integer(QStringLiteral("openApiPort"), 23330),
                       Settings::boolean(QStringLiteral("openApiLocalOnly"), true));
        else api->stop();
    }
    // 取链策略/音质变了：已缓存的直链与歌词不再可信，全部作废
    if (key == QLatin1String("quality") || key == QLatin1String("sourceFirst"))
        clearPreloadCache();
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
    // 未通过风险确认闸门（输入「我自己承担后果」+ 连点导入）不允许解析仓库
    if (!onlineSourcesUnlocked()) {
        emit notify(QStringLiteral("在线解析音源未启用：请在下方输入框输入确认文字「我自己承担后果」并连点「导入」完成风险确认"),
                    QStringLiteral("warning"));
        return;
    }
    ScriptSourceManager::instance()->rescanRepository();
    emit toast(QStringLiteral("正在从仓库页面发现音源…"));
}

void AppController::seedOnlineSources()
{
    if (!Settings::get(QStringLiteral("onlineSourcesUnlocked")).toBool()) {
        Settings::set(QStringLiteral("onlineSourcesUnlocked"), true);
        emit onlineSourcesUnlockedChanged();
    }
    // 解锁同时覆盖两层：内置平台取链（MusicSource::call 的锁）+ 仓库脚本发现。
    // 走 rescanRepository 绕过每日节流，保证首次确认后立即真正初始化。
    ScriptSourceManager::instance()->rescanRepository();
    emit toast(QStringLiteral("已确认自行承担风险：内置平台音源已解锁，正在解析仓库页面初始化脚本音源…"));
}

} // namespace Furui
