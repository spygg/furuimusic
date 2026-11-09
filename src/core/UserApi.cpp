#include "UserApi.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QUrlQuery>
#include <QVariantMap>
#include <memory>

#include "Aes128.h"
#include "JsonMapper.h"
#include "KugouProtocol.h"
#include "Library.h"
#include "MusicSource.h"
#include "NeteaseCrypto.h"
#include "QqProtocol.h"
#include "Settings.h"
#include "SourceConfig.h"
#include "SourceManager.h"

namespace Furui {

namespace {

QStringList listOf(const QVariant &v)
{
    QStringList out;
    for (const QVariant &x : v.toList()) out << x.toString();
    return out;
}

// http 驱动的默认账号 API 契约（与 tools/account_api_server.py 一致）
Endpoint contract(const QString &name)
{
    Endpoint e;
    e.method = QStringLiteral("GET");
    if (name == QLatin1String("verify")) {
        e.path = QStringLiteral("/user/verify");
        e.params = QVariantMap{{QStringLiteral("source"), QStringLiteral("{source}")},
                               {QStringLiteral("cookie"), QStringLiteral("{cookie}")}};
        e.root = QStringLiteral("profile");
        e.fields = QVariantMap{{QStringLiteral("uid"), QStringLiteral("uid")},
                               {QStringLiteral("nickname"), QStringLiteral("nickname")},
                               {QStringLiteral("avatar"), QStringLiteral("avatar")}};
    } else if (name == QLatin1String("profile")) {
        e.path = QStringLiteral("/user/profile");
        e.params = QVariantMap{{QStringLiteral("source"), QStringLiteral("{source}")},
                               {QStringLiteral("cookie"), QStringLiteral("{cookie}")}};
        e.root = QStringLiteral("profile");
        e.fields = QVariantMap{{QStringLiteral("uid"), QStringLiteral("uid")},
                               {QStringLiteral("nickname"), QStringLiteral("nickname")},
                               {QStringLiteral("avatar"), QStringLiteral("avatar")}};
    } else if (name == QLatin1String("playlists")) {
        e.path = QStringLiteral("/user/playlists");
        e.params = QVariantMap{{QStringLiteral("source"), QStringLiteral("{source}")},
                               {QStringLiteral("cookie"), QStringLiteral("{cookie}")},
                               {QStringLiteral("uid"), QStringLiteral("{uid}")},
                               {QStringLiteral("limit"), QStringLiteral("{limit}")},
                               {QStringLiteral("offset"), QStringLiteral("{offset}")}};
        e.root = QStringLiteral("playlists");
        e.fields = QVariantMap{{QStringLiteral("id"), QStringLiteral("id")},
                               {QStringLiteral("title"), QStringLiteral("title")},
                               {QStringLiteral("cover"), QStringLiteral("cover")},
                               {QStringLiteral("count"), QStringLiteral("count")},
                               {QStringLiteral("owner"), QStringLiteral("owner")}};
    } else if (name == QLatin1String("favorites")) {
        e.path = QStringLiteral("/user/favorites");
        e.params = QVariantMap{{QStringLiteral("source"), QStringLiteral("{source}")},
                               {QStringLiteral("cookie"), QStringLiteral("{cookie}")},
                               {QStringLiteral("uid"), QStringLiteral("{uid}")},
                               {QStringLiteral("limit"), QStringLiteral("{limit}")}};
        e.root = QStringLiteral("songs");
        e.fields = QVariantMap{{QStringLiteral("id"), QStringLiteral("id")},
                               {QStringLiteral("title"), QStringLiteral("title")},
                               {QStringLiteral("artist"), QStringLiteral("artist")},
                               {QStringLiteral("album"), QStringLiteral("album")},
                               {QStringLiteral("cover"), QStringLiteral("cover")},
                               {QStringLiteral("duration"), QStringLiteral("duration")},
                               {QStringLiteral("durationScale"), 1}};
    } else if (name == QLatin1String("playlistSongs")) {
        e.path = QStringLiteral("/user/playlistSongs");
        e.params = QVariantMap{{QStringLiteral("source"), QStringLiteral("{source}")},
                               {QStringLiteral("cookie"), QStringLiteral("{cookie}")},
                               {QStringLiteral("uid"), QStringLiteral("{uid}")},
                               {QStringLiteral("id"), QStringLiteral("{id}")},
                               {QStringLiteral("limit"), QStringLiteral("{limit}")}};
        e.root = QStringLiteral("songs");
        e.fields = QVariantMap{{QStringLiteral("id"), QStringLiteral("id")},
                               {QStringLiteral("title"), QStringLiteral("title")},
                               {QStringLiteral("artist"), QStringLiteral("artist")},
                               {QStringLiteral("album"), QStringLiteral("album")},
                               {QStringLiteral("cover"), QStringLiteral("cover")},
                               {QStringLiteral("duration"), QStringLiteral("duration")},
                               {QStringLiteral("durationScale"), 1}};
    }
    return e;
}

// 网易云内置端点（代码内置，配置文件只用于覆盖；避免 qrc/配置缺失时功能失效）
Endpoint neteaseContract(const QString &name)
{
    Endpoint e;
    e.method = QStringLiteral("POST");
    if (name == QLatin1String("profile")) {
        e.path = QStringLiteral("/api/nuser/account/get");
        e.root = QStringLiteral("profile");
        e.extra = QVariantMap{{QStringLiteral("forwardMethod"), QStringLiteral("GET")}};
        e.fields = QVariantMap{{QStringLiteral("uid"), QStringLiteral("userId")},
                               {QStringLiteral("nickname"), QStringLiteral("nickname")},
                               {QStringLiteral("avatar"), QStringLiteral("avatarUrl")}};
    } else if (name == QLatin1String("playlists")) {
        e.path = QStringLiteral("/api/user/playlist");
        e.params = QVariantMap{{QStringLiteral("uid"), QStringLiteral("{uid}")},
                               {QStringLiteral("limit"), QStringLiteral("{limit}")},
                               {QStringLiteral("offset"), QStringLiteral("{offset}")}};
        e.root = QStringLiteral("playlist");
        e.extra = QVariantMap{{QStringLiteral("forwardMethod"), QStringLiteral("POST")}};
        e.fields = QVariantMap{{QStringLiteral("id"), QStringLiteral("id")},
                               {QStringLiteral("title"), QStringLiteral("name")},
                               {QStringLiteral("cover"), QStringLiteral("coverImgUrl")},
                               {QStringLiteral("count"), QStringLiteral("trackCount")},
                               {QStringLiteral("owner"), QStringLiteral("creator.nickname")}};
    } else if (name == QLatin1String("playlistSongs") || name == QLatin1String("favorites")) {
        e.path = QStringLiteral("/api/v3/playlist/detail");
        e.params = QVariantMap{{QStringLiteral("id"), QStringLiteral("{id}")},
                               {QStringLiteral("n"), QStringLiteral("{limit}")},
                               {QStringLiteral("s"), QStringLiteral("8")}};
        e.root = QStringLiteral("playlist.tracks");
        e.extra = QVariantMap{{QStringLiteral("forwardMethod"), QStringLiteral("POST")}};
        e.fields = QVariantMap{{QStringLiteral("id"), QStringLiteral("id")},
                               {QStringLiteral("title"), QStringLiteral("name")},
                               {QStringLiteral("artist"), QStringLiteral("ar[].name")},
                               {QStringLiteral("album"), QStringLiteral("al.name")},
                               {QStringLiteral("cover"), QStringLiteral("al.picUrl")},
                               {QStringLiteral("duration"), QStringLiteral("dt")},
                               {QStringLiteral("vip"), QStringLiteral("fee")},
                               {QStringLiteral("vipValues"), QVariantList{1, 4, 8}},
                               {QStringLiteral("durationScale"), 1}};
    }
    return e;
}

HttpClient::Reply failure(const QString &message)
{
    HttpClient::Reply r;
    r.error = message;
    return r;
}

// 由 AuthManager 原生实现二维码登录的平台（新增时同步改 AuthManager::startQrLogin 的分支）
bool hasNativeQrLogin(const QString &sourceId)
{
    return sourceId == QLatin1String("qq") || sourceId == QLatin1String("kugou");
}

} // namespace

UserApi *UserApi::instance()
{
    static UserApi *api = new UserApi();
    return api;
}

UserApi::UserApi(QObject *parent) : QObject(parent)
{
    // 两套加密都在启动时自检：Aes128 供 linuxapi（歌单/收藏同步），
    // NeteaseCrypto 供 weapi（扫码/密码登录）。坏了要当场知道，而不是等用户
    // 扫码无反应时才发现。原先只有前者自检，weapi 从未验证。
    if (!Aes128::selfTest())
        qWarning("[FuruiMusic] AES-128 self test failed: netease linuxapi will not work");
    if (!Netease::selfTest())
        qWarning("[FuruiMusic] weapi self test failed: netease QR/password login will not work");
}

UserApiConfig UserApi::config(const QString &sourceId) const
{
    UserApiConfig c = SourceConfig::instance()->source(sourceId).userApi;
    // 网易云是内置实现，即使配置文件缺失也不应退化成外部服务
    if (sourceId == QLatin1String("netease") && c.driver.isEmpty())
        c.driver = QStringLiteral("netease");

    const QString perSource = Settings::string(QStringLiteral("accountApiBase/") + sourceId);
    const QString global = Settings::string(QStringLiteral("accountApiBase"));
    const QString base = perSource.isEmpty() ? global : perSource;
    const QString driverOverride = Settings::string(QStringLiteral("accountApiDriver/") + sourceId);

    if (!driverOverride.isEmpty()) c.driver = driverOverride;
    if (!base.isEmpty() && (c.driver.isEmpty() || c.driver == QLatin1String("http")
                            || driverOverride == QLatin1String("http"))) {
        c.base = base;
        c.driver = QStringLiteral("http");
    }
    return c;
}

bool UserApi::available(const QString &sourceId) const
{
    const UserApiConfig c = config(sourceId);
    if (c.driver == QLatin1String("netease")) return true;
    if (c.driver == QLatin1String("direct"))
        return !SourceConfig::instance()->source(sourceId).base.trimmed().isEmpty();
    if (c.driver == QLatin1String("http")) return !c.base.trimmed().isEmpty();
    return false;
}

QString UserApi::statusOf(const QString &sourceId) const
{
    const UserApiConfig c = config(sourceId);
    if (c.driver == QLatin1String("netease"))
        return QStringLiteral("内置网易云账号接口（Cookie + linuxapi 签名）");
    if (c.driver == QLatin1String("direct"))
        return QStringLiteral("内置直连账号接口（用登录 Cookie 直接请求 %1 官方接口）")
                   .arg(SourceConfig::instance()->nameOf(sourceId));
    if (c.driver == QLatin1String("http") && !c.base.trimmed().isEmpty())
        return QStringLiteral("账号 API 服务：") + c.base;
    // 有内置扫码的平台不该被说成「必须部署服务」，只是歌单同步还缺一套直连接口
    if (hasNativeQrLogin(sourceId))
        return QStringLiteral("未配置账号 API 服务：该平台可直接用内置二维码登录，登录后的歌单同步尚未内置。");
    // 酷我/咪咕/汽水这类只有 Cookie 的平台：让用户「改用网易云」没有意义。
    // 这里只讲账号接口，登录方式的解释交给 qrUnavailableReason，两段话不要互相重复
    return QStringLiteral("该平台未内置账号接口：歌单与收藏同步需要在设置里配置账号 API 服务。");
}

QStringList UserApi::methods(const QString &sourceId) const
{
    QStringList out;
    out << QStringLiteral("cookie");
    // 网易云内置 weapi 实现：扫码 + 密码（手机号/邮箱）登录均可用，无需外部服务
    const UserApiConfig c = config(sourceId);
    if (c.driver == QLatin1String("netease"))
        return out << QStringLiteral("qrcode") << QStringLiteral("password");
    const LoginConfig legacy = SourceConfig::instance()->source(sourceId).login;
    if (hasNativeQrLogin(sourceId) || c.driver == QLatin1String("direct")
        || c.extraMethods.contains(QStringLiteral("qrcode")) || legacy.qrCreate.valid())
        out << QStringLiteral("qrcode");
    if (c.extraMethods.contains(QStringLiteral("phone")) || legacy.phoneLogin.valid())
        out << QStringLiteral("phone");
    return out;
}

QString UserApi::qrUnavailableReason(const QString &sourceId) const
{
    if (methods(sourceId).contains(QStringLiteral("qrcode"))) return QString();
    // 2026-11-07 逐家实测线上接口后的结论：这三家当前拿不到「扫码后一定能登录」的证据，
    // 与其出一张扫了没反应的码，不如把卡在哪一步说清楚
    if (sourceId == QLatin1String("kuwo"))
        return QStringLiteral("酷我没有可用的扫码接口：网页端扫码已下线，passport.kuwo.cn 的扫码端点返回 "
                              "502，App 端扫码接口又要求先登录。请改用 Cookie 登录。");
    if (sourceId == QLatin1String("migu"))
        return QStringLiteral("咪咕的出码与轮询接口纯 HTTP 就能调通，但扫码成功后的令牌换取环节尚未实测通过，"
                              "贸然出码会出现「手机已确认、应用却登不上」。请改用 Cookie 登录。");
    if (sourceId == QLatin1String("qishui"))
        return QStringLiteral("汽水的出码接口可以调通（需带完整设备参数），但确认后的会话换取受设备风控约束"
                              "（命中风险会强制二次验证），登录闭环尚未实测通过。请改用 Cookie 登录。");
    return QStringLiteral("该平台未配置二维码登录接口，请改用 Cookie 登录，"
                          "或在设置里配置实现了扫码端点的账号 API 服务。");
}

QString UserApi::cookieLoginHint(const QString &sourceId) const
{
    // 桌面端与移动端都取这一句，避免两端各写一份、写歪了没人发现
    if (sourceId == QLatin1String("netease")) {
        return QStringLiteral(
            "获取 Cookie：① 浏览器打开 https://music.163.com 并登录；"
            "② 按 F12 → Network → 刷新页面 → 点任一请求 → 复制 Request Headers 里的整行 Cookie；"
            "③ 必须包含 MUSIC_U（网易云长期有效的登录凭据），__csrf 可选。"
            "粘贴后点「验证并同步」，验证通过会立即落库并拉取歌单/收藏。");
    }

    QString neededKeys;
    if (sourceId == QLatin1String("qq")) neededKeys = QStringLiteral("qqmusic_key 与 uin");
    else if (sourceId == QLatin1String("kugou")) neededKeys = QStringLiteral("token 与 userid");
    else if (sourceId == QLatin1String("kuwo")) neededKeys = QStringLiteral("kw_token 与 uid");

    QString text = QStringLiteral("从浏览器开发者工具复制已登录账号的完整 Cookie");
    if (!neededKeys.isEmpty())
        text += QStringLiteral("（该平台需要 %1）").arg(neededKeys);
    text += available(sourceId)
            ? QStringLiteral("。验证通过后会自动同步该账号的歌单与收藏。")
            : QStringLiteral("。该平台尚未内置歌单同步接口，登录后只用于播放鉴权。");
    return text;
}

Endpoint UserApi::endpoint(const QString &sourceId, const QString &name) const
{
    const UserApiConfig c = config(sourceId);
    const Endpoint configured = c.endpoints.value(name);
    if (configured.valid()) return configured;
    if (c.driver == QLatin1String("netease")) return neteaseContract(name);
    if (c.driver == QLatin1String("http")) return contract(name);
    return Endpoint();
}

QVariantMap UserApi::vars(const QString &sourceId, const QString &cookie) const
{
    QVariantMap v;
    v.insert(QStringLiteral("source"), sourceId);
    v.insert(QStringLiteral("cookie"), cookie);
    const QVector<Account> accounts = Library::instance()->accounts();
    for (const Account &a : accounts) {
        if (a.source == sourceId) {
            v.insert(QStringLiteral("uid"), a.uid);
            v.insert(QStringLiteral("token"), a.token);
            break;
        }
    }
    v.insert(QStringLiteral("limit"), QString::number(Settings::integer(QStringLiteral("pageSize"), 30) * 3));
    v.insert(QStringLiteral("offset"), QStringLiteral("0"));
    v.insert(QStringLiteral("id"), QString());
    v.insert(QStringLiteral("favoriteId"), QString());
    if (config(sourceId).driver == QLatin1String("direct") && sourceId == QLatin1String("qq")) {
        // QQ 官方接口要求这三个参数从登录 Cookie 里现算：g_tk=hash33(登录 key)，
        // hostuin/musickey 分别是数字号与 qqmusic_key（Cookie 里没有时退回已保存的 uid）
        QString uin = cookieValue(cookie, {QStringLiteral("uin"), QStringLiteral("musicid")});
        if (uin.isEmpty()) uin = v.value(QStringLiteral("uid")).toString();
        v.insert(QStringLiteral("uin"), Qq::numericUin(uin));
        v.insert(QStringLiteral("musickey"), cookieValue(cookie, {QStringLiteral("qqmusic_key"),
                                                                 QStringLiteral("qm_keyst")}));
        v.insert(QStringLiteral("gtk"), Qq::gTk(cookie));
    }
    if (sourceId == QLatin1String("kugou")) {
        // 设备公共参数与登录态两项（kgToken/kgUserid）都从扫码存下的 Cookie 摊出来；
        // 网关接口不认 Cookie 头，这两个值是当参数送并进签名的
        Kugou::addDeviceVars(&v);
        Kugou::addAuthVars(&v, cookie);
    }
    if (sourceId == QLatin1String("kuwo")) {
        // 酷我 www 接口把 csrf 当校验头。开放源码里它取自一个固定的 Hm_Iuvt_* 会话 Cookie
        // （形如 7 位数字），与 kw_token 尾段哪种写法才是本机的正确值没有证据可验证：
        // www.kuwo.cn/api/www 的用户数据接口对非浏览器指纹一律回 "The request is illegal!"，
        // 游客态拿不到可比对的响应。这里按 kw_token 尾段填，拿不到就留空由适配器丢头。
        const QString kwToken = cookieValue(cookie, {QStringLiteral("kw_token")});
        if (!kwToken.isEmpty())
            v.insert(QStringLiteral("csrf"), kwToken.section(QLatin1Char('_'), -1));
        const QString uid = cookieValue(cookie, {QStringLiteral("uid")});
        if (!uid.isEmpty()) v.insert(QStringLiteral("uid"), uid);
    }
    return v;
}

void UserApi::call(const QString &sourceId, const QString &name, const QVariantMap &v,
                   const HttpClient::Callback &cb)
{
    const UserApiConfig c = config(sourceId);
    if (!available(sourceId)) {
        if (cb) cb(failure(statusOf(sourceId)));
        return;
    }
    const Endpoint ep = endpoint(sourceId, name);
    if (!ep.valid()) {
        if (cb) cb(failure(QStringLiteral("该平台的账号 API 未实现「%1」接口").arg(name)));
        return;
    }
    if (c.driver == QLatin1String("netease")) callNetease(c, ep, v, cb);
    else if (c.driver == QLatin1String("direct")) callDirect(sourceId, ep, v, cb);
    else callHttp(c, ep, v, cb);
}

// 直连驱动复用平台适配器：base/公共请求头（含 Cookie 模板）与端点渲染都由 MusicSource 负责。
// 变量表以平台适配器的公共变量为底，再用本模块算出的账号变量覆盖，
// 这样直连端点既能用 {cookie}/{limit}，也能用 {page}/{quality} 这些原有占位符。
void UserApi::callDirect(const QString &sourceId, const Endpoint &ep,
                         const QVariantMap &v, const HttpClient::Callback &cb)
{
    MusicSource *s = SourceManager::instance()->source(sourceId);
    MusicSource fallback(SourceConfig::instance()->source(sourceId));
    MusicSource *target = s ? s : &fallback;
    QVariantMap merged = target->baseVars();
    for (auto it = v.constBegin(); it != v.constEnd(); ++it) merged.insert(it.key(), it.value());
    target->call(ep, merged, cb);
}

void UserApi::callNetease(const UserApiConfig &cfg, const Endpoint &ep,
                          const QVariantMap &v, const HttpClient::Callback &cb)
{
    const QString forwardBase = cfg.extra.value(QStringLiteral("forwardBase"))
                                    .toString().isEmpty()
        ? QStringLiteral("https://music.163.com")
        : cfg.extra.value(QStringLiteral("forwardBase")).toString();
    const QString forwardUrl = cfg.extra.value(QStringLiteral("forwardUrl"))
                                   .toString().isEmpty()
        ? QStringLiteral("https://music.163.com/api/linux/forward")
        : cfg.extra.value(QStringLiteral("forwardUrl")).toString();

    QJsonObject payload;
    payload.insert(QStringLiteral("method"),
                   ep.extra.value(QStringLiteral("forwardMethod"), QStringLiteral("POST")).toString());
    payload.insert(QStringLiteral("url"), forwardBase + Json::render(ep.path, v));
    payload.insert(QStringLiteral("params"),
                   QJsonObject::fromVariantMap(Json::renderMap(ep.params, v)));

    QVariantMap headers = Json::renderMap(cfg.headers, v);
    const QVariantMap extraHeaders = Json::renderMap(ep.headers, v);
    for (auto it = extraHeaders.constBegin(); it != extraHeaders.constEnd(); ++it)
        headers.insert(it.key(), it.value());
    headers.insert(QStringLiteral("Cookie"), v.value(QStringLiteral("cookie")).toString());
    headers.insert(QStringLiteral("Content-Type"), QStringLiteral("application/x-www-form-urlencoded"));
    if (!headers.contains(QStringLiteral("User-Agent")))
        headers.insert(QStringLiteral("User-Agent"),
            QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                           "(KHTML, like Gecko) Chrome/60.0.3112.90 Safari/537.36"));
    if (!headers.contains(QStringLiteral("Referer")))
        headers.insert(QStringLiteral("Referer"), QStringLiteral("https://music.163.com/"));
    for (auto it = headers.begin(); it != headers.end();) {
        if (it.value().toString().trimmed().isEmpty()) it = headers.erase(it);
        else ++it;
    }

