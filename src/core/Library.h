#pragma once
// 福瑞音乐 - 本地资料库（SQLite）：歌单、收藏、历史、账号、自定义音源
#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include "Types.h"

namespace Furui {

class Library : public QObject {
    Q_OBJECT
public:
    static Library *instance();
    void init();

    QVector<Playlist> playlists(const QString &type = QString());
    void savePlaylists(const QVector<Playlist> &list);
    void savePlaylist(const Playlist &p);
    void removePlaylist(const QString &uid);

    void saveSongs(const QVector<Song> &list);
    Song song(const QString &uid);
    QVector<Song> playlistSongs(const QString &playlistUid);
    void setPlaylistSongs(const QString &playlistUid, const QVector<Song> &songs);

    bool isFavorite(const QString &uid);
    void addFavorite(const Song &s);
    void removeFavorite(const QString &uid);
    QVector<Song> favorites();

    void addHistory(const Song &s);
    QVector<Song> history(int limit = 200);
    void addSearchHistory(const QString &keyword);
    QStringList searchHistory();

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
    void exec(const QString &sql);
    void insertSong(const Song &s);
    static Song songFromQuery(const QSqlQuery &q);
    QSqlDatabase m_db;
    bool m_ready = false;
};

} // namespace Furui
