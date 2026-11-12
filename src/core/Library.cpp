#include "Library.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include "core/QtCompat.h"
#include <QSqlError>
#include <QSqlQuery>
#include <QSysInfo>
#include <QVariant>

#include "Settings.h"

namespace Furui {

// 写库失败以前是静默的：内存态已经按「成功」更新、界面也提示了，重启才发现收藏和歌单没了。
// 统一在这里报警，调用方据此决定是否更新内存态。
static bool execWrite(QSqlQuery &q, const char *what)
{
    if (q.exec()) return true;
    qWarning() << "[FuruiMusic] Library" << what << "写入失败:" << q.lastError().text();
    return false;
}

Library *Library::instance()
{
    static Library *l = new Library();
    return l;
}

Library::Library(QObject *parent)
    : QObject(parent)
{
    init();
}

void Library::exec(const QString &sql)
{
    QSqlQuery q(m_db);
    if (!q.exec(sql))
        qWarning() << "[FuruiMusic] SQL:" << q.lastError().text() << sql;
}

void Library::init()
{
    if (m_ready) return;
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("furui_music"));
    m_db.setDatabaseName(Settings::filePath(QStringLiteral("furui.db")));
    if (!m_db.open()) {
        qWarning() << "[FuruiMusic] SQLite 无法打开:" << m_db.lastError().text();
        return;
    }
    m_ready = true;
    // WAL + NORMAL：移动端每次自动提交都要 fsync，播放/收藏这类高频写入会明显卡顿
    exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    exec(QStringLiteral("CREATE TABLE IF NOT EXISTS songs (uid TEXT PRIMARY KEY, source TEXT, source_id TEXT, title TEXT, artist TEXT, album TEXT, cover TEXT, duration INTEGER, vip INTEGER, extra TEXT)"));
    exec(QStringLiteral("CREATE TABLE IF NOT EXISTS playlists (uid TEXT PRIMARY KEY, source TEXT, source_id TEXT, title TEXT, cover TEXT, owner TEXT, count INTEGER, type TEXT, extra TEXT)"));
    exec(QStringLiteral("CREATE TABLE IF NOT EXISTS playlist_songs (playlist_uid TEXT, song_uid TEXT, pos INTEGER)"));
    exec(QStringLiteral("CREATE TABLE IF NOT EXISTS favorites (song_uid TEXT PRIMARY KEY, added INTEGER)"));
    exec(QStringLiteral("CREATE TABLE IF NOT EXISTS history (id INTEGER PRIMARY KEY AUTOINCREMENT, song_uid TEXT, played INTEGER)"));
    exec(QStringLiteral("CREATE TABLE IF NOT EXISTS accounts (source TEXT PRIMARY KEY, uid TEXT, nickname TEXT, avatar TEXT, cookie TEXT, token TEXT, logged INTEGER, updated INTEGER, status TEXT)"));
    exec(QStringLiteral("CREATE TABLE IF NOT EXISTS custom_sources (id TEXT PRIMARY KEY, name TEXT, type TEXT, value TEXT, enabled INTEGER)"));
    exec(QStringLiteral("CREATE TABLE IF NOT EXISTS script_sources (id TEXT PRIMARY KEY, name TEXT, url TEXT, version TEXT, script TEXT, hash TEXT, enabled INTEGER, status TEXT, error_type TEXT, error_msg TEXT, actions TEXT, qualities TEXT, sources TEXT, updated INTEGER, checked INTEGER)"));
    exec(QStringLiteral("CREATE TABLE IF NOT EXISTS search_history (keyword TEXT PRIMARY KEY, ts INTEGER)"));
    // 高频查询的索引：歌单曲目按 pos 排序、历史按播放时间倒序、歌曲按来源筛选
    exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_playlist_songs_pos ON playlist_songs(playlist_uid, pos)"));
    // 老版本的 history 是「每次播放插一行」，先把重复行压成每首一条，
    // 再建唯一索引，addHistory 才能用 INSERT OR REPLACE 把表控制在歌曲数量级。
    // 这段只在从旧库升级时跑一次：NOT IN 全表扫是纯开机开销，唯一索引建好后它永远是空操作。
    QSqlQuery ver(m_db);
    int schemaVer = 0;
    if (ver.exec(QStringLiteral("PRAGMA user_version")) && ver.next())
        schemaVer = ver.value(0).toInt();
    if (schemaVer < 2) {
        exec(QStringLiteral("DELETE FROM history WHERE id NOT IN (SELECT MAX(id) FROM history GROUP BY song_uid)"));
        exec(QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_history_song ON history(song_uid)"));
        // (song_uid, played) 与上面的唯一索引前缀重复，只会拖慢写入，升级时一并清掉
        exec(QStringLiteral("DROP INDEX IF EXISTS idx_history_song_played"));
        exec(QStringLiteral("PRAGMA user_version = 2"));
    }
    // 历史版本把「加载榜单」的结果当用户歌单存了进来（数量为 0、点开是公开榜单）。
    // 榜单属于每次拉取的公开数据，不该留在本地歌单表里，升级时清一次。
    if (schemaVer < 3) {
        exec(QStringLiteral("DELETE FROM playlists WHERE type='rank'"));
        exec(QStringLiteral("PRAGMA user_version = 3"));
    }
    exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_favorites_added ON favorites(added)"));
    exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_songs_source ON songs(source)"));
    exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_songs_source_id ON songs(source, source_id)"));
    reloadFavoriteCache();
}

void Library::reloadFavoriteCache()
{
    m_favoriteUids.clear();
    if (!m_ready) return;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT song_uid FROM favorites")))
        while (q.next()) m_favoriteUids.insert(q.value(0).toString());
}

Song Library::songFromQuery(const QSqlQuery &q)
{
    Song s;
    s.uid       = q.value(QStringLiteral("uid")).toString();
    s.source    = q.value(QStringLiteral("source")).toString();
    s.sourceId  = q.value(QStringLiteral("source_id")).toString();
    s.title     = q.value(QStringLiteral("title")).toString();
    s.artist    = q.value(QStringLiteral("artist")).toString();
    s.album     = q.value(QStringLiteral("album")).toString();
    s.cover     = q.value(QStringLiteral("cover")).toString();
    s.duration  = q.value(QStringLiteral("duration")).toLongLong();
    s.vip       = q.value(QStringLiteral("vip")).toBool();
    const QByteArray raw = q.value(QStringLiteral("extra")).toString().toUtf8();
    s.extra     = QJsonDocument::fromJson(raw).object().toVariantMap();
    return s;
}

void Library::insertSong(const Song &s)
{
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO songs (uid,source,source_id,title,artist,album,cover,duration,vip,extra) "
                             "VALUES (?,?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(s.uid);
    q.addBindValue(s.source);
    q.addBindValue(s.sourceId);
    q.addBindValue(s.title);
    q.addBindValue(s.artist);
    q.addBindValue(s.album);
    q.addBindValue(s.cover);
    q.addBindValue(s.duration);
    q.addBindValue(s.vip ? 1 : 0);
    q.addBindValue(QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(s.extra)).toJson(QJsonDocument::Compact)));
    if (!q.exec()) qWarning() << "[FuruiMusic] insertSong:" << q.lastError().text();
}