    HttpClient::instance()->request(QStringLiteral("POST"), QUrl(forwardUrl), headers,
                                    Aes128::linuxapiForm(payload), cb);
}

void UserApi::callHttp(const UserApiConfig &cfg, const Endpoint &ep,
                       const QVariantMap &v, const HttpClient::Callback &cb)
{
    if (cfg.base.trimmed().isEmpty()) {
        cb(failure(QStringLiteral("账号 API 服务地址为空")));
        return;
    }
    const QString path = Json::render(ep.path, v);
    QUrl url(path.startsWith(QLatin1String("http"), Qt::CaseInsensitive)
                 ? path : cfg.base + path);
    QVariantMap headers = Json::renderMap(cfg.headers, v);
    const QVariantMap extraHeaders = Json::renderMap(ep.headers, v);
    for (auto it = extraHeaders.constBegin(); it != extraHeaders.constEnd(); ++it)
        headers.insert(it.key(), it.value());
    headers.insert(QStringLiteral("X-Account-Cookie"), v.value(QStringLiteral("cookie")).toString());
    for (auto it = headers.begin(); it != headers.end();) {
        if (it.value().toString().trimmed().isEmpty()) it = headers.erase(it);
        else ++it;
    }

    const QVariantMap params = Json::renderMap(ep.params, v);
    const bool isPost = (ep.method.toUpper() == QLatin1String("POST"));
    QUrlQuery query(url);
    QByteArray body;
    if (isPost && ep.contentType == QLatin1String("json")) {
        body = QJsonDocument(QJsonObject::fromVariantMap(Json::renderMap(ep.jsonBody, v)))
                   .toJson(QJsonDocument::Compact);
        headers.insert(QStringLiteral("Content-Type"), QStringLiteral("application/json"));
    } else if (isPost && ep.contentType == QLatin1String("form")) {
        QUrlQuery form;
        for (auto it = params.constBegin(); it != params.constEnd(); ++it)
            form.addQueryItem(it.key(), it.value().toString());
        body = form.toString(QUrl::FullyEncoded).toUtf8();
        headers.insert(QStringLiteral("Content-Type"),
                       QStringLiteral("application/x-www-form-urlencoded"));
    }
    for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
        if (isPost && ep.contentType == QLatin1String("form")) continue;
        query.addQueryItem(it.key(), it.value().toString());
    }
    url.setQuery(query);
    HttpClient::instance()->request(ep.method, url, headers, body, cb);
}

