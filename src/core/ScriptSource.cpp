#include "core/QtCompat.h"
#include "ScriptSource.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <memory>

#include "HttpClient.h"
#include "Library.h"
#include "Settings.h"

namespace Furui {

namespace {

// 内置在线音源（LX 脚本协议），来自 github.com/skxingyu/lx_music- 整理的
// pdone/lx-music-source 在线导入地址（README 测试可用至 2026-11-06）。
// 同一脚本给多条镜像（加速站可达性随时变化，依次回退，成功即用）；
// 仅首次启动预置下载，用户在设置页删除后不再补回（记入 builtinScriptRemoved）。
// 注意：六音（sixyin）已从内置列表移除——脚本自身声明「已关闭，请去
// www.sixyin.com 下载最新版」，且是 333 KB 的混淆代码，内置 JS 引擎解析时
// 直接报「语法错误」，留着只会每次启动刷一条报错。
struct BuiltinSource {
    const char *name;
    const char *mirrors[4];
};
const BuiltinSource kBuiltinSources[] = {
    { u8"Huibq",  { "https://ghproxy.cn/raw.githubusercontent.com/pdone/lx-music-source/main/huibq/latest.js",
                    "https://gh-proxy.org/raw.githubusercontent.com/pdone/lx-music-source/main/huibq/latest.js",
                    "https://ghproxy.net/raw.githubusercontent.com/pdone/lx-music-source/main/huibq/latest.js",
                    "https://raw.githubusercontent.com/pdone/lx-music-source/main/huibq/latest.js" } },
    { u8"Fish",   { "https://ghproxy.cn/raw.githubusercontent.com/pdone/lx-music-source/main/fish/latest.js",
                    "https://gh-proxy.org/raw.githubusercontent.com/pdone/lx-music-source/main/fish/latest.js",
                    "https://ghproxy.net/raw.githubusercontent.com/pdone/lx-music-source/main/fish/latest.js",
                    "https://raw.githubusercontent.com/pdone/lx-music-source/main/fish/latest.js" } },
    { u8"洛雪LX", { "https://ghproxy.cn/raw.githubusercontent.com/pdone/lx-music-source/main/lx/latest.js",
                    "https://gh-proxy.org/raw.githubusercontent.com/pdone/lx-music-source/main/lx/latest.js",
                    "https://ghproxy.net/raw.githubusercontent.com/pdone/lx-music-source/main/lx/latest.js",
                    "https://raw.githubusercontent.com/pdone/lx-music-source/main/lx/latest.js" } },
    { u8"长青",   { "https://ghproxy.cn/raw.githubusercontent.com/pdone/lx-music-source/main/changqing/latest.js",
                    "https://gh-proxy.org/raw.githubusercontent.com/pdone/lx-music-source/main/changqing/latest.js",
                    "https://ghproxy.net/raw.githubusercontent.com/pdone/lx-music-source/main/changqing/latest.js",
                    "https://raw.githubusercontent.com/pdone/lx-music-source/main/changqing/latest.js" } },
    { u8"幻音",   { "https://ghproxy.cn/raw.githubusercontent.com/pdone/lx-music-source/main/huanyin/latest.js",
                    "https://gh-proxy.org/raw.githubusercontent.com/pdone/lx-music-source/main/huanyin/latest.js",
                    "https://ghproxy.net/raw.githubusercontent.com/pdone/lx-music-source/main/huanyin/latest.js",
                    "https://raw.githubusercontent.com/pdone/lx-music-source/main/huanyin/latest.js" } },
    { u8"星海",   { "https://ghproxy.cn/raw.githubusercontent.com/pdone/lx-music-source/main/xinghai/latest.js",
                    "https://gh-proxy.org/raw.githubusercontent.com/pdone/lx-music-source/main/xinghai/latest.js",
                    "https://ghproxy.net/raw.githubusercontent.com/pdone/lx-music-source/main/xinghai/latest.js",
                    "https://raw.githubusercontent.com/pdone/lx-music-source/main/xinghai/latest.js" } },
};

QJsonArray stringsToJson(const QStringList &list)
{
    QJsonArray arr;
    for (const QString &s : list) arr.append(s);
    return arr;
}

QStringList jsonToStrings(const QJsonValue &value)
{
    QStringList out;
    for (const QJsonValue &v : value.toArray()) out << v.toString();
    return out;
}

QString formatInterval(qint64 ms)
{
    if (ms <= 0) return QString();
    const qint64 total = ms / 1000;
    return QStringLiteral("%1:%2").arg(total / 60, 2, 10, QLatin1Char('0'))
                                  .arg(total % 60, 2, 10, QLatin1Char('0'));
}

qint64 parseInterval(const QVariant &value)
{
    if (value.isValid() && !value.toString().isEmpty()) {
        const QString text = value.toString();
        if (text.contains(QLatin1Char(':'))) {
            const QStringList parts = text.split(QLatin1Char(':'));
            qint64 sec = 0;
            for (const QString &p : parts) sec = sec * 60 + p.toLongLong();
            return sec * 1000;
        }
        const qint64 n = value.toLongLong();
        if (n > 0) return n < 1000 ? n * 1000 : n;
    }
    return 0;
}

QString entrySourceId(const QVariantMap &m)
{
    for (const QString &key : {QStringLiteral("songmid"), QStringLiteral("id"),
                               QStringLiteral("hash"), QStringLiteral("copyrightId"),
                               QStringLiteral("rid"), QStringLiteral("musicId")}) {
        const QString v = m.value(key).toString();
        if (!v.isEmpty()) return v;
    }
    return QString();
}

QString entryTitle(const QVariantMap &m)
{
    for (const QString &key : {QStringLiteral("name"), QStringLiteral("songname"),
                               QStringLiteral("songName"), QStringLiteral("title")}) {
        const QString v = m.value(key).toString();
        if (!v.isEmpty()) return v;
    }
    return QString();
}

QString entryArtist(const QVariantMap &m)
{
    const QVariant singer = m.value(QStringLiteral("singer"));
    if (singer.canConvert<QVariantList>()) {
        QStringList names;
        for (const QVariant &v : singer.toList()) {
            if (v.canConvert<QVariantMap>()) {
                const QString n = v.toMap().value(QStringLiteral("name")).toString();
                if (!n.isEmpty()) names << n;
            } else if (!v.toString().isEmpty()) {
                names << v.toString();
            }
        }
        if (!names.isEmpty()) return names.join(QLatin1Char('/'));
    }
    const QString s = singer.toString();
    if (!s.isEmpty()) return s;
    return m.value(QStringLiteral("artist")).toString();
}

QString entryText(const QVariantMap &m, const QStringList &keys)
{
    for (const QString &key : keys) {
        const QString v = m.value(key).toString();
        if (!v.isEmpty()) return v;
    }
    return QString();
}

QString displayStatus(const QString &status)
{
    if (status == QLatin1String("ok")) return QStringLiteral("可用");
    if (status == QLatin1String("error")) return QStringLiteral("失败");
    if (status == QLatin1String("disabled")) return QStringLiteral("已禁用");
    if (status == QLatin1String("pending")) return QStringLiteral("未检查");
    return status;
}

} // namespace

ScriptSourceManager *ScriptSourceManager::instance()
{
    static ScriptSourceManager *s = new ScriptSourceManager();
    return s;
}

ScriptSourceManager::ScriptSourceManager(QObject *parent)
    : QObject(parent)
{
    load();
    // 重启后不自动联网，先用落库状态展示；用户点「检查」或真正用到时再初始化。
    // 例外：内置在线音源只在缺失时补齐（首次启动/新版本追加），补齐动作入事件循环。
    QTimer::singleShot(0, this, [this] { seedBuiltinSources(); });
}

void ScriptSourceManager::load()
{
    m_items.clear();
    const QVariantList rows = Library::instance()->scriptSources();
    for (const QVariant &v : rows) {
        const QVariantMap m = v.toMap();
        Item item;
        item.id        = m.value(QStringLiteral("id")).toString();
        item.name      = m.value(QStringLiteral("name")).toString();
        item.url       = m.value(QStringLiteral("url")).toString();
        item.version   = m.value(QStringLiteral("version")).toString();
        item.script    = m.value(QStringLiteral("script")).toString();
        item.hash      = m.value(QStringLiteral("hash")).toString();
        item.enabled   = m.value(QStringLiteral("enabled"), true).toBool();
        item.status    = m.value(QStringLiteral("status"), QStringLiteral("pending")).toString();
        item.errorType = m.value(QStringLiteral("errorType")).toString();
        item.errorMsg  = m.value(QStringLiteral("errorMsg")).toString();
        item.updated   = m.value(QStringLiteral("updated")).toLongLong();
        item.checked   = m.value(QStringLiteral("checked")).toLongLong();
        const QJsonDocument actionsDoc =
            QJsonDocument::fromJson(m.value(QStringLiteral("actions")).toString().toUtf8());
        for (const QJsonValue &a : actionsDoc.array()) item.actions << a.toString();
        const QJsonDocument qualitiesDoc =
            QJsonDocument::fromJson(m.value(QStringLiteral("qualities")).toString().toUtf8());
        for (const QJsonValue &q : qualitiesDoc.array()) item.qualities << q.toString();
        const QJsonDocument sourcesDoc =
            QJsonDocument::fromJson(m.value(QStringLiteral("sources")).toString().toUtf8());
        if (sourcesDoc.isArray()) item.sources = sourcesDoc.array();
        if (!item.enabled && item.status != QLatin1String("error"))
            item.status = QStringLiteral("disabled");
        if (!item.id.isEmpty() && !item.script.isEmpty()) m_items.insert(item.id, item);
    }
}

ScriptSourceManager::Item *ScriptSourceManager::find(const QString &id)
{
    auto it = m_items.find(id);
    return it == m_items.end() ? nullptr : &it.value();
}

QString ScriptSourceManager::scriptIdForUrl(const QString &url)
{
    return QStringLiteral("lx_") +
           QString::fromUtf8(QCryptographicHash::hash(url.trimmed().toUtf8(),
                                                      QCryptographicHash::Sha1).toHex()).left(12);
}

bool ScriptSourceManager::isBuiltinUrl(const QString &url)
{
    const QString trimmed = url.trimmed();
    for (const BuiltinSource &b : kBuiltinSources)
        for (const char *m : b.mirrors)
            if (trimmed == QLatin1String(m)) return true;
    return false;
}

void ScriptSourceManager::seedBuiltinSources()
{
    // 内置源自动预置要「安静且不折腾」：
    //   · 全程 silent，成功失败都不弹提示（失败原因留在音源条目的 errorMsg 里）；
    //   · 失败源（脚本停更 / 镜像被墙 / 语法与内置 JS 引擎不兼容）不能每次启动
    //     都重下 4 条镜像再报错——那就是纯粹的骚扰。已有一个可用源时最多每天
    //     自动重试一次；一个都没有（例如冷启动没网）时 5 分钟后允许再试。
    //   用户随时可以在设置页对单个音源手动「更新」。
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 lastSeed = Settings::get(QStringLiteral("builtinSeedAt")).toLongLong();
    bool haveAnyBuiltin = false;
    for (const Item &it : m_items) {
        if (isBuiltinUrl(it.url)) { haveAnyBuiltin = true; break; }
    }
    const qint64 minGap = haveAnyBuiltin ? 24LL * 3600 * 1000 : 5LL * 60 * 1000;
    if (lastSeed > 0 && now - lastSeed < minGap)
        return;
    Settings::set(QStringLiteral("builtinSeedAt"), now);

    const QStringList removed = Settings::get(QStringLiteral("builtinScriptRemoved"))
                                    .toStringList();
    for (const BuiltinSource &b : kBuiltinSources) {
        QStringList urls;
        bool skip = false;
        for (const char *m : b.mirrors) {
            const QString u = QLatin1String(m);
            urls << u;
            if (removed.contains(u)) { skip = true; continue; }
            // 已有可用条目（ok/pending/disabled）→ 该源已就位；error 条目是
            // 上次回退链留下的坏镜像产物，本次清掉并重新走回退
            const Item it = m_items.value(scriptIdForUrl(u));
            if (it.id.isEmpty()) continue;
            if (it.status == QLatin1String("error")) {
                m_items.remove(it.id);
                Library::instance()->removeScriptSource(it.id);
            } else {
                skip = true;
            }
        }
        if (skip) continue;
        // 依次回退：加速站可达性随时变化，某镜像成功后 item.url 固定为该镜像，
        // 之后「更新」按钮继续用同一条；失败则试下一镜像。
        importWithFallback(urls, 0);
    }
}

void ScriptSourceManager::importWithFallback(const QStringList &urls, int index)
{
    if (index >= urls.size()) return;
    QPointer<ScriptSourceManager> self(this);
    // silent：自动预置属于后台行为，成功失败都不弹提示（失败细节留在音源列表里）
    importUrl(urls.at(index), [self, urls, index](bool ok, const QString &) {
        if (ok || !self) return;
        QTimer::singleShot(0, self.data(), [self, urls, index] {
            if (self) self->importWithFallback(urls, index + 1);
        });
    }, true);
}

QVariantMap ScriptSourceManager::itemToVariant(const Item &item) const
{
    QVariantMap m;
    m.insert(QStringLiteral("id"), item.id);
    m.insert(QStringLiteral("name"), item.name);
    m.insert(QStringLiteral("url"), item.url);
    m.insert(QStringLiteral("version"), item.version.isEmpty()
                                        ? QStringLiteral("未知") : item.version);
    m.insert(QStringLiteral("enabled"), item.enabled);
    m.insert(QStringLiteral("status"), item.status);
    m.insert(QStringLiteral("statusText"), displayStatus(item.status));
    m.insert(QStringLiteral("errorType"), item.errorType);
    m.insert(QStringLiteral("errorMsg"), item.errorMsg);
    m.insert(QStringLiteral("actions"), item.actions);
    m.insert(QStringLiteral("qualities"), item.qualities);
    m.insert(QStringLiteral("sources"), item.sources.toVariantList());

    bool search = false, musicUrl = false, lyric = false;
    for (const QJsonValue &v : item.sources) {
        const QJsonObject o = v.toObject();
        const QStringList acts = jsonToStrings(o.value(QStringLiteral("actions")));
        if (acts.contains(QStringLiteral("search"))) search = true;
        if (acts.contains(QStringLiteral("musicUrl"))) musicUrl = true;
        if (acts.contains(QStringLiteral("lyric"))) lyric = true;
    }
    QString capability;
    if (search && musicUrl) capability = QStringLiteral("搜索 + 取链");
    else if (search) capability = QStringLiteral("仅搜索");
    else if (musicUrl) capability = QStringLiteral("仅取链（无搜索能力）");
    else capability = QStringLiteral("未声明可用能力");
    if (lyric && !capability.contains(QStringLiteral("歌词")))
        capability += QStringLiteral(" · 含歌词");
    m.insert(QStringLiteral("capability"), capability);

    QStringList lines;
    for (const QJsonValue &v : item.sources) {
        const QJsonObject o = v.toObject();
        const QString key = o.value(QStringLiteral("key")).toString();
        const QString name = o.value(QStringLiteral("name")).toString();
        const QStringList acts = jsonToStrings(o.value(QStringLiteral("actions")));
        QStringList caps;
        if (acts.contains(QStringLiteral("search"))) caps << QStringLiteral("搜索");
        if (acts.contains(QStringLiteral("musicUrl"))) caps << QStringLiteral("取链");
        if (acts.contains(QStringLiteral("lyric"))) caps << QStringLiteral("歌词");
        lines << QStringLiteral("%1(%2)：%3")
                    .arg(name.isEmpty() ? key : name, key, caps.join(QLatin1Char('+')));
    }
    m.insert(QStringLiteral("sourcesText"), lines.join(QStringLiteral("；")));
    m.insert(QStringLiteral("checkedText"),
             item.checked > 0
                 ? QDateTime::fromMSecsSinceEpoch(item.checked).toString(QStringLiteral("MM-dd HH:mm"))
                 : QStringLiteral("未检查"));
    m.insert(QStringLiteral("updatedText"),
             item.updated > 0
                 ? QDateTime::fromMSecsSinceEpoch(item.updated).toString(QStringLiteral("MM-dd HH:mm"))
                 : QStringLiteral("未知"));
    return m;
}

QVariantList ScriptSourceManager::items() const
{
    QVariantList out;
    QStringList ids = m_items.keys();
    std::sort(ids.begin(), ids.end(), [this](const QString &a, const QString &b) {
        return m_items.value(a).name < m_items.value(b).name;
    });
    for (const QString &id : ids) out << itemToVariant(m_items.value(id));
    return out;
}

bool ScriptSourceManager::hasEnabled() const
{
    for (const Item &i : m_items) if (i.enabled) return true;
    return false;
}

bool ScriptSourceManager::hasSearch() const
{
    for (const Item &i : m_items)
        if (i.enabled && i.actions.contains(QStringLiteral("search"))) return true;
    return false;
}

bool ScriptSourceManager::hasMusicUrl() const
{
    for (const Item &i : m_items)
        if (i.enabled && i.actions.contains(QStringLiteral("musicUrl"))) return true;
    return false;
}

QString ScriptSourceManager::metaLine(const QString &script, const QString &key)
{
    // 兼容 "// @name xxx"、"// @name: xxx" 与 jsdoc "* @name xxx"
    const QRegularExpression re(QStringLiteral("@%1\\s*:?\\s*([^\\r\\n*]+)").arg(key));
    const QRegularExpressionMatch m = re.match(script.left(6000));
    if (m.hasMatch()) {
        QString text = m.captured(1).trimmed();
        text.remove(QLatin1Char('*'));
        return text.trimmed();
    }
    return QString();
}

void ScriptSourceManager::fillMeta(Item &item, const QString &script)
{
    QString name = metaLine(script, QStringLiteral("name"));
    if (name.isEmpty()) {
        const QString path = QUrl(item.url).path();
        name = path.section(QLatin1Char('/'), -2, -2);
        if (name.isEmpty()) name = QStringLiteral("LX 音源");
    }
    item.name = name;
    item.version = metaLine(script, QStringLiteral("version"));
}

void ScriptSourceManager::persist(const Item &item)
{
    QVariantMap m;
    m.insert(QStringLiteral("id"), item.id);
    m.insert(QStringLiteral("name"), item.name);
    m.insert(QStringLiteral("url"), item.url);
    m.insert(QStringLiteral("version"), item.version);
    m.insert(QStringLiteral("script"), item.script);
    m.insert(QStringLiteral("hash"), item.hash);
    m.insert(QStringLiteral("enabled"), item.enabled);
    m.insert(QStringLiteral("status"), item.status);
    m.insert(QStringLiteral("errorType"), item.errorType);
    m.insert(QStringLiteral("errorMsg"), item.errorMsg);
    m.insert(QStringLiteral("actions"),
             QString::fromUtf8(QJsonDocument(stringsToJson(item.actions)).toJson(QJsonDocument::Compact)));
    m.insert(QStringLiteral("qualities"),
             QString::fromUtf8(QJsonDocument(stringsToJson(item.qualities)).toJson(QJsonDocument::Compact)));
    m.insert(QStringLiteral("sources"),
             QString::fromUtf8(QJsonDocument(item.sources).toJson(QJsonDocument::Compact)));
    m.insert(QStringLiteral("updated"), item.updated);
    m.insert(QStringLiteral("checked"), item.checked);
    Library::instance()->saveScriptSource(m);
}

void ScriptSourceManager::updateFromInit(Item &item, const ScriptInitResult &r)
{
    item.ready = r.ok;
    item.checked = QDateTime::currentMSecsSinceEpoch();
    item.errorType.clear();
    item.errorMsg.clear();
    item.actions.clear();
    item.qualities.clear();
    item.sources = QJsonArray();
    if (!r.ok) {
        item.status = item.enabled ? QStringLiteral("error") : QStringLiteral("disabled");
        item.errorType = r.errorType;
        item.errorMsg = r.errorMsg;
        return;
    }
    item.status = item.enabled ? QStringLiteral("ok") : QStringLiteral("disabled");
    QSet<QString> actionSet;
    QSet<QString> qualitySet;
    for (const ScriptSourceInfo &s : r.sources) {
        QJsonObject o;
        o.insert(QStringLiteral("key"), s.key);
        o.insert(QStringLiteral("name"), s.name);
        o.insert(QStringLiteral("type"), s.type);
        o.insert(QStringLiteral("actions"), stringsToJson(s.actions));
        o.insert(QStringLiteral("qualities"), stringsToJson(s.qualities));
        item.sources.append(o);
        for (const QString &a : s.actions) actionSet.insert(a);
        for (const QString &q : s.qualities) qualitySet.insert(q);
    }
    QStringList actions = actionSet.values();
    actions.sort();
    QStringList qualities = qualitySet.values();
    qualities.sort();
    item.actions = actions;
    item.qualities = qualities;
}

void ScriptSourceManager::startEngine(Item &item, DoneCallback cb, bool persistResult)
{
    const QString id = item.id;
    if (QPointer<ScriptEngine> old = m_engines.value(id)) {
        old->stop();
        old->deleteLater();
    }
    ScriptEngine *engine = new ScriptEngine(item.name, this);
    m_engines.insert(id, engine);
    m_starting.insert(id);

    QVariantMap meta;
    meta.insert(QStringLiteral("name"), item.name);
    meta.insert(QStringLiteral("version"), item.version);
    meta.insert(QStringLiteral("author"), metaLine(item.script, QStringLiteral("author")));
    meta.insert(QStringLiteral("homepage"), metaLine(item.script, QStringLiteral("homepage")));
    meta.insert(QStringLiteral("description"), metaLine(item.script, QStringLiteral("description")));
    meta.insert(QStringLiteral("rawScript"), item.script);

    QPointer<ScriptSourceManager> self(this);
    // 提示里只用名字：条目可能在初始化过程中被删除，先拷一份
    const QString srcName = item.name.isEmpty() ? id : item.name;
    engine->start(item.script, meta, [self, id, cb, persistResult, srcName](const ScriptInitResult &r) {
        ScriptSourceManager *mgr = self.data();
        if (!mgr) return;
        mgr->m_starting.remove(id);
        Item *it = mgr->find(id);
        if (it) {
            // 先拷贝再写回：find 返回的是 QHash 内部元素的指针，
            // 直接 insert(id, *it) 在 rehash 时读到的已是失效内存。
            Item item = *it;
            mgr->updateFromInit(item, r);
            if (persistResult) mgr->persist(item);
            mgr->m_items.insert(id, item);
        }
        if (!r.ok) {
            // 初始化回调可能正运行在 QuickJS 内部（脚本同步 send(inited) 触发），
            // 不能在这里立刻 stop/free 上下文，否则等于在 JS 执行中途销毁宿主。
            QPointer<ScriptEngine> dead = mgr->m_engines.value(id);
            mgr->m_engines.remove(id);
            if (dead) {
                QTimer::singleShot(0, dead.data(), [dead] {
                    if (dead) dead->stop();
                    if (dead) dead->deleteLater();
                });
            }
        }
        emit mgr->changed();

        // 提示统一短句（参考洛雪的轻提示）：成功只在用户显式操作（导入/更新/
        // 检查，带 cb）时提示一次；搜索/播放时引擎在后台自动拉起，不是用户动作，
        // 不该刷屏。失败只报类型，完整错误文本留在 item.errorMsg（设置页可查）。
        QString text;
        if (r.ok) {
            text = QStringLiteral("音源「%1」可用").arg(srcName);
        } else {
            const QString typeName = [&r] {
                if (r.errorType == QLatin1String("syntax")) return QStringLiteral("语法错误");
                if (r.errorType == QLatin1String("timeout")) return QStringLiteral("初始化超时");
                if (r.errorType == QLatin1String("init")) return QStringLiteral("脚本自检失败");
                if (r.errorType == QLatin1String("empty")) return QStringLiteral("未声明音源");
                if (r.errorType == QLatin1String("unsupported")) return QStringLiteral("缺少宿主能力");
                return QStringLiteral("运行错误");
            }();
            text = QStringLiteral("音源「%1」不可用（%2）").arg(srcName, typeName);
        }
        if (!r.ok || cb)
            emit mgr->message(id, r.ok, text);
        if (cb) cb(r.ok, text);

        const QVector<std::function<void(bool)>> waiters = mgr->m_waiters.take(id);
        for (const auto &w : waiters) w(r.ok);
    });
}

void ScriptSourceManager::ensureEngine(const QString &id, std::function<void(bool)> cb)
{
    Item *item = find(id);
    if (!item || !item->enabled) {
        if (cb) cb(false);
        return;
    }
    QPointer<ScriptEngine> engine = m_engines.value(id);
    if (engine && engine->ready()) {
        if (cb) cb(true);
        return;
    }
    if (m_starting.contains(id)) {
        m_waiters[id].append(cb);
        return;
    }
    m_waiters[id].append(cb);
    startEngine(*item, {}, true);
}

void ScriptSourceManager::importUrl(const QString &url, DoneCallback cb, bool silent)
{
    const QString trimmed = url.trimmed();
    const QUrl target(trimmed);
    if (!target.isValid() || target.scheme().isEmpty()
        || !(target.scheme() == QLatin1String("http") || target.scheme() == QLatin1String("https"))) {
        const QString msg = QStringLiteral("请输入 http/https 开头的脚本地址");
        if (!silent) emit message(QString(), false, msg);
        if (cb) cb(false, msg);
        return;
    }
    // 下载内容会被直接 JS_Eval，明文 HTTP 存在中间人注入风险。
    // 确需内网/自建 HTTP 源时设 FURUI_SCRIPT_ALLOW_HTTP=1 显式放开。
    static const bool allowHttp = qEnvironmentVariableIsSet("FURUI_SCRIPT_ALLOW_HTTP");
    if (target.scheme() == QLatin1String("http") && !allowHttp) {
        const QString msg = QStringLiteral("在线脚本仅允许 https 地址");
        if (!silent) emit message(QString(), false, msg);
        if (cb) cb(false, msg);
        return;
    }

    QVariantMap headers;
    headers.insert(QStringLiteral("User-Agent"), QStringLiteral("FuruiMusic/1.0"));
    headers.insert(QStringLiteral("Accept"), QStringLiteral("*/*"));

    QPointer<ScriptSourceManager> self(this);
    HttpClient::instance()->request(QStringLiteral("GET"), target, headers, QByteArray(),
        [self, trimmed, cb, silent](const HttpClient::Reply &r) {
            ScriptSourceManager *mgr = self.data();
            if (!mgr) return;
            // 提示统一走短句（参考洛雪的轻提示）：完整原因写进 item.errorMsg，
            // 在设置页的音源列表里看，不把整段异常糊到屏幕上
            if (!r.ok) {
                const QString msg = QStringLiteral("下载失败（%1）")
                                        .arg(classifyNetworkError(r.error, r.status));
                if (!silent) emit mgr->message(QString(), false, msg);
                if (cb) cb(false, msg + QStringLiteral("：") + r.error);
                return;
            }
            if (r.body.trimmed().isEmpty()) {
                const QString msg = QStringLiteral("下载到的脚本为空");
                if (!silent) emit mgr->message(QString(), false, msg);
                if (cb) cb(false, msg);
                return;
            }
            if (r.body.size() > 2 * 1024 * 1024) {
                const QString msg = QStringLiteral("脚本超过 2 MB，已拒绝载入");
                if (!silent) emit mgr->message(QString(), false, msg);
                if (cb) cb(false, msg);
                return;
            }
            // 加速站被限流/命中错误页时经常回 HTML：去掉 UTF-8 BOM 与前导空白后
            // 首个字符是 '<' 就一律按「不是脚本」拒掉，否则会被 JS 引擎当代码执行，
            // 报出一句莫名其妙的「语法错误」
            QByteArray head = r.body;
            if (head.startsWith("\xEF\xBB\xBF")) head = head.mid(3);
            head = head.trimmed().toLower();
            if (r.body.contains('\0') || head.startsWith('<')) {
                const QString msg = QStringLiteral("下载内容不是脚本（疑似网页），已取消");
                if (!silent) emit mgr->message(QString(), false, msg);
                if (cb) cb(false, msg);
                return;
            }
            Item item;
            item.id = scriptIdForUrl(trimmed);
            item.url = trimmed;
            item.script = QString::fromUtf8(r.body);
            item.hash = QString::fromUtf8(QCryptographicHash::hash(r.body, QCryptographicHash::Sha256).toHex());
            item.enabled = true;
            item.status = QStringLiteral("pending");
            item.updated = QDateTime::currentMSecsSinceEpoch();
            mgr->fillMeta(item, item.script);
            // 完整性可追溯：同一 URL 再次导入时若指纹变化，明确提示用户复核来源
            const Item *previous = mgr->find(item.id);
            if (previous && !previous->hash.isEmpty() && previous->hash != item.hash) {
                if (!silent) {
                    emit mgr->message(item.id, true,
                        QStringLiteral("脚本内容已变化，请确认来源可信"));
                }
            }
            mgr->m_items.insert(item.id, item);
            mgr->persist(item);
            emit mgr->changed();
            if (!silent)
                emit mgr->message(item.id, true, QStringLiteral("已添加音源「%1」").arg(item.name));

            Item snapshot = mgr->m_items.value(item.id);
            mgr->startEngine(snapshot, [cb, mgr, silent](bool ok, const QString &msg) {
                emit mgr->changed();
                if (cb) {
                    cb(ok, ok ? QStringLiteral("导入成功：") + msg
                              : QStringLiteral("已保存但当前不可用：") + msg);
                }
            }, true);
        }, 30000, true);
}

void ScriptSourceManager::update(const QString &id, DoneCallback cb)
{
    Item *item = find(id);
    if (!item) {
        if (cb) cb(false, QStringLiteral("音源不存在"));
        return;
    }
    importUrl(item->url, cb);
}

void ScriptSourceManager::check(const QString &id, DoneCallback cb)
{
    Item *item = find(id);
    if (!item) {
        if (cb) cb(false, QStringLiteral("音源不存在"));
        return;
    }
    if (!item->enabled) {
        if (cb) cb(false, QStringLiteral("该音源已禁用"));
        return;
    }
    startEngine(*item, cb, true);
}

void ScriptSourceManager::checkAll()
{
    for (const QString &id : m_items.keys()) {
        Item &item = m_items[id];
        if (!item.enabled || item.script.isEmpty()) continue;
        if (m_starting.contains(id)) continue;
        startEngine(item, {}, true);
    }
}

void ScriptSourceManager::remove(const QString &id)
{
    // 内置音源被用户显式删除后不再随启动补回
    const Item removed = m_items.value(id);
    if (isBuiltinUrl(removed.url)) {
        QStringList list = Settings::get(QStringLiteral("builtinScriptRemoved"))
                               .toStringList();
        if (!list.contains(removed.url)) {
            list << removed.url;
            Settings::set(QStringLiteral("builtinScriptRemoved"), list);
        }
    }
    if (QPointer<ScriptEngine> e = m_engines.take(id)) {
        e->stop();
        e->deleteLater();
    }
    m_starting.remove(id);
    m_waiters.remove(id);
    m_items.remove(id);
    Library::instance()->removeScriptSource(id);
    emit changed();
}

void ScriptSourceManager::setEnabled(const QString &id, bool enabled)
{
    Item *item = find(id);
    if (!item) return;
    item->enabled = enabled;
    if (!enabled) {
        if (QPointer<ScriptEngine> e = m_engines.take(id)) {
            e->stop();
            e->deleteLater();
        }
        m_starting.remove(id);
        item->status = QStringLiteral("disabled");
    } else if (item->status == QLatin1String("disabled")) {
        item->status = QStringLiteral("pending");
        item->errorType.clear();
        item->errorMsg.clear();
    }
    persist(*item);
    emit changed();
    if (enabled) startEngine(*item, {}, true);
}

QString ScriptSourceManager::platformToLx(const QString &platform)
{
    if (platform == QLatin1String("netease") || platform == QLatin1String("wy")) return QStringLiteral("wy");
    if (platform == QLatin1String("qq") || platform == QLatin1String("tx")) return QStringLiteral("tx");
    if (platform == QLatin1String("kugou") || platform == QLatin1String("kg")) return QStringLiteral("kg");
    if (platform == QLatin1String("kuwo") || platform == QLatin1String("kw")) return QStringLiteral("kw");
    if (platform == QLatin1String("migu") || platform == QLatin1String("mg")) return QStringLiteral("mg");
    return QString();
}

QString ScriptSourceManager::qualityToLx(const QString &quality)
{
    const QString q = quality.toLower();
    if (q == QLatin1String("high")) return QStringLiteral("320k");
    if (q == QLatin1String("lossless")) return QStringLiteral("flac");
    if (q == QLatin1String("hires")) return QStringLiteral("flac24bit");
    return QStringLiteral("128k");
}

QString ScriptSourceManager::classifyNetworkError(const QString &error, int status)
{
    if (status == 404) return QStringLiteral("HTTP 404");
    if (status >= 400) return QStringLiteral("HTTP %1").arg(status);
    const QString lower = error.toLower();
    if (lower.contains(QLatin1String("ssl")) || lower.contains(QLatin1String("tls"))
        || lower.contains(QLatin1String("certificate")))
        return QStringLiteral("TLS/证书");
    if (lower.contains(QLatin1String("timed out")) || lower.contains(QLatin1String("超时")))
        return QStringLiteral("超时");
    if (lower.contains(QLatin1String("host")) || lower.contains(QLatin1String("conn"))
        || lower.contains(QLatin1String("network")))
        return QStringLiteral("网络");
    return QStringLiteral("网络");
}

QVector<ScriptEngine *> ScriptSourceManager::enginesFor(const QString &lxKey, const QString &action)
{
    QVector<ScriptEngine *> out;
    for (auto it = m_engines.constBegin(); it != m_engines.constEnd(); ++it) {
        ScriptEngine *e = it.value().data();
        if (!e || !e->ready()) continue;
        auto item = m_items.constFind(it.key());
        if (item == m_items.constEnd() || !item->enabled) continue;
        if (e->supports(lxKey, action)) out << e;
    }
    return out;
}

QVariantMap ScriptSourceManager::musicInfoFor(const Song &song, const QString &lxKey) const
{
    QVariantMap info = song.extra;
    info.insert(QStringLiteral("name"), song.title);
    info.insert(QStringLiteral("singer"), song.artist);
    info.insert(QStringLiteral("albumName"), song.album);
    info.insert(QStringLiteral("albumId"), song.albumId);
    info.insert(QStringLiteral("source"), lxKey);
    info.insert(QStringLiteral("img"), song.cover);
    const QString interval = formatInterval(song.duration);
    if (!interval.isEmpty()) info.insert(QStringLiteral("interval"), interval);
    if (!song.sourceId.isEmpty()) {
        // 不同平台/脚本对同一个 id 的字段名不一样，全部给上别名，避免脚本读到 undefined
        for (const QString &key : {QStringLiteral("id"), QStringLiteral("musicId"),
                                   QStringLiteral("songmid"), QStringLiteral("hash"),
                                   QStringLiteral("copyrightId"), QStringLiteral("rid"),
                                   QStringLiteral("albumId")}) {
            if (!info.contains(key) || info.value(key).toString().isEmpty())
                info.insert(key, song.sourceId);
        }
    }
    return info;
}

void ScriptSourceManager::search(const QString &keyword, int limit, SongsCallback cb)
{
    if (keyword.trimmed().isEmpty() || !hasSearch()) {
        if (cb) cb(QVector<Song>());
        return;
    }

    struct State {
        QVector<Song> songs;
        int pending = 0;
        bool done = false;
        SongsCallback cb;
    };
    auto state = std::make_shared<State>();
    state->cb = cb;

    // 先把还未初始化的源拉起来（首次搜索时脚本需要联网自检）
    QStringList notReady;
    for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
        if (!it->enabled) continue;
        QPointer<ScriptEngine> e = m_engines.value(it.key());
        if (!e || !e->ready()) notReady << it.key();
    }