void Library::saveSongs(const QVector<Song> &list)
{
    if (!m_ready || list.isEmpty()) return;
    // 一次搜索动辄 30 首歌：原来每首单独 INSERT 且自动提交，等于 30 次 fsync。
    // 包进事务 + 复用一条 prepared statement，写库时间从几十毫秒降到个位数。
    m_db.transaction();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO songs (uid,source,source_id,title,artist,album,cover,duration,vip,extra) "
                             "VALUES (?,?,?,?,?,?,?,?,?,?)"));
    for (const Song &s : list) {
        q.bindValue(0, s.uid);
        q.bindValue(1, s.source);
        q.bindValue(2, s.sourceId);
        q.bindValue(3, s.title);
        q.bindValue(4, s.artist);
        q.bindValue(5, s.album);
        q.bindValue(6, s.cover);
        q.bindValue(7, s.duration);
        q.bindValue(8, s.vip ? 1 : 0);
        q.bindValue(9, QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(s.extra))
                                             .toJson(QJsonDocument::Compact)));
        if (!q.exec())
            qWarning() << "[FuruiMusic] saveSongs:" << q.lastError().text();
    }
    m_db.commit();
}

Song Library::song(const QString &uid)
{
    if (!m_ready) return Song();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT * FROM songs WHERE uid=?"));
    q.addBindValue(uid);
    if (q.exec() && q.next()) return songFromQuery(q);
    return Song();
}

