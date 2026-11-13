#include "MusicSource.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "core/QtCompat.h"
#include <QUrlQuery>

#include <memory>

#include "JsonMapper.h"
#include "KugouProtocol.h"
#include "Settings.h"

namespace Furui {

// Qt 的 errorString 前面带完整请求 URL，界面只该显示后面的原因
static QString shortError(const QString &error)
{
    const int sep = error.lastIndexOf(QStringLiteral(" - "));
    return sep > 0 ? error.mid(sep + 3) : error;
}

// 网易云 MV 榜单返回的是原图地址（http + 无尺寸参数），实测单张 1~7 秒、
// 140KB+，整屏 24 张会长时间留白。加 ?param=400y225 后约 30KB / 80ms，
// 与 AlgerMusicPlayer 的 getImgUrl(cover, '400y225') 同一处理；顺手升 HTTPS，
// 避免 Android 明文流量限制把封面全部吞掉。
static QString mvCoverThumb(const QString &url)
{
    if (url.isEmpty()) return url;
    QString out = url;
    if (out.startsWith(QLatin1String("http://")))
        out = QStringLiteral("https://") + out.mid(7);
    if (!out.contains(QLatin1Char('?')) && out.contains(QLatin1String("music.126.net")))
        out += QStringLiteral("?param=400y225");
    return out;
}

MusicSource::MusicSource(const SourceDesc &desc, QObject *parent)
    : QObject(parent), m_desc(desc)
{
}

QVariantMap MusicSource::baseVars() const
{
    QVariantMap v;
    v.insert(QStringLiteral("cookie"), m_session.cookie);
    v.insert(QStringLiteral("token"), m_session.token);
    v.insert(QStringLiteral("uid"), m_session.uid);
    v.insert(QStringLiteral("csrf"), m_session.token);
    v.insert(QStringLiteral("favoriteId"), m_session.token);   // 可由配置/账号覆盖
    v.insert(QStringLiteral("timestamp"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    v.insert(QStringLiteral("guid"), QString::number(QRandomGenerator::global()->bounded(100000000, 999999999)));
    v.insert(QStringLiteral("page"), QStringLiteral("1"));
    v.insert(QStringLiteral("offset"), QStringLiteral("0"));
    v.insert(QStringLiteral("limit"), QString::number(Settings::integer(QStringLiteral("pageSize"), 30)));
    v.insert(QStringLiteral("quality"), QStringLiteral("standard"));
    v.insert(QStringLiteral("br"), QStringLiteral("320000"));
    v.insert(QStringLiteral("key"), QString());
    v.insert(QStringLiteral("phone"), QString());
    v.insert(QStringLiteral("code"), QString());
    v.insert(QStringLiteral("keyword"), QString());
    v.insert(QStringLiteral("id"), QString());
    if (m_desc.id == QLatin1String("kugou")) {
        Kugou::addDeviceVars(&v);
        Kugou::addAuthVars(&v, m_session.cookie);
    }
    return v;
}

QVariantMap MusicSource::songVars(const Song &song, const QString &quality) const
{
    QVariantMap v = baseVars();
    v.insert(QStringLiteral("id"), song.sourceId);
    v.insert(QStringLiteral("source"), song.source);
    v.insert(QStringLiteral("name"), song.title);
    v.insert(QStringLiteral("artist"), song.artist);
    v.insert(QStringLiteral("album"), song.album);
    v.insert(QStringLiteral("albumId"), song.albumId);
    v.insert(QStringLiteral("quality"), quality);
    QString br = QStringLiteral("320000");
    if (quality == QLatin1String("standard")) br = QStringLiteral("128000");
    else if (quality == QLatin1String("lossless")) br = QStringLiteral("999000");
    else if (quality == QLatin1String("hires")) br = QStringLiteral("2000000");
    v.insert(QStringLiteral("br"), br);
    v.insert(QStringLiteral("toneFlag"), quality == QLatin1String("lossless") ? QStringLiteral("SQ") : QStringLiteral("HQ"));
    return v;
}

QUrl MusicSource::buildUrl(const Endpoint &ep, const QVariantMap &vars, QVariantMap *headers,
                           QByteArray *body) const
{
    const QString path = Json::render(ep.path, vars);
    QUrl url(path.startsWith(QLatin1String("http"), Qt::CaseInsensitive)
                 ? path : m_desc.base + path);

    QVariantMap h = Json::renderMap(m_desc.headers, vars);
    const QVariantMap extra = Json::renderMap(ep.headers, vars);
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it) h.insert(it.key(), it.value());
    for (auto it = h.begin(); it != h.end();) {
        if (it.value().toString().trimmed().isEmpty()) it = h.erase(it);
        else ++it;
    }
    if (headers) *headers = h;

    const QVariantMap params = Json::renderMap(ep.params, vars);
    const bool isPost = (ep.method.toUpper() == QLatin1String("POST"));
    QUrlQuery query(url);
    QByteArray payload;
    if (isPost && ep.contentType == QLatin1String("json")) {
        QVariantMap j = Json::renderMap(ep.jsonBody, vars);
        payload = QJsonDocument(QJsonObject::fromVariantMap(j)).toJson(QJsonDocument::Compact);
        for (auto it = params.constBegin(); it != params.constEnd(); ++it)
            query.addQueryItem(it.key(), it.value().toString());
        // 酷狗网关要求 charset 也在 Content-Type 里，配置显式写了就别覆盖
        if (!h.contains(QLatin1String("Content-Type")))
            h.insert(QStringLiteral("Content-Type"), QStringLiteral("application/json"));
    } else if (isPost && ep.contentType == QLatin1String("form")) {
        QUrlQuery form;
        for (auto it = params.constBegin(); it != params.constEnd(); ++it)
            form.addQueryItem(it.key(), it.value().toString());
        payload = form.toString(QUrl::FullyEncoded).toUtf8();
        h.insert(QStringLiteral("Content-Type"), QStringLiteral("application/x-www-form-urlencoded"));
    } else {
        for (auto it = params.constBegin(); it != params.constEnd(); ++it)
            query.addQueryItem(it.key(), it.value().toString());
    }
    // 酷狗接口用 signature 校验「排序后的 query 参数 + 请求体原文」，
    // 必须在两者都定稿之后补算，否则送出的字节与参与签名的字节会不一致。
    // 扫码用 Web 盐（kugou-web），gateway.kugou.com 的业务接口用 App 盐（kugou-app）。
    const QString signMode = ep.extra.value(QStringLiteral("sign")).toString();
    if (signMode == QLatin1String("kugou-web") || signMode == QLatin1String("kugou-app")) {
        query.addQueryItem(QStringLiteral("signature"),
                           Kugou::sign(params, payload,
                                        signMode == QLatin1String("kugou-app")
                                            ? Kugou::Keyparam::App : Kugou::Keyparam::Web));
    }
    url.setQuery(query);
    if (body) *body = payload;
    if (headers) *headers = h;
    return url;
}

void MusicSource::call(const Endpoint &ep, const QVariantMap &vars, ReplyCallback cb)
{
    if (!ep.valid()) {
        HttpClient::Reply r;
        r.error = QStringLiteral("该平台未配置此接口");
        if (cb) cb(r);
        return;
    }
    QVariantMap headers;
    QByteArray payload;
    const QUrl url = buildUrl(ep, vars, &headers, &payload);
    if (qEnvironmentVariableIsSet("FURUI_HTTP_DEBUG")) {
        QStringList hs;
        for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
            hs << it.key() + '=' + it.value().toString();
        qWarning().noquote() << "[http]" << ep.method << url.toString() << "headers:" << hs.join("; ");
    }
    HttpClient::instance()->request(ep.method, url, headers, payload, cb);
}

void MusicSource::callLogin(const QString &name, const QVariantMap &vars, ReplyCallback cb)
{
    Endpoint ep;
    if (name == QLatin1String("qrCreate")) ep = m_desc.login.qrCreate;
    else if (name == QLatin1String("qrCheck")) ep = m_desc.login.qrCheck;
    else if (name == QLatin1String("userInfo")) ep = m_desc.login.userInfo;
    else if (name == QLatin1String("phoneSend")) ep = m_desc.login.phoneSend;
    else if (name == QLatin1String("phoneLogin")) ep = m_desc.login.phoneLogin;
    else if (name == QLatin1String("playlists")) ep = m_desc.login.playlists;
    else if (name == QLatin1String("favorites")) ep = m_desc.login.favorites;
    else if (name == QLatin1String("playlistDetail")) ep = m_desc.login.playlistDetail;
    call(ep, vars, cb);
}

void MusicSource::requestSongs(const Endpoint &ep, const QVariantMap &vars, SongsCallback cb)
{
    call(ep, vars, [this, ep, cb](const HttpClient::Reply &r) {
        QVector<Song> out;
        if (r.ok) {
            const QJsonValue root = Json::rootOf(r.json);
            const QJsonArray arr = Json::arrayAt(root, ep.root);
            for (const QJsonValue &v : arr) {
                Song s = Json::mapSong(v, ep.fields, m_desc.id);
                if (!s.title.isEmpty()) out << s;
            }
            // 「一首歌都没有」是最难查的一类问题：把「接口没通」和「回包结构和配置对不上」
            // 分开记一条，页面上只会看到 0 首，日志里能看到真正原因
            if (out.isEmpty())
                qWarning().noquote() << "[source]" << m_desc.id << "songs empty, root="
                                     << ep.root << "body="
                                     << QString::fromUtf8(r.body.left(180));
        } else {
            qWarning().noquote() << "[source]" << m_desc.id << "songs request failed:"
                                 << r.error << "status=" << r.status << ep.path;
        }
        if (cb) cb(out);
    });
}

void MusicSource::requestSongsPaged(const Endpoint &ep, const QVariantMap &vars, int limit,
                                    int offset, SongsCallback cb)
{
    const int cap = m_desc.pageLimit > 0 ? m_desc.pageLimit : limit;
    if (cap >= limit || limit <= 0) { requestSongs(ep, vars, cb); return; }

    auto acc = std::make_shared<QVector<Song>>();
    auto stepPtr = std::make_shared<std::function<void()>>();
    *stepPtr = [this, ep, vars, limit, cap, offset, acc, stepPtr, cb]() {
        const int done = acc->size();
        if (done >= limit) { if (cb) cb(*acc); return; }
        const int take = qMin(cap, limit - done);
        QVariantMap v = vars;
        v.insert(QStringLiteral("limit"), QString::number(take));
        v.insert(QStringLiteral("offset"), QString::number(offset + done));
        v.insert(QStringLiteral("page"), QString::number((offset + done) / qMax(1, take) + 1));
        requestSongs(ep, v, [take, acc, stepPtr, cb](const QVector<Song> &songs) {
            *acc += songs;
            // 短页说明该平台已到底，不用再翻
            if (songs.size() < take) { if (cb) cb(*acc); return; }
            (*stepPtr)();
        });
    };
    (*stepPtr)();
}

void MusicSource::requestPlaylists(const Endpoint &ep, const QVariantMap &vars,
                                   const QString &type, PlaylistsCallback cb)
{
    call(ep, vars, [this, ep, type, cb](const HttpClient::Reply &r) {
        QVector<Playlist> out;
        if (r.ok) {
            const QJsonValue root = Json::rootOf(r.json);
            const QJsonArray arr = Json::arrayAt(root, ep.root);
            for (const QJsonValue &v : arr) {
                Playlist p = Json::mapPlaylist(v, ep.fields, m_desc.id, type);
                if (!p.title.isEmpty()) out << p;
            }
        }
        if (cb) cb(out);
    });
}

void MusicSource::search(const QString &keyword, int limit, int offset, SongsCallback cb)
{
    if (m_desc.driver == QLatin1String("qishui"))
        return qishuiSearch(keyword, limit, offset, cb);
    const Endpoint ep = m_desc.endpoints.value(QStringLiteral("search"));
    QVariantMap v = baseVars();
    v.insert(QStringLiteral("keyword"), keyword);
    v.insert(QStringLiteral("limit"), QString::number(limit));
    v.insert(QStringLiteral("offset"), QString::number(offset));
    v.insert(QStringLiteral("page"), QString::number(offset / qMax(1, limit) + 1));
    requestSongs(ep, v, cb);
}

void MusicSource::searchPlaylist(const QString &keyword, int limit, int offset,
                                 PlaylistsCallback cb)
{
    if (m_desc.driver == QLatin1String("qishui"))
        return qishuiSearchPlaylists(keyword, limit, offset, cb);
    const Endpoint ep = m_desc.endpoints.value(QStringLiteral("searchPlaylist"));
    QVariantMap v = baseVars();
    v.insert(QStringLiteral("keyword"), keyword);
    v.insert(QStringLiteral("limit"), QString::number(limit));
    v.insert(QStringLiteral("offset"), QString::number(offset));
    v.insert(QStringLiteral("page"), QString::number(offset / qMax(1, limit) + 1));
    requestPlaylists(ep, v, QStringLiteral("search"), cb);
}

void MusicSource::rank(int limit, int offset, PlaylistsCallback cb)
{
    const Endpoint ep = m_desc.endpoints.value(QStringLiteral("rank"));
    // 汽水音乐：推荐歌单走 Luna 发现流（免登录），结构需专用展平解析
    if (m_desc.driver == QLatin1String("qishui") && ep.valid())
        return qishuiRank(limit, offset, cb);
    // 没有 rank 端点（如汽水音乐）：直接回退静态默认榜单，未登录也展示
    if (!ep.valid()) {
        if (cb) cb(defaultBoards());
        return;
    }
    QVariantMap v = baseVars();
    v.insert(QStringLiteral("limit"), QString::number(limit));
    v.insert(QStringLiteral("offset"), QString::number(offset));
    v.insert(QStringLiteral("page"), QString::number(offset / qMax(1, limit) + 1));
    // 端点被反爬/失效（如咪咕老 remoting 返回 HTML 页）导致空结果时，回退默认榜单，
    // 避免「切换平台刷新后一片空白」。defaultBoards 为空则维持空结果。
    requestPlaylists(ep, v, QStringLiteral("rank"), [this, cb](const QVector<Playlist> &list) {
        if (cb) cb(list.isEmpty() ? defaultBoards() : list);
    });
}

QVector<Playlist> MusicSource::defaultBoards() const
{
    QVector<Playlist> out;
    for (const QVariant &b : m_desc.defaultBoards) {
        const QVariantMap m = b.toMap();
        Playlist p;
        p.source = m_desc.id;
        p.type = QStringLiteral("rank");
        p.sourceId = m.value(QStringLiteral("id")).toString();
        p.title = m.value(QStringLiteral("title")).toString();
        p.cover = Json::fillImageSize(m.value(QStringLiteral("cover")).toString());
        p.count = m.value(QStringLiteral("count")).toInt();
        if (p.sourceId.isEmpty()) p.sourceId = QString::number(qHash(p.title), 16);
        p.uid = p.source + QLatin1Char(':') + p.sourceId;
        if (p.title.isEmpty()) continue;
        out << p;
    }
    return out;
}

void MusicSource::playlistDetail(const QString &id, int limit, int offset, SongsCallback cb)
{
    if (m_desc.driver == QLatin1String("qishui"))
        return qishuiPlaylistDetail(id, limit, offset, cb);
    const Endpoint ep = m_desc.endpoints.value(QStringLiteral("playlistDetail"));
    QVariantMap v = baseVars();
    v.insert(QStringLiteral("id"), id);
    v.insert(QStringLiteral("limit"), QString::number(limit));
    v.insert(QStringLiteral("offset"), QString::number(offset));
    v.insert(QStringLiteral("page"), QString::number(offset / qMax(1, limit) + 1));
    requestSongsPaged(ep, v, limit, offset, cb);
}

void MusicSource::rankDetail(const QString &id, int limit, int offset, SongsCallback cb)
{
    const Endpoint ep = m_desc.endpoints.value(QStringLiteral("rankDetail"));
    if (ep.path.isEmpty())
        return playlistDetail(id, limit, offset, cb);
    QVariantMap v = baseVars();
    v.insert(QStringLiteral("id"), id);
    v.insert(QStringLiteral("limit"), QString::number(limit));
    v.insert(QStringLiteral("offset"), QString::number(offset));
    v.insert(QStringLiteral("page"), QString::number(offset / qMax(1, limit) + 1));
    requestSongsPaged(ep, v, limit, offset, cb);
}

bool MusicSource::isVipResponse(const QVariantMap &rule, const QJsonValue &root,
                                const QByteArray &body, const QString &resolvedUrl)
{
    if (rule.isEmpty()) return false;
    if (resolvedUrl.isEmpty() && rule.value(QStringLiteral("emptyIsVip"), true).toBool())
        return true;
    const QVariantList equals = rule.value(QStringLiteral("equals")).toList();
    for (const QVariant &e : equals) {
        const QVariantMap m = e.toMap();
        const QString value = Json::stringAt(root, m.value(QStringLiteral("path")).toString());
        for (const QVariant &x : m.value(QStringLiteral("values")).toList()) {
            if (!value.isEmpty() && value == x.toString()) return true;
        }
    }
    const QString text = QString::fromUtf8(body);
    for (const QVariant &k : rule.value(QStringLiteral("bodyContains")).toList()) {
        const QString s = k.toString();
        if (!s.isEmpty() && text.contains(s, Qt::CaseInsensitive)) return true;
    }
    return false;
}

QVariantMap MusicSource::playbackHeaders() const
{
    QVariantMap out;
    const QVariantMap vars = baseVars();
    for (auto it = m_desc.headers.constBegin(); it != m_desc.headers.constEnd(); ++it) {
        const QString value = Json::render(it.value().toString(), vars);
        if (!value.trimmed().isEmpty()) out.insert(it.key(), value);
    }
    return out;
}

void MusicSource::resolveUrl(const Song &song, const QString &quality, PlayableCallback cb)
{
    const Endpoint ep = m_desc.endpoints.value(QStringLiteral("url"));
    const QVariantMap vars = songVars(song, quality);
    call(ep, vars, [this, ep, song, cb](const HttpClient::Reply &r) {
        Playable p;
        p.headers = playbackHeaders();
        const QJsonValue root = r.ok ? Json::rootOf(r.json) : QJsonValue();
        QString url = Json::firstString(root, ep.resultPaths);
        if (url.isEmpty() && !r.body.isEmpty() && !r.json.isObject() && !r.json.isArray()) {
            const QString plain = QString::fromUtf8(r.body).trimmed();
            if (plain.startsWith(QLatin1String("http"), Qt::CaseInsensitive)) url = plain;
        }
        const bool vip = isVipResponse(ep.vipRule, root, r.body, url);
        if (!r.ok) {
            // 请求本身失败时，服务端的原因必须显示出来；原来这段写在下面，
            // url 为空已经把 needSubstitute 置上，错误串永远到不了界面
            p.ok = false;
            p.needSubstitute = song.vip;
            p.reason = QStringLiteral("error");
            p.message = r.error.isEmpty() ? QStringLiteral("播放地址请求失败") : shortError(r.error);
        } else if (vip || url.isEmpty()) {
            p.ok = false;
            p.needSubstitute = true;
            p.reason = vip ? QStringLiteral("vip") : QStringLiteral("empty");
            p.message = vip ? QStringLiteral("该歌曲需要会员或暂不可播放")
                            : QStringLiteral("该平台未返回播放地址");
        } else {
            p.ok = true;
            p.url = QUrl(url);
            if (!p.url.isValid() || p.url.scheme().isEmpty())
                p.url = QUrl::fromLocalFile(url);
        }
        if (cb) cb(p);
    });
}

void MusicSource::lyric(const Song &song, StringCallback cb)
{
    const Endpoint ep = m_desc.endpoints.value(QStringLiteral("lyric"));
    if (!ep.valid()) { if (cb) cb(QString()); return; }
    call(ep, songVars(song, QStringLiteral("standard")), [ep, cb](const HttpClient::Reply &r) {
        QString out;
        if (r.ok) out = Json::firstString(Json::rootOf(r.json), ep.resultPaths);
        if (out.isEmpty() && !r.body.isEmpty() && r.json.isEmpty())
            out = QString::fromUtf8(r.body);
        if (cb) cb(out);
    });
}

void MusicSource::cover(const Song &song, StringCallback cb)
{
    const Endpoint ep = m_desc.endpoints.value(QStringLiteral("cover"));
    if (!ep.valid() || song.sourceId.isEmpty()) {
        if (cb) cb(QString());
        return;
    }
    call(ep, songVars(song, QStringLiteral("standard")), [ep, cb](const HttpClient::Reply &r) {
        QString out;
        if (r.ok) out = Json::firstString(Json::rootOf(r.json), ep.resultPaths).trimmed();
        // 接口偶尔会返回空串或错误页文本，别把它当图片地址塞给 QML Image
        if (!out.startsWith(QLatin1String("http"), Qt::CaseInsensitive)) out.clear();
        if (cb) cb(out);
    });
}

void MusicSource::mv(const Song &song, MvCallback cb)
{
    const Endpoint mvEp = m_desc.endpoints.value(QStringLiteral("mvUrl"));
    if (!mvEp.valid()) {
        if (cb) cb(false, QUrl(), QStringLiteral("该平台暂未配置 MV"));
        return;
    }
    auto deliverUrl = [this, mvEp, song, cb](const QString &mvId) {
        if (mvId.isEmpty() || mvId == QLatin1String("0")) {
            if (cb) cb(false, QUrl(), QStringLiteral("该歌曲没有 MV"));
            return;
        }
        auto attempt = [this, mvEp, song](const QString &id, const QString &r) {
            QVariantMap v = songVars(song, QStringLiteral("standard"));
            v.insert(QStringLiteral("mvId"), id);
            v.insert(QStringLiteral("mvR"), r);
            return v;
        };
        call(mvEp, attempt(mvId, QStringLiteral("1080")),
             [this, mvEp, song, mvId, cb, attempt](const HttpClient::Reply &r1) {
            const QString url = r1.ok
                ? Json::firstString(Json::rootOf(r1.json), mvEp.resultPaths) : QString();
            if (!url.isEmpty()) { if (cb) cb(true, QUrl(url), QString()); return; }
            // 高清源缺失（老 MV 只有 480p）退一档再试一次
            call(mvEp, attempt(mvId, QStringLiteral("480")), [mvEp, cb](const HttpClient::Reply &r2) {
                const QString url2 = r2.ok
                    ? Json::firstString(Json::rootOf(r2.json), mvEp.resultPaths) : QString();
                if (!url2.isEmpty()) { if (cb) cb(true, QUrl(url2), QString()); return; }
                if (cb) cb(false, QUrl(), QStringLiteral("未取到 MV 播放地址"));
            });
        });
    };
    const QVariantMap v = songVars(song, QStringLiteral("standard"));
    // 网易云搜索响应里每首歌自带 mvid 字段，mapSong 把原始 JSON 整个存进了 Song.extra
    const QString mvId = song.extra.value(QStringLiteral("mvid")).toString();
    if (!mvId.isEmpty() && mvId != QLatin1String("0")) {
        deliverUrl(mvId);
        return;
    }
    const Endpoint infoEp = m_desc.endpoints.value(QStringLiteral("mvInfo"));
    if (!infoEp.valid()) {
        if (cb) cb(false, QUrl(), QStringLiteral("该歌曲没有 MV"));
        return;
    }
    call(infoEp, v, [deliverUrl, infoEp](const HttpClient::Reply &r) {
        const QString id2 = r.ok
            ? Json::firstString(Json::rootOf(r.json), infoEp.resultPaths) : QString();
        deliverUrl(id2);
    });
}

// 公开 MV 榜单：字段映射全部来自 mvList 端点配置，与歌单/歌曲共用同一套
// Json::stringAt 取值规则，平台没配这个端点就返回空列表（UI 显示空态而不是报错）。
void MusicSource::mvList(const QString &area, int limit, int offset, MvListCallback cb)
{
    const Endpoint ep = m_desc.endpoints.value(QStringLiteral("mvList"));
    if (!ep.valid()) {
        if (cb) cb(QVector<QVariantMap>(), false);
        return;
    }
    QVariantMap v = baseVars();
    v.insert(QStringLiteral("area"), area);
    v.insert(QStringLiteral("limit"), QString::number(limit));
    v.insert(QStringLiteral("offset"), QString::number(offset));
    v.insert(QStringLiteral("page"), QString::number(offset / qMax(1, limit) + 1));
    call(ep, v, [this, ep, cb](const HttpClient::Reply &r) {
        QVector<QVariantMap> out;
        bool hasMore = false;
        if (r.ok) {
            const QJsonValue root = Json::rootOf(r.json);
            const QJsonArray arr = Json::arrayAt(root, ep.root);
            const QVariantMap f = ep.fields;
            for (const QJsonValue &val : arr) {
                const QString id = Json::stringAt(val, f.value(QStringLiteral("id")).toString());
                if (id.isEmpty()) continue;
                QVariantMap m;
                m.insert(QStringLiteral("id"), id);
                m.insert(QStringLiteral("title"),
                        Json::stringAt(val, f.value(QStringLiteral("title")).toString()));
                m.insert(QStringLiteral("artist"),
                        Json::stringAt(val, f.value(QStringLiteral("artist")).toString()));
                m.insert(QStringLiteral("cover"), mvCoverThumb(
                        Json::stringAt(val, f.value(QStringLiteral("cover")).toString())));
                m.insert(QStringLiteral("playCount"),
                        Json::stringAt(val, f.value(QStringLiteral("playCount")).toString()).toLongLong());
                m.insert(QStringLiteral("duration"),
                        Json::stringAt(val, f.value(QStringLiteral("duration")).toString()).toLongLong());
                m.insert(QStringLiteral("source"), m_desc.id);
                out << m;
            }
            // 服务端 hasMore 为真但本页空数组时也要收口，否则「加载更多」会无限打转
            const QJsonValue more = Json::valueAt(root, QStringLiteral("hasMore"));
            hasMore = (more.isBool() ? more.toBool() : !arr.isEmpty()) && !out.isEmpty();
        }
        if (cb) cb(out, hasMore);
    });
}

void MusicSource::userPlaylists(PlaylistsCallback cb)
{
    Endpoint ep = m_desc.login.playlists;
    if (!ep.valid()) {
        if (cb) cb(QVector<Playlist>());
        return;
    }
    if (ep.fields.isEmpty()) ep.fields = m_desc.login.fields.value(QStringLiteral("playlists")).toMap();
    QVariantMap v = baseVars();
    v.insert(QStringLiteral("limit"), QString::number(Settings::integer(QStringLiteral("pageSize"), 30) * 3));
    requestPlaylists(ep, v, QStringLiteral("user"), cb);
}

void MusicSource::userFavorites(SongsCallback cb)
{
    Endpoint ep = m_desc.login.favorites;
    if (!ep.valid()) {
        if (cb) cb(QVector<Song>());
        return;
    }
    if (ep.fields.isEmpty()) ep.fields = m_desc.login.fields.value(QStringLiteral("favorites")).toMap();
    QVariantMap v = baseVars();
    v.insert(QStringLiteral("limit"), QString::number(Settings::integer(QStringLiteral("pageSize"), 30) * 5));
    requestSongs(ep, v, cb);
}

void MusicSource::userPlaylistDetail(const QString &id, SongsCallback cb)
{
    Endpoint ep = m_desc.login.playlistDetail;
    if (!ep.valid()) ep = m_desc.endpoints.value(QStringLiteral("playlistDetail"));
    QVariantMap v = baseVars();
    v.insert(QStringLiteral("id"), id);
    v.insert(QStringLiteral("limit"), QString::number(Settings::integer(QStringLiteral("pageSize"), 30) * 5));
    requestSongs(ep, v, cb);
}

namespace {
// 汽水各端点的数组容器并不统一：PC 搜索是 result_groups[].data[]，
// 部分歌单/详情接口是顶层 data[] / playlists[] / media_resources[]，
// 还有的直接把实体放在 result_groups[] 里。统一在这里收集，避免某一路永远解析为空。
inline QJsonArray qishuiCollectItems(const QJsonValue &root)
{
    QJsonArray items;
    const QJsonArray groups = Json::arrayAt(root, QStringLiteral("result_groups"));
    for (const QJsonValue &g : groups) {
        const QJsonArray data = Json::arrayAt(g, QStringLiteral("data"));
        if (!data.isEmpty()) {
            for (const QJsonValue &d : data) items.append(d);
        } else {
            items.append(g);
        }
    }
    if (items.isEmpty()) {
        const QStringList keys = {QStringLiteral("data"), QStringLiteral("playlists"),
                                  QStringLiteral("media_resources"), QStringLiteral("list"),
                                  QStringLiteral("results")};
        for (const QString &k : keys) {
            const QJsonArray a = Json::arrayAt(root, k);
            if (a.isEmpty()) continue;
            for (const QJsonValue &v : a) items.append(v);
            break;
        }
    }
    return items;
}

// 汽水 track 嵌套：entity.track_wrapper.track / entity.track / 直接 track
inline QJsonValue qishuiUnwrapTrack(const QJsonValue &v)
{
    QJsonValue t = Json::valueAt(v, QStringLiteral("entity.track_wrapper.track"));
    if (t.isNull()) t = Json::valueAt(v, QStringLiteral("entity.track"));
    if (t.isNull()) t = Json::valueAt(v, QStringLiteral("track"));
    if (t.isNull() && v.isObject()
        && v.toObject().contains(QStringLiteral("id"))
        && (v.toObject().contains(QStringLiteral("name"))
            || v.toObject().contains(QStringLiteral("title"))))
        t = v;   // 某些端点直接给 track 实体
    return t;
}

inline Song qishuiTrackToSong(const QJsonValue &t, const QString &source)
{
    Song s;
    s.source = source;
    s.sourceId = Json::stringAt(t, QStringLiteral("id"));
    s.title    = Json::stringAt(t, QStringLiteral("name"));
    s.artist   = Json::stringAt(t, QStringLiteral("artists[].name"));
    const QJsonValue album = Json::valueAt(t, QStringLiteral("album"));
    s.album  = Json::stringAt(album, QStringLiteral("name"));
    s.cover  = Json::stringAt(album, QStringLiteral("url_cover"));
    const double dur = Json::stringAt(t, QStringLiteral("duration")).toDouble();
    s.duration = dur > 0 ? qint64(dur) : 0;
    if (s.sourceId.isEmpty())
        s.sourceId = QString::number(qHash(s.normalizedKey()), 16);
    s.uid = source + QLatin1Char(':') + s.sourceId;
    if (t.isObject()) s.extra = t.toObject().toVariantMap();
    return s;
}

inline QJsonValue qishuiUnwrapPlaylist(const QJsonValue &v)
{
    QJsonValue p = Json::valueAt(v, QStringLiteral("entity.playlist_wrapper.playlist"));
    if (p.isNull()) p = Json::valueAt(v, QStringLiteral("entity.playlist"));
    if (p.isNull()) p = Json::valueAt(v, QStringLiteral("playlist"));
    if (p.isNull() && v.isObject()
        && (v.toObject().contains(QStringLiteral("id"))
            || v.toObject().contains(QStringLiteral("playlist_id")))
        && (v.toObject().contains(QStringLiteral("title"))
            || v.toObject().contains(QStringLiteral("name"))))
        p = v;   // 直接给 playlist 实体
    return p;
}

// 汽水封面字段兼容两种形态：
//   - 字符串（多数歌单详情/搜索）；
//   - {uri, urls:[prefix,...]} 对象（发现流 discover/mix 的 url_cover，需 prefix+uri 拼接）。
inline QString qishuiCoverOf(const QJsonValue &p)
{
    QJsonValue cv = Json::valueAt(p, QStringLiteral("url_cover"));
    if (cv.isString()) return cv.toString();
    if (cv.isObject()) {
        const QJsonObject o = cv.toObject();
        const QString uri = o.value(QStringLiteral("uri")).toString();
        const QJsonArray urls = o.value(QStringLiteral("urls")).toArray();
        const QString prefix = urls.isEmpty() ? QString() : urls.at(0).toString();
        if (!prefix.isEmpty() || !uri.isEmpty()) return prefix + uri;
    }
    return Json::stringAt(p, QStringLiteral("cover_url"));
}

inline Playlist qishuiPlaylistToPlaylist(const QJsonValue &p, const QString &source)
{
    Playlist pl;
    pl.source = source;
    pl.type = QStringLiteral("search");
    pl.sourceId = Json::stringAt(p, QStringLiteral("id"));
    pl.title = Json::stringAt(p, QStringLiteral("title"));
    pl.cover = qishuiCoverOf(p);
    pl.owner = Json::stringAt(p, QStringLiteral("owner.nickname"));
    pl.count = Json::stringAt(p, QStringLiteral("count_tracks")).toInt();
    if (pl.sourceId.isEmpty()) pl.sourceId = QString::number(qHash(pl.title), 16);
    pl.uid = source + QLatin1Char(':') + pl.sourceId;
    if (p.isObject()) pl.extra = p.toObject().toVariantMap();
    return pl;
}
} // anonymous namespace

void MusicSource::qishuiSearch(const QString &keyword, int limit, int offset, SongsCallback cb)
{
    const Endpoint ep = m_desc.endpoints.value(QStringLiteral("search"));
    QVariantMap v = baseVars();
    v.insert(QStringLiteral("keyword"), keyword);
    v.insert(QStringLiteral("limit"), QString::number(limit));
    v.insert(QStringLiteral("offset"), QString::number(offset));
    v.insert(QStringLiteral("page"), QString::number(offset / qMax(1, limit) + 1));
    call(ep, v, [this, cb](const HttpClient::Reply &r) {
        QVector<Song> out;
        if (r.ok) {
            const QJsonValue root = Json::rootOf(r.json);
            const QJsonArray items = qishuiCollectItems(root);
            for (const QJsonValue &d : items) {
                const QJsonValue t = qishuiUnwrapTrack(d);
                if (!t.isNull() && !Json::stringAt(t, QStringLiteral("id")).isEmpty()
                    && !Json::stringAt(t, QStringLiteral("name")).isEmpty())
                    out << qishuiTrackToSong(t, m_desc.id);
            }
        }
        if (cb) cb(out);
    });
}

void MusicSource::qishuiPlaylistDetail(const QString &id, int limit, int offset, SongsCallback cb)
{
    const Endpoint ep = m_desc.endpoints.value(QStringLiteral("playlistDetail"));
    QVariantMap v = baseVars();
    v.insert(QStringLiteral("id"), id);
    v.insert(QStringLiteral("limit"), QString::number(limit));
    v.insert(QStringLiteral("offset"), QString::number(offset));
    v.insert(QStringLiteral("page"), QString::number(offset / qMax(1, limit) + 1));
    call(ep, v, [this, cb](const HttpClient::Reply &r) {
        QVector<Song> out;
        if (r.ok) {
            const QJsonValue root = Json::rootOf(r.json);
            const QJsonArray items = qishuiCollectItems(root);
            for (const QJsonValue &m : items) {
                const QJsonValue t = qishuiUnwrapTrack(m);
                if (!t.isNull() && !Json::stringAt(t, QStringLiteral("id")).isEmpty()
                    && !Json::stringAt(t, QStringLiteral("name")).isEmpty())
                    out << qishuiTrackToSong(t, m_desc.id);
            }
        }
        if (cb) cb(out);
    });
}

void MusicSource::qishuiSearchPlaylists(const QString &keyword, int limit, int offset, PlaylistsCallback cb)
{
    const Endpoint ep = m_desc.endpoints.value(QStringLiteral("searchPlaylist"));
    QVariantMap v = baseVars();
    v.insert(QStringLiteral("keyword"), keyword);
    v.insert(QStringLiteral("limit"), QString::number(limit));
    v.insert(QStringLiteral("offset"), QString::number(offset));
    v.insert(QStringLiteral("page"), QString::number(offset / qMax(1, limit) + 1));
    call(ep, v, [this, cb](const HttpClient::Reply &r) {
        QVector<Playlist> out;
        if (r.ok) {
            const QJsonValue root = Json::rootOf(r.json);
            const QJsonArray items = qishuiCollectItems(root);
            for (const QJsonValue &d : items) {
                const QJsonValue p = qishuiUnwrapPlaylist(d);
                if (!p.isNull() && !Json::stringAt(p, QStringLiteral("id")).isEmpty()
                    && !Json::stringAt(p, QStringLiteral("title")).isEmpty())
                    out << qishuiPlaylistToPlaylist(p, m_desc.id);
            }
        }
        if (cb) cb(out);
    });
}

void MusicSource::qishuiRank(int limit, int offset, PlaylistsCallback cb)
{
    const Endpoint ep = m_desc.endpoints.value(QStringLiteral("rank"));
    QVariantMap v = baseVars();
    v.insert(QStringLiteral("limit"), QString::number(limit));
    v.insert(QStringLiteral("offset"), QString::number(offset));
    v.insert(QStringLiteral("page"), QString::number(offset / qMax(1, limit) + 1));
    call(ep, v, [this, cb](const HttpClient::Reply &r) {
        QVector<Playlist> out;
        if (r.ok) {
            const QJsonValue root = Json::rootOf(r.json);
            // 发现流结构：inner_block[].resources[].entity.playlist
            const QJsonArray blocks = Json::arrayAt(root, QStringLiteral("inner_block"));
            for (const QJsonValue &b : blocks) {
                const QJsonArray res = Json::arrayAt(b, QStringLiteral("resources"));
                for (const QJsonValue &rsc : res) {
                    const QJsonValue p = qishuiUnwrapPlaylist(rsc);
                    if (!p.isNull() && !Json::stringAt(p, QStringLiteral("id")).isEmpty()
                        && !Json::stringAt(p, QStringLiteral("title")).isEmpty())
                        out << qishuiPlaylistToPlaylist(p, m_desc.id);
                }
            }
        }
        // 接口偶发失败/被限流时回退静态默认榜单，避免空白
        if (cb) cb(out.isEmpty() ? defaultBoards() : out);
    });
}

} // namespace Furui
