#pragma once
// 福瑞音乐 - 通用平台适配器
// 搜索 / 榜单 / 歌单详情 / 播放地址 / 歌词 / 登录后用户歌单与收藏，
// 全部由 sources.json 里的「端点 + 字段映射」驱动，不写死任何一家平台协议。
#include <QByteArray>
#include <QJsonValue>
#include <QObject>
#include <QUrl>
#include <QVariantMap>
#include <functional>

#include "HttpClient.h"
#include "SourceConfig.h"
#include "Types.h"

namespace Furui {

class MusicSource : public QObject {
    Q_OBJECT
public:
    using SongsCallback = std::function<void(const QVector<Song> &)>;
    using PlaylistsCallback = std::function<void(const QVector<Playlist> &)>;
    using PlayableCallback = std::function<void(const Playable &)>;
    using StringCallback = std::function<void(const QString &)>;
    using ReplyCallback = HttpClient::Callback;

    explicit MusicSource(const SourceDesc &desc, QObject *parent = nullptr);

    QString id() const { return m_desc.id; }
    QString name() const { return m_desc.name; }
    const SourceDesc &desc() const { return m_desc; }
    void setSession(const Account &a) { m_session = a; }
    Account session() const { return m_session; }

    void search(const QString &keyword, int limit, int offset, SongsCallback cb);
    void searchPlaylist(const QString &keyword, int limit, int offset, PlaylistsCallback cb);
    void rank(int limit, int offset, PlaylistsCallback cb);
    void playlistDetail(const QString &id, int limit, int offset, SongsCallback cb);
    // 榜单详情：配置了 rankDetail 端点的平台走专属接口（如 QQ musicu GetDetail、
    // 酷狗 v3/rank/song），未配置的平台回退 playlistDetail（榜单 id 兼容歌单 id）。
    void rankDetail(const QString &id, int limit, int offset, SongsCallback cb);
    void resolveUrl(const Song &song, const QString &quality, PlayableCallback cb);
    void lyric(const Song &song, StringCallback cb);
    // 搜索接口不返回封面时（网易云新版只给 album.picId），用配置里的
    // cover 端点按歌曲 id 单独补一张，避免播放页/播放栏一直是占位图。
    void cover(const Song &song, StringCallback cb);

    void userPlaylists(PlaylistsCallback cb);
    void userFavorites(SongsCallback cb);
    void userPlaylistDetail(const QString &id, SongsCallback cb);

    // 登录流程复用同一套请求构造器
    void call(const Endpoint &ep, const QVariantMap &vars, ReplyCallback cb);
    void callLogin(const QString &name, const QVariantMap &vars, ReplyCallback cb);
    QVariantMap baseVars() const;
    QVariantMap songVars(const Song &song, const QString &quality) const;

private:
    QUrl buildUrl(const Endpoint &ep, const QVariantMap &vars, QVariantMap *headers,
                  QByteArray *body) const;
    void requestSongs(const Endpoint &ep, const QVariantMap &vars, SongsCallback cb);
    void requestPlaylists(const Endpoint &ep, const QVariantMap &vars, const QString &type,
                          PlaylistsCallback cb);
    static bool isVipResponse(const QVariantMap &rule, const QJsonValue &root,
                              const QByteArray &body, const QString &resolvedUrl);
    QVariantMap playbackHeaders() const;

    // 汽水音乐：官方接口返回双层嵌套（result_groups[].data[].entity.track_wrapper.track），
    // 框架通用 JsonMapper 不展平子数组，故单独展平解析。
    void qishuiSearch(const QString &keyword, int limit, int offset, SongsCallback cb);
    void qishuiPlaylistDetail(const QString &id, int limit, int offset, SongsCallback cb);
    void qishuiSearchPlaylists(const QString &keyword, int limit, int offset, PlaylistsCallback cb);

    SourceDesc m_desc;
    Account m_session;
};

} // namespace Furui