QVector<Playlist> Library::playlists(const QString &type)
{
    QVector<Playlist> out;
    if (!m_ready) return out;
    QSqlQuery q(m_db);
    if (type.isEmpty())
        q.exec(QStringLiteral("SELECT * FROM playlists ORDER BY type, title"));
    else {
        q.prepare(QStringLiteral("SELECT * FROM playlists WHERE type=? ORDER BY title"));
        q.addBindValue(type);
        q.exec();
    }
    while (q.next()) {
        Playlist p;
        p.uid      = q.value(QStringLiteral("uid")).toString();
        p.source   = q.value(QStringLiteral("source")).toString();
        p.sourceId = q.value(QStringLiteral("source_id")).toString();
        p.title    = q.value(QStringLiteral("title")).toString();
        p.cover    = q.value(QStringLiteral("cover")).toString();
        p.owner    = q.value(QStringLiteral("owner")).toString();
        p.count    = q.value(QStringLiteral("count")).toInt();
        p.type     = q.value(QStringLiteral("type")).toString();
        const QByteArray raw = q.value(QStringLiteral("extra")).toString().toUtf8();
        p.extra    = QJsonDocument::fromJson(raw).object().toVariantMap();
        out << p;
    }
    return out;
}

void Library::savePlaylist(const Playlist &p)
{
    if (!m_ready) return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO playlists (uid,source,source_id,title,cover,owner,count,type,extra) "
                             "VALUES (?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(p.uid);
    q.addBindValue(p.source);
    q.addBindValue(p.sourceId);
    q.addBindValue(p.title);
    q.addBindValue(p.cover);
    q.addBindValue(p.owner);
    q.addBindValue(p.count);
    q.addBindValue(p.type);
    q.addBindValue(QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(p.extra)).toJson(QJsonDocument::Compact)));
    if (!q.exec()) qWarning() << "[FuruiMusic] savePlaylist:" << q.lastError().text();
}

void Library::savePlaylists(const QVector<Playlist> &list)
{
    if (!m_ready || list.isEmpty()) return;
    // 和 saveSongs 一致：一条 prepared statement + 一个事务，
    // 避免每个歌单一次自动提交（几十次 fsync）
    m_db.transaction();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO playlists (uid,source,source_id,title,cover,owner,count,type,extra) "
                             "VALUES (?,?,?,?,?,?,?,?,?)"));
    for (const Playlist &p : list) {
        q.bindValue(0, p.uid);
        q.bindValue(1, p.source);
        q.bindValue(2, p.sourceId);
        q.bindValue(3, p.title);
        q.bindValue(4, p.cover);
        q.bindValue(5, p.owner);
        q.bindValue(6, p.count);
        q.bindValue(7, p.type);
        q.bindValue(8, QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(p.extra))
                                             .toJson(QJsonDocument::Compact)));
        if (!q.exec()) qWarning() << "[FuruiMusic] savePlaylists:" << q.lastError().text();
    }
    m_db.commit();
    emit changed();
}

