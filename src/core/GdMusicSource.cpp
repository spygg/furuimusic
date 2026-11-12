#include "core/QtCompat.h"
#include "GdMusicSource.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

#include "HttpClient.h"
#include "Settings.h"

namespace Furui {

namespace {

const char *kBaseUrl = "https://music-api.gdstudio.xyz/api.php";
// 与 AlgerMusicPlayer 内置顺序一致（其实测 joox/tidal 命中率高；netease 兜底）
const char *kSources[] = { "joox", "tidal", "netease" };
constexpr int kSourceCount = 3;
constexpr int kTimeoutMs = 8000;   // 参考实现搜索/取链各 5s，这里合并成单次 8s

using Callback = GdMusicSource::PlayableCallback;
void trySource(int index, const Song &song, Callback cb);   // 前置声明：fetchUrl 失败要回退到下一个上游

// 歌手串按 feat./&/,/、 等拆成归一化列表
QStringList expectedArtists(const Song &song)
{
    static const QRegularExpression sep(
        QStringLiteral("[/&,，、；;]|feat\\.|Feat\\."));
    QStringList out;
    const QStringList parts = song.artist.split(sep, Qt::SkipEmptyParts);
    for (const QString &a : parts) {
        const QString n = normalize(a.trimmed());
        if (!n.isEmpty()) out << n;
    }
    return out;
}

// 候选与期望的歌名是否匹配（归一化后互为包含即算）
bool nameMatched(const QString &want, const QString &candidate)
{
    const QString w = normalize(want);
    const QString c = normalize(candidate);
    if (w.isEmpty() || c.isEmpty()) return false;
    return w == c || c.contains(w) || w.contains(c);
}

// 候选 artist 字段可能是字符串，也可能是 [{name:...}] 数组（与参考实现同款形状）
QString candidateArtistText(const QJsonObject &item)
{
    const QJsonValue a = item.value(QStringLiteral("artist"));
    if (a.isString()) return a.toString();
    if (a.isArray()) {
        QStringList names;
        for (const QJsonValue &v : a.toArray()) {
            const QString n = v.toObject().value(QStringLiteral("name")).toString();
            if (!n.isEmpty()) names << n;
        }
        return names.join(QStringLiteral(" "));
    }
    return QString();
}

// 候选歌手是否匹配：原曲无歌手信息或候选缺歌手时放行（低置信候选，与参考实现一致）
bool artistMatched(const QStringList &want, const QString &candidate)
{
    if (want.isEmpty()) return true;
    const QString have = normalize(candidate);
    if (have.isEmpty()) return true;
    for (const QString &w : want) {
        if (!w.isEmpty() && (have.contains(w) || w.contains(have))) return true;
    }
    return false;
}

// 从 count=5 的候选里挑与原曲匹配的一条：歌名必须匹配，候选带歌手时歌手也必须匹配。
// 返回空 QJsonObject 表示全部拒绝（宁可失败不返回错歌）。
QJsonObject pickBest(const QJsonArray &list, const Song &song)
{
    const QStringList wantArtists = expectedArtists(song);
    QJsonObject best;
    int bestScore = 0;
    for (const QJsonValue &v : list) {
        const QJsonObject item = v.toObject();
        const QJsonValue idv = item.value(QStringLiteral("id"));
        const QString id = idv.isString() ? idv.toString()
                         : (idv.isDouble() ? QString::number(idv.toDouble()) : QString());
        if (id.isEmpty()) continue;
        const QString candName = item.value(QStringLiteral("name")).toString();
        if (!nameMatched(song.title, candName)) continue;

        const QString candArtist = candidateArtistText(item);
        if (!artistMatched(wantArtists, candArtist)) continue;

        int score;
        if (wantArtists.isEmpty()) score = 2;
        else if (normalize(candArtist).isEmpty()) score = 1;
        else score = 3;
        if (score > bestScore) { best = item; bestScore = score; }
    }
    return best;
}

Playable makeSuccess(const QJsonObject &item, const QString &trackSource)
{
    Playable p;
    p.ok = true;
    p.url = QUrl(item.value(QStringLiteral("url")).toString());
    p.substituted = true;
    p.substituteSource = QStringLiteral("GD音乐台 · %1").arg(trackSource);
    p.message = QStringLiteral("已由内置音源「GD音乐台 · %1」提供").arg(trackSource);
    return p;
}

void finishFail(Callback cb)
{
    Playable fail;
    fail.reason = QStringLiteral("empty");
    fail.message = QStringLiteral("GD音乐台未解析到可用音频");
    if (cb) cb(fail);
}

void fetchUrl(int index, const Song &song, const QString &trackSource,
              const QString &trackId, Callback cb)
{
    QUrl url(QString::fromLatin1(kBaseUrl));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("types"), QStringLiteral("url"));
    q.addQueryItem(QStringLiteral("source"), trackSource);
    q.addQueryItem(QStringLiteral("id"), trackId);
    q.addQueryItem(QStringLiteral("br"), QStringLiteral("999"));
    url.setQuery(q);
    HttpClient::instance()->request(
        QStringLiteral("GET"), url, QVariantMap(), QByteArray(),
        [index, song, trackSource, cb](const HttpClient::Reply &r) {
            if (r.ok && r.json.isObject()) {
                const QJsonObject obj = r.json.object();
                const QJsonValue u = obj.value(QStringLiteral("url"));
                if (u.isString() && !u.toString().isEmpty()) {
                    QJsonObject item = obj;
                    item.insert(QStringLiteral("url"), u);
                    Playable p = makeSuccess(item, trackSource);
                    if (cb) cb(p);
                    return;
                }
            }
            // 该上游没取到链接 → 换下一个上游
            trySource(index + 1, song, cb);
        }, kTimeoutMs);
}

void trySource(int index, const Song &song, Callback cb)
{
    if (index >= kSourceCount) { finishFail(cb); return; }
    const QString src = QString::fromLatin1(kSources[index]);
    const QString query = (song.title + QLatin1Char(' ') + song.artist).trimmed();
    if (query.isEmpty()) { finishFail(cb); return; }

    QUrl url(QString::fromLatin1(kBaseUrl));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("types"), QStringLiteral("search"));
    q.addQueryItem(QStringLiteral("source"), src);
    q.addQueryItem(QStringLiteral("name"), query);
    q.addQueryItem(QStringLiteral("count"), QStringLiteral("5"));
    q.addQueryItem(QStringLiteral("pages"), QStringLiteral("1"));
    url.setQuery(q);