QJsonValue UserApi::businessRoot(const QString &sourceId, const QString &name,
                                 const HttpClient::Reply &r, QString *error) const
{
    if (!r.ok) {
        if (error) *error = r.error.isEmpty()
            ? QStringLiteral("请求失败（%1）").arg(name) : r.error;
        return QJsonValue();
    }
    const QJsonValue root = Json::rootOf(r.json);
    const QString code = Json::stringAt(root, QStringLiteral("code"));
    if (!code.isEmpty() && code != QLatin1String("200") && code != QLatin1String("0")) {
        if (error) *error = QStringLiteral("%1：%2")
                                .arg(SourceConfig::instance()->nameOf(sourceId),
                                     Json::firstString(root, {QStringLiteral("message"),
                                                              QStringLiteral("msg")}));
        return QJsonValue();
    }
    if (Json::stringAt(root, QStringLiteral("ok")) == QLatin1String("false")) {
        if (error) *error = Json::firstString(root, {QStringLiteral("error"),
                                                     QStringLiteral("message")});
        return QJsonValue();
    }
    // 部分平台外层 code 恒为 0，真实成败写在业务包装里（QQ 的 musicu.fcg 是 req.code）
    const Endpoint ep = endpoint(sourceId, name);
    const QString codePath = ep.extra.value(QStringLiteral("codePath")).toString();
    if (!codePath.isEmpty()) {
        const QString inner = Json::stringAt(root, codePath);
        QStringList ok = listOf(ep.extra.value(QStringLiteral("successValues")));
        if (ok.isEmpty()) ok << QStringLiteral("0") << QStringLiteral("200");
        if (!inner.isEmpty() && !ok.contains(inner)) {
            // 平台把「登录态失效」编成自己的错误码（酷狗的 20010/20017），
            // 这类码要直接告诉用户该重新登录，而不是抛一个看不懂的数字
            const QStringList authCodes = listOf(ep.extra.value(QStringLiteral("authCodes")));
            if (authCodes.contains(inner)) {
                if (error) *error = SourceConfig::instance()->nameOf(sourceId)
                    + QStringLiteral("：登录态无效或已过期，请重新扫码登录");
                return QJsonValue();
            }
            QString msg = Json::firstString(root, {
                ep.extra.value(QStringLiteral("messagePath")).toString(),
                QStringLiteral("message"), QStringLiteral("msg")});
            if (msg.isEmpty()) msg = QStringLiteral("接口返回错误码 %1").arg(inner);
            if (error) *error = SourceConfig::instance()->nameOf(sourceId) + QStringLiteral("：") + msg;
            return QJsonValue();
        }
    }
    return root;
}