void Library::removePlaylist(const QString &uid)
{
    if (!m_ready) return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM playlists WHERE uid=?"));
    q.addBindValue(uid);
    execWrite(q, "deletePlaylist(playlists)");
    q.prepare(QStringLiteral("DELETE FROM playlist_songs WHERE playlist_uid=?"));
    q.addBindValue(uid);
    execWrite(q, "deletePlaylist(playlist_songs)");
}

void Library::setPlaylistSongs(const QString &playlistUid, const QVector<Song> &songs)
{
    if (!m_ready) return;
    m_db.transaction();

    QSqlQuery songQuery(m_db);
    songQuery.prepare(QStringLiteral("INSERT OR REPLACE INTO songs (uid,source,source_id,title,artist,album,cover,duration,vip,extra) "
                                     "VALUES (?,?,?,?,?,?,?,?,?,?)"));
    for (const Song &s : songs) {
        songQuery.bindValue(0, s.uid);
        songQuery.bindValue(1, s.source);
        songQuery.bindValue(2, s.sourceId);
        songQuery.bindValue(3, s.title);
        songQuery.bindValue(4, s.artist);
        songQuery.bindValue(5, s.album);
        songQuery.bindValue(6, s.cover);
        songQuery.bindValue(7, s.duration);
        songQuery.bindValue(8, s.vip ? 1 : 0);
        songQuery.bindValue(9, QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(s.extra))
                                                     .toJson(QJsonDocument::Compact)));
        if (!songQuery.exec())
            qWarning() << "[FuruiMusic] setPlaylistSongs song:" << songQuery.lastError().text();
    }

    QSqlQuery del(m_db);
    del.prepare(QStringLiteral("DELETE FROM playlist_songs WHERE playlist_uid=?"));
    del.addBindValue(playlistUid);
    execWrite(del, "setPlaylistSongs clear");

    QSqlQuery ins(m_db);
    ins.prepare(QStringLiteral("INSERT INTO playlist_songs (playlist_uid,song_uid,pos) VALUES (?,?,?)"));
    int failed = 0;
    for (int i = 0; i < songs.size(); ++i) {
        ins.bindValue(0, playlistUid);
        ins.bindValue(1, songs.at(i).uid);
        ins.bindValue(2, i);
        if (!ins.exec())
            ++failed;
    }
    // 循环里逐条报警会把日志刷满，只在有失败时汇总一次；少掉的行就是界面上「歌单变短」的原因
    if (failed > 0)
        qWarning() << "[FuruiMusic] Library setPlaylistSongs 丢失" << failed << "/"
                   << songs.size() << "行:" << ins.lastError().text();
    m_db.commit();
    emit changed();
}

QVector<Song> Library::playlistSongs(const QString &playlistUid)
{
    QVector<Song> out;
    if (!m_ready) return out;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT s.* FROM playlist_songs ps JOIN songs s ON s.uid=ps.song_uid "
                             "WHERE ps.playlist_uid=? ORDER BY ps.pos"));
    q.addBindValue(playlistUid);
    if (q.exec()) while (q.next()) out << songFromQuery(q);
    return out;
}

bool Library::isFavorite(const QString &uid)
{
    if (!m_ready) return false;
    return m_favoriteUids.contains(uid);
}

void Library::addFavorite(const Song &s)
{
    if (!m_ready) return;
    // 歌曲信息 + 收藏关系是一次用户动作，放同一个事务里只 fsync 一次
    m_db.transaction();
    insertSong(s);
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO favorites (song_uid,added) VALUES (?,?)"));
    q.addBindValue(s.uid);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!execWrite(q, "addFavorite")) {
        m_db.rollback();
        return;
    }
    m_db.commit();
    m_favoriteUids.insert(s.uid);
    emit changed();
}

void Library::removeFavorite(const QString &uid)
{
    if (!m_ready) return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM favorites WHERE song_uid=?"));
    q.addBindValue(uid);
    if (!execWrite(q, "removeFavorite"))
        return;
    m_favoriteUids.remove(uid);
    emit changed();
}

