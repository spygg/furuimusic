#pragma once
// 福瑞音乐 - 平台描述配置
// 每个平台（网易云/QQ/酷狗/酷我/咪咕）都是一份 JSON 描述：
// 请求端点 + 字段映射 + VIP 判定 + 登录端点。C++ 侧只有一套通用适配器。
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

namespace Furui {

struct Endpoint {
    QString method = QStringLiteral("GET");
    QString path;
    QString contentType;          // form / json / 空
    QVariantMap params;           // GET query 或 POST form 模板
    QVariantMap jsonBody;         // POST json 模板
    QVariantMap headers;          // 额外 header 模板
    QString root;                 // 结果数组路径
    QVariantMap fields;           // 字段映射
    QStringList resultPaths;      // 单值结果候选路径
    QVariantMap vipRule;          // VIP / 不可播放判定
    QVariantMap extra;

    bool valid() const { return !path.isEmpty(); }
    static Endpoint fromVariant(const QVariant &v);
};

struct LoginConfig {
    QStringList methods;          // qrcode / cookie / phone
    Endpoint qrCreate;
    Endpoint qrCheck;
    Endpoint userInfo;
    Endpoint phoneSend;
    Endpoint phoneLogin;
    Endpoint playlists;
    Endpoint favorites;
    Endpoint playlistDetail;
    QVariantMap qr;               // keyPath / imagePath / statusPath / statusMap / successValues ...
    QVariantMap fields;           // userInfo/playlists/favorites 的字段映射
    static LoginConfig fromVariant(const QVariant &v);
};

// 账号 API：登录后读取用户自己的资料 / 歌单 / 收藏
// driver = "netease" 走内置真实实现（linuxapi 加密 + /api/linux/forward）
// driver = "direct"  走内置直连实现（用登录 Cookie 直接请求该平台官方接口，端点见 userApi.endpoints）
// driver = "http"    走用户自建的合规账号 API 服务（契约见 src/tools/account_api_server.py）
// driver 为空表示未配置，登录会给出明确提示而不是请求一个不存在的 URL
struct UserApiConfig {
    QString driver;
    QString base;
    QVariantMap headers;
    QHash<QString, Endpoint> endpoints;  // profile / playlists / favorites / playlistSongs / verify ...
    QStringList extraMethods;            // 服务额外支持的 qrcode / phone
    QVariantMap extra;                   // forwardUrl / favoritePlaylistName / 判定规则等
    bool configured() const { return !driver.isEmpty(); }
    static UserApiConfig fromVariant(const QVariant &v);
};

struct SourceDesc {
    QString id;
    QString name;
    QString color;
    QString base;
    bool enabled = true;
    QString driver;               // 可选：平台特定解析驱动（如 "qishui" 展平双层嵌套响应）
    QVariantMap headers;
    QHash<QString, Endpoint> endpoints;   // search / rank / playlistDetail / url / lyric
    LoginConfig login;
    UserApiConfig userApi;
    QVariantMap raw;
};

class SourceConfig : public QObject {
    Q_OBJECT
public:
    static SourceConfig *instance();

    QVector<SourceDesc> sources() const { return m_sources; }
    SourceDesc source(const QString &id) const;
    QStringList ids() const;
    QStringList names() const;
    QString nameOf(const QString &id) const;
    void reload();
    QString note() const { return m_note; }

signals:
    void changed();

private:
    explicit SourceConfig(QObject *parent = nullptr);
    QVector<SourceDesc> m_sources;
    QString m_note;
};

} // namespace Furui