    HttpClient::instance()->request(
        QStringLiteral("GET"), url, QVariantMap(), QByteArray(),
        [index, song, src, cb](const HttpClient::Reply &r) {
            if (!r.ok || !r.json.isArray()) { trySource(index + 1, song, cb); return; }
            const QJsonObject best = pickBest(r.json.array(), song);
            if (best.isEmpty()) { trySource(index + 1, song, cb); return; }
            // 候选可能带自己的 source 字段（聚合结果），取链以它为准
            const QString trackSource = best.value(QStringLiteral("source")).toString().isEmpty()
                ? src : best.value(QStringLiteral("source")).toString();
            const QJsonValue idv = best.value(QStringLiteral("id"));
            const QString trackId = idv.isString() ? idv.toString() : QString::number(idv.toDouble());
            fetchUrl(index, song, trackSource, trackId, cb);
        }, kTimeoutMs);
}

} // namespace

void GdMusicSource::resolve(const Song &song, const QString &quality, PlayableCallback cb)
{
    Q_UNUSED(quality);   // 始终请求 br=999（服务端给最高可用音质），与参考实现一致
    if (!Settings::boolean(QStringLiteral("gdMusicSource"), true)) {
        Playable skip;
        skip.reason = QStringLiteral("disabled");
        skip.message = QStringLiteral("内置 GD音乐台音源已在设置中关闭");
        if (cb) cb(skip);
        return;
    }
    if (song.title.trimmed().isEmpty()) { finishFail(cb); return; }
    trySource(0, song, cb);
}

} // namespace Furui