QVector<Song> Library::favorites()
{
    QVector<Song> out;
    if (!m_ready) return out;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT s.* FROM favorites f JOIN songs s ON s.uid=f.song_uid ORDER BY f.added DESC")))
        while (q.next()) out << songFromQuery(q);
    return out;
}

void Library::addHistory(const Song &s)
{
    if (!m_ready) return;
    m_db.transaction();
    insertSong(s);
    QSqlQuery q(m_db);
    // 每首歌只保留最近一次播放时间（idx_history_song 唯一索引），表不会随播放次数无限增长
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO history (song_uid,played) VALUES (?,?)"));
    q.addBindValue(s.uid);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    execWrite(q, "addHistory");
    m_db.commit();
    emit changed();
}

QVector<Song> Library::history(int limit)
{
    QVector<Song> out;
    if (!m_ready) return out;
    QSqlQuery q(m_db);
    // 每首歌只留最近一次播放时间：history 表是每次播放都 INSERT 的，
    // 直接 JOIN 会让听过 N 次的歌在「最近播放」里出现 N 行（老库里也已经存了重复行）
    q.prepare(QStringLiteral("SELECT s.* FROM songs s JOIN "
                             "(SELECT song_uid, MAX(played) AS played FROM history GROUP BY song_uid) h "
                             "ON s.uid=h.song_uid ORDER BY h.played DESC LIMIT ?"));
    q.addBindValue(limit);
    if (q.exec()) while (q.next()) out << songFromQuery(q);
    return out;
}

void Library::addSearchHistory(const QString &keyword)
{
    if (!m_ready || keyword.trimmed().isEmpty()) return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO search_history (keyword,ts) VALUES (?,?)"));
    q.addBindValue(keyword.trimmed());
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    execWrite(q, "addSearchHistory");
}

QStringList Library::searchHistory()
{
    QStringList out;
    if (!m_ready) return out;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT keyword FROM search_history ORDER BY ts DESC LIMIT 20")))
        while (q.next()) out << q.value(0).toString();
    return out;
}

QVector<Account> Library::accounts()
{
    QVector<Account> out;
    if (!m_ready) return out;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT * FROM accounts"))) {
        while (q.next()) {
            Account a;
            a.source   = q.value(QStringLiteral("source")).toString();
            a.uid      = q.value(QStringLiteral("uid")).toString();
            a.nickname = q.value(QStringLiteral("nickname")).toString();
            a.avatar   = q.value(QStringLiteral("avatar")).toString();
            a.cookie   = deobfuscate(q.value(QStringLiteral("cookie")).toString());
            a.token    = deobfuscate(q.value(QStringLiteral("token")).toString());
            a.logged   = q.value(QStringLiteral("logged")).toBool();
            a.updated  = q.value(QStringLiteral("updated")).toLongLong();
            a.status   = q.value(QStringLiteral("status")).toString();
            out << a;
        }
    }
    return out;
}

void Library::saveAccount(const Account &a)
{
    if (!m_ready) return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO accounts "
                             "(source,uid,nickname,avatar,cookie,token,logged,updated,status) VALUES (?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(a.source);
    q.addBindValue(a.uid);
    q.addBindValue(a.nickname);
    q.addBindValue(a.avatar);
    q.addBindValue(obfuscate(a.cookie));
    q.addBindValue(obfuscate(a.token));
    q.addBindValue(a.logged ? 1 : 0);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(a.status);
    if (!q.exec()) qWarning() << "[FuruiMusic] saveAccount:" << q.lastError().text();
    emit changed();
}

void Library::removeAccount(const QString &source)
{
    if (!m_ready) return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM accounts WHERE source=?"));
    q.addBindValue(source);
    q.exec();
    emit changed();
}