    auto run = [this, state, keyword, limit] {
        for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
            if (!it->enabled) continue;
            ScriptEngine *engine = m_engines.value(it.key()).data();
            if (!engine || !engine->ready()) continue;
            for (const ScriptSourceInfo &src : engine->sources()) {
                if (!src.actions.contains(QStringLiteral("search"))) continue;
                ++state->pending;
                QVariantMap info;
                info.insert(QStringLiteral("keyword"), keyword);
                info.insert(QStringLiteral("page"), 1);
                info.insert(QStringLiteral("limit"), limit);
                info.insert(QStringLiteral("type"), 1);
                QPointer<ScriptEngine> guard(engine);
                const QString scriptId = it.key();
                const QString scriptName = it->name;
                engine->callAction(src.key, QStringLiteral("search"), info,
                    [state, guard, scriptId, scriptName, src, limit](const ScriptActionResult &r) {
                        Q_UNUSED(guard);
                        if (!state->done) {
                            QVariant data = r.data;
                            QVariantList list;
                            if (data.canConvert<QVariantList>()) {
                                list = data.toList();
                            } else if (data.canConvert<QVariantMap>()) {
                                const QVariantMap m = data.toMap();
                                for (const QString &key : {QStringLiteral("list"), QStringLiteral("data"),
                                                           QStringLiteral("songs"), QStringLiteral("result")}) {
                                    if (m.value(key).canConvert<QVariantList>()) {
                                        list = m.value(key).toList();
                                        break;
                                    }
                                }
                            }
                            for (const QVariant &v : list) {
                                if (state->songs.size() >= qMax(5, limit) * 3) break;
                                const QVariantMap raw = v.toMap();
                                const QString title = entryTitle(raw);
                                if (title.isEmpty()) continue;
                                Song s;
                                const QString sid = entrySourceId(raw);
                                s.uid = QStringLiteral("script:%1:%2:%3")
                                            .arg(scriptId, src.key,
                                                 sid.isEmpty() ? QString::number(state->songs.size()) : sid);
                                s.source = QStringLiteral("custom");
                                s.sourceId = sid.isEmpty() ? s.uid : sid;
                                s.title = title;
                                s.artist = entryArtist(raw);
                                s.album = entryText(raw, {QStringLiteral("albumName"), QStringLiteral("album")});
                                s.albumId = entryText(raw, {QStringLiteral("albumId")});
                                s.cover = entryText(raw, {QStringLiteral("img"), QStringLiteral("pic"),
                                                          QStringLiteral("cover"), QStringLiteral("albumPic")});
                                s.duration = parseInterval(raw.value(QStringLiteral("interval")));
                                if (s.duration <= 0)
                                    s.duration = parseInterval(raw.value(QStringLiteral("duration")));
                                s.vip = raw.value(QStringLiteral("vip")).toBool();
                                s.extra = raw;
                                s.extra.insert(QStringLiteral("scriptId"), scriptId);
                                s.extra.insert(QStringLiteral("scriptName"), scriptName);
                                s.extra.insert(QStringLiteral("lxKey"), src.key);
                                state->songs << s;
                            }
                        }
                        if (--state->pending == 0 && !state->done) {
                            state->done = true;
                            if (state->cb) state->cb(state->songs);
                        }
                    }, 15000);
            }
        }
        // 没有任何可用源时立刻回调，避免界面空转
        if (state->pending == 0 && !state->done) {
            state->done = true;
            if (state->cb) state->cb(state->songs);
        }
    };

    if (notReady.isEmpty()) {
        run();
        return;
    }
    auto remaining = std::make_shared<int>(notReady.size());
    for (const QString &id : notReady) {
        ensureEngine(id, [remaining, run](bool) {
            if (--(*remaining) == 0) run();
        });
    }
    // 大体积混淆脚本（如 sixyin）初始化可能超过 20s，兜底放宽到 32s
    QTimer::singleShot(32000, this, [state] {
        if (state->done) return;
        state->done = true;
        if (state->cb) state->cb(state->songs);
    });
}

