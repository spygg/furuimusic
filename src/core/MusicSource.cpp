#include "MusicSource.h"

#include <QDateTime>
#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QStringList>
#include "core/QtCompat.h"
#include <QUrlQuery>

#include <memory>

#include "JsonMapper.h"
#include "KugouProtocol.h"
#include "NeteaseCrypto.h"
#include "Settings.h"
#include <QRegularExpression>

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
    // 部分平台页码从 0 开始（酷我 pl.svc 的 pn）
    v.insert(QStringLiteral("pageIndex"), QStringLiteral("0"));
    v.insert(QStringLiteral("offset"), QStringLiteral("0"));
    v.insert(QStringLiteral("limit"), QString::number(Settings::integer(QStringLiteral("pageSize"), 30)));
    v.insert(QStringLiteral("quality"), QStringLiteral("standard"));
    v.insert(QStringLiteral("br"), QStringLiteral("320000"));
    v.insert(QStringLiteral("key"), QString());
    v.insert(QStringLiteral("phone"), QString());
    v.insert(QStringLiteral("code"), QString());
    v.insert(QStringLiteral("keyword"), QString());
    v.insert(QStringLiteral("id"), QString());
    // 汽水 PC 接口（/luna/pc/*，搜索/取流）会校验设备签名头，缺失时一律回
    // ERR_REQUEST_FORBIDDEN。与 reference/qishui-api 的 QISHUI_X_HELIOS /
    // QISHUI_X_MEDUSA 对应；账号同步已改走免签名的 web 接口，不再读这两个值。
    // 留空时 buildUrl 会丢掉这两个 header，endpoint 里也能用空串覆盖掉。
    if (m_desc.id == QLatin1String("qishui")) {
        v.insert(QStringLiteral("qishuiHelios"),
                 Settings::string(QStringLiteral("qishuiXHelios")));
        v.insert(QStringLiteral("qishuiMedusa"),
                 Settings::string(QStringLiteral("qishuiXMedusa")));
    }
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
    // 酷狗歌词搜索按「歌名 + 时长」挑候选，时长缺失时同名歌曲容易取错
    v.insert(QStringLiteral("duration"), song.duration);
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

    // 放开 150 首上限后，分页平台（酷我单页 30）理论上要为一个大歌单跑几十次请求。
    // 这里兜一个页数上限，宁可少几首也不能让界面卡在半张歌单上（可用 playlistMaxPages 调）。
    const int maxPages = qBound(1, Settings::integer(QStringLiteral("playlistMaxPages"), 20), 200);

    auto acc = std::make_shared<QVector<Song>>();
    auto pages = std::make_shared<int>(0);
    auto stepPtr = std::make_shared<std::function<void()>>();
    *stepPtr = [this, ep, vars, limit, cap, offset, acc, pages, maxPages, stepPtr, cb]() {
        const int done = acc->size();
        if (done >= limit) { if (cb) cb(*acc); return; }
        if (*pages >= maxPages) {
            qWarning().noquote() << "[source]" << m_desc.id << "playlist paging stopped at"
                                 << *pages << "pages," << acc->size() << "songs";
            if (cb) cb(*acc);
            return;
        }
        ++(*pages);
        const int take = qMin(cap, limit - done);
        QVariantMap v = vars;
        v.insert(QStringLiteral("limit"), QString::number(take));
        v.insert(QStringLiteral("offset"), QString::number(offset + done));
        v.insert(QStringLiteral("page"), QString::number((offset + done) / qMax(1, take) + 1));
        v.insert(QStringLiteral("pageIndex"), QString::number((offset + done) / qMax(1, take)));
        requestSongs(ep, v, [take, acc, stepPtr, cb](const QVector<Song> &songs) {
            *acc += songs;
            // 短页说明该平台已到底，不用再翻
            if (songs.size() < take) { if (cb) cb(*acc); return; }
            (*stepPtr)();
        });
    };
    (*stepPtr)();
}

bool MusicSource::supportsFullPlaylist() const
{
    const Endpoint ep = m_desc.endpoints.value(QStringLiteral("playlistDetail"));
    return !ep.extra.value(QStringLiteral("fullIdsPath")).toString().isEmpty()
           && !ep.extra.value(QStringLiteral("fillEndpoint")).toString().isEmpty();
}

// trackIds 的元素是对象（{"id": 123, "v": 0, ...}），个别平台直接给数字串，两种都收。
static QStringList trackIdsAt(const QJsonValue &root, const QString &path)
{
    QStringList out;
    const QJsonArray arr = Json::arrayAt(root, path);
    out.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        const QString id = v.isObject() ? Json::stringAt(v, QStringLiteral("id")) : Json::str(v);
        if (!id.isEmpty()) out << id;
    }
    return out;
}

void MusicSource::fillSongsByIds(const Endpoint &ep, const Endpoint &fillEp,
                                 const QVariantMap &vars, const QStringList &ids,
                                 const QVector<Song> &preview, SongsCallback cb)
{
    if (ids.isEmpty()) { if (cb) cb(preview); return; }
    // fillBatch 声明在歌单详情端点（fullIdsPath/fillEndpoint 旁边），不是填充端点。
    // 必须从 ep 读，否则配置里的 200 会被忽略；网易云 /api/song/detail 实测 >=202 个 id
    // 只回 201 首，批量上限收紧到 200，避免每批静默丢歌。
    const int batch = qBound(1,
                             ep.extra.value(QStringLiteral("fillBatch"),
                                            fillEp.extra.value(QStringLiteral("fillBatch"), 200))
                                 .toInt(),
                             200);

    auto want = std::make_shared<QStringList>(ids);
    auto got = std::make_shared<QHash<QString, Song>>();
    auto next = std::make_shared<int>(0);
    auto step = std::make_shared<std::function<void()>>();
    *step = [this, fillEp, vars, want, got, next, step, preview, cb, batch]() {
        if (*next >= want->size()) {
            QVector<Song> out;
            out.reserve(want->size());
            for (const QString &id : *want) {
                const auto it = got->constFind(id);
                if (it != got->constEnd()) out << it.value();
            }
            // 一条都没补到（接口变动/全部下架）时保留原本的预览结果，不要变成空歌单
            if (out.isEmpty()) out = preview;
            if (cb) cb(out);
            return;
        }
        const QStringList slice = want->mid(*next, batch);
        *next += slice.size();
        QVariantMap v = vars;
        v.insert(QStringLiteral("ids"), slice.join(QLatin1Char(',')));
        requestSongs(fillEp, v, [this, got, step, slice](const QVector<Song> &songs) {
            if (songs.size() < slice.size())
                qWarning().noquote() << "[source]" << m_desc.id << "songDetail batch short:"
                                     << songs.size() << "/" << slice.size();
            for (const Song &s : songs) got->insert(s.sourceId, s);
            (*step)();
        });
    };
    (*step)();
}