QVariantList Library::customSources()
{
    QVariantList out;
    if (!m_ready) return out;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT * FROM custom_sources ORDER BY id"))) {
        while (q.next()) {
            QVariantMap m;
            m.insert(QStringLiteral("id"), q.value(QStringLiteral("id")).toString());
            m.insert(QStringLiteral("name"), q.value(QStringLiteral("name")).toString());
            m.insert(QStringLiteral("type"), q.value(QStringLiteral("type")).toString());
            m.insert(QStringLiteral("value"), q.value(QStringLiteral("value")).toString());
            m.insert(QStringLiteral("enabled"), q.value(QStringLiteral("enabled")).toBool());
            out << m;
        }
    }
    return out;
}

void Library::addCustomSource(const QVariantMap &item)
{
    if (!m_ready) return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO custom_sources (id,name,type,value,enabled) VALUES (?,?,?,?,?)"));
    q.addBindValue(item.value(QStringLiteral("id")).toString());
    q.addBindValue(item.value(QStringLiteral("name")).toString());
    q.addBindValue(item.value(QStringLiteral("type")).toString());
    q.addBindValue(item.value(QStringLiteral("value")).toString());
    q.addBindValue(item.value(QStringLiteral("enabled"), true).toBool() ? 1 : 0);
    q.exec();
    emit changed();
}

void Library::removeCustomSource(const QString &id)
{
    if (!m_ready) return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM custom_sources WHERE id=?"));
    q.addBindValue(id);
    q.exec();
    emit changed();
}

void Library::setCustomLocalDirectory(const QString &path)
{
    QVariantMap m;
    m.insert(QStringLiteral("id"), QStringLiteral("local"));
    m.insert(QStringLiteral("name"), QStringLiteral("本地音乐目录"));
    m.insert(QStringLiteral("type"), QStringLiteral("local"));
    m.insert(QStringLiteral("value"), path);
    addCustomSource(m);
}

QString Library::customLocalDirectory() const
{
    if (!m_ready) return QString();
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT value FROM custom_sources WHERE id=?"));
    q.addBindValue(QStringLiteral("local"));
    if (q.exec() && q.next()) return q.value(0).toString();
    return QString();
}

QVariantList Library::scriptSources()
{
    QVariantList out;
    if (!m_ready) return out;
    QSqlQuery q(m_db);
    if (q.exec(QStringLiteral("SELECT * FROM script_sources ORDER BY updated DESC"))) {
        while (q.next()) {
            QVariantMap m;
            m.insert(QStringLiteral("id"), q.value(QStringLiteral("id")).toString());
            m.insert(QStringLiteral("name"), q.value(QStringLiteral("name")).toString());
            m.insert(QStringLiteral("url"), q.value(QStringLiteral("url")).toString());
            m.insert(QStringLiteral("version"), q.value(QStringLiteral("version")).toString());
            m.insert(QStringLiteral("script"), q.value(QStringLiteral("script")).toString());
            m.insert(QStringLiteral("hash"), q.value(QStringLiteral("hash")).toString());
            m.insert(QStringLiteral("enabled"), q.value(QStringLiteral("enabled")).toBool());
            m.insert(QStringLiteral("status"), q.value(QStringLiteral("status")).toString());
            m.insert(QStringLiteral("errorType"), q.value(QStringLiteral("error_type")).toString());
            m.insert(QStringLiteral("errorMsg"), q.value(QStringLiteral("error_msg")).toString());
            m.insert(QStringLiteral("actions"), q.value(QStringLiteral("actions")).toString());
            m.insert(QStringLiteral("qualities"), q.value(QStringLiteral("qualities")).toString());
            m.insert(QStringLiteral("sources"), q.value(QStringLiteral("sources")).toString());
            m.insert(QStringLiteral("updated"), q.value(QStringLiteral("updated")).toLongLong());
            m.insert(QStringLiteral("checked"), q.value(QStringLiteral("checked")).toLongLong());
            out << m;
        }
    }
    return out;
}