UserProfile UserApi::parseProfile(const QString &sourceId, const QString &name,
                                  const HttpClient::Reply &r) const
{
    UserProfile p;
    QString error;
    const QJsonValue root = businessRoot(sourceId, name, r, &error);
    if (!root.isObject() && !root.isArray()) { p.message = error; return p; }
    const Endpoint ep = endpoint(sourceId, name);
    const QJsonValue obj = ep.root.isEmpty() ? root : Json::valueAt(root, ep.root);
    p.uid = Json::firstString(obj, {ep.fields.value(QStringLiteral("uid")).toString(),
                                    QStringLiteral("uid"), QStringLiteral("userId")});
    p.nickname = Json::firstString(obj, {ep.fields.value(QStringLiteral("nickname")).toString(),
                                         QStringLiteral("nickname"), QStringLiteral("nick")});
    p.avatar = Json::firstString(obj, {ep.fields.value(QStringLiteral("avatar")).toString(),
                                       QStringLiteral("avatar"), QStringLiteral("avatarUrl")});
    p.ok = !p.uid.isEmpty() || !p.nickname.isEmpty();
    if (!p.ok) p.message = QStringLiteral("登录态无效或 Cookie 已过期，请重新从浏览器复制完整 Cookie");
    return p;
}

QVector<Playlist> UserApi::parsePlaylists(const QString &sourceId, const QString &name,
                                          const HttpClient::Reply &r) const
{
    QVector<Playlist> out;
    QString error;
    const QJsonValue root = businessRoot(sourceId, name, r, &error);
    if (root.isUndefined() || root.isNull()) return out;
    const Endpoint ep = endpoint(sourceId, name);
    const QJsonArray arr = Json::arrayAt(root, ep.root);
    for (const QJsonValue &v : arr) {
        Playlist p = Json::mapPlaylist(v, ep.fields, sourceId, QStringLiteral("user"));
        if (!p.title.isEmpty()) out << p;
    }
    return out;
}