void ScriptSourceManager::resolve(const Song &song, const QString &quality, PlayableCallback cb)
{
    if (!hasMusicUrl()) {
        Playable p;
        p.reason = QStringLiteral("no-source");
        p.message = QStringLiteral("没有启用的在线取链音源（导入后请确认状态为「正常」；失效音源需更换）");
        if (cb) cb(p);
        return;
    }

    struct Candidate {
        QString engineId;
        QString lxKey;
        QString quality;
        QString name;
    };
    QVector<Candidate> candidates;
    const QString wanted = qualityToLx(quality);

    // 脚本只声明自己支持的音质。宿主要 high/lossless 时若直接把 320k/flac
    // 丢给只支持 128k 的音源，取链会被拒；这里收敛到声明范围内。
    auto pickQuality = [&wanted](const QJsonObject &o) {
        QStringList qs = jsonToStrings(o.value(QStringLiteral("qualities")));
        if (qs.isEmpty()) qs = jsonToStrings(o.value(QStringLiteral("qualitys")));
        if (qs.isEmpty() || qs.contains(wanted)) return wanted;
        if (qs.contains(QStringLiteral("128k"))) return QStringLiteral("128k");
        return qs.first();
    };
    auto addByKey = [this, &candidates, &pickQuality](const QString &key) {
        if (key.isEmpty()) return;
        for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
            if (!it->enabled) continue;
            for (const QJsonValue &v : it->sources) {
                const QJsonObject o = v.toObject();
                if (o.value(QStringLiteral("key")).toString() != key) continue;
                if (!jsonToStrings(o.value(QStringLiteral("actions"))).contains(QStringLiteral("musicUrl")))
                    continue;
                bool dup = false;
                for (const Candidate &c : candidates)
                    if (c.engineId == it.key() && c.lxKey == key) { dup = true; break; }
                if (!dup) candidates << Candidate{it.key(), key, pickQuality(o), it->name};
                break;
            }
        }
    };

    const QString storedScript = song.extra.value(QStringLiteral("scriptId")).toString();
    const QString storedKey = song.extra.value(QStringLiteral("lxKey")).toString();
    if (!storedScript.isEmpty() && !storedKey.isEmpty()) {
        // 脚本搜索出来的歌优先用原脚本；失败后仍继续尝试其它同类音源
        if (Item *it = find(storedScript)) {
            if (it->enabled) {
                QString q = wanted;
                for (const QJsonValue &v : it->sources) {
                    const QJsonObject o = v.toObject();
                    if (o.value(QStringLiteral("key")).toString() == storedKey) {
                        q = pickQuality(o);
                        break;
                    }
                }
                candidates << Candidate{storedScript, storedKey, q, it->name};
            }
        }
        addByKey(storedKey);
    }
    addByKey(platformToLx(song.source));

    if (candidates.isEmpty()) {
        Playable p;
        p.reason = QStringLiteral("no-capability");
        p.message = song.source == QLatin1String("custom")
            ? QStringLiteral("该歌曲没有对应的在线音源")
            : QStringLiteral("已导入的在线音源未声明该平台（%1）的取链能力").arg(song.source);
        if (cb) cb(p);
        return;
    }

    struct ResolveState {
        PlayableCallback cb;
        Song song;
        QVector<Candidate> candidates;
        QStringList failures;   // 逐个音源记录真实失败原因，别再只报「均未取到」
        int index = 0;
    };
    auto state = std::make_shared<ResolveState>();
    state->cb = cb;
    state->song = song;
    state->candidates = candidates;

    // 用 weak_ptr 打断「shared_ptr -> function -> shared_ptr」的自捕获循环；
    // 链上的异步回调各自捕获局部强引用 step，链结束即随回调一起释放。
    auto next = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weakNext = next;
    *next = [this, state, weakNext]() {
        auto step = weakNext.lock();
        if (!step) return;
        if (state->index >= state->candidates.size()) {
            Playable p;
            p.reason = QStringLiteral("script-failed");
            p.message = state->failures.isEmpty()
                ? QStringLiteral("所有在线音源均未能取到音频地址")
                : QStringLiteral("在线音源取链失败：%1").arg(state->failures.join(QStringLiteral("；")));
            if (state->cb) state->cb(p);
            return;
        }
        const Candidate cand = state->candidates.at(state->index++);
        ensureEngine(cand.engineId, [this, state, cand, step](bool ok) {
            if (!ok) {
                state->failures << QStringLiteral("%1：初始化失败").arg(cand.name);
                (*step)();
                return;
            }
            ScriptEngine *engine = m_engines.value(cand.engineId).data();
            if (!engine || !engine->ready()) {
                state->failures << QStringLiteral("%1：脚本未就绪").arg(cand.name);
                (*step)();
                return;
            }
            QVariantMap info;
            info.insert(QStringLiteral("type"), cand.quality);
            info.insert(QStringLiteral("musicInfo"), musicInfoFor(state->song, cand.lxKey));
            engine->callAction(cand.lxKey, QStringLiteral("musicUrl"), info,
                [state, cand, step](const ScriptActionResult &r) {
                    QString url;
                    if (r.ok) {
                        if (r.data.canConvert<QVariantMap>()) {
                            const QVariantMap m = r.data.toMap();
                            url = m.value(QStringLiteral("url")).toString();
                        } else {
                            url = r.data.toString();
                        }
                    }
                    if (!url.isEmpty()) {
                        Playable p;
                        p.ok = true;
                        p.url = QUrl(url);
                        p.substituted = true;
                        p.substituteSource = QStringLiteral("在线音源 · ") + cand.name;
                        p.message = QStringLiteral("已由在线音源「%1」提供").arg(cand.name);
                        if (state->cb) state->cb(p);
                        return;
                    }
                    QString reason = r.ok ? QStringLiteral("未返回音频地址") : r.error;
                    if (reason.trimmed().isEmpty()) reason = QStringLiteral("取链失败");
                    state->failures << QStringLiteral("%1：%2").arg(cand.name, reason.left(80));
                    (*step)();
                }, 20000);
        });
    };
    (*next)();
}

