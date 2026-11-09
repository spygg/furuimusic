#pragma once
// 福瑞音乐 - UI 门面（单例）
// Widgets 前端与 Qt Quick 前端都只依赖本文件定义的 API。
// 所有方法都是同步返回 / 异步发信号，内部网络请求不阻塞 UI 线程。
#include <QAbstractListModel>
#include <QObject>
#include <QVariant>

#include "Types.h"

namespace Furui {

class SongListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles {
        UidRole = Qt::UserRole + 1, TitleRole, ArtistRole, AlbumRole, DurationRole,
        CoverRole, SourceRole, SourceNameRole, VipRole, BadgeRole, SourceIdRole,
        FavoriteRole
    };
    Q_ENUM(Roles)
    explicit SongListModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setSongs(const QVector<Song> &songs);
    void appendSongs(const QVector<Song> &songs);
    void refreshRow(int row);
    const QVector<Song> &songs() const { return m_songs; }
    void clear();
private:
    QVector<Song> m_songs;
    QString m_sourceNames; // 占位，保持 ABI 稳定
};

class PlaylistListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { UidRole = Qt::UserRole + 1, TitleRole, CoverRole, CountRole,
                 SourceRole, SourceNameRole, TypeRole, OwnerRole };
    Q_ENUM(Roles)
    explicit PlaylistListModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setPlaylists(const QVector<Playlist> &list);
    const QVector<Playlist> &playlists() const { return m_list; }
    void clear();
private:
    QVector<Playlist> m_list;
};

class AccountListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Roles { SourceRole = Qt::UserRole + 1, NameRole, NicknameRole, AvatarRole,
                 LoggedRole, StatusRole, MethodsRole, ApiAvailableRole, ApiStatusRole,
                 PlaylistCountRole };
    Q_ENUM(Roles)
    explicit AccountListModel(QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    // 元数据由门面统一组装：平台名 / 登录方式 / 是否能同步歌单 / 歌单数量
    struct Meta {
        QString name;
        QStringList methods;
        bool apiAvailable = false;
        QString apiStatus;
        int playlistCount = 0;
    };
    void setAccounts(const QVector<Account> &list, const QHash<QString, Meta> &meta);
    const QVector<Account> &accounts() const { return m_list; }
private:
    QVector<Account> m_list;
    QHash<QString, Meta> m_meta;
};

class AppController : public QObject {
    Q_OBJECT
    // ---- 模型 ----
    Q_PROPERTY(SongListModel *results READ results CONSTANT)
    Q_PROPERTY(SongListModel *queue READ queue CONSTANT)
    Q_PROPERTY(PlaylistListModel *playlists READ playlists CONSTANT)
    Q_PROPERTY(AccountListModel *accounts READ accounts CONSTANT)
    // ---- 播放器状态 ----
    Q_PROPERTY(QString currentTitle READ currentTitle NOTIFY playerChanged)
    Q_PROPERTY(QString currentArtist READ currentArtist NOTIFY playerChanged)
    Q_PROPERTY(QString currentCover READ currentCover NOTIFY playerChanged)
    Q_PROPERTY(QString currentSourceLabel READ currentSourceLabel NOTIFY playerChanged)
    Q_PROPERTY(QString substituteBadge READ substituteBadge NOTIFY playerChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY playerChanged)
    Q_PROPERTY(int currentQueueIndex READ currentQueueIndex NOTIFY playerChanged)
    Q_PROPERTY(bool currentFavorite READ currentFavorite NOTIFY playerChanged)
    Q_PROPERTY(QString theme READ theme NOTIFY settingsChanged)
    Q_PROPERTY(int volume READ volume NOTIFY settingsChanged)
    Q_PROPERTY(int playMode READ playMode NOTIFY settingsChanged)
    Q_PROPERTY(QString playModeName READ playModeName NOTIFY settingsChanged)
    Q_PROPERTY(QString lyricsText READ lyricsText NOTIFY lyricsChanged)
    Q_PROPERTY(QString currentLyricLine READ currentLyricLine NOTIFY lyricsChanged)
    Q_PROPERTY(int currentLyricIndex READ currentLyricIndex NOTIFY lyricsChanged)
    // ---- 全局状态 ----
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusChanged)
    Q_PROPERTY(QString quality READ quality NOTIFY settingsChanged)
    Q_PROPERTY(bool vipSubstituteEnabled READ vipSubstituteEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool autoToggleSource READ autoToggleSource NOTIFY settingsChanged)
    Q_PROPERTY(QString searchType READ searchType NOTIFY settingsChanged)
    Q_PROPERTY(bool openApiEnabled READ openApiEnabled NOTIFY settingsChanged)
    Q_PROPERTY(QString openApiAddress READ openApiAddress NOTIFY settingsChanged)
    Q_PROPERTY(QStringList sourceIds READ sourceIds CONSTANT)
    Q_PROPERTY(QStringList sourceNames READ sourceNames CONSTANT)
    Q_PROPERTY(QStringList customSourceIds READ customSourceIds NOTIFY customSourcesChanged)
    Q_PROPERTY(QStringList customSourceNames READ customSourceNames NOTIFY customSourcesChanged)
    Q_PROPERTY(QString customLocalDirectory READ customLocalDirectory NOTIFY settingsChanged)
    // ---- 登录 / 二维码 ----
    Q_PROPERTY(QString qrImagePath READ qrImagePath NOTIFY qrChanged)
    Q_PROPERTY(QString qrHint READ qrHint NOTIFY qrChanged)
    Q_PROPERTY(QString qrStatus READ qrStatus NOTIFY qrChanged)
    Q_PROPERTY(QString qrSource READ qrSource NOTIFY qrChanged)
    Q_PROPERTY(bool qrActive READ qrActive NOTIFY qrChanged)
    // 自建账号 API 服务地址（未配置时只有网易云是内置真实实现）
    Q_PROPERTY(QString accountApiBase READ accountApiBase WRITE setAccountApiBase NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList scriptSources READ scriptSources NOTIFY scriptSourcesChanged)
    // ---- 应用信息 ----
    Q_PROPERTY(QString appName READ appName CONSTANT)
    Q_PROPERTY(QString version READ version CONSTANT)