QVector<Song> UserApi::parseSongs(const QString &sourceId, const QString &name,
                                  const HttpClient::Reply &r) const
{
    QVector<Song> out;
    QString error;
    const QJsonValue root = businessRoot(sourceId, name, r, &error);
    if (root.isUndefined() || root.isNull()) return out;
    const Endpoint ep = endpoint(sourceId, name);
    const QJsonArray arr = Json::arrayAt(root, ep.root);
    for (const QJsonValue &v : arr) {
        Song s = Json::mapSong(v, ep.fields, sourceId);
        if (!s.title.isEmpty()) out << s;
    }
    return out;
}

void UserApi::verify(const QString &sourceId, const QString &cookie,
                     const std::function<void(const UserProfile &)> &cb)
{
    if (cookie.trimmed().isEmpty()) {
        UserProfile p;
        p.message = QStringLiteral("Cookie 不能为空");
        if (cb) cb(p);
        return;
    }
    const QVariantMap v = vars(sourceId, cookie.trimmed());
    // 没有账号接口的平台（酷我/咪咕/汽水）只能把 Cookie 交给播放鉴权用，
    // 应用内没有任何办法验证它，所以直说「未验证」，别让用户以为登录成功了
    if (!available(sourceId)) {
        UserProfile p;
        p.noAccountApi = true;
        p.message = QStringLiteral("Cookie 已保存：该平台未内置账号接口，"
                                   "登录态只用于播放鉴权，应用内未做在线验证");
        if (cb) cb(p);
        return;
    }
    // 没有可用的用户资料端点时（酷狗的用户信息接口要 RSA，酷我 Web 端没有开放资料接口），
    // 改用「服务端是否接受了这套 Cookie」来判断登录态：能正常拉到自己的歌单列表即视为有效。
    if (!endpoint(sourceId, QStringLiteral("profile")).valid()
        && endpoint(sourceId, QStringLiteral("playlists")).valid()) {
        call(sourceId, QStringLiteral("playlists"), v,
            [this, sourceId, v, cb](const HttpClient::Reply &r) {
                QString error;
                const QJsonValue root = businessRoot(sourceId, QStringLiteral("playlists"), r, &error);
                UserProfile p;
                p.uid = v.value(QStringLiteral("uid")).toString();
                const QVector<Playlist> lists = parsePlaylists(sourceId, QStringLiteral("playlists"), r);
                p.ok = !root.isUndefined() && !root.isNull();
                p.networkError = !r.ok;      // 拿不到回复不等于登录态作废
                if (p.ok)
                    p.message = lists.isEmpty() ? QStringLiteral("登录态有效：未读到歌单")
                                                 : QStringLiteral("登录态有效：歌单 %1 个")
                                                       .arg(lists.size());
                else
                    p.message = error.isEmpty() ? QStringLiteral("登录态无效，请重新从浏览器复制完整 Cookie")
                                                : error;
                if (cb) cb(p);
            });
        return;
    }
    call(sourceId, QStringLiteral("profile"), v,
        [this, sourceId, cb](const HttpClient::Reply &r) {
            UserProfile p = parseProfile(sourceId, QStringLiteral("profile"), r);
            p.networkError = !r.ok;          // 网络失败不能当成「登录态无效」
            if (p.ok) {
                p.message = QStringLiteral("登录态有效：%1")
                                .arg(p.nickname.isEmpty() ? p.uid : p.nickname);
            }
            if (cb) cb(p);
        });
}