void Library::saveScriptSource(const QVariantMap &item)
{
    if (!m_ready) return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO script_sources "
                             "(id,name,url,version,script,hash,enabled,status,error_type,error_msg,actions,qualities,sources,updated,checked) "
                             "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    q.addBindValue(item.value(QStringLiteral("id")).toString());
    q.addBindValue(item.value(QStringLiteral("name")).toString());
    q.addBindValue(item.value(QStringLiteral("url")).toString());
    q.addBindValue(item.value(QStringLiteral("version")).toString());
    q.addBindValue(item.value(QStringLiteral("script")).toString());
    q.addBindValue(item.value(QStringLiteral("hash")).toString());
    q.addBindValue(item.value(QStringLiteral("enabled"), true).toBool() ? 1 : 0);
    q.addBindValue(item.value(QStringLiteral("status")).toString());
    q.addBindValue(item.value(QStringLiteral("errorType")).toString());
    q.addBindValue(item.value(QStringLiteral("errorMsg")).toString());
    q.addBindValue(item.value(QStringLiteral("actions")).toString());
    q.addBindValue(item.value(QStringLiteral("qualities")).toString());
    q.addBindValue(item.value(QStringLiteral("sources")).toString());
    q.addBindValue(item.value(QStringLiteral("updated")).toLongLong());
    q.addBindValue(item.value(QStringLiteral("checked")).toLongLong());
    if (!q.exec()) qWarning() << "[FuruiMusic] saveScriptSource:" << q.lastError().text();
    emit changed();
}

void Library::removeScriptSource(const QString &id)
{
    if (!m_ready) return;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("DELETE FROM script_sources WHERE id=?"));
    q.addBindValue(id);
    q.exec();
    emit changed();
}


namespace {
// 本地账号凭据存储 v2：设备绑定密钥 + 每次写入随机 nonce + HMAC 完整性校验。
// 旧版是「固定字符串循环 XOR + base64」，任何人拿到数据库都能直接还原 Cookie，
// 且没有完整性校验（篡改不会被发现）。v2 仍不是对抗本机的强加密——密钥在本机，
// 目标是防止「数据库被拷走后离线批量还原」以及「随意篡改」，不是防调试器。
const char kStorePrefix[] = "FM2.";

QByteArray storageKey()
{
    static const QByteArray key = []() {
        QByteArray seed = furuiMachineUniqueId();
        if (seed.isEmpty()) {
            // 少数平台拿不到 machineUniqueId，退回到环境指纹（仍比硬编码常量强）
            seed = qgetenv("USERNAME");
            seed += ':';
            seed += qgetenv("COMPUTERNAME");
            seed += ':';
            seed += QSysInfo::productType().toUtf8();
        }
        seed += ":FuruiMusic:local-account-key:v2";
        return QCryptographicHash::hash(seed, QCryptographicHash::Sha256);
    }();
    return key;
}

QByteArray keyStream(const QByteArray &key, const QByteArray &nonce, int size)
{
    QByteArray out;
    out.reserve(size + 32);
    for (quint32 counter = 0; out.size() < size; ++counter) {
        QByteArray block = key + nonce;
        block.append(reinterpret_cast<const char *>(&counter), int(sizeof(counter)));
        out += QCryptographicHash::hash(block, QCryptographicHash::Sha256);
    }
    out.truncate(size);
    return out;
}

QByteArray hmacOf(const QByteArray &nonce, const QByteArray &cipher)
{
    QMessageAuthenticationCode mac(QCryptographicHash::Sha256, storageKey());
    mac.addData(nonce);
    mac.addData(cipher);
    return mac.result();
}

QByteArray randomNonce()
{
    QByteArray n(16, char(0));
    QRandomGenerator *rng = QRandomGenerator::system();
    for (int i = 0; i < 4; ++i) {
        const quint32 v = rng->generate();
        n[i * 4 + 0] = char(v & 0xff);
        n[i * 4 + 1] = char((v >> 8) & 0xff);
        n[i * 4 + 2] = char((v >> 16) & 0xff);
        n[i * 4 + 3] = char((v >> 24) & 0xff);
    }
    return n;
}

// 旧格式：仅用于读取历史数据库，读取成功后下次保存会自动升级为 v2
QString legacyDeobfuscate(const QString &stored)
{
    if (stored.isEmpty()) return QString();
    static const char key[] = "FuruiMusic-Local-Storage-Key";
    QByteArray d = QByteArray::fromBase64(stored.toLatin1());
    const int n = int(sizeof(key)) - 1;
    for (int i = 0; i < d.size(); ++i) d[i] = char(d.at(i) ^ key[i % n]);
    return QString::fromUtf8(d);
}

// 首次写凭据时自检：升级后绝不能让历史数据库里的 Cookie 读不出来
void credentialSelfTest()
{
    static bool done = false;
    if (done) return;
    done = true;
    static const char key[] = "FuruiMusic-Local-Storage-Key";
    const QString probeText = QStringLiteral("FuruiMusic-legacy-probe");
    QByteArray probe = probeText.toUtf8();
    const int n = int(sizeof(key)) - 1;
    for (int i = 0; i < probe.size(); ++i) probe[i] = char(probe.at(i) ^ key[i % n]);
    if (legacyDeobfuscate(QString::fromLatin1(probe.toBase64())) != probeText)
        qWarning() << "[FuruiMusic] 旧格式本地凭据解码自检失败（历史 Cookie 可能无法读取）";
}
} // namespace

