#include "SourceConfig.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <QDebug>

#include "Settings.h"

// 必须在全局作用域调用：qrc 生成的 qInitResources_config() 位于全局命名空间。
// 放在静态库里时，这个引用同时保证链接器不会丢弃 qrc_config.o。
static void furuiInitConfigResource()
{
    Q_INIT_RESOURCE(config);
}

namespace Furui {

static QStringList toStringList(const QVariant &v)
{
    QStringList out;
    for (const QVariant &x : v.toList()) out << x.toString();
    return out;
}

Endpoint Endpoint::fromVariant(const QVariant &v)
{
    Endpoint e;
    const QVariantMap m = v.toMap();
    if (m.isEmpty()) return e;
    e.method      = m.value(QStringLiteral("method"), QStringLiteral("GET")).toString();
    e.path        = m.value(QStringLiteral("path")).toString();
    e.contentType = m.value(QStringLiteral("contentType")).toString();
    e.params      = m.value(QStringLiteral("params")).toMap();
    e.jsonBody    = m.value(QStringLiteral("jsonBody")).toMap();
    e.headers     = m.value(QStringLiteral("headers")).toMap();
    e.root        = m.value(QStringLiteral("root")).toString();
    e.fields      = m.value(QStringLiteral("fields")).toMap();
    e.resultPaths = toStringList(m.value(QStringLiteral("resultPaths")));
    const QString single = m.value(QStringLiteral("resultPath")).toString();
    if (!single.isEmpty() && !e.resultPaths.contains(single)) e.resultPaths.prepend(single);
    e.vipRule     = m.value(QStringLiteral("vipRule")).toMap();
    e.extra       = m.value(QStringLiteral("extra")).toMap();
    return e;
}

LoginConfig LoginConfig::fromVariant(const QVariant &v)
{
    LoginConfig l;
    const QVariantMap m = v.toMap();
    if (m.isEmpty()) return l;
    l.methods         = toStringList(m.value(QStringLiteral("methods")));
    l.qrCreate        = Endpoint::fromVariant(m.value(QStringLiteral("qrCreate")));
    l.qrCheck         = Endpoint::fromVariant(m.value(QStringLiteral("qrCheck")));
    l.userInfo        = Endpoint::fromVariant(m.value(QStringLiteral("userInfo")));
    l.phoneSend       = Endpoint::fromVariant(m.value(QStringLiteral("phoneSend")));
    l.phoneLogin      = Endpoint::fromVariant(m.value(QStringLiteral("phoneLogin")));
    l.playlists       = Endpoint::fromVariant(m.value(QStringLiteral("playlists")));
    l.favorites       = Endpoint::fromVariant(m.value(QStringLiteral("favorites")));
    l.playlistDetail  = Endpoint::fromVariant(m.value(QStringLiteral("playlistDetail")));
    l.qr              = m.value(QStringLiteral("qr")).toMap();
    l.fields          = m.value(QStringLiteral("fields")).toMap();
    return l;
}

UserApiConfig UserApiConfig::fromVariant(const QVariant &v)
{
    UserApiConfig u;
    const QVariantMap m = v.toMap();
    if (m.isEmpty()) return u;
    u.driver       = m.value(QStringLiteral("driver")).toString();
    u.base         = m.value(QStringLiteral("base")).toString();
    u.headers      = m.value(QStringLiteral("headers")).toMap();
    u.extra        = m.value(QStringLiteral("extra")).toMap();
    u.extraMethods = toStringList(m.value(QStringLiteral("extraMethods")));
    const QVariantMap eps = m.value(QStringLiteral("endpoints")).toMap();
    for (auto it = eps.constBegin(); it != eps.constEnd(); ++it)
        u.endpoints.insert(it.key(), Endpoint::fromVariant(it.value()));
    return u;
}

SourceConfig *SourceConfig::instance()
{
    static SourceConfig *c = new SourceConfig();
    return c;
}

SourceConfig::SourceConfig(QObject *parent)
    : QObject(parent)
{
    reload();
}

static QByteArray readJsonFile(const QString &path)
{
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return QByteArray();
    const QByteArray data = f.readAll();
    f.close();
    return data;
}

// 解析只写进调用方给的容器，不碰 SourceConfig 的状态，失败时调用方还能退回内置配置。
static bool parseSources(const QByteArray &data, QVector<SourceDesc> *out, QString *note)
{
    if (data.isEmpty()) return false;
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) return false;
    const QJsonObject root = doc.object();
    const QJsonArray arr = root.value(QStringLiteral("sources")).toArray();
    if (arr.isEmpty()) return false;