void ScriptSourceManager::lyric(const Song &song, StringCallback cb)
{
    // 内置平台歌曲的歌词接口常常返回空（VIP / 下架 / 无版权），此时也应该
    // 让所有声明了 lyric 能力的脚本依次兜底，而不是只认脚本搜索出来的歌。
    if (!hasEnabled()) { if (cb) cb(QString()); return; }

    struct Candidate { QString engineId; QString key; };
    QVector<Candidate> candidates;
    auto add = [this, &candidates](const QString &key) {
        if (key.isEmpty()) return;
        for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
            if (!it->enabled) continue;
            for (const QJsonValue &v : it->sources) {
                const QJsonObject o = v.toObject();
                if (o.value(QStringLiteral("key")).toString() != key) continue;
                if (!jsonToStrings(o.value(QStringLiteral("actions"))).contains(QStringLiteral("lyric")))
                    continue;
                bool dup = false;
                for (const Candidate &c : candidates)
                    if (c.engineId == it.key() && c.key == key) { dup = true; break; }
                if (!dup) candidates << Candidate{it.key(), key};
                break;
            }
        }
    };
    const QString storedScript = song.extra.value(QStringLiteral("scriptId")).toString();
    const QString storedKey = song.extra.value(QStringLiteral("lxKey")).toString();
    if (!storedScript.isEmpty() && !storedKey.isEmpty()) {
        if (Item *it = find(storedScript)) {
            if (it->enabled) candidates << Candidate{storedScript, storedKey};
        }
        add(storedKey);
    }
    add(platformToLx(song.source));

    if (candidates.isEmpty()) { if (cb) cb(QString()); return; }

    struct State {
        StringCallback cb;
        Song song;
        QVector<Candidate> candidates;
        int index = 0;
    };
    auto state = std::make_shared<State>();
    state->cb = cb;
    state->song = song;
    state->candidates = candidates;
    auto next = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weakNext = next;
    *next = [this, state, weakNext]() {
        auto step = weakNext.lock();
        if (!step) return;
        if (state->index >= state->candidates.size()) {
            if (state->cb) state->cb(QString());
            return;
        }
        const Candidate cand = state->candidates.at(state->index++);
        ensureEngine(cand.engineId, [this, state, cand, step](bool ok) {
            if (!ok) { (*step)(); return; }
            ScriptEngine *engine = m_engines.value(cand.engineId).data();
            if (!engine || !engine->ready() || !engine->supports(cand.key, QStringLiteral("lyric"))) {
                (*step)();
                return;
            }
            QVariantMap info;
            info.insert(QStringLiteral("musicInfo"), musicInfoFor(state->song, cand.key));
            engine->callAction(cand.key, QStringLiteral("lyric"), info,
                [state, step](const ScriptActionResult &r) {
                    QString text;
                    if (r.ok) {
                        if (r.data.canConvert<QVariantMap>()) {
                            const QVariantMap m = r.data.toMap();
                            text = m.value(QStringLiteral("lyric")).toString();
                            if (text.isEmpty()) text = m.value(QStringLiteral("lrc")).toString();
                        } else {
                            text = r.data.toString();
                        }
                    }
                    if (!text.trimmed().isEmpty()) { if (state->cb) state->cb(text); return; }
                    (*step)();
                }, 15000);
        });
    };
    (*next)();
}

} // namespace Furui