QString Library::obfuscate(const QString &plain)
{
    if (plain.isEmpty()) return QString();
    credentialSelfTest();
    const QByteArray key = storageKey();
    const QByteArray nonce = randomNonce();
    QByteArray cipher = plain.toUtf8();
    const QByteArray stream = keyStream(key, nonce, cipher.size());
    for (int i = 0; i < cipher.size(); ++i)
        cipher[i] = char(cipher.at(i) ^ stream.at(i));
    const QString out = QString::fromLatin1(kStorePrefix)
        + QString::fromLatin1(nonce.toBase64())
        + QLatin1Char('.')
        + QString::fromLatin1(cipher.toBase64())
        + QLatin1Char('.')
        + QString::fromLatin1(hmacOf(nonce, cipher).toBase64());
    static bool roundTripChecked = false;
    if (!roundTripChecked) {
        roundTripChecked = true;
        if (deobfuscate(out) != plain)
            qWarning() << "[FuruiMusic] 本地凭据 v2 编解码自检失败（登录态可能无法保存）";
    }
    return out;
}

QString Library::deobfuscate(const QString &stored)
{
    if (stored.isEmpty()) return QString();
    if (!stored.startsWith(QLatin1String(kStorePrefix)))
        return legacyDeobfuscate(stored);

    const QList<QByteArray> parts = stored.toLatin1().split('.');
    if (parts.size() != 4) {
        qWarning() << "[FuruiMusic] 本地凭据格式损坏，请重新登录对应平台";
        return QString();
    }
    const QByteArray nonce = QByteArray::fromBase64(parts.at(1));
    const QByteArray cipher = QByteArray::fromBase64(parts.at(2));
    const QByteArray mac = QByteArray::fromBase64(parts.at(3));
    if (hmacOf(nonce, cipher) != mac) {
        // 常见原因：数据库被跨机器拷贝（设备密钥不同）或内容被改动，不能当明文用
        qWarning() << "[FuruiMusic] 本地凭据校验失败，请重新登录对应平台";
        return QString();
    }
    QByteArray plain = cipher;
    const QByteArray stream = keyStream(storageKey(), nonce, plain.size());
    for (int i = 0; i < plain.size(); ++i)
        plain[i] = char(plain.at(i) ^ stream.at(i));
    return QString::fromUtf8(plain);
}

} // namespace Furui