void MusicSource::requestDetailSongs(const Endpoint &ep, const QVariantMap &vars, int limit,
                                     int offset, SongsCallback cb)
{
    const QString idsPath = ep.extra.value(QStringLiteral("fullIdsPath")).toString();
    const QString fillName = ep.extra.value(QStringLiteral("fillEndpoint")).toString();
    if (idsPath.isEmpty() || fillName.isEmpty())
        return requestSongsPaged(ep, vars, limit, offset, cb);

    QVariantMap v = vars;
    v.insert(QStringLiteral("limit"), QString::number(limit));
    v.insert(QStringLiteral("offset"), QString::number(offset));
    v.insert(QStringLiteral("page"), QString::number(offset / qMax(1, limit) + 1));
    call(ep, v, [this, ep, v, limit, idsPath, fillName, cb](const HttpClient::Reply &r) {
        QVector<Song> preview;
        QStringList ids;
        if (r.ok) {
            const QJsonValue root = Json::rootOf(r.json);
            const QJsonArray arr = Json::arrayAt(root, ep.root);
            for (const QJsonValue &x : arr) {
                Song s = Json::mapSong(x, ep.fields, m_desc.id);
                if (!s.title.isEmpty()) preview << s;
            }
            ids = trackIdsAt(root, idsPath);
        } else {
            qWarning().noquote() << "[source]" << m_desc.id << "detail request failed:" << r.error
                                 << "status=" << r.status << ep.path;
        }
        // trackIds 不比预览多 = 这份歌单本来就短（或接口没给全量 id），照原样返回
        if (ids.size() <= preview.size()) { if (cb) cb(preview); return; }
        if (limit > 0 && ids.size() > limit) ids = ids.mid(0, limit);
        const Endpoint fillEp = m_desc.endpoints.value(fillName);
        if (!fillEp.valid()) {
            qWarning().noquote() << "[source]" << m_desc.id << "fill endpoint missing:" << fillName;
            if (cb) cb(preview);
            return;
        }
        fillSongsByIds(ep, fillEp, v, ids, preview, cb);
    });
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
    requestDetailSongs(ep, v, limit, offset, cb);
}

