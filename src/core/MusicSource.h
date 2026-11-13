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
    using MvCallback = std::function<void(bool ok, const QUrl &url, const QString &error)>;
    // MV 榜单：扁平化的卡片数据（id/title/artist/cover/playCount/duration/source）
    using MvListCallback = std::function<void(const QVector<QVariantMap> &items, bool hasMore)>;
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
    // 把配置里的静态默认榜单（defaultBoards）转成 Playlist 列表；无 rank 端点或
    // rank 拉取失败/被反爬时回退用，保证未登录也能按平台展示默认歌单（洛雪式）。
    QVector<Playlist> defaultBoards() const;
    void playlistDetail(const QString &id, int limit, int offset, SongsCallback cb);
    // 榜单详情：配置了 rankDetail 端点的平台走专属接口（如 QQ musicu GetDetail、
    // 酷狗 v3/rank/song），未配置的平台回退 playlistDetail（榜单 id 兼容歌单 id）。
    void rankDetail(const QString &id, int limit, int offset, SongsCallback cb);
    void resolveUrl(const Song &song, const QString &quality, PlayableCallback cb);
    void lyric(const Song &song, StringCallback cb);
    // 搜索接口不返回封面时（网易云新版只给 album.picId），用配置里的
    // cover 端点按歌曲 id 单独补一张，避免播放页/播放栏一直是占位图。
    void cover(const Song &song, StringCallback cb);
    // 解析歌曲的 MV 播放地址：mvid 优先取搜索结果透传的原始字段（Song.extra），
    // 没有再用 mvInfo 端点查歌曲详情；取址失败退低一档清晰度再试一次。
    void mv(const Song &song, MvCallback cb);
    // 公开 MV 榜单（无需登录），按地区分页；字段映射完全由 mvList 端点描述。
    void mvList(const QString &area, int limit, int offset, MvListCallback cb);

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
    // 按 pageLimit 分页补齐：单页上限小于要的数量时循环请求，避免「平台把 rn 砍到 30，
    // 我们一次性要 150 得到 code:-1 / 0 首」。
    void requestSongsPaged(const Endpoint &ep, const QVariantMap &vars, int limit, int offset,
                           SongsCallback cb);
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
    // 汽水推荐/发现歌单：POST /luna/discover/mix，免登录，结构 inner_block[].resources[].entity.playlist
    void qishuiRank(int limit, int offset, PlaylistsCallback cb);

    SourceDesc m_desc;
    Account m_session;
};

} // namespace Furui
