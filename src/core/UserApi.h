#pragma once
// 福瑞音乐 - 账号 API：登录态验证 + 读取用户自己的资料/歌单/收藏
//
// 三套驱动：
//   netease : 内置真实实现。Cookie(MUSIC_U) + linuxapi(AES-128-ECB) + /api/linux/forward
//             转发到 /api/nuser/account/get、/api/user/playlist、/api/v3/playlist/detail。
//             算法与端点依据洛雪 wy/utils/crypto.js、wy/songList.js，并已实测返回业务 JSON。
//   direct  : 内置直连实现。用用户自己的 Cookie 按 sources.json 里的 userApi.endpoints
//             直接请求该平台官方接口（QQ 音乐已实现），不经过任何第三方服务。
//             平台私有的变量（如 g_tk / uin / musickey）由 vars() 从 Cookie 里解出。
//   http    : 用户自建的合规账号 API 服务。统一契约见 src/tools/account_api_server.py，
//             地址写进设置 accountApiBase（或 accountApiBase/<source>）。
//
// 未配置驱动时 explicit 报错，不会去请求一个不存在的 URL。
#include <QObject>
#include <QStringList>
#include <functional>

#include "HttpClient.h"
#include "SourceConfig.h"
#include "Types.h"

namespace Furui {

struct UserProfile {
    bool ok = false;
    // 传输失败/接口未配置：无法判定登录态，调用方不能因此清掉用户已保存的 Cookie
    bool networkError = false;
    // 该平台没有账号接口，Cookie 只能用于播放鉴权，应用内无法在线校验
    bool noAccountApi = false;
    QString uid;
    QString nickname;
    QString avatar;
    QString message;
};

struct UserLibraryData {
    bool ok = false;
    QString message;
    QVector<Playlist> playlists;
    QVector<Song> favorites;
    // 歌单 uid -> 该歌单的歌曲，同步时直接写入本地 SQLite
    QVector<QPair<QString, QVector<Song>>> playlistSongs;
    int failedPlaylists = 0;
};

class UserApi : public QObject {
    Q_OBJECT
public:
    static UserApi *instance();

    // 应用设置覆盖后的最终配置
    UserApiConfig config(const QString &sourceId) const;
    bool available(const QString &sourceId) const;
    // 给 UI 的如实状态描述
    QString statusOf(const QString &sourceId) const;
    // 真实可用的登录方式；二维码/手机号只在服务声明并实现端点后出现（Cookie 登录已移除）
    QStringList methods(const QString &sourceId) const;
    // 没有扫码方式时，给用户提供一句实话（为什么这个平台在应用内扫不了码）
    QString qrUnavailableReason(const QString &sourceId) const;
    // 该平台连密码/手机号都没有时，说清楚卡在哪（酷我 502、汽水原生签名等）
    QString loginUnavailableReason(const QString &sourceId) const;

    void verify(const QString &sourceId, const QString &cookie,
                const std::function<void(const UserProfile &)> &cb);
    void fetchLibrary(const QString &sourceId, const QString &cookie,
                      const std::function<void(const UserLibraryData &)> &cb);
    // 单个用户歌单的歌曲（点开歌单时的网络回退）
    void fetchPlaylistSongs(const QString &sourceId, const QString &cookie, const QString &id,
                            const std::function<void(const QVector<Song> &)> &cb);

private:
    explicit UserApi(QObject *parent = nullptr);
    Endpoint endpoint(const QString &sourceId, const QString &name) const;
    QVariantMap vars(const QString &sourceId, const QString &cookie) const;

    void call(const QString &sourceId, const QString &name, const QVariantMap &v,
              const HttpClient::Callback &cb);
    void callNetease(const UserApiConfig &cfg, const Endpoint &ep, const QVariantMap &v,
                     const HttpClient::Callback &cb);
    void callDirect(const QString &sourceId, const Endpoint &ep, const QVariantMap &v,
                    const HttpClient::Callback &cb);
    void callHttp(const UserApiConfig &cfg, const Endpoint &ep, const QVariantMap &v,
                  const HttpClient::Callback &cb);

    UserProfile parseProfile(const QString &sourceId, const QString &name,
                             const HttpClient::Reply &r) const;
    QVector<Playlist> parsePlaylists(const QString &sourceId, const QString &name,
                                     const HttpClient::Reply &r) const;
    QVector<Song> parseSongs(const QString &sourceId, const QString &name,
                             const HttpClient::Reply &r) const;
    QJsonValue businessRoot(const QString &sourceId, const QString &name,
                            const HttpClient::Reply &r, QString *error) const;
    // 歌单接口只回预览曲目时（网易云 playlist.tracks 实测 10 首），用端点配置的
    // fullIdsPath（playlist.trackIds）+ fillEndpoint（songDetail）按 id 批量补齐，
    // 输出顺序与歌单一致。maxSongs <= 0 表示不限制。
    void completePlaylistSongs(const QString &sourceId, const QString &cookie,
                               const QString &name, const HttpClient::Reply &r, int maxSongs,
                               const std::function<void(const QVector<Song> &)> &cb);
};

} // namespace Furui