    const QVariantMap searchPlaylistEndpoints = root.value(QStringLiteral("searchPlaylistEndpoints"))
                                                .toObject().toVariantMap();
    for (const QJsonValue &v : arr) {
        const QVariantMap m = v.toObject().toVariantMap();
        SourceDesc s;
        s.id      = m.value(QStringLiteral("id")).toString();
        s.name    = m.value(QStringLiteral("name")).toString();
        s.color   = m.value(QStringLiteral("color")).toString();
        s.base    = m.value(QStringLiteral("base")).toString();
        s.enabled = m.value(QStringLiteral("enabled"), true).toBool();
        s.driver  = m.value(QStringLiteral("driver")).toString();
        s.headers = m.value(QStringLiteral("headers")).toMap();
        s.raw     = m;
        const QVariantMap eps = m.value(QStringLiteral("endpoints")).toMap();
        for (auto it = eps.constBegin(); it != eps.constEnd(); ++it)
            s.endpoints.insert(it.key(), Endpoint::fromVariant(it.value()));
        // 默认榜单：未登录也展示的公开榜单（参考洛雪 leaderboard.js 的静态 boardList）
        const QVariantList dbs = m.value(QStringLiteral("defaultBoards")).toList();
        for (const QVariant &b : dbs) {
            const QVariantMap bm = b.toMap();
            if (!bm.value(QStringLiteral("id")).toString().isEmpty()
                && !bm.value(QStringLiteral("title")).toString().isEmpty())
                s.defaultBoards.append(bm);
        }
        if (searchPlaylistEndpoints.contains(s.id))
            s.endpoints.insert(QStringLiteral("searchPlaylist"),
                               Endpoint::fromVariant(searchPlaylistEndpoints.value(s.id)));
        s.login = LoginConfig::fromVariant(m.value(QStringLiteral("login")));
        s.userApi = UserApiConfig::fromVariant(m.value(QStringLiteral("userApi")));
        if (!s.id.isEmpty() && s.enabled) out->append(s);
    }
    *note = root.value(QStringLiteral("_note")).toString();
    return !out->isEmpty();
}

void SourceConfig::reload()
{
    // 核心在 CMake 下被编成静态库：必须显式引用 qrc 的初始化符号，
    // 否则链接器会丢弃 qrc_config.o，运行时 :/config/sources.json 不存在。
    furuiInitConfigResource();

    // 先解析进临时容器，成功后才替换：用户手改数据目录里的 sources.json 时，
    // 少一个逗号就会让解析返回空数组，原来的写法已经把 m_sources 清空了，
    // 结果是所有平台静默消失。
    QVector<SourceDesc> parsed;
    QString note;
    const QString externalPath = Settings::filePath(QStringLiteral("sources.json"));
    const QByteArray external = readJsonFile(externalPath);
    bool ok = !external.isEmpty() && parseSources(external, &parsed, &note);
    if (!ok) {
        if (!external.isEmpty())
            qWarning() << "[FuruiMusic] 数据目录的 sources.json 解析失败，改用内置配置:" << externalPath;
        parsed.clear();
        ok = parseSources(readJsonFile(QStringLiteral(":/config/sources.json")), &parsed, &note);
    }
    if (!ok || parsed.isEmpty()) {
        qWarning() << "[FuruiMusic] 未能读到可用的平台配置，保留当前平台列表";
        return;
    }
    m_sources = parsed;
    m_note = note;
    emit changed();
}

SourceDesc SourceConfig::source(const QString &id) const
{
    for (const SourceDesc &s : m_sources)
        if (s.id == id) return s;
    return SourceDesc();
}

QStringList SourceConfig::ids() const
{
    QStringList out;
    for (const SourceDesc &s : m_sources) out << s.id;
    return out;
}

QStringList SourceConfig::names() const
{
    QStringList out;
    for (const SourceDesc &s : m_sources) out << s.name;
    return out;
}

QString SourceConfig::nameOf(const QString &id) const
{
    if (id == QLatin1String("custom")) return QStringLiteral("自定义音源");
    for (const SourceDesc &s : m_sources)
        if (s.id == id) return s.name;
    return id;
}

} // namespace Furui
