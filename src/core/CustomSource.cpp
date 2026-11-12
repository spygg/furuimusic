#include "core/QtCompat.h"
#include "CustomSource.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QUrlQuery>

#include "HttpClient.h"
#include "JsonMapper.h"
#include "Library.h"
#include "GdMusicSource.h"
#include "ScriptSource.h"
#include "Settings.h"

namespace Furui {

static void tryScriptSources(const Song &song, const QString &quality,
                             CustomSourceManager::PlayableCallback cb)
{
    // 默认音源：GD音乐台（内置）先行；关闭或没解析到再交给用户导入的 LX 在线脚本音源
    GdMusicSource::resolve(song, quality, [song, quality, cb](const Playable &gd) {
        if (gd.ok) {
            if (cb) cb(gd);
            return;
        }
        ScriptSourceManager::instance()->resolve(song, quality, [cb](const Playable &p) {
            if (p.ok) {
                if (cb) cb(p);
                return;
            }
            Playable fail;
            // 之前把脚本侧的具体原因吞掉了，界面只会显示「未找到替代音频」，
            // 用户分不清是没导入音源、音源初始化失败还是服务端 404。这里原样透传。
            fail.reason = p.reason;
            fail.message = p.message.isEmpty()
                ? QStringLiteral("自定义音源未找到可用的替代音频")
                : QStringLiteral("自定义音源未找到可用的替代音频（%1）").arg(p.message);
            if (cb) cb(fail);
        });
    });
}

CustomSourceManager *CustomSourceManager::instance()
{
    static CustomSourceManager *c = new CustomSourceManager();
    return c;
}

CustomSourceManager::CustomSourceManager(QObject *parent)
    : QObject(parent)
{
    refresh();
}

void CustomSourceManager::refresh()
{
    m_items.clear();
    const QVariantList rows = Library::instance()->customSources();
    for (const QVariant &v : rows) {
        const QVariantMap m = v.toMap();
        Item i;
        i.id      = m.value(QStringLiteral("id")).toString();
        i.name    = m.value(QStringLiteral("name")).toString();
        i.type    = m.value(QStringLiteral("type")).toString();
        i.value   = m.value(QStringLiteral("value")).toString();
        i.enabled = m.value(QStringLiteral("enabled"), true).toBool();
        if (!i.id.isEmpty() && !i.value.isEmpty()) m_items << i;
    }
    m_scannedDir.clear();
    m_localFiles.clear();
    emit changed();
}

QStringList CustomSourceManager::ids() const
{
    QStringList out;
    for (const Item &i : m_items) out << i.id;
    return out;
}

QStringList CustomSourceManager::names() const
{
    QStringList out;
    for (const Item &i : m_items) out << i.name;
    return out;
}

QString CustomSourceManager::localDirectory() const
{
    for (const Item &i : m_items)
        if (i.type == QLatin1String("local")) return i.value;
    return QString();
}

void CustomSourceManager::addHttp(const QString &name, const QString &baseUrl)
{
    if (name.trimmed().isEmpty() || baseUrl.trimmed().isEmpty()) return;
    QVariantMap m;
    m.insert(QStringLiteral("id"), QStringLiteral("http_") + QString::number(qHash(baseUrl), 16));
    m.insert(QStringLiteral("name"), name.trimmed());
    m.insert(QStringLiteral("type"), QStringLiteral("http"));
    m.insert(QStringLiteral("value"), baseUrl.trimmed());
    m.insert(QStringLiteral("enabled"), true);
    Library::instance()->addCustomSource(m);
    refresh();
}

void CustomSourceManager::setLocalDirectory(const QString &path)
{
    Library::instance()->setCustomLocalDirectory(path.trimmed());
    refresh();
}

void CustomSourceManager::remove(const QString &id)
{
    Library::instance()->removeCustomSource(id);
    refresh();
}

static QVariantMap songVarsFor(const Song &s, const QString &quality)
{
    QVariantMap v;
    v.insert(QStringLiteral("name"), s.title);
    v.insert(QStringLiteral("artist"), s.artist);
    v.insert(QStringLiteral("album"), s.album);
    v.insert(QStringLiteral("id"), s.sourceId);
    v.insert(QStringLiteral("source"), s.source);
    v.insert(QStringLiteral("quality"), quality);
    return v;
}

void CustomSourceManager::resolve(const Song &song, const QString &quality, PlayableCallback cb)
{
    if (m_items.isEmpty()) {
        tryScriptSources(song, quality, cb);
        return;
    }
    resolveAt(0, song, quality, cb);
}

void CustomSourceManager::resolveAt(int index, const Song &song, const QString &quality,
                                    PlayableCallback cb)
{
    if (index >= m_items.size()) {
        tryScriptSources(song, quality, cb);
        return;
    }
    const Item item = m_items.at(index);
    if (!item.enabled) { resolveAt(index + 1, song, quality, cb); return; }

    if (item.type == QLatin1String("local")) {
        scanLocal();
        Playable p = matchLocal(song);
        if (p.ok) {
            p.substituted = true;
            p.substituteSource = item.name;
            if (cb) cb(p);
        } else {
            resolveAt(index + 1, song, quality, cb);
        }
        return;
    }

    const QVariantMap vars = songVarsFor(song, quality);
    QUrl url(item.value + QStringLiteral("/musicUrl"));
    QUrlQuery q;
    for (auto it = vars.constBegin(); it != vars.constEnd(); ++it)
        q.addQueryItem(it.key(), it.value().toString());
    url.setQuery(q);

    HttpClient::instance()->request(QStringLiteral("GET"), url, QVariantMap(), QByteArray(),
        [this, index, song, quality, cb, item](const HttpClient::Reply &r) {
            Playable p;
            if (r.ok) {
                const QJsonValue root = Json::rootOf(r.json);
                const QString found = Json::firstString(root, {
                    QStringLiteral("url"), QStringLiteral("data.url"),
                    QStringLiteral("data[0].url"), QStringLiteral("data.playUrl")
                });
                if (!found.isEmpty()) {
                    p.ok = true;
                    p.url = QUrl(found);
                    if (!p.url.isValid() || p.url.scheme().isEmpty()) p.url = QUrl::fromLocalFile(found);
                    p.substituted = true;
                    p.substituteSource = item.name;
                }
            }
            if (p.ok) {
                if (cb) cb(p);
            } else {
                resolveAt(index + 1, song, quality, cb);
            }
        });
}

void CustomSourceManager::scanLocal()
{
    const QString dir = localDirectory();
    if (dir.isEmpty() || (m_scannedDir == dir && !m_localFiles.isEmpty())) return;
    m_scannedDir = dir;
    m_localFiles.clear();
    QDirIterator it(dir, QStringList() << QStringLiteral("*.mp3") << QStringLiteral("*.flac")
                                        << QStringLiteral("*.m4a") << QStringLiteral("*.wav")
                                        << QStringLiteral("*.ogg") << QStringLiteral("*.aac"),
                    QDir::Files, QDirIterator::Subdirectories);
    int count = 0;
    while (it.hasNext() && count < 8000) {
        const QString path = it.next();
        const QString base = QFileInfo(path).completeBaseName();
        m_localFiles.append({path, base, normalize(base)});
        ++count;
    }
}

Playable CustomSourceManager::matchLocal(const Song &song) const
{
    Playable p;
    if (m_localFiles.isEmpty()) return p;
    const QString ntTitle = normalize(song.title);
    const QString ntTarget = normalize(song.title + QLatin1Char(' ') + song.artist);
    qreal best = 0.0;
    QString bestPath;
    for (const LocalFile &f : m_localFiles) {
        // 归一化结果已缓存，只对长度接近的候选做编辑距离；长度差过大的必然低于 0.55
        const qreal s1 = similarityNormalized(f.norm, ntTitle);
        const qreal s2 = similarityNormalized(f.norm, ntTarget);
        const qreal s = qMax(s1, s2);
        if (s > best) { best = s; bestPath = f.path; }
    }
    if (best >= 0.55 && !bestPath.isEmpty()) {
        p.ok = true;
        p.url = QUrl::fromLocalFile(bestPath);
        p.message = QStringLiteral("已匹配到本地音乐（相似度 %1%）").arg(int(best * 100));
    }
    return p;
}

void CustomSourceManager::search(const QString &keyword, int limit, SongsCallback cb)
{
    QVector<Song> out;
    int pending = 0;
    for (const Item &i : m_items)
        if (i.enabled && i.type == QLatin1String("http")) ++pending;
    const bool hasScripts = ScriptSourceManager::instance()->hasSearch();
    if (pending == 0) {
        if (hasScripts) {
            ScriptSourceManager::instance()->search(keyword, limit, cb);
            return;
        }
        if (cb) cb(out);
        return;
    }

    auto remaining = std::make_shared<int>(pending);
    auto collected = std::make_shared<QVector<Song>>();
    for (const Item &item : m_items) {
        if (!item.enabled || item.type != QLatin1String("http")) continue;
        QUrl url(item.value + QStringLiteral("/search"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("keyword"), keyword);
        q.addQueryItem(QStringLiteral("limit"), QString::number(limit));
        url.setQuery(q);
        HttpClient::instance()->request(QStringLiteral("GET"), url, QVariantMap(), QByteArray(),
            [this, item, collected, remaining, cb, hasScripts, keyword, limit](const HttpClient::Reply &r) {
                if (r.ok) {
                    const QJsonValue root = Json::rootOf(r.json);
                    QJsonArray arr = Json::arrayAt(root, QStringLiteral("list"));
                    if (arr.isEmpty()) arr = Json::arrayAt(root, QStringLiteral("data.list"));
                    if (arr.isEmpty() && root.isArray()) arr = root.toArray();
                    QVariantMap fields;
                    fields.insert(QStringLiteral("id"), QStringLiteral("id"));
                    fields.insert(QStringLiteral("title"), QStringLiteral("title"));
                    fields.insert(QStringLiteral("artist"), QStringLiteral("artist"));
                    fields.insert(QStringLiteral("album"), QStringLiteral("album"));
                    fields.insert(QStringLiteral("cover"), QStringLiteral("cover"));
                    fields.insert(QStringLiteral("duration"), QStringLiteral("duration"));
                    for (const QJsonValue &v : arr) {
                        Song s = Json::mapSong(v, fields, QStringLiteral("custom"));
                        s.extra.insert(QStringLiteral("customBase"), item.value);
                        s.extra.insert(QStringLiteral("customName"), item.name);
                        if (!s.title.isEmpty()) collected->append(s);
                    }
                }
                if (--(*remaining) == 0) {
                    if (hasScripts) {
                        ScriptSourceManager::instance()->search(keyword, limit,
                            [collected, cb](const QVector<Song> &extra) {
                                QVector<Song> all = *collected;
                                all += extra;
                                if (cb) cb(all);
                            });
                    } else if (cb) {
                        cb(*collected);
                    }
                }
            });
    }
}

void CustomSourceManager::lyric(const Song &song, StringCallback cb)
{
    // 来自在线脚本的歌曲：直接交给脚本的 lyric 能力
    if (song.extra.contains(QStringLiteral("scriptId"))) {
        ScriptSourceManager::instance()->lyric(song, cb);
        return;
    }
    lyricAt(0, song, cb);
}

void CustomSourceManager::lyricAt(int index, const Song &song, StringCallback cb)
{
    // 逐个尝试 HTTP 自定义源；某个源返回空歌词时不能就此收工，
    // 后面还有其它 HTTP 源与所有声明了 lyric 能力的在线脚本。
    while (index < m_items.size()) {
        const Item item = m_items.at(index++);
        if (!item.enabled || item.type != QLatin1String("http")) continue;
        QUrl url(item.value + QStringLiteral("/lyric"));
        QUrlQuery q;
        const QVariantMap vars = songVarsFor(song, QStringLiteral("standard"));
        for (auto it = vars.constBegin(); it != vars.constEnd(); ++it)
            q.addQueryItem(it.key(), it.value().toString());
        url.setQuery(q);
        HttpClient::instance()->request(QStringLiteral("GET"), url, QVariantMap(), QByteArray(),
            [this, index, song, cb](const HttpClient::Reply &r) {
                QString text;
                if (r.ok) text = Json::firstString(Json::rootOf(r.json),
                    {QStringLiteral("lyric"), QStringLiteral("data.lyric"), QStringLiteral("lrc")});
                if (!text.trimmed().isEmpty()) { if (cb) cb(text); return; }
                lyricAt(index, song, cb);
            });
        return;
    }
    ScriptSourceManager::instance()->lyric(song, cb);
}

} // namespace Furui