public:
    static AppController *instance();
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;

    SongListModel *results() const { return m_results; }
    SongListModel *queue() const { return m_queue; }
    PlaylistListModel *playlists() const { return m_playlists; }
    AccountListModel *accounts() const { return m_accounts; }

    QString currentTitle() const;
    QString currentArtist() const;
    QString currentCover() const;
    QString currentSourceLabel() const;
    QString substituteBadge() const;
    bool playing() const;
    qint64 position() const;
    qint64 duration() const;
    int currentQueueIndex() const;
    bool currentFavorite() const;
    int volume() const;
    int playMode() const;
    QString playModeName() const;
    QString lyricsText() const;
    QString currentLyricLine() const;
    int currentLyricIndex() const;
    QString statusMessage() const;
    QString quality() const;
    bool vipSubstituteEnabled() const;
    bool autoToggleSource() const;
    QString searchType() const;
    bool openApiEnabled() const;
    QString openApiAddress() const;
    QStringList sourceIds() const;
    QStringList sourceNames() const;
    QStringList customSourceIds() const;
    QStringList customSourceNames() const;
    QVariantList scriptSources() const;
    QString customLocalDirectory() const;
    QString qrImagePath() const;
    QString qrHint() const;
    QString qrStatus() const;
    QString qrSource() const;
    // 扫码会话是否仍在轮询（终态后为 false），界面用它判断二维码是否已失效
    bool qrActive() const;
    QString accountApiBase() const;
    void setAccountApiBase(const QString &value);
    QString appName() const { return QStringLiteral("福瑞音乐"); }
    QString version() const { return QStringLiteral("1.0.0"); }

    Q_INVOKABLE QString sourceName(const QString &id) const;   // 平台 id -> 显示名

    Q_INVOKABLE void setSearchType(const QString &type);   // song / songlist / album / singer
    Q_INVOKABLE void search(const QString &keyword, const QString &sourceId = QString());
    Q_INVOKABLE void loadRank(const QString &sourceId);
    Q_INVOKABLE void loadPlaylist(const QString &playlistUid);
    Q_INVOKABLE void loadFavorites();
    Q_INVOKABLE void loadHistory();
    // 聚合视图：按平台筛选歌单 / 收藏 / 历史，空串表示全部平台
    Q_INVOKABLE void filterPlaylists(const QString &sourceId);
    Q_INVOKABLE void loadFavoritesBySource(const QString &sourceId);
    Q_INVOKABLE void loadHistoryBySource(const QString &sourceId);
    Q_INVOKABLE QString sourceColor(const QString &sourceId) const;
    Q_INVOKABLE QStringList searchHistory() const;
    Q_INVOKABLE void refreshPlaylists();
    Q_INVOKABLE void refreshAccounts();

    Q_INVOKABLE void playResult(int index);
    Q_INVOKABLE void playQueue(int index);
    Q_INVOKABLE void addResultToQueue(int index);
    Q_INVOKABLE void addPlaylistToQueue(const QString &playlistUid);
    Q_INVOKABLE void toggleFavoriteResult(int index);
    Q_INVOKABLE void toggleFavoriteQueue(int index);
    Q_INVOKABLE bool isFavoriteCurrent() const;
    Q_INVOKABLE void toggleFavoriteCurrent();
    Q_INVOKABLE void removeQueue(int index);
    Q_INVOKABLE void clearQueue();
    Q_INVOKABLE void downloadResult(int index);

    Q_INVOKABLE void togglePause();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void seek(qint64 positionMs);
    Q_INVOKABLE void setVolume(int value);       // 0..100
    Q_INVOKABLE void setPlayMode(int mode);      // 0 顺序 1 列表循环 2 单曲循环 3 随机
    Q_INVOKABLE void cyclePlayMode();

    // ---- 登录 ----
    Q_INVOKABLE QStringList loginMethods(const QString &sourceId) const; // qrcode / cookie / password / phone
    Q_INVOKABLE QString accountApiStatus(const QString &sourceId) const; // 该平台账号接口的如实状态
    // 逐平台账号 API 服务地址：未内置直连的平台配好后即可同步歌单与收藏
    Q_INVOKABLE QString accountApiBaseFor(const QString &sourceId) const;
    Q_INVOKABLE void setAccountApiBaseFor(const QString &sourceId, const QString &value);
    Q_INVOKABLE bool userApiAvailable(const QString &sourceId) const;    // 登录后能否直接同步歌单
    Q_INVOKABLE QString qrUnavailableReason(const QString &sourceId) const; // 没有扫码方式时的说明
    Q_INVOKABLE QString cookieLoginHint(const QString &sourceId) const;     // Cookie 页的说明
    Q_INVOKABLE void startQrLogin(const QString &sourceId);
    Q_INVOKABLE void cancelQrLogin();
    Q_INVOKABLE void loginByCookie(const QString &sourceId, const QString &cookie);
    Q_INVOKABLE void loginByPassword(const QString &sourceId, const QString &account, const QString &password);
    Q_INVOKABLE void sendPhoneCode(const QString &sourceId, const QString &phone);
    Q_INVOKABLE void loginByPhone(const QString &sourceId, const QString &phone, const QString &code);
    Q_INVOKABLE void logout(const QString &sourceId);
    Q_INVOKABLE void syncUserLibrary(const QString &sourceId);
    Q_INVOKABLE void syncAllUserLibraries();

    // ---- 设置 / 自定义音源 ----
    QString theme() const;
    Q_INVOKABLE QVariant setting(const QString &key, const QVariant &fallback = QVariant()) const;
    Q_INVOKABLE void setSetting(const QString &key, const QVariant &value);
    Q_INVOKABLE void addCustomHttpSource(const QString &name, const QString &baseUrl);
    Q_INVOKABLE void setCustomLocalDirectory(const QString &path);
    Q_INVOKABLE void removeCustomSource(const QString &id);
    Q_INVOKABLE void refreshCustomSources();
    // ---- 在线 JS 音源（LX 脚本）----
    Q_INVOKABLE void importScriptSource(const QString &url);
    Q_INVOKABLE void updateScriptSource(const QString &id);
    Q_INVOKABLE void removeScriptSource(const QString &id);
    Q_INVOKABLE void setScriptSourceEnabled(const QString &id, bool enabled);
    Q_INVOKABLE void checkScriptSource(const QString &id);
    Q_INVOKABLE void checkAllScriptSources();
    Q_INVOKABLE void openExternal(const QString &url);

signals:
    void toast(const QString &message);
    void statusChanged();
    void playerChanged();
    void playingChanged();
    void positionChanged();
    void lyricsChanged();
    void settingsChanged();
    void customSourcesChanged();
    void scriptSourcesChanged();
    void scriptSourceMessage(const QString &id, bool ok, const QString &message);
    void searchStarted();
    void searchFinished(int count);
    void playlistsLoaded();
    // 某个平台的登录后同步结果（歌单/收藏），供登录页显示逐平台状态
    void syncFinished(const QString &sourceId, bool ok, const QString &message);
    void loginStateChanged(const QString &sourceId, bool ok, const QString &message);
    void qrChanged();
    void substituteInfo(const QString &text);
    void downloadFinished(const QString &path);

private:
    void setStatus(const QString &text);
    void resolveAndPlay(const Song &song);
    void syncQueueModel();
    void syncAccountModel();
    // 按 Impl::playlistFilter 把本地全部歌单过滤后写入模型
    void applyPlaylistFilter();
    void syncCustomModel();
    struct Impl;
    Impl *d;
    SongListModel *m_results;
    SongListModel *m_queue;
    PlaylistListModel *m_playlists;
    AccountListModel *m_accounts;
    static AppController *s_instance;
};

} // namespace Furui
