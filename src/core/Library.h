#pragma once
// 福瑞音乐 - 本地资料库（SQLite）：歌单、收藏、历史、账号、自定义音源
#include <QObject>
#include <QMutex>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <functional>

#include "Types.h"

namespace Furui {

class LibraryWorker;

class Library : public QObject {
    Q_OBJECT
public:
    static Library *instance();
    void init();
    void shutdown();

    QVector<Playlist> playlists(const QString &type = QString());
    void savePlaylists(const QVector<Playlist> &list);
    bool savePlaylist(const Playlist &p);
    bool removePlaylist(const QString &uid);

    void saveSongs(const QVector<Song> &list);
    Song song(const QString &uid);
    QVector<Song> playlistSongs(const QString &playlistUid);
    bool setPlaylistSongs(const QString &playlistUid, const QVector<Song> &songs);

    bool isFavorite(const QString &uid);
    void addFavorite(const Song &s);
    void removeFavorite(const QString &uid);
    bool replaceSourceFavorites(const QString &source, const QVector<Song> &songs,
                                QSet<QString> *removedUids = nullptr);
    QVector<Song> favorites();

    void addHistory(const Song &s);
    QVector<Song> history(int limit = 200);
    void addSearchHistory(const QString &keyword);
    QStringList searchHistory();
    void clearSearchHistory();

    QVector<Account> accounts();
    void saveAccount(const Account &a);
    void removeAccount(const QString &source);

    QVariantList customSources();
    void addCustomSource(const QVariantMap &item);
    void removeCustomSource(const QString &id);
    void setCustomLocalDirectory(const QString &path);
    QString customLocalDirectory() const;

    // 在线 JS 音源（LX 脚本）：脚本正文、状态与能力都会落库，重启后可直接恢复
    QVariantList scriptSources();
    void saveScriptSource(const QVariantMap &item);
    void removeScriptSource(const QString &id);

    // 本地凭据 v2：设备绑定密钥 + 随机 nonce + HMAC 完整性校验，兼容读取旧版混淆数据。
    // 密钥仍在本机，目标是防「数据库被拷走后离线还原」与「静默篡改」，不是对抗本机调试。
    static QString obfuscate(const QString &plain);
    static QString deobfuscate(const QString &stored);

signals:
    void changed();

private:
    explicit Library(QObject *parent = nullptr);
    ~Library() override;
    bool enqueueAndWait(const std::function<void()> &task) const;
    void enqueue(std::function<void()> task);
    LibraryWorker *m_worker = nullptr;
    // 收藏 uid 常驻 GUI 内存：SongListModel 每行渲染都会问一次 isFavorite，
    // 这里只做纯内存查询，不进入 SQLite worker，也不等待队列。
    QSet<QString> m_favoriteUids;
    bool m_ready = false;
    QMutex m_shutdownMutex;
    bool m_shutdownComplete = false;
};

} // namespace Furui