void UserApi::fetchPlaylistSongs(const QString &sourceId, const QString &cookie,
                                const QString &id,
                                const std::function<void(const QVector<Song> &)> &cb)
{
    QVariantMap v = vars(sourceId, cookie);
    v.insert(QStringLiteral("id"), id);
    call(sourceId, QStringLiteral("playlistSongs"), v,
        [this, sourceId, cb](const HttpClient::Reply &r) {
            const QVector<Song> songs = parseSongs(sourceId, QStringLiteral("playlistSongs"), r);
            if (cb) cb(songs);
        });
}

void UserApi::fetchLibrary(const QString &sourceId, const QString &cookie,
                           const std::function<void(const UserLibraryData &)> &cb)
{
    auto out = std::make_shared<UserLibraryData>();
    auto finalize = [out, sourceId]() {
        const int songLists = out->playlistSongs.size();
        int songs = 0;
        for (const auto &pair : out->playlistSongs) songs += pair.second.size();
        out->ok = !out->playlists.isEmpty() || !out->favorites.isEmpty();
        const QString counts =
            QStringLiteral("%1：歌单 %2 个，收藏 %3 首，已拉取 %4 个歌单共 %5 首")
                .arg(SourceConfig::instance()->nameOf(sourceId))
                .arg(out->playlists.size()).arg(out->favorites.size())
                .arg(songLists).arg(songs);
        if (out->ok) {
            out->message = counts;
            return;
        }
        // 服务端已经说明原因（签名不过、登录态失效等）时保留它，
        // 泛泛的「未读到数据」会把用户该做的事（重新扫码）藏起来。
        if (out->message.isEmpty())
            out->message = counts + QStringLiteral("；未读到数据，请检查登录态与账号接口配置");
    };

    const QVariantMap base = vars(sourceId, cookie.trimmed());
    call(sourceId, QStringLiteral("profile"), base,
        [this, sourceId, base, out, finalize, cb](const HttpClient::Reply &pr) {
            QVariantMap v = base;
            const UserProfile p = parseProfile(sourceId, QStringLiteral("profile"), pr);
            if (p.ok) v.insert(QStringLiteral("uid"), p.uid);
            else out->message = p.message;

            call(sourceId, QStringLiteral("playlists"), v,
                [this, sourceId, v, out, finalize, cb](const HttpClient::Reply &lr) {
                    QString error;
                    const QJsonValue root = businessRoot(sourceId, QStringLiteral("playlists"), lr, &error);
                    if (root.isUndefined() || root.isNull()) {
                        out->message = error.isEmpty() ? QStringLiteral("歌单列表请求失败") : error;
                    } else {
                        // 歌单接口本身是通的，之前 profile 端点的提示不再相关
                        out->message.clear();
                    }
                    out->playlists = parsePlaylists(sourceId, QStringLiteral("playlists"), lr);

                    // 网易云的「我喜欢的音乐」是一个普通歌单，从歌单列表里识别
                    const UserApiConfig cfg = config(sourceId);
                    QString favoriteId;
                    const QString favoriteName = cfg.extra.value(QStringLiteral("favoritePlaylistName"))
                                                     .toString();
                    for (const Playlist &pl : out->playlists) {
                        if ((!favoriteName.isEmpty() && pl.title.contains(favoriteName))
                            || pl.title.contains(QStringLiteral("喜欢的音乐"))) {
                            favoriteId = pl.sourceId;
                            break;
                        }
                    }

                    // 收集阶段 C 的所有并行任务
                    // 平台没配「歌单内歌曲」端点时（酷狗目前只有歌单列表）不必逐个空跑，
                    // 否则每个歌单都会撞一次「未配置此接口」，还把失败数写进提示里
                    const bool listSongsKnown =
                        endpoint(sourceId, QStringLiteral("playlistSongs")).valid();
                    struct Task { QString name; QVariantMap v; QString uid; bool favorites; };
                    QVector<Task> tasks;
                    if (cfg.driver == QLatin1String("http") &&
                        endpoint(sourceId, QStringLiteral("favorites")).valid() &&
                        cfg.endpoints.contains(QStringLiteral("favorites"))) {
                        QVariantMap fv = v;
                        tasks.append({QStringLiteral("favorites"), fv, QString(), true});
                    } else if (listSongsKnown && !favoriteId.isEmpty()) {
                        QVariantMap fv = v;
                        fv.insert(QStringLiteral("id"), favoriteId);
                        tasks.append({QStringLiteral("playlistSongs"), fv, QString(), true});
                    }
                    const int maxLists = qMax(1, Settings::integer(QStringLiteral("syncPlaylistLimit"), 12));
                    for (int i = 0; listSongsKnown && i < qMin(out->playlists.size(), maxLists); ++i) {
                        QVariantMap pv = v;
                        pv.insert(QStringLiteral("id"), out->playlists.at(i).sourceId);
                        tasks.append({QStringLiteral("playlistSongs"), pv,
                                      out->playlists.at(i).uid, false});
                    }

                    if (tasks.isEmpty()) {
                        finalize();
                        if (cb) cb(*out);
                        return;
                    }
                    auto remaining = std::make_shared<int>(tasks.size());
                    auto oneDone = [remaining, out, finalize, cb]() {
                        if (--(*remaining) > 0) return;
                        finalize();
                        if (cb) cb(*out);
                    };
                    for (const Task &t : tasks) {
                        call(sourceId, t.name, t.v,
                            [this, sourceId, t, out, oneDone](const HttpClient::Reply &r) {
                                const QVector<Song> songs = parseSongs(sourceId, t.name, r);
                                if (t.favorites) out->favorites = songs;
                                else {
                                    if (songs.isEmpty()) ++out->failedPlaylists;
                                    out->playlistSongs.append(qMakePair(t.uid, songs));
                                }
                                oneDone();
                            });
                    }
                });
        });
}

} // namespace Furui