// 「下一首 / 音乐推荐」：目前只有汽水有免签名的相似歌曲接口；其他平台回调空列表，
// 由调用方（AppController）回退到原来的队列顺序，不影响其他音源的下一首行为。
void MusicSource::related(const Song &song, int limit, SongsCallback cb)
{
    if (m_desc.driver == QLatin1String("qishui") && !song.sourceId.isEmpty()) {
        return qishuiRelated(song.sourceId, limit, cb);
    }
    if (cb) cb(QVector<Song>());
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
    requestDetailSongs(ep, v, limit, offset, cb);
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

// 汽水 KRC 歌词正文："[25240,1800]<0,240,0>Je <240,250,0>me <490,480,0>souviens ...\n[...]"  —— 首段
// "[startMs,dur]" 是本行起播时间（毫秒）；"<>" 包住的是逐词时间标记，剥掉后就是歌词文本。
// 播放器侧统一吃 "[mm:ss.xx]text"（标准 LRC）行（PlayerController::parseLrc），
// 这里把 KRC 转成那一格式；不像 KRC 的行原样保留（兼容普通 LRC/纯文本）。
inline QString qishuiKrcToLrc(const QString &content)
{
    static const QRegularExpression krcStart(
        QRegularExpression(QStringLiteral("^\\[(\\d{1,6}),\\d+\\]")));
    static const QRegularExpression mark(
        QRegularExpression(QStringLiteral("<[^<>]*>")));
    QStringList out;
    const QStringList lines = content.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (QString line : lines) {
        line.remove(QLatin1Char('\r'));
        QString text = line;
        QRegularExpressionMatch m = krcStart.match(line);
        if (m.hasMatch()) {
            text = line.mid(m.capturedEnd(0));
            text.replace(mark, QStringLiteral(""));
            text = text.trimmed();
            if (text.isEmpty()) continue;
            const qint64 startMs = m.captured(1).toLongLong();
            const int secs = int(startMs / 1000);
            const int min = secs / 60;
            const int s = secs % 60;
            const int ms = int(startMs % 1000);
            QString t = QString::number(min) + QLatin1Char(':') + QString::number(s).rightJustified(2, QLatin1Char('0'))
                        + QLatin1Char('.') + QString::number(ms).rightJustified(3, QLatin1Char('0'));
            out << (QStringLiteral("[") + t + QStringLiteral("]") + text);
            continue;
        }
        text = text.trimmed();
        if (!text.isEmpty()) out << text;
    }
    return out.isEmpty() ? QString() : out.join(QLatin1Char('\n'));
}

inline QString qishuiKrcToLrc(const QString &content);

void MusicSource::lyric(const Song &song, LyricCallback cb)
{
    const Endpoint ep = m_desc.endpoints.value(QStringLiteral("lyric"));
    if (!ep.valid()) { if (cb) cb(QString(), QString()); return; }

    // 酷狗：没有单步歌词接口。先搜候选拿 id/accesskey，再下载 base64 编码的 LRC。
    if (ep.extra.value(QStringLiteral("mode")).toString() == QLatin1String("kugouLyric")) {
        const QString tpl = ep.extra.value(QStringLiteral("download")).toString();
        auto download = [this, song, tpl, cb](const QString &id, const QString &key) {
            if (id.isEmpty() || key.isEmpty() || tpl.isEmpty()) {
                if (cb) cb(QString(), QString());
                return;
            }
            QVariantMap dv;
            dv.insert(QStringLiteral("lyricId"), id);
            dv.insert(QStringLiteral("accesskey"), key);
            QVariantMap headers = Json::renderMap(m_desc.headers, songVars(song, QStringLiteral("standard")));
            for (auto it = headers.begin(); it != headers.end();) {
                if (it.value().toString().trimmed().isEmpty()) it = headers.erase(it);
                else ++it;
            }
            HttpClient::instance()->request(
                QStringLiteral("GET"), QUrl(Json::render(tpl, dv)), headers, QByteArray(),
                [cb](const HttpClient::Reply &rr) {
                    QString out;
                    if (rr.ok) {
                        const QString b64 = Json::stringAt(Json::rootOf(rr.json),
                                                          QStringLiteral("content"));
                        if (!b64.isEmpty())
                            out = QString::fromUtf8(QByteArray::fromBase64(b64.toLatin1()));
                    }
                    // 酷狗部分歌词（如稻香）首行是平台塞的翻唱推广，直接丢掉
                    if (out.contains(QStringLiteral("翻唱由"))) {
                        QStringList kept;
                        for (const QString &line : out.split(QLatin1Char('\n')))
                            if (!line.contains(QStringLiteral("翻唱由"))
                                && !line.contains(QStringLiteral("一键翻唱")))
                                kept << line;
                        out = kept.join(QLatin1Char('\n'));
                    }
                    if (!isLyricsUsable(out)) out.clear();
                    if (cb) cb(out, QString());
                });
        };
        // 候选挑选：同名歌曲多，优先时长最接近的；duration 字段不可靠时退回第一个候选
        auto pick = [song](const HttpClient::Reply &r) -> QPair<QString, QString> {
            if (!r.ok) return qMakePair(QString(), QString());
            const QJsonArray list = r.json.object().value(QStringLiteral("candidates")).toArray();
            QString firstId, firstKey, bestId, bestKey;
            qint64 bestDelta = -1;
            for (const QJsonValue &v : list) {
                const QJsonObject c = v.toObject();
                const QString id = c.value(QStringLiteral("id")).toVariant().toString();
                const QString key = c.value(QStringLiteral("accesskey")).toString();
                if (id.isEmpty() || key.isEmpty()) continue;
                if (firstId.isEmpty()) { firstId = id; firstKey = key; }
                const qint64 dur = c.value(QStringLiteral("duration")).toVariant().toLongLong();
                if (song.duration <= 0 || dur <= 0) continue;
                const qint64 delta = qAbs(dur - song.duration);
                if (bestDelta < 0 || delta < bestDelta) {
                    bestDelta = delta;
                    bestId = id;
                    bestKey = key;
                }
            }
            if (!bestId.isEmpty()) return qMakePair(bestId, bestKey);
            return qMakePair(firstId, firstKey);
        };
        const QVariantMap vars = songVars(song, QStringLiteral("standard"));
        call(ep, vars, [this, ep, song, vars, download, pick](const HttpClient::Reply &r1) {
            const QPair<QString, QString> hit = pick(r1);
            if (!hit.first.isEmpty()) { download(hit.first, hit.second); return; }
            // krcs 对关键词很挑：有的歌只能「歌名」搜到（如晴天），有的必须带歌手
            // （如反方向的钟）。第一次为空就用「歌名 歌手」再搜一次。
            QVariantMap v2 = vars;
            v2.insert(QStringLiteral("name"),
                      (song.title + QLatin1Char(' ') + song.artist).trimmed());
            call(ep, v2, [download, pick](const HttpClient::Reply &r2) {
                const QPair<QString, QString> hit2 = pick(r2);
                download(hit2.first, hit2.second);
            });
        });
        return;
    }

    call(ep, songVars(song, QStringLiteral("standard")), [this, ep, cb](const HttpClient::Reply &r) {
        QString out;
        // 只认从 JSON 结构里解析出的歌词字段。此前这里有一段
        // `if (out.isEmpty() && r.json.isEmpty()) out = QString::fromUtf8(r.body)` 的
        // 裸 body 回退：咪咕/网易云的歌词端点被反爬时返回的是完整 HTML 页（HTTP 200），
        // JSON 解析为空 → 整段 HTML 被当成歌词塞给 parseLrc，界面就显示成「乱码」。
        // 删掉裸回退：拿不到结构化歌词就返回空，交给上游跨源兜底。
        if (r.ok) out = Json::firstString(Json::rootOf(r.json), ep.resultPaths).trimmed();
        if (!isLyricsUsable(out)) out.clear();
        // 翻译轨：tlyric/translateLyric/trans 等候选路径，空串表示平台没给翻译。
        QString trans;
        if (r.ok && !ep.transPaths.isEmpty())
            trans = Json::firstString(Json::rootOf(r.json), ep.transPaths).trimmed();
        // 汽水歌词 type="krc"：正文是 "[startMs,dur]<offset,dur>text…" 这种带标记的格式，
        // 转成播放器统一吃的 "[mm:ss.xx]text"（标准 LRC）行后再交给 parseLrc。
        if (m_desc.driver == QLatin1String("qishui") && r.ok) {
            out = qishuiKrcToLrc(out);
        }
        if (cb) cb(out, trans);
    });
}

// 把秒级时间戳格式化为「昨天/MM-DD HH:mm」这类展示文本
static QString commentTimeText(qint64 ms)
{
    if (ms <= 0) return QString();
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(ms);
    const QDateTime now = QDateTime::currentDateTime();
    const qint64 days = dt.daysTo(now);
    if (days == 0) return QStringLiteral("今天 ") + dt.toString(QStringLiteral("HH:mm"));
    if (days == 1) return QStringLiteral("昨天 ") + dt.toString(QStringLiteral("HH:mm"));
    if (days < 7) return dt.toString(QStringLiteral("MM-DD HH:mm"));
    return dt.toString(QStringLiteral("yyyy-MM-dd"));
}

static QString md5Hex(const QString &text)
{
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Md5).toHex());
}

// QQ 评论正文带 [em]e400822[/em] 表情码和字面 "\\n"，清理成纯文本
static QString cleanQqComment(QString s)
{
    s.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
    static const QRegularExpression em(QStringLiteral("\\[em\\]e\\d+\\[/em\\]"));
    s.remove(em);
    return s.trimmed();
}

// 评论平台标识：sources.json 里只有汽水声明了顶层 driver，其余内置平台为空，
// 按 id 分发（此前按 driver 分发导致内置平台评论分支全部落空，永远返回「暂无评论」）
static QString commentPlatformOf(const SourceDesc &desc)
{
    return desc.driver.isEmpty() ? desc.id : desc.driver;
}

void MusicSource::comments(const Song &song, int page, CommentCallback cb)
{
    auto done = [cb](const QVector<Comment> &list, int total) {
        if (cb) cb(list, total);
    };
    const QString id = song.sourceId;
    if (id.isEmpty() || page < 1) { done(QVector<Comment>(), -1); return; }
    const int pageSize = 20;
    const QString plat = commentPlatformOf(m_desc);

    if (plat == QLatin1String("migu")) {
        // 咪咕评论：GET app.c.nf.migu.cn/MIGUM3.0/user/comment/stack/v1.0
        // 成功码字符串 "000000"，热评 queryType=2、最新 queryType=1，游标分页。
        const QString url = QStringLiteral("https://app.c.nf.migu.cn/MIGUM3.0/user/comment/stack/v1.0")
            + QStringLiteral("?pageSize=%1&queryType=%2&resourceId=%3&resourceType=2&commentId=")
                  .arg(pageSize).arg(page == 1 ? 2 : 1).arg(id);
        QVariantMap h;
        h.insert(QStringLiteral("User-Agent"), QStringLiteral("Mozilla/5.0"));
        h.insert(QStringLiteral("Referer"), QStringLiteral("https://app.c.nf.migu.cn/"));
        HttpClient::instance()->request(QStringLiteral("GET"), QUrl(url), h, QByteArray(),
            [done](const HttpClient::Reply &r) {
                QVector<Comment> out;
                int total = -1;
                if (r.ok) {
                    const QJsonValue root = Json::rootOf(r.json);
                    if (Json::stringAt(root, QStringLiteral("code")) == QLatin1String("000000")) {
                        const QJsonValue data = Json::valueAt(root, QStringLiteral("data"));
                        total = Json::stringAt(data, QStringLiteral("commentNums")).toInt();
                        const QJsonArray arr = Json::arrayAt(data, QStringLiteral("comments"));
                        for (const QJsonValue &v : arr) {
                            Comment c;
                            c.source = QStringLiteral("migu");
                            c.id = Json::stringAt(v, QStringLiteral("commentId"));
                            c.text = Json::stringAt(v, QStringLiteral("content"));
                            c.userName = Json::firstString(v, {QStringLiteral("nickname"), QStringLiteral("userName")});
                            c.avatar = Json::firstString(v, {QStringLiteral("avatar"), QStringLiteral("userAvatar")});
                            const qint64 sec = Json::stringAt(v, QStringLiteral("createTime")).toLongLong();
                            c.time = sec > 0 ? sec * 1000 : 0;
                            c.timeText = commentTimeText(c.time);
                            c.likedCount = Json::stringAt(v, QStringLiteral("likeCount")).toInt();
                            c.replyCount = Json::stringAt(v, QStringLiteral("replyCount")).toInt();
                            if (!c.text.isEmpty()) out << c;
                        }
                    }
                }
                done(out, total);
            });
        return;
    }

    if (plat == QLatin1String("kuwo")) {
        // 酷我评论：GET ncomment.kuwo.cn/com.s，无签名。第 1 页取热评+最新，后续取最新。
        const int start = (page - 1) * pageSize;
        const QString url = QStringLiteral("http://ncomment.kuwo.cn/com.s?f=web&type=%1")
            + QStringLiteral("&aapiver=1&prod=kwplayer_ar_10.5.2.0&digest=15&sid=%2&start=%3")
                  .arg(page == 1 ? QStringLiteral("get_rec_comment") : QStringLiteral("get_comment"),
                       id, QString::number(start))
            + QStringLiteral("&msgflag=1&count=%1&newver=3&uid=0").arg(pageSize);
        QVariantMap h;
        h.insert(QStringLiteral("User-Agent"), QStringLiteral("Dalvik/2.1.0 (Linux; U; Android 9;)"));
        HttpClient::instance()->request(QStringLiteral("GET"), QUrl(url), h, QByteArray(),
            [done](const HttpClient::Reply &r) {
                QVector<Comment> out;
                int total = -1;
                if (r.ok) {
                    const QJsonValue root = Json::rootOf(r.json);
                    if (Json::stringAt(root, QStringLiteral("code")) == QLatin1String("200")) {
                        total = Json::stringAt(root, QStringLiteral("comments_counts")).toInt();
                        QJsonArray arr = Json::arrayAt(root, QStringLiteral("hot_comments"));
                        if (arr.isEmpty()) arr = Json::arrayAt(root, QStringLiteral("comments"));
                        for (const QJsonValue &v : arr) {
                            Comment c;
                            c.source = QStringLiteral("kuwo");
                            c.id = Json::stringAt(v, QStringLiteral("id"));
                            c.text = Json::stringAt(v, QStringLiteral("msg"));
                            c.userName = Json::stringAt(v, QStringLiteral("u_name"));
                            c.avatar = Json::stringAt(v, QStringLiteral("u_pic"));
                            const qint64 sec = Json::stringAt(v, QStringLiteral("time")).toLongLong();
                            c.time = sec > 0 ? sec * 1000 : 0;
                            c.timeText = commentTimeText(c.time);
                            c.likedCount = Json::stringAt(v, QStringLiteral("like_num")).toInt();
                            if (!c.text.isEmpty()) out << c;
                        }
                    }
                }
                done(out, total);
            });
        return;
    }

    if (plat == QLatin1String("qq")) {
        // QQ 评论：songmid → songid（fcg_play_single_song）→ fcg_global_comment_h5（topid=songid）
        QVariantMap h;
        h.insert(QStringLiteral("User-Agent"), QStringLiteral("Mozilla/5.0"));
        h.insert(QStringLiteral("Referer"), QStringLiteral("https://y.qq.com/"));
        const QUrl infoUrl(QStringLiteral("https://c.y.qq.com/v8/fcg-bin/fcg_play_single_song.fcg?songmid=%1&format=json").arg(id));
        HttpClient::instance()->request(QStringLiteral("GET"), infoUrl, h, QByteArray(),
            [done, page, h](const HttpClient::Reply &r) {
                qint64 songId = 0;
                if (r.ok) {
                    const QJsonValue root = Json::rootOf(r.json);
                    QJsonArray arr = root.isArray() ? root.toArray() : Json::arrayAt(root, QStringLiteral("data"));
                    if (!arr.isEmpty())
                        songId = arr.at(0).toObject().value(QStringLiteral("id")).toVariant().toLongLong();
                }
                if (songId <= 0) { done(QVector<Comment>(), -1); return; }
                const QUrl url(QStringLiteral("https://c.y.qq.com/base/fcgi-bin/fcg_global_comment_h5.fcg")
                    + QStringLiteral("?g_tk=5381&loginUin=0&hostUin=0&format=json&inCharset=utf8&outCharset=utf-8")
                    + QStringLiteral("&notice=0&platform=yqq.json&needNewCode=0&cid=205360772&reqtype=2&biztype=1")
                    + QStringLiteral("&topid=%1&cmd=8&needmusiccrit=0&pagenum=%2&pagesize=%3&domain=qq.com&ct=24&cv=10101010")
                          .arg(songId).arg(page - 1).arg(20));
                HttpClient::instance()->request(QStringLiteral("GET"), url, h, QByteArray(),
                    [done](const HttpClient::Reply &r2) {
                        QVector<Comment> out;
                        int total = -1;
                        if (r2.ok) {
                            const QJsonValue root = Json::rootOf(r2.json);
                            const QJsonValue cmt = Json::valueAt(root, QStringLiteral("comment"));
                            total = Json::stringAt(cmt, QStringLiteral("commenttotal")).toInt();
                            const QJsonArray arr = Json::arrayAt(cmt, QStringLiteral("commentlist"));
                            for (const QJsonValue &v : arr) {
                                Comment c;
                                c.source = QStringLiteral("qq");
                                c.id = Json::stringAt(v, QStringLiteral("commentid"));
                                // 楼层回复的正文不在 rootcommentcontent（为空），
                                // 而在 middlecommentcontent[0].subcommentcontent
                                c.text = cleanQqComment(Json::stringAt(v, QStringLiteral("rootcommentcontent")));
                                if (c.text.isEmpty()) {
                                    const QJsonArray mid = Json::arrayAt(v, QStringLiteral("middlecommentcontent"));
                                    if (!mid.isEmpty()) {
                                        c.text = cleanQqComment(Json::stringAt(
                                            mid.at(0), QStringLiteral("subcommentcontent")));
                                        const QString replyed = Json::stringAt(mid.at(0),
                                            QStringLiteral("replyednick"));
                                        if (!replyed.isEmpty()) c.replyText = replyed;
                                    }
                                }
                                c.userName = Json::stringAt(v, QStringLiteral("nick"));
                                c.avatar = Json::stringAt(v, QStringLiteral("avatarurl"));
                                const qint64 sec = Json::stringAt(v, QStringLiteral("time")).toLongLong();
                                c.time = sec > 0 ? sec * 1000 : 0;
                                c.timeText = commentTimeText(c.time);
                                c.likedCount = Json::stringAt(v, QStringLiteral("praisenum")).toInt();
                                if (!c.text.isEmpty()) out << c;
                            }
                        }
                        done(out, total);
                    });
            });
        return;
    }

    if (plat == QLatin1String("kugou")) {
        // 酷狗评论：songId 是 hash，而评论接口只认 mixsongid（=album_audio_id）。
        // v3 搜索结果同时带 hash 与 album_audio_id，按 hash 精确匹配换 id 后再取评论。
        const QString kw = (song.title + QLatin1Char(' ') + song.artist).trimmed();
        const QUrl sUrl(QStringLiteral("http://mobilecdnbj.kugou.com/api/v3/search/song?pagesize=30&page=1&keyword=%1")
                            .arg(QString::fromUtf8(QUrl::toPercentEncoding(kw))));
        HttpClient::instance()->request(QStringLiteral("GET"), sUrl, QVariantMap(), QByteArray(),
            [done, page, id](const HttpClient::Reply &r) {
                qint64 mixId = 0;
                if (r.ok) {
                    const QJsonValue root = Json::rootOf(r.json);
                    const QJsonArray arr = Json::arrayAt(root, QStringLiteral("data.info"));
                    for (const QJsonValue &v : arr) {
                        const QJsonObject o = v.toObject();
                        if (o.value(QStringLiteral("hash")).toString().compare(id, Qt::CaseInsensitive) == 0) {
                            mixId = o.value(QStringLiteral("album_audio_id")).toVariant().toLongLong();
                            break;
                        }
                    }
                }
                if (mixId <= 0) { done(QVector<Comment>(), -1); return; }
                // Android 网关签名：md5(salt + 按 key 排序的 "k=v" 拼接 + body + salt)
                const QString mid = md5Hex(QStringLiteral("-"));
                const QString uuid = md5Hex(QStringLiteral("-") + mid);
                const QString now = QString::number(QDateTime::currentSecsSinceEpoch());
                QMap<QString, QString> p;   // QMap 按 key 有序，直接拼接即签名顺序
                p.insert(QStringLiteral("appid"), QStringLiteral("1005"));
                p.insert(QStringLiteral("clienttime"), now);
                p.insert(QStringLiteral("clientver"), QStringLiteral("12569"));
                p.insert(QStringLiteral("code"), QStringLiteral("fc4be23b4e972707f36b8a828a93ba8a"));
                p.insert(QStringLiteral("dfid"), QStringLiteral("-"));
                p.insert(QStringLiteral("extdata"), QStringLiteral("0"));
                p.insert(QStringLiteral("mid"), mid);
                p.insert(QStringLiteral("mixsongid"), QString::number(mixId));
                p.insert(QStringLiteral("need_show_image"), QStringLiteral("1"));
                p.insert(QStringLiteral("p"), QString::number(page));
                p.insert(QStringLiteral("pagesize"), QString::number(pageSize));
                p.insert(QStringLiteral("show_classify"), QStringLiteral("1"));
                p.insert(QStringLiteral("show_hotword_list"), QStringLiteral("1"));
                p.insert(QStringLiteral("userid"), QStringLiteral("0"));
                p.insert(QStringLiteral("uuid"), uuid);
                QStringList kv;
                for (auto it = p.constBegin(); it != p.constEnd(); ++it)
                    kv << it.key() + QLatin1Char('=') + it.value();
                const QString sig = md5Hex(QStringLiteral("OIlwieks28dk2k092lksi2UIkp")
                                           + kv.join(QString()) + QString()
                                           + QStringLiteral("OIlwieks28dk2k092lksi2UIkp"));
                QUrlQuery q;
                for (auto it = p.constBegin(); it != p.constEnd(); ++it)
                    q.addQueryItem(it.key(), it.value());
                q.addQueryItem(QStringLiteral("signature"), sig);
                QUrl url(QStringLiteral("https://gateway.kugou.com/mcomment/v1/cmtlist"));
                url.setQuery(q);
                QVariantMap h;
                h.insert(QStringLiteral("User-Agent"),
                         QStringLiteral("Android15-1070-11083-46-0-DiscoveryDRADProtocol-wifi"));
                h.insert(QStringLiteral("dfid"), QStringLiteral("-"));
                h.insert(QStringLiteral("clienttime"), now);
                h.insert(QStringLiteral("mid"), mid);
                HttpClient::instance()->request(QStringLiteral("POST"), url, h, QByteArray(),
                    [done](const HttpClient::Reply &r2) {
                        QVector<Comment> out;
                        int total = -1;
                        if (r2.ok) {
                            const QJsonValue root = Json::rootOf(r2.json);
                            if (Json::stringAt(root, QStringLiteral("status")).toInt() == 1) {
                                total = Json::stringAt(root, QStringLiteral("count")).toInt();
                                const QJsonArray arr = Json::arrayAt(root, QStringLiteral("list"));
                                for (const QJsonValue &v : arr) {
                                    Comment c;
                                    c.source = QStringLiteral("kugou");
                                    c.id = Json::stringAt(v, QStringLiteral("id"));
                                    c.text = Json::stringAt(v, QStringLiteral("content"));
                                    c.userName = Json::stringAt(v, QStringLiteral("user_name"));
                                    c.avatar = Json::stringAt(v, QStringLiteral("user_pic"));
                                    // addtime 是 "2023-12-28 02:43:51" 文本，直接展示
                                    c.timeText = Json::stringAt(v, QStringLiteral("addtime"));
                                    c.likedCount = 0;
                                    c.replyCount = Json::stringAt(v, QStringLiteral("reply_num")).toInt();
                                    if (!c.text.isEmpty()) out << c;
                                }
                            }
                        }
                        done(out, total);
                    });
            });
        return;
    }

    if (plat == QLatin1String("netease")) {
        // 网易云评论：POST weapi/comment/resource/comments/get，rid = R_SO_4_<songId>
        // 用项目已有的 NeteaseCrypto::weapiForm 生成 {params, encSecKey} 表单。
        const QString rid = QStringLiteral("R_SO_4_") + id;
        const qint64 offset = (page - 1) * pageSize;
        QJsonObject payload;
        payload.insert(QStringLiteral("rid"), rid);
        payload.insert(QStringLiteral("threadId"), rid);
        payload.insert(QStringLiteral("cursor"), offset);
        payload.insert(QStringLiteral("offset"), offset);
        payload.insert(QStringLiteral("orderType"), 1);
        payload.insert(QStringLiteral("pageNo"), page);
        payload.insert(QStringLiteral("pageSize"), pageSize);
        // payload 字段与洛雪 wy/comment.js 的 weapi 参数逐项一致，不额外塞 total/csrf_token
        const QByteArray body = Netease::weapiForm(payload);
        QVariantMap h;
        h.insert(QStringLiteral("User-Agent"),
                 QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/60.0.3112.90 Safari/537.36"));
        h.insert(QStringLiteral("Origin"), QStringLiteral("https://music.163.com"));
        h.insert(QStringLiteral("Referer"), QStringLiteral("http://music.163.com/"));
        h.insert(QStringLiteral("Content-Type"), QStringLiteral("application/x-www-form-urlencoded"));
        HttpClient::instance()->request(QStringLiteral("POST"),
            QUrl(QStringLiteral("https://music.163.com/weapi/comment/resource/comments/get")),
            h, body, [done](const HttpClient::Reply &r) {
                QVector<Comment> out;
                int total = -1;
                if (r.ok) {
                    const QJsonValue root = Json::rootOf(r.json);
                    if (Json::stringAt(root, QStringLiteral("code")).toInt() == 200) {
                        const QJsonValue data = Json::valueAt(root, QStringLiteral("data"));
                        total = Json::stringAt(data, QStringLiteral("totalCount")).toInt();
                        const QJsonArray hot = Json::arrayAt(data, QStringLiteral("hotComments"));
                        const QJsonArray arr = Json::arrayAt(data, QStringLiteral("comments"));
                        auto parse = [&out](const QJsonArray &a) {
                            for (const QJsonValue &v : a) {
                                Comment c;
                                c.source = QStringLiteral("netease");
                                c.id = Json::stringAt(v, QStringLiteral("commentId"));
                                c.text = Json::stringAt(v, QStringLiteral("content"));
                                const QJsonValue u = Json::valueAt(v, QStringLiteral("user"));
                                c.userName = Json::stringAt(u, QStringLiteral("nickname"));
                                c.avatar = Json::stringAt(u, QStringLiteral("avatarUrl"));
                                const qint64 sec = Json::stringAt(v, QStringLiteral("time")).toLongLong();
                                c.time = sec > 0 ? sec : 0;
                                c.timeText = commentTimeText(c.time);
                                c.likedCount = Json::stringAt(v, QStringLiteral("likedCount")).toInt();
                                const QJsonValue rsp = Json::valueAt(v, QStringLiteral("reply"));
                                c.replyCount = Json::stringAt(rsp, QStringLiteral("replyCount")).toInt();
                                c.replyText = Json::stringAt(rsp, QStringLiteral("content"));
                                if (!c.text.isEmpty()) out << c;
                            }
                        };
                        if (Json::arrayAt(data, QStringLiteral("hotComments")).size() > 0) parse(hot);
                        parse(arr);
                    }
                }
                done(out, total);
            });
        return;
    }

    // 其它平台（QQ/酷狗/汽水）暂未配置评论解析，明确回空，UI 显示「该平台暂无评论」
    done(QVector<Comment>(), -1);
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
    // 腾讯视频（QQ 音乐 MV 的实际托管方）回包是 QZOutputJson={...};，不是纯 JSON，
    // 播放地址还得把 ul.ui[0].url + fn + ?vkey=fvkey 拼起来
    const bool tencent = mvEp.extra.value(QStringLiteral("mode")).toString()
                         == QLatin1String("tencentVideo");
    auto urlOf = [mvEp, tencent](const HttpClient::Reply &r) -> QString {
        if (!r.ok) return QString();
        if (!tencent)
            return Json::firstString(Json::rootOf(r.json), mvEp.resultPaths);
        QByteArray raw = r.body.trimmed();
        const int eq = raw.indexOf('=');
        if (eq >= 0) raw = raw.mid(eq + 1);
        if (raw.endsWith(';')) raw.chop(1);
        const QJsonValue root = QJsonDocument::fromJson(raw).object();
        const QJsonValue vi = Json::valueAt(root, QStringLiteral("vl.vi[0]"));
        const QString base = Json::stringAt(vi, QStringLiteral("ul.ui[0].url"));
        const QString fn = Json::stringAt(vi, QStringLiteral("fn"));
        const QString key = Json::stringAt(vi, QStringLiteral("fvkey"));
        if (base.isEmpty() || fn.isEmpty() || key.isEmpty()) return QString();
        return base + fn + QStringLiteral("?vkey=") + key;
    };
    auto deliverUrl = [this, mvEp, song, cb, tencent, urlOf](const QString &mvId) {
        if (mvId.isEmpty() || mvId == QLatin1String("0")) {
            if (cb) cb(false, QUrl(), QStringLiteral("该歌曲没有 MV"));
            return;
        }
        auto attempt = [this, song, tencent](const QString &id, const QString &r) {
            QVariantMap v = songVars(song, QStringLiteral("standard"));
            v.insert(QStringLiteral("mvId"), id);
            // 腾讯视频用 fhd/shd/hd/sd 描述清晰度，其它平台直接吃 1080/480
            v.insert(QStringLiteral("mvR"),
                     tencent ? (r == QLatin1String("1080") ? QStringLiteral("fhd")
                                                           : QStringLiteral("hd"))
                             : r);
            return v;
        };
        call(mvEp, attempt(mvId, QStringLiteral("1080")),
             [this, mvEp, song, mvId, cb, attempt, urlOf](const HttpClient::Reply &r1) {
            const QString url = urlOf(r1);
            if (!url.isEmpty()) { if (cb) cb(true, QUrl(url), QString()); return; }
            // 高清源缺失（老 MV 只有 480p）退一档再试一次
            call(mvEp, attempt(mvId, QStringLiteral("480")), [mvEp, cb, urlOf](const HttpClient::Reply &r2) {
                const QString url2 = urlOf(r2);
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

void MusicSource::mvSearch(const QString &keyword, int limit, SongsCallback cb)
{
    const Endpoint ep = m_desc.endpoints.value(QStringLiteral("mvSearch"));
    if (!ep.valid()) {
        if (cb) cb(QVector<Song>());
        return;
    }
    QVariantMap v = baseVars();
    v.insert(QStringLiteral("keyword"), keyword);
    v.insert(QStringLiteral("limit"), QString::number(limit));
    v.insert(QStringLiteral("offset"), QStringLiteral("0"));
    call(ep, v, [this, ep, cb](const HttpClient::Reply &r) {
        QVector<Song> out;
        if (r.ok) {
            const QJsonArray arr = Json::arrayAt(Json::rootOf(r.json), ep.root);
            for (const QJsonValue &val : arr) {
                Song s = Json::mapSong(val, ep.fields, m_desc.id);
                if (!s.title.isEmpty()) out << s;
            }
        } else {
            qWarning().noquote() << "[source]" << m_desc.id << "mv search failed:"
                                 << r.error << "status=" << r.status;
        }
        if (cb) cb(out);
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
    // 自己的歌单同样会只回预览曲目（网易云的「我喜欢的音乐」也是普通歌单），
    // 走和公共歌单一致的 trackIds 补全路径
    const int limit = qBound(1, Settings::integer(QStringLiteral("playlistMaxSongs"), 1000), 5000);
    QVariantMap v = baseVars();
    v.insert(QStringLiteral("id"), id);
    v.insert(QStringLiteral("limit"), QString::number(limit));
    requestDetailSongs(ep, v, limit, 0, cb);
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
                                  QStringLiteral("items"), QStringLiteral("results")};
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
    s.cover  = Json::coverUrlOf(album, QStringLiteral("url_cover"));
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
//   - {uri, urls:[prefix,...], template_prefix} 对象（发现流 discover/mix 的 url_cover）。
// 对象形态必须补 template_prefix 模板后缀，否则 CDN 一律 400，交给 Json::coverUrlOf 统一处理。
inline QString qishuiCoverOf(const QJsonValue &p)
{
    const QString cover = Json::coverUrlOf(p, QStringLiteral("url_cover"));
    if (!cover.isEmpty()) return cover;
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
    // 同 qishuiRank：公开歌单详情匿名访问，避免登录 Cookie 污染导致空列表
    v.insert(QStringLiteral("cookie"), QString());
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

// GET /luna/track/related：items[].entity.track_wrapper.track，和歌单详情同一套 track 结构。
// 用 qishuiCollectItems 兜住 items/list 等容器差异，再用 qishuiUnwrapTrack 展平。
void MusicSource::qishuiRelated(const QString &trackId, int limit, SongsCallback cb)
{
    const Endpoint ep = m_desc.endpoints.value(QStringLiteral("related"));
    if (!ep.valid()) {
        if (cb) cb(QVector<Song>());
        return;
    }
    QVariantMap v = baseVars();
    // 相似推荐同样是公开接口，登录后也用匿名请求
    v.insert(QStringLiteral("cookie"), QString());
    v.insert(QStringLiteral("id"), trackId);
    v.insert(QStringLiteral("limit"), QString::number(limit));
    call(ep, v, [this, cb](const HttpClient::Reply &r) {
        QVector<Song> out;
        if (r.ok) {
            const QJsonValue root = Json::rootOf(r.json);
            const QJsonArray items = qishuiCollectItems(root);
            for (const QJsonValue &d : items) {
                const QJsonValue t = qishuiUnwrapTrack(d);
                if (t.isNull()) continue;
                if (Json::stringAt(t, QStringLiteral("id")).isEmpty()
                    || Json::stringAt(t, QStringLiteral("name")).isEmpty())
                    continue;
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
    // 歌单搜索是公开数据：登录后 baseVars 会把 cookie 注入源级 Cookie 头，
    // /luna/pc/* 族带登录态反而被拒/空返回。与 qishuiRank/qishuiRelated 一致显式匿名。
    // 注：/luna/pc/* 还需官方 bdms.node 动态签名，配置注释已记录该限制（静态签名返回
    // 1000006）；签名需设备密钥，当前无可用的免签名歌单搜索端点，故本路可能仍为空，
    // 由其它音源的歌单搜索兜底。
    v.insert(QStringLiteral("cookie"), QString());
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
    // 推荐歌单/榜单是公开接口：登录后 m_session.cookie 会被 baseVars 填进
    // 源级 "Cookie: {cookie}" 头，发给 beta-luna/api.qishui.com 会被拒或返回空，
    // 表现就是「登录前有权单，登录后没歌单」。这里显式清空让 buildUrl 丢掉该头。
    v.insert(QStringLiteral("cookie"), QString());
    v.insert(QStringLiteral("limit"), QString::number(limit));
    v.insert(QStringLiteral("offset"), QString::number(offset));
    v.insert(QStringLiteral("page"), QString::number(offset / qMax(1, limit) + 1));

    // 发现流结构：inner_block[].resources[].entity.playlist
    const auto parse = [this](const HttpClient::Reply &r) {
        QVector<Playlist> out;
        if (!r.ok) return out;
        const QJsonValue root = Json::rootOf(r.json);
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
        return out;
    };

    // 真机实测：beta-luna.douyin.com 在部分网络下 DNS/连接失败，导致「我的歌单」
    // 里汽水一个官方歌单都没有。api.qishui.com 是搜索/详情/账号共用的主域名，
    // 同样支持 /luna/discover/mix（实测返回 8 个推荐歌单），真机网络必通。
    // 故配置端点（beta-luna）失败或空时，改打主域名重试一次，再回退静态 defaultBoards。
    call(ep, v, [this, cb, ep, v, parse](const HttpClient::Reply &r) {
        const QVector<Playlist> first = parse(r);
        if (!first.isEmpty()) { if (cb) cb(first); return; }
        if (!ep.path.startsWith(m_desc.base, Qt::CaseInsensitive)) {
            Endpoint fallback = ep;
            fallback.path = m_desc.base + QStringLiteral("/luna/discover/mix");
            call(fallback, v, [this, cb, parse](const HttpClient::Reply &r2) {
                const QVector<Playlist> second = parse(r2);
                if (cb) cb(second.isEmpty() ? defaultBoards() : second);
            });
            return;
        }
        // 接口偶发失败/被限流时回退静态默认榜单，避免空白
        if (cb) cb(defaultBoards());
    });
}

} // namespace Furui
