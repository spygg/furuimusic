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
#include <limits>
#include <memory>

#include "HttpClient.h"
#include "Library.h"
#include "Settings.h"

namespace Furui {

namespace {

QString normalizeSourceName(const QString &raw);

// 内置在线音源（LX 脚本协议）动态发现：
// 直接解析 github.com/pdone/lx-music-source 仓库页面（README 原文 + 每日可用性
// health-state.json），不再维护硬编码清单。README 的每个「### 名称」小节给出一份
// latest.js 原始地址，健康状态用目录名标记 可用/异常/失效。
// 同一脚本给多条镜像（加速站可达性随时变化，依次回退，成功即用）。
// 仅首次启动 / 缺源时预置下载，用户在设置页删除后不再补回（记入 builtinScriptRemoved）。
// 页面里解析到的音源全部导入，健康状态只用于排序，不再作为筛选条件。
// README 页尾列出的加速站（访问 GitHub 受限时轮换使用）。可达性随时变化，
// 所以不挑「唯一可用」的那家：按顺序逐条回退，内容校验不过就换下一条。
// 已知 ghproxy.cn 会返回 200 + 反爬 HTML，放最后再用（内容校验会拦掉）。
const char *kMirrorPrefixes[] = {
    "https://gh-proxy.org/",
    "https://ghfast.top/",
    "https://ghproxy.net/",
    // 下面几条在 2026-11 实测不可达或返回反爬页（内容校验会拦下），只作兜底
    "https://gh.llkk.cc/",
    "https://github.moeyy.xyz/",
    "https://ghp.ci/",
    "https://gh.api.99988866.xyz/",
    "https://ghproxy.cc/",
    "https://ghproxy.cn/",
};

const QString kRepoRawRoot = QStringLiteral("https://raw.githubusercontent.com/");

// 仓库文件的多条地址：全部加速站在前，直连原始地址兜底
QStringList repoFileMirrors(const QString &path)
{
    const QString suffix =
        QStringLiteral("raw.githubusercontent.com/pdone/lx-music-source/") + path;
    const QString relative = QStringLiteral("pdone/lx-music-source/") + path;
    QStringList urls;
    for (const char *prefix : kMirrorPrefixes)
        urls << QLatin1String(prefix) + suffix;
    urls << kRepoRawRoot + relative;
    return urls;
}

void fetchFirst(const QStringList &urls, int index,
                std::function<void(bool, const QString &)> cb,
                std::function<bool(const QString &)> accept,
                const std::shared_ptr<qint64> &deadline = std::shared_ptr<qint64>())
{
    // 10 条镜像 × 20s = 最坏 200s，启动时会把界面卡在「正在发现音源」。
    // 所有镜像共用一条总期限，单条超时再按剩余时间收紧。
    if (index >= urls.size()
        || (deadline && QDateTime::currentMSecsSinceEpoch() >= *deadline)) {
        if (cb) cb(false, QString());
        return;
    }
    const int perMirror = deadline
        ? qMin(20000, int(qMax<qint64>(1, *deadline - QDateTime::currentMSecsSinceEpoch())))
        : 20000;
    QVariantMap headers;
    headers.insert(QStringLiteral("User-Agent"), QStringLiteral("FuruiMusic/1.0"));
    headers.insert(QStringLiteral("Accept"), QStringLiteral("*/*"));
    HttpClient::instance()->request(
        QStringLiteral("GET"), QUrl(urls.at(index)), headers, QByteArray(),
        [urls, index, cb, accept, deadline](const HttpClient::Reply &r) {
            // 部分加速站会返回 200 + 反爬 HTML（例如 ghproxy.cn），必须校验内容，
            // 否则第一条镜像就把后续可用镜像挡掉了。
            if (r.ok && !r.body.trimmed().isEmpty()) {
                const QString text = QString::fromUtf8(r.body);
                if (!accept || accept(text)) {
                    if (cb) cb(true, text);
                    return;
                }
                qWarning("镜像内容不可用，继续下一条：%s",
                         qUtf8Printable(urls.at(index)));
            }
            fetchFirst(urls, index + 1, cb, accept, deadline);
        }, perMirror, true);
}

QStringList mirrorUrlsForImpl(const QString &rawUrl)
{
    const QString host = QStringLiteral("raw.githubusercontent.com");
    const int at = rawUrl.indexOf(host);
    if (at < 0) return QStringList() << rawUrl;
    // 保留 host 之后的完整路径：加速站的形态就是 <前缀>/raw.githubusercontent.com/<owner>/...
    const QString suffix = rawUrl.mid(at);
    QStringList urls;
    for (const char *prefix : kMirrorPrefixes)
        urls << QLatin1String(prefix) + suffix;
    urls << QStringLiteral("https://") + suffix;
    return urls;
}

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

// 按 key 候选顺序取第一个非空字符串（脚本评论字段名宽松匹配用）
QString firstVariantString(const QVariantMap &m, const QStringList &keys)
{
    for (const QString &k : keys) {
        const QString s = m.value(k).toString();
        if (!s.isEmpty()) return s;
    }
    return QString();
}

// 把毫秒时间戳格式化为「昨天/MM-DD HH:mm」展示文本（与 MusicSource 同规则）
QString commentTimeText(qint64 ms)
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

// 脚本的 search 返回值形状不统一：可能直接是数组，也可能包在 list/data/songs/result 里
QVariantList actionList(const QVariant &data)
{
    if (data.canConvert<QVariantList>())
        return data.toList();
    if (data.canConvert<QVariantMap>()) {
        const QVariantMap m = data.toMap();
        for (const QString &key : {QStringLiteral("list"), QStringLiteral("data"),
                                   QStringLiteral("songs"), QStringLiteral("result")}) {
            if (m.value(key).canConvert<QVariantList>())
                return m.value(key).toList();
        }
    }
    return QVariantList();
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
    // 在线解析音源（pdone/lx-music-source 仓库自动发现）默认完全不做初始化：
    // 必须在设置页输入「我自己承担后果」并连点「导入」确认（AppController::seedOnlineSources）
    // 置解锁标记后才放开预置；已确认过的用户重启时才补齐缺失源。
    QTimer::singleShot(0, this, [this] {
        if (Settings::get(QStringLiteral("onlineSourcesUnlocked")).toBool())
            seedBuiltinSources();
    });
}

void ScriptSourceManager::load()
{
    m_items.clear();
    const QVariantList rows = Library::instance()->scriptSources();
    QStringList removedList = Settings::get(QStringLiteral("builtinScriptRemoved")).toStringList();
    bool removedChanged = false;
    for (const QVariant &v : rows) {
        const QVariantMap m = v.toMap();
        Item item;
        item.id        = m.value(QStringLiteral("id")).toString();
        item.name      = m.value(QStringLiteral("name")).toString();
        item.url       = m.value(QStringLiteral("url")).toString();
        // 上游已下架的恶意脚本：历史安装直接清掉，并记入「不再自动补回」
        if (isBlockedScriptUrl(item.url)) {
            Library::instance()->removeScriptSource(item.id);
            if (!removedList.contains(item.url)) {
                removedList << item.url;
                removedChanged = true;
            }
            qWarning("已移除上游下架的恶意音源：%s", qUtf8Printable(item.name));
            continue;
        }
        // 花体字/@name 里的形近字在车机字体下会显示成方框，载入时统一规整并回写
        const QString normalized = normalizeSourceName(item.name);
        const bool nameChanged = normalized != item.name;
        if (nameChanged) item.name = normalized;
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
        if (!item.id.isEmpty() && !item.script.isEmpty()) {
            m_items.insert(item.id, item);
            if (nameChanged) persist(item);
        }
    }
    if (removedChanged) Settings::set(QStringLiteral("builtinScriptRemoved"), removedList);
    // 音源使用顺序 = 权重顺序。老库没有该键时按名称补一份，之后由取链结果动态调整。
    const QStringList saved = Settings::get(QStringLiteral("scriptSourceOrder")).toStringList();
    m_order.clear();
    for (const QString &id : saved) {
        if (m_items.contains(id) && !m_order.contains(id)) m_order << id;
    }
    QStringList rest = m_items.keys();
    std::sort(rest.begin(), rest.end(), [this](const QString &a, const QString &b) {
        return m_items.value(a).name < m_items.value(b).name;
    });
    for (const QString &id : rest) {
        if (!m_order.contains(id)) m_order << id;
    }
    loadScores();
}

int ScriptSourceManager::rankOf(const QString &id) const
{
    const int index = m_order.indexOf(id);
    return index < 0 ? std::numeric_limits<int>::max() : index;
}

QStringList ScriptSourceManager::orderedIds() const
{
    QStringList ids = m_items.keys();
    std::sort(ids.begin(), ids.end(), [this](const QString &a, const QString &b) {
        const int ra = rankOf(a);
        const int rb = rankOf(b);
        if (ra != rb) return ra < rb;
        return m_items.value(a).name < m_items.value(b).name;
    });
    return ids;
}

void ScriptSourceManager::saveOrder()
{
    Settings::set(QStringLiteral("scriptSourceOrder"), m_order);
}

void ScriptSourceManager::loadScores()
{
    m_score.clear();
    const QString raw = Settings::string(QStringLiteral("scriptSourceScore"));
    if (raw.isEmpty())
        return;
    const QStringList parts = raw.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const int eq = part.lastIndexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        bool ok = false;
        const int v = part.mid(eq + 1).toInt(&ok);
        if (!ok)
            continue;
        m_score.insert(part.left(eq), qBound(-6, v, 12));
    }
}

void ScriptSourceManager::saveScores()
{
    QStringList parts;
    for (auto it = m_score.constBegin(); it != m_score.constEnd(); ++it) {
        if (it.value() == 0)
            continue;   // 0 是初始值，不落库，避免成绩表无限膨胀
        parts << (it.key() + QLatin1Char('=') + QString::number(it.value()));
    }
    Settings::set(QStringLiteral("scriptSourceScore"), parts.join(QLatin1Char(';')));
}

int ScriptSourceManager::scoreOf(const QString &id, const QString &lxKey) const
{
    if (id.isEmpty() || lxKey.isEmpty())
        return 0;
    return m_score.value(id + QLatin1Char('|') + lxKey, 0);
}

void ScriptSourceManager::reportResult(const QString &id, const QString &lxKey, bool ok, int weight)
{
    if (id.isEmpty() || lxKey.isEmpty())
        return;
    const QString key = id + QLatin1Char('|') + lxKey;
    const int now = m_score.value(key, 0);
    // 成功 +2 / 失败 -2：一次成功刚好抵一次失败，既不会被一次偶然失败沉底，
    // 也不至于「曾经成功过一次就永远排第一」。上下限防止极端值锁死排序。
    // weight 非 0 时用它代替默认步长：「返回了地址但根本播不出来」比普通失败更严重。
    const int delta = weight != 0 ? weight : (ok ? 2 : -2);
    m_score.insert(key, qBound(-6, now + delta, 12));
    saveScores();
    // 会话级失败计数：连续/累计失败 2 次的候选，后续换源直接跳过（成功一次即恢复）。
    // 只记「失败」这一极性：weight 为正但 ok=false（如播放失败 -4/-8）也计入。
    if (ok)
        m_candidateFails.remove(key);
    else
        ++m_candidateFails[key];
}

void ScriptSourceManager::notePlaybackFailure(const QString &id, const QString &lxKey,
                                              const QString &songKey)
{
    if (id.isEmpty() || lxKey.isEmpty() || songKey.isEmpty())
        return;
    if (songKey != m_badPlaybackSong) {
        m_badPlaybackSong = songKey;
        m_badPlayback.clear();
    }
    m_badPlayback.insert(songKey + QLatin1Char('|') + id + QLatin1Char('|') + lxKey);
    qWarning().noquote() << QStringLiteral("[换源] 本首跳过该候选：") + id
                                + QStringLiteral(" [") + lxKey + QLatin1Char(']');
}

void ScriptSourceManager::promoteSource(const QString &id)
{
    if (id.isEmpty()) return;
    const int index = m_order.indexOf(id);
    if (index == 0) return;
    if (index > 0) m_order.removeAt(index);
    m_order.prepend(id);
    saveOrder();
}

void ScriptSourceManager::demoteSource(const QString &id)
{
    if (id.isEmpty()) return;
    const int index = m_order.indexOf(id);
    if (index < 0) {
        m_order << id;
        saveOrder();
        return;
    }
    // 只后移一位：一次失败（版权/临时 5xx）不该把音源直接沉底
    if (index + 1 >= m_order.size()) return;
    m_order.move(index, index + 1);
    saveOrder();
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
    // 兼容所有加速镜像：只要指向 pdone/lx-music-source 仓库里的 .js 就算内置发现源
    const QString trimmed = url.trimmed();
    return trimmed.contains(QLatin1String("pdone/lx-music-source/"))
        && trimmed.endsWith(QLatin1String(".js"), Qt::CaseInsensitive);
}

bool ScriptSourceManager::isBlockedScriptUrl(const QString &url)
{
    // 上游 2026-11-22 下架通告：LX（独家音源）在具备 Node 能力的宿主里执行
    // shutdown 强制关机。本应用用内嵌 QuickJS、没有 Node/process/shutdown，
    // 但仍然按上游通告整条拉黑，避免其它宿主能力开放后重新中招。
    const QString p = QUrl(url.trimmed()).path().toLower();
    return p.contains(QLatin1String("/pdone/lx-music-source/"))
        && p.contains(QLatin1String("/lx/latest.js"));
}

namespace {

// 部分音源作者用「花体字」写 @name（数学字母 + 各种语言的形近字），普通字体
// 渲染成方框/乱码。先做 Unicode NFKC（把数学粗体/花体还原成 ASCII），再补几个
// 生成站点常用的形近字映射。
QString normalizeSourceName(const QString &raw)
{
    QString s = raw.normalized(QString::NormalizationForm_KC).trimmed();
    static const QVector<QPair<QChar, QChar>> kLookAlikes = {
        {QChar(0x0C7F), QLatin1Char('e')}, // ౿ Telugu → e
        {QChar(0x1963), QLatin1Char('l')}, // ᥣ Tai Le → l
        {QChar(0x051C), QLatin1Char('W')}, // Ԝ Cyrillic → W
        {QChar(0x146F), QLatin1Char('d')}, // ᑯ Canadian → d
        {QChar(0x0131), QLatin1Char('i')}, // ı dotless i → i
        {QChar(0x0456), QLatin1Char('i')}, // і Cyrillic → i
    };
    for (const auto &pair : kLookAlikes) s.replace(pair.first, pair.second);
    return s;
}

} // namespace

QVector<ScriptSourceManager::DiscoveredSource>
ScriptSourceManager::parseRepoPage(const QString &text)
{
    QVector<DiscoveredSource> out;
    QSet<QString> seen;
    static const QRegularExpression heading(
        QStringLiteral("^#{2,4}\\s*([^\\r\\n#]+?)\\s*$"));
    static const QRegularExpression link(
        QStringLiteral("https?://[^\\s`\"'<>]+/latest\\.js"));
    QString currentName;
    QStringList sectionLinks;

    // 每个小节可能给多条地址（原始 raw + 各加速站）。全部收下来当回退链，
    // 只取第一条会在该镜像被墙/限流时整节失效。
    auto flush = [&out, &seen, &sectionLinks, &currentName]() {
        if (currentName.isEmpty() || sectionLinks.isEmpty()) {
            sectionLinks.clear();
            return;
        }
        const QStringList parts =
            QUrl(sectionLinks.first()).path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.size() >= 2) {
            const QString key = parts.at(parts.size() - 2);
            if (!key.isEmpty() && !seen.contains(key)) {
                seen.insert(key);
                DiscoveredSource d;
                d.key = key;
                d.name = currentName;
                for (const QString &url : sectionLinks) {
                    // 统一出 raw 原始地址，便于再按镜像表生成回退链
                    const int at = url.indexOf(QStringLiteral("raw.githubusercontent.com"));
                    const QString raw = at > 0
                        ? QStringLiteral("https://") + url.mid(at) : url;
                    if (isBlockedScriptUrl(raw)) continue;
                    if (d.url.isEmpty()) d.url = raw;
                    // 页面里实际给出的地址原样保留（可能含镜像表里没有的加速站）
                    if (!d.mirrors.contains(url)) d.mirrors << url;
                }
                if (!d.url.isEmpty()) out.append(d);
            }
        }
        sectionLinks.clear();
    };

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        const QRegularExpressionMatch hm = heading.match(line);
        if (hm.hasMatch()) {
            flush();
            currentName = hm.captured(1).trimmed();
            continue;
        }
        if (currentName.isEmpty()) continue;
        auto it = link.globalMatch(line);
        while (it.hasNext()) {
            const QString url = it.next().captured(0);
            if (!sectionLinks.contains(url)) sectionLinks << url;
        }
    }
    flush();
    return out;
}

QStringList ScriptSourceManager::mirrorUrlsFor(const QString &rawUrl)
{
    return mirrorUrlsForImpl(rawUrl.trimmed());
}

void ScriptSourceManager::rescanRepository()
{
    // 用户点「发现音源」：跳过 24h 节流，立即重新解析仓库页面
    Settings::set(QStringLiteral("builtinSeedAt"), 0);
    seedBuiltinSources();
}

void ScriptSourceManager::seedBuiltinSources()
{
    if (m_seeding) return;
    // 内置源自动预置要「安静且不折腾」：
    //   · 全程 silent，成功失败都不弹提示（失败原因留在音源条目的 errorMsg 里）；
    //   · 失败源不能每次启动都重下再报错。已有一个可用源时最多每天自动重试一次；
    //     一个都没有（例如冷启动没网）时 5 分钟后允许再试。
    //   用户随时可以在设置页对单个音源手动「更新」，或点「发现音源」立即重扫仓库。
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
    m_seeding = true;

    QStringList readmeUrls = repoFileMirrors(QStringLiteral("main/README.md"));
    QStringList healthUrls =
        repoFileMirrors(QStringLiteral("health-status/health-state.json"));

    QPointer<ScriptSourceManager> self(this);
    // README 与健康状态两条镜像链共用 30s 总期限（含所有回退）
    auto deadline = std::make_shared<qint64>(QDateTime::currentMSecsSinceEpoch() + 30000);
    // 1) 仓库页面（README 原文）→ 解析出当前在维护的音源清单
    fetchFirst(readmeUrls, 0, [self, healthUrls, deadline](bool ok, const QString &page) {
        ScriptSourceManager *mgr = self.data();
        if (!ok || !mgr) {
            qWarning("音源仓库发现：仓库页面获取失败，本次跳过");
            if (mgr) mgr->m_seeding = false;
            return;
        }
        const QVector<DiscoveredSource> found = parseRepoPage(page);
        if (found.isEmpty()) {
            qWarning("音源仓库发现：页面解析结果为空，本次跳过");
            mgr->m_seeding = false;
            return;
        }
        // 2) 每日可用性检测：只导入当天标记「可用」的源；取不到状态时按全部候选处理
        fetchFirst(healthUrls, 0, [self, found](bool healthOk, const QString &json) {
            ScriptSourceManager *mgr2 = self.data();
            if (!mgr2) return;
            QSet<QString> usable;
            if (healthOk) {
                const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
                if (doc.isObject()) {
                    const QJsonObject sources =
                        doc.object().value(QStringLiteral("sources")).toObject();
                    for (auto it = sources.constBegin(); it != sources.constEnd(); ++it) {
                        if (it.value().toString() == QStringLiteral("可用"))
                            usable.insert(it.key());
                    }
                }
            }
            QVector<DiscoveredSource> chosen = found;
            qInfo("音源仓库发现：解析 %d 个，健康可用 %d 个，全部导入",
                  int(found.size()), int(usable.size()));
            if (chosen.isEmpty()) {
                mgr2->m_seeding = false;
                return;
            }
            // 排序只影响取链优先级（列表顺序即优先级）：当天「可用」的排前面，
            // 其次历史验证较稳的源，其余按页面顺序；不据此剔除任何源。
            static const QStringList preferred = {
                QStringLiteral("huibq"), QStringLiteral("fish"), QStringLiteral("lx"),
                QStringLiteral("changqing"), QStringLiteral("huanyin"),
                QStringLiteral("xinghai")};
            std::stable_sort(chosen.begin(), chosen.end(),
                             [&usable](const DiscoveredSource &a, const DiscoveredSource &b) {
                const bool ua = usable.isEmpty() || usable.contains(a.key);
                const bool ub = usable.isEmpty() || usable.contains(b.key);
                if (ua != ub) return ua;
                const int ra = preferred.indexOf(a.key);
                const int rb = preferred.indexOf(b.key);
                if (ra < 0 && rb < 0) return false;
                if (ra < 0) return false;
                if (rb < 0) return true;
                return ra < rb;
            });
            mgr2->seedDiscovered(chosen, [self]() {
                if (self) self->m_seeding = false;
            });
        }, [](const QString &text) {
            // 健康状态必须是含 sources 的 JSON
            return text.contains(QStringLiteral("\"sources\""));
        }, deadline);
    }, [](const QString &text) {
        // README 必须真的包含脚本地址，避免把反爬 HTML 当成功
        return text.contains(QStringLiteral("latest.js"));
    }, deadline);
}

void ScriptSourceManager::seedDiscovered(const QVector<DiscoveredSource> &list,
                                          std::function<void()> done)
{
    const QStringList removed = Settings::get(QStringLiteral("builtinScriptRemoved"))
                                    .toStringList();
    QVector<QStringList> queue;
    for (const DiscoveredSource &d : list) {
        // 先按镜像表铺一遍，再补上页面里实际给出的地址（去重、保持顺序）
        QStringList urls = mirrorUrlsFor(d.url);
        for (const QString &u : d.mirrors) {
            if (!urls.contains(u)) urls << u;
        }
        bool skip = false;
        bool exists = false;
        for (const QString &u : urls) {
            if (removed.contains(u)) { skip = true; break; }
            const QString id = scriptIdForUrl(u);
            auto it = m_items.constFind(id);
            if (it == m_items.constEnd()) continue;
            if (it->status == QLatin1String("error")) {
                // 上次回退链留下的坏镜像产物：清掉并重新走回退
                m_items.remove(id);
                Library::instance()->removeScriptSource(id);
                continue;
            }
            exists = true;
        }
        if (skip || exists) continue;
        queue << urls;
    }
    if (queue.isEmpty()) {
        if (done) done();
        return;
    }

    // 逐个导入：同时下载 + 初始化一堆脚本会把首次启动的网络和 CPU 打满
    auto index = std::make_shared<int>(0);
    auto chain = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak = chain;
    QPointer<ScriptSourceManager> self(this);
    *chain = [self, queue, index, weak, done]() {
        auto step = weak.lock();
        if (!step) return;
        ScriptSourceManager *mgr = self.data();
        if (!mgr) return;
        if (*index >= queue.size()) {
            if (done) done();
            return;
        }
        const QStringList urls = queue.at((*index)++);
        mgr->importWithFallback(urls, 0, [step]() { (*step)(); });
    };
    (*chain)();
}

void ScriptSourceManager::importWithFallback(const QStringList &urls, int index,
                                             std::function<void()> done)
{
    if (index >= urls.size()) {
        qWarning("音源下载：%d 条镜像全部失败", int(urls.size()));
        if (done) done();
        return;
    }
    QPointer<ScriptSourceManager> self(this);
    const QString url = urls.at(index);
    // silent：自动预置属于后台行为，成功失败都不弹提示（失败细节留在音源列表里）
    importUrl(url, [self, urls, index, done, url](bool ok, const QString &) {
        ScriptSourceManager *mgr = self.data();
        if (!mgr) return;
        // 脚本已经落库就说明这条镜像可用：初始化失败是音源自身暂时不可用，
        // 换镜像重下同一份脚本没有意义，只会在列表里堆出一串重复条目。
        auto saved = mgr->m_items.constFind(scriptIdForUrl(url));
        if (ok || (saved != mgr->m_items.constEnd() && !saved->script.isEmpty())) {
            if (done) done();
            return;
        }
        qWarning("音源下载：镜像不可用，换下一条 %s", qUtf8Printable(url));
        QTimer::singleShot(0, mgr, [self, urls, index, done] {
            if (self) self->importWithFallback(urls, index + 1, done);
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

    bool search = false, musicUrl = false, lyric = false, comment = false;
    for (const QJsonValue &v : item.sources) {
        const QJsonObject o = v.toObject();
        const QStringList acts = jsonToStrings(o.value(QStringLiteral("actions")));
        if (acts.contains(QStringLiteral("search"))) search = true;
        if (acts.contains(QStringLiteral("musicUrl"))) musicUrl = true;
        if (acts.contains(QStringLiteral("lyric"))) lyric = true;
        if (acts.contains(QStringLiteral("comment"))) comment = true;
    }
    QString capability;
    if (search && musicUrl) capability = QStringLiteral("搜索 + 取链");
    else if (search) capability = QStringLiteral("仅搜索");
    else if (musicUrl) capability = QStringLiteral("仅取链（无搜索能力）");
    else capability = QStringLiteral("未声明可用能力");
    if (lyric && !capability.contains(QStringLiteral("歌词")))
        capability += QStringLiteral(" · 含歌词");
    if (comment && !capability.contains(QStringLiteral("评论")))
        capability += QStringLiteral(" · 含评论");
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
        if (acts.contains(QStringLiteral("comment"))) caps << QStringLiteral("评论");
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
    // 列表顺序 = 实际使用顺序（权重从高到低），用户能直观看到谁在优先取链
    for (const QString &id : orderedIds()) out << itemToVariant(m_items.value(id));
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
        name = QUrl::fromPercentEncoding(
            path.section(QLatin1Char('/'), -2, -2).toUtf8());
        if (name.isEmpty()) name = QStringLiteral("LX 音源");
    }
    item.name = normalizeSourceName(name);
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

void ScriptSourceManager::startEngine(Item &item, DoneCallback cb, bool persistResult, bool silent)
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
    engine->start(item.script, meta, [self, id, cb, persistResult, srcName, silent](const ScriptInitResult &r) {
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
        // 引擎初始化成败记录：失败进冷却（运行期不再反复拉起），成功移除冷却
        if (r.ok)
            mgr->m_engineFailAt.remove(id);
        else
            mgr->m_engineFailAt.insert(id, QDateTime::currentMSecsSinceEpoch());
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
        if ((!r.ok && !silent) || cb)
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
    // 失败冷却：初始化失败过的引擎 10 分钟内不再反复拉起（音源只在启动时检查一次，
    // 运行期每次取链都重试只会拖慢换源、反复报同样的错误）。冷却期内直接判失败，
    // 让换源链路立刻跳到下一个候选。用户在设置页手动「检查」走 check()，不受此限制。
    const qint64 lastFail = m_engineFailAt.value(id, 0);
    if (lastFail > 0 && QDateTime::currentMSecsSinceEpoch() - lastFail < 10 * 60 * 1000) {
        if (cb) cb(false);
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
    if (isBlockedScriptUrl(trimmed)) {
        const QString msg = QStringLiteral("该音源已被上游下架（LX 独家音源含强制关机恶意逻辑），禁止导入");
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
            if (!mgr->m_order.contains(item.id)) {
                mgr->m_order << item.id;
                mgr->saveOrder();
            }
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

void ScriptSourceManager::warmUp()
{
    // 只预热真正能取播放链接的脚本；搜索/歌词类脚本等用到时再起。
    // persistResult=false、silent=true：不改库、不弹提示（失败状态仍记在内存里）。
    //
    // 移动端限制数量：每个引擎常驻约 2~3MB，17 个全量预热在手机上就是 40MB 上下的
    // 常驻内存 + 一轮密集 QuickJS 初始化（实测还会把某个脚本顶到 32s 初始化超时）。
    // 只预热用户排序最前的几个，其余在第一次播放时按需拉起。
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    int budget = 4;
#else
    int budget = std::numeric_limits<int>::max();
#endif
    for (const QString &id : orderedIds()) {
        Item item = m_items.value(id);
        if (!item.enabled || item.script.isEmpty()
            || !item.actions.contains(QStringLiteral("musicUrl")))
            continue;
        if (m_starting.contains(id)) continue;
        if (QPointer<ScriptEngine> e = m_engines.value(id)) {
            if (e->ready()) continue;
        }
        if (budget-- <= 0) break;
        startEngine(item, {}, false, true);
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
    m_order.removeAll(id);
    saveOrder();
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
    for (const QString &id : orderedIds()) {
        const Item item = m_items.value(id);
        if (!item.enabled) continue;
        QPointer<ScriptEngine> e = m_engines.value(id);
        if (!e || !e->ready()) notReady << id;
    }

    auto run = [this, state, keyword, limit] {
        // 按权重顺序发起搜索：高质量音源的结果先进入列表
        for (const QString &id : orderedIds()) {
            const Item item = m_items.value(id);
            if (!item.enabled) continue;
            ScriptEngine *engine = m_engines.value(id).data();
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
                const QString scriptId = id;
                const QString scriptName = item.name;
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

// 脚本 musicUrl 的返回形态很不统一：字符串、{url}、{data:{url}}、{url, headers}、
// 甚至 [{url}]。统一收敛成「直链 + 请求头」—— 防盗链直链不带 Referer/UA 会被 CDN 拒，
// 表现成播放器 4ms 就报 I/O error（连握手都没做成）。
static QString playUrlFrom(const QVariant &data, QVariantMap *headers)
{
    QVariantMap hdr;
    auto takeHeaders = [&hdr](const QVariantMap &m) {
        const QVariant h = m.value(QStringLiteral("headers"));
        if (!h.canConvert<QVariantMap>()) return;
        const QVariantMap hm = h.toMap();
        for (auto it = hm.constBegin(); it != hm.constEnd(); ++it)
            hdr.insert(it.key(), it.value());
    };

    QString url;
    // 先按真实类型分派，不能用 canConvert<QVariantList>() 试探：
    // QVariant(QString).canConvert<QVariantList>() 也返回 true，toList() 会把
    // "https://..." 拆成单字符序列，list.first() 拿到 'h' —— 实测所有脚本的直链
    // 都被截成首字母，播放器 4ms 报 I/O error。
    const int type = data.userType();
    if (type == QMetaType::QVariantList) {
        const QVariantList list = data.toList();
        if (!list.isEmpty()) return playUrlFrom(list.first(), headers);
    } else if (type == QMetaType::QVariantMap) {
        const QVariantMap m = data.toMap();
        takeHeaders(m);
        url = m.value(QStringLiteral("url")).toString();
        if (url.isEmpty()) {
            const QVariant d = m.value(QStringLiteral("data"));
            if (d.userType() == QMetaType::QVariantMap) {
                const QVariantMap dm = d.toMap();
                takeHeaders(dm);
                url = dm.value(QStringLiteral("url")).toString();
            } else if (d.userType() == QMetaType::QVariantList) {
                const QVariantList dl = d.toList();
                if (!dl.isEmpty()) url = dl.first().toMap().value(QStringLiteral("url")).toString();
            }
        }
    } else {
        url = data.toString();
    }
    if (headers) *headers = hdr;
    return url.trimmed();
}

void ScriptSourceManager::resolve(const Song &song, const QString &quality, PlayableCallback cb)
{
    if (!hasMusicUrl()) {
        QStringList dump;
        for (const Item &i : m_items)
            dump << QStringLiteral("%1(%2, %3)").arg(i.name,
                        i.enabled ? QStringLiteral("启用") : QStringLiteral("禁用"),
                        i.actions.join(QLatin1Char('/')));
        qWarning().noquote() << QStringLiteral("[换源] 没有启用的取链音源，跳过脚本层；已导入 %1 个：%2")
                                    .arg(int(m_items.size()))
                                    .arg(dump.isEmpty() ? QStringLiteral("无") : dump.join(QStringLiteral("；")));
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
        // 跨平台候选：手上只有「歌名 + 歌手」，要先 search 拿到该平台的条目再取链。
        // 这是 VIP 歌曲最有效的一条兜底：某个平台全线取不到时，同一首歌在别的平台
        // 常常有免费源，比在同一个平台上反复换脚本有用得多。
        bool viaSearch = false;
    };
    QVector<Candidate> candidates;
    const QString wanted = qualityToLx(quality);
    qInfo().noquote() << QStringLiteral("[换源] 开始：%1 - %2（源=%3 音质=%4 lxKey=%5 vip=%6）")
                             .arg(song.title, song.artist, song.source, quality,
                                  song.extra.value(QStringLiteral("lxKey")).toString())
                             .arg(song.vip);

    // 脚本只声明自己支持的音质。宿主要 high/lossless 时若直接把 320k/flac
    // 丢给只支持 128k 的音源，取链会被拒；这里收敛到声明范围内。
    auto pickQuality = [&wanted](const QJsonObject &o) {
        QStringList qs = jsonToStrings(o.value(QStringLiteral("qualities")));
        if (qs.isEmpty()) qs = jsonToStrings(o.value(QStringLiteral("qualitys")));
        if (qs.isEmpty() || qs.contains(wanted)) return wanted;
        if (qs.contains(QStringLiteral("128k"))) return QStringLiteral("128k");
        return qs.first();
    };
    auto addByKey = [this, &candidates, &pickQuality](const QString &key, bool viaSearch) {
        if (key.isEmpty()) return;
        const QStringList need = viaSearch
            ? QStringList{QStringLiteral("search"), QStringLiteral("musicUrl")}
            : QStringList{QStringLiteral("musicUrl")};
        // 按权重顺序挑选同类音源：同一 key 下先交给最近取链成功的脚本
        for (const QString &id : orderedIds()) {
            const Item item = m_items.value(id);
            if (!item.enabled) continue;
            for (const QJsonValue &v : item.sources) {
                const QJsonObject o = v.toObject();
                if (o.value(QStringLiteral("key")).toString() != key) continue;
                const QStringList actions = jsonToStrings(o.value(QStringLiteral("actions")));
                bool capable = true;
                for (const QString &a : need)
                    if (!actions.contains(a)) capable = false;
                if (!capable) continue;
                bool dup = false;
                for (const Candidate &c : candidates)
                    if (c.engineId == id && c.lxKey == key && c.viaSearch == viaSearch) {
                        dup = true;
                        break;
                    }
                if (!dup) candidates << Candidate{id, key, pickQuality(o), item.name, viaSearch};
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
                candidates << Candidate{storedScript, storedKey, q, it->name, false};
            }
        }
        addByKey(storedKey, false);
    }
    const QString ownKey = !storedKey.isEmpty() ? storedKey : platformToLx(song.source);
    addByKey(platformToLx(song.source), false);

    // ---- 跨平台兜底：同一首歌去别的平台找 ----
    // 「自动跨源换源」开关已下线、固定开启；只追加同平台候选之后的备选，命中即停。
    if (hasSearch() && !song.title.trimmed().isEmpty()) {
        QStringList keys{QStringLiteral("tx"), QStringLiteral("kg"), QStringLiteral("kw"),
                         QStringLiteral("mg"), QStringLiteral("wy")};
        // 平台顺序 = 该平台上所有脚本的平均成绩：先搜最靠谱的平台，
        // 多数情况第一个平台就命中，后面几个的搜索请求根本发不出去
        auto keyScore = [this](const QString &key) {
            int sum = 0, n = 0;
            const QStringList ids = orderedIds();
            for (const QString &id : ids) {
                const Item item = m_items.value(id);
                if (!item.enabled) continue;
                for (const QJsonValue &v : item.sources) {
                    if (v.toObject().value(QStringLiteral("key")).toString() == key) {
                        sum += scoreOf(id, key);
                        ++n;
                        break;
                    }
                }
            }
            return n > 0 ? qreal(sum) / n : 0.0;
        };
        std::stable_sort(keys.begin(), keys.end(),
                         [&keyScore](const QString &a, const QString &b) {
                             return keyScore(a) > keyScore(b);
                         });
        for (const QString &key : keys) {
            if (key == ownKey) continue;
            addByKey(key, true);
        }
    }

    if (candidates.isEmpty()) {
        Playable p;
        p.reason = QStringLiteral("no-capability");
        p.message = song.source == QLatin1String("custom")
            ? QStringLiteral("该歌曲没有对应的在线音源")
            : QStringLiteral("已导入的在线音源未声明该平台（%1）的取链能力").arg(song.source);
        if (cb) cb(p);
        return;
    }

    // 排序：先看「这个脚本在这个平台上」的成绩，再看全局权重。
    // addByKey 已按全局权重插入，stable_sort 保证同分时维持原顺序 —— 等于把
    // 「整体最强」和「在这个平台上最能打」两个维度合并，且跨重启保留。
    std::stable_sort(candidates.begin(), candidates.end(),
                     [this](const Candidate &a, const Candidate &b) {
                         const int sa = scoreOf(a.engineId, a.lxKey);
                         const int sb = scoreOf(b.engineId, b.lxKey);
                         if (sa != sb) return sa > sb;
                         return false;
                     });
    // 兜底链上限：15 个脚本 × 20s 会让用户以为卡死，一场播放最多试这么多次
    // 同一首歌内，播放器已经证明「打不开」的候选先剔掉：这样一次点击就能轮到下一个源，
    // 而不是每轮都从成绩最高的那个重来（用户看到的是反复换源却始终没声）。
    const QString songKey = song.title + QLatin1Char('|') + song.artist;
    if (songKey == m_badPlaybackSong && !m_badPlayback.isEmpty()) {
        QVector<Candidate> kept;
        kept.reserve(candidates.size());
        for (const Candidate &c : candidates) {
            if (m_badPlayback.contains(songKey + QLatin1Char('|') + c.engineId
                                       + QLatin1Char('|') + c.lxKey))
                continue;
            kept << c;
        }
        if (kept.size() != candidates.size()) {
            qInfo().noquote() << QStringLiteral("[换源] 跳过本首已证明打不开的 %1 个候选，剩 %2 个")
                                     .arg(int(candidates.size() - kept.size())).arg(int(kept.size()));
            candidates = kept;
        }
    }
    if (candidates.isEmpty()) {
        Playable p;
        p.reason = QStringLiteral("script-failed");
        p.message = QStringLiteral("这首歌的在线音源都试过了，拿到的地址都播不出来");
        if (cb) cb(p);
        return;
    }
    // 会话级跳过：本会话内已累计失败 ≥2 次的「脚本×平台」候选直接剔除，
    // 避免每首歌都先撞一遍已知失败的源再层层回落（用户看到的就是反复报失败）。
    // 全部候选都被剔除时保留原列表兜底——宁可再试一次，也不让取链无源可用。
    {
        QVector<Candidate> healthy;
        healthy.reserve(candidates.size());
        for (const Candidate &c : candidates) {
            if (m_candidateFails.value(c.engineId + QLatin1Char('|') + c.lxKey, 0) >= 2)
                continue;
            healthy << c;
        }
        if (!healthy.isEmpty() && healthy.size() != candidates.size()) {
            qInfo().noquote() << QStringLiteral("[换源] 会话内失败≥2次，跳过 %1 个候选，剩 %2 个")
                                     .arg(int(candidates.size() - healthy.size()))
                                     .arg(int(healthy.size()));
            candidates = healthy;
        }
    }
    if (candidates.size() > 14)
        candidates.resize(14);
    {
        QStringList plan;
        for (int i = 0; i < candidates.size(); ++i) {
            const Candidate &c = candidates.at(i);
            plan << QStringLiteral("%1.%2[%3%4]").arg(i + 1).arg(c.name, c.lxKey,
                              c.viaSearch ? QStringLiteral("/搜索") : QString())
                        + QStringLiteral("(%1)").arg(scoreOf(c.engineId, c.lxKey));
        }
        qInfo().noquote() << QStringLiteral("[换源] 候选 %1 个：%2")
                                 .arg(int(candidates.size())).arg(plan.join(QStringLiteral(" ")));
    }

    struct ResolveState {
        PlayableCallback cb;
        Song song;
        QVector<Candidate> candidates;
        QStringList failures;   // 逐个音源记录真实失败原因，别再只报「均未取到」
        int index = 0;          // 下一个待启动的候选
        int running = 0;        // 在途竞速请求数
        bool done = false;      // 已有赢家 / 全部失败，迟到结果一律丢弃
        bool launching = false; // 防止同步回调重入 launch
        // 补位泵：竞速里每个请求结束（成功/失败/超时）后由 finishOne 调用，
        // 在还有候选时继续发下一批。必须挂在 state 上——早先用 weak_ptr
        // 指向 resolve() 栈上的 shared_ptr，函数返回后强引用就没了，异步失败
        // 后的补位全部失效，回调永远不来，上层「正在切换音源」卡死（真机复现）。
        std::function<void()> pump;
    };
    auto state = std::make_shared<ResolveState>();
    state->cb = cb;
    state->song = song;
    state->candidates = candidates;

    // 跨平台候选要先搜索再取链：把一条搜索结果转成 Song，按相似度挑最像的
    auto bestMatch = [](const Song &target, const QVariantList &list) {
        Song best;
        const qreal threshold = 0.62;   // 低于这个分宁可不换，避免播成翻唱/伴奏/纯音乐
        qreal bestScore = threshold;
        qreal maxScore = 0.0;
        QString maxLabel;
        qInfo().noquote() << QStringLiteral("[换源] 搜索结果 %1 条；目标：%2 - %3（时长 %4）")
                                 .arg(int(list.size())).arg(target.title, target.artist)
                                 .arg(target.duration);
        for (const QVariant &v : list) {
            const QVariantMap raw = v.toMap();
            Song s;
            s.title = entryTitle(raw);
            if (s.title.isEmpty()) continue;
            s.artist = entryArtist(raw);
            s.album = entryText(raw, {QStringLiteral("albumName"), QStringLiteral("album")});
            s.duration = parseInterval(raw.value(QStringLiteral("interval")));
            if (s.duration <= 0)
                s.duration = parseInterval(raw.value(QStringLiteral("duration")));
            s.sourceId = entrySourceId(raw);
            s.extra = raw;
            const qreal t = similarity(target.title, s.title);
            const qreal a = similarity(target.artist, s.artist);
            qreal d = 0.0;
            if (target.duration > 0 && s.duration > 0) {
                const qint64 diff = qAbs(target.duration - s.duration);
                d = qMax(0.0, 1.0 - qreal(diff) / qreal(qMax<qint64>(target.duration, 10000)));
            }
            const qreal score = t * 0.60 + a * 0.25 + d * 0.15;
            if (score > maxScore) { maxScore = score; maxLabel = s.title + QStringLiteral(" - ") + s.artist; }
            qInfo().noquote() << QStringLiteral("[换源]   %1 - %2  t=%3 a=%4 d=%5 → %6")
                                     .arg(s.title, s.artist)
                                     .arg(t, 0, 'f', 2).arg(a, 0, 'f', 2)
                                     .arg(d, 0, 'f', 2).arg(score, 0, 'f', 2);
            if (t < 0.6) continue;
            if (score > bestScore) {
                bestScore = score;
                best = s;
            }
        }
        if (best.title.isEmpty()) {
            qInfo().noquote() << QStringLiteral("[换源] 无匹配（最高 %1 分：%2）")
                                     .arg(maxScore, 0, 'f', 2).arg(maxLabel);
        } else {
            qInfo().noquote() << QStringLiteral("[换源] 匹配：%1 - %2（%3 分）")
                                     .arg(best.title, best.artist)
                                     .arg(bestScore, 0, 'f', 2);
        }
        return best;
    };

    // 取链成功：记成绩 → 置顶 → 回调，三条路径共用
    auto succeed = [this, state](const Candidate &cand, const QString &url,
                                 const QVariantMap &headers) {
        if (state->done) return;   // 竞速中别的源已经赢了，迟到结果直接丢弃
        state->done = true;
        qInfo().noquote() << QStringLiteral("[换源] OK ") + cand.name + QStringLiteral(" [") + cand.lxKey
                                 + QLatin1Char(']')
                                 + (cand.viaSearch ? QStringLiteral("(跨平台)") : QString())
                                 + QStringLiteral(" url=") + url
                                 + QStringLiteral(" len=") + QString::number(url.size())
                                 + QStringLiteral(" headers=")
                                 + QStringList(headers.keys()).join(QLatin1Char(','));
        // +1 只是「确实取到了地址」的弱信号；能不能播要等播放器确认（见 AppController），
        // 这是坏源没法靠假成功长期霸榜的关键。
        reportResult(cand.engineId, cand.lxKey, true, 1);
        // 置顶也推迟到「确认能播」那一刻（AppController 进度过 2 秒），
        // 否则返回坏地址的源会被一次假成功顶到全局第一，后面每首歌都先撞它一遍。
        Playable p;
        p.ok = true;
        p.url = QUrl(url);
        p.headers = headers;
        // 把提供者带回 AppController：等播放器真的放出声音才补记成功分
        p.engineId = cand.engineId;
        p.platformKey = cand.lxKey;
        p.substituted = true;
        p.substituteSource = cand.viaSearch
            ? QStringLiteral("在线音源 · %1（跨平台）").arg(cand.name)
            : QStringLiteral("在线音源 · ") + cand.name;
        p.message = cand.viaSearch
            ? QStringLiteral("原平台不可播放，已由在线音源「%1」跨平台提供").arg(cand.name)
            : QStringLiteral("已由在线音源「%1」提供").arg(cand.name);
        if (state->cb) state->cb(p);
        // 补位泵持有 state 的强引用，结果出来后必须断开，否则每次换源泄漏一份状态
        QTimer::singleShot(0, this, [state] { state->pump = nullptr; });
    };
    // 一次取链失败：记成绩；只有同一「脚本 × 平台」累计到两次失败才动全局顺序，
    // 免得一次版权/网络抖动把一个在别的平台上很稳的脚本整体沉下去
    auto failed = [this, state](const Candidate &cand, const QString &reason) {
        QString text = reason.trimmed().isEmpty() ? QStringLiteral("取链失败") : reason;
        text = text.left(80);
        qWarning().noquote() << QStringLiteral("[换源] FAIL %1 [%2]%3：%4")
                                    .arg(cand.name, cand.lxKey,
                                         cand.viaSearch ? QStringLiteral("（跨平台）") : QString(), text);
        state->failures << QStringLiteral("%1%2：%3")
                               .arg(cand.name,
                                    cand.viaSearch ? QStringLiteral("(跨平台)") : QString(),
                                    text);
        reportResult(cand.engineId, cand.lxKey, false);
        if (scoreOf(cand.engineId, cand.lxKey) <= -2)
            demoteSource(cand.engineId);
    };

    // 并行竞速：同时发最多 concurrency 个候选，谁先拿到可用地址谁赢；
    // 迟到的成功一律丢弃（succeed 里的 done 守卫），失败的候选立刻补位。
    // sourceRace=false 时 concurrency=1，退回原来的串行逐源尝试。
    const int concurrency = Settings::boolean(QStringLiteral("sourceRace"), true) ? 3 : 1;
    qInfo().noquote() << QStringLiteral("[换源] 竞速启动：并发 %1，候选 %2")
                             .arg(concurrency).arg(int(state->candidates.size()));
    auto finishOne = [state]() {
        --state->running;
        if (state->done) return;
        if (state->pump) state->pump();
    };
    auto finishAll = [this, state]() {
        if (state->done) return;
        state->done = true;
        Playable p;
        p.reason = QStringLiteral("script-failed");
        if (state->failures.isEmpty())
            p.message = QStringLiteral("所有在线音源均未能取到音频地址");
        else
            p.message = QStringLiteral("在线音源取链失败：%1")
                            .arg(state->failures.join(QStringLiteral("；")));
        if (state->cb) state->cb(p);
        QTimer::singleShot(0, this, [state] { state->pump = nullptr; });
    };
    state->pump = [this, state, finishOne, finishAll, succeed, failed, bestMatch,
                   concurrency]() {
        if (state->launching) return;
        state->launching = true;
        while (!state->done && state->running < concurrency
               && state->index < state->candidates.size()) {
            const Candidate cand = state->candidates.at(state->index++);
            ++state->running;
            qInfo().noquote() << QStringLiteral("[换源] 竞速尝试 %1/%2：%3 [%4]%5 分数=%6")
                                     .arg(state->index)
                                     .arg(int(state->candidates.size()))
                                     .arg(cand.name, cand.lxKey,
                                          cand.viaSearch ? QStringLiteral("（跨平台搜索）") : QString())
                                     .arg(scoreOf(cand.engineId, cand.lxKey));

            ensureEngine(cand.engineId, [this, state, cand, finishOne, succeed, failed,
                                         bestMatch](bool ok) {
                if (state->done) { finishOne(); return; }
                if (!ok) {
                    failed(cand, QStringLiteral("脚本初始化失败"));
                    finishOne();
                    return;
                }
                ScriptEngine *engine = m_engines.value(cand.engineId).data();
                if (!engine || !engine->ready()) {
                    failed(cand, QStringLiteral("脚本未就绪"));
                    finishOne();
                    return;
                }
                QPointer<ScriptEngine> guard(engine);

                // 取链：target 是「带该平台 id 的 Song」（原歌，或跨平台匹配到的同曲）
                auto callUrl = [this, state, cand, finishOne, succeed, failed, guard](const Song &target) {
                    ScriptEngine *e = guard.data();
                    if (!e) {
                        failed(cand, QStringLiteral("脚本已卸载"));
                        finishOne();
                        return;
                    }
                    QVariantMap info;
                    info.insert(QStringLiteral("type"), cand.quality);
                    info.insert(QStringLiteral("musicInfo"), musicInfoFor(target, cand.lxKey));
                    e->callAction(cand.lxKey, QStringLiteral("musicUrl"), info,
                        [this, state, cand, finishOne, succeed, failed](const ScriptActionResult &r) {
                            QString url;
                            QVariantMap hdr;
                            if (r.ok) {
                                qInfo().noquote() << QStringLiteral("[换源] 原始返回 类型=")
                                                         + QString::fromLatin1(r.data.typeName())
                                                     + QStringLiteral(" 文本=") + r.data.toString().left(90);
                                url = playUrlFrom(r.data, &hdr);
                            }
                            if (state->done) { finishOne(); return; }
                            if (url.isEmpty()) {
                                failed(cand, r.ok ? QStringLiteral("未返回音频地址") : r.error);
                                finishOne();
                                return;
                            }
                            // 地址必须能直接交给播放器：脚本偶发会把 undefined/半截字符串当成
                            // url 回调回来（实测有源返回单个字符），以前直接 succeed，结果
                            // 「取链成功 → 播放器 4ms 报错 → 再换源」无限循环。这里先卡一道。
                            const QString scheme = QUrl(url).scheme().toLower();
                            if (scheme != QLatin1String("http") && scheme != QLatin1String("https")
                                && scheme != QLatin1String("file")) {
                                failed(cand, QStringLiteral("返回的地址无效：%1").arg(url.left(40)));
                                finishOne();
                                return;
                            }
                            succeed(cand, url, hdr);
                            finishOne();
                        }, 15000);
                };

                if (!cand.viaSearch) {
                    callUrl(state->song);
                    return;
                }
                // 跨平台：先按「歌名 + 歌手」搜这个平台，再挑最像的一条取链
                QVariantMap sinfo;
                const QString keyword = (state->song.title + QLatin1Char(' ') + state->song.artist).trimmed();
                sinfo.insert(QStringLiteral("keyword"), keyword);
                sinfo.insert(QStringLiteral("page"), 1);
                sinfo.insert(QStringLiteral("limit"), 8);
                sinfo.insert(QStringLiteral("type"), 1);
                qInfo().noquote() << QStringLiteral("[换源] 跨平台搜索 %1 [%2] 关键词「%3」")
                                         .arg(cand.name, cand.lxKey, keyword);
                engine->callAction(cand.lxKey, QStringLiteral("search"), sinfo,
                    [state, cand, finishOne, failed, bestMatch, callUrl](const ScriptActionResult &sr) {
                        if (state->done) { finishOne(); return; }
                        if (!sr.ok)
                            qWarning().noquote() << QStringLiteral("[换源] 跨平台搜索失败 %1 [%2]：%3")
                                                        .arg(cand.name, cand.lxKey, sr.error);
                        const Song matched = bestMatch(state->song, actionList(sr.data));
                        if (matched.title.isEmpty()) {
                            failed(cand, QStringLiteral("其它平台没有匹配的同曲"));
                            finishOne();
                            return;
                        }
                        callUrl(matched);
                    }, 15000);
            });
        }
        state->launching = false;
        // 同步回调可能在 while 内部就完成了全部候选，这里兜底收尾
        if (!state->done && state->running == 0 && state->index >= state->candidates.size())
            finishAll();
    };
    state->pump();
}

void ScriptSourceManager::lyric(const Song &song, LyricCallback cb)
{
    // 内置平台歌曲的歌词接口常常返回空（VIP / 下架 / 无版权），此时也应该
    // 让所有声明了 lyric 能力的脚本依次兜底，而不是只认脚本搜索出来的歌。
    if (!hasEnabled()) { if (cb) cb(QString(), QString()); return; }

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

    if (candidates.isEmpty()) { if (cb) cb(QString(), QString()); return; }

    struct State {
        LyricCallback cb;
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
            if (state->cb) state->cb(QString(), QString());
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
                    QString trans;
                    if (r.ok) {
                        if (r.data.canConvert<QVariantMap>()) {
                            const QVariantMap m = r.data.toMap();
                            text = m.value(QStringLiteral("lyric")).toString();
                            if (text.isEmpty()) text = m.value(QStringLiteral("lrc")).toString();
                            // 洛雪脚本协议：tlrc / translation 是可选的翻译轨
                            trans = m.value(QStringLiteral("tlrc")).toString();
                            if (trans.isEmpty()) trans = m.value(QStringLiteral("translation")).toString();
                        } else {
                            text = r.data.toString();
                        }
                    }
                    if (!text.trimmed().isEmpty()) {
                        if (state->cb) state->cb(text, trans.trimmed());
                        return;
                    }
                    (*step)();
                }, 15000);
        });
    };
    (*next)();
}

// 脚本评论：与 lyric() 同构 —— 所有声明 comment 能力的「脚本 × 平台」依次兜底。
// 脚本协议：info = {musicInfo, page}，返回 {comments:[...], total}；字段名宽松匹配。
void ScriptSourceManager::comments(const Song &song, int page, CommentCallback cb)
{
    if (!hasEnabled()) { if (cb) cb(QVector<Comment>(), -1); return; }

    struct Candidate { QString engineId; QString key; };
    QVector<Candidate> candidates;
    auto add = [this, &candidates](const QString &key) {
        if (key.isEmpty()) return;
        for (auto it = m_items.constBegin(); it != m_items.constEnd(); ++it) {
            if (!it->enabled) continue;
            for (const QJsonValue &v : it->sources) {
                const QJsonObject o = v.toObject();
                if (o.value(QStringLiteral("key")).toString() != key) continue;
                if (!jsonToStrings(o.value(QStringLiteral("actions"))).contains(QStringLiteral("comment")))
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
    if (!storedScript.isEmpty() && !storedKey.isEmpty())
        add(storedKey);
    add(platformToLx(song.source));

    if (candidates.isEmpty()) { if (cb) cb(QVector<Comment>(), -1); return; }

    struct State {
        CommentCallback cb;
        Song song;
        int page;
        QVector<Candidate> candidates;
        int index = 0;
    };
    auto state = std::make_shared<State>();
    state->cb = cb;
    state->song = song;
    state->page = page;
    state->candidates = candidates;
    auto next = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weakNext = next;
    *next = [this, state, weakNext]() {
        auto step = weakNext.lock();
        if (!step) return;
        if (state->index >= state->candidates.size()) {
            if (state->cb) state->cb(QVector<Comment>(), -1);
            return;
        }
        const Candidate cand = state->candidates.at(state->index++);
        ensureEngine(cand.engineId, [this, state, cand, step](bool ok) {
            if (!ok) { (*step)(); return; }
            ScriptEngine *engine = m_engines.value(cand.engineId).data();
            if (!engine || !engine->ready() || !engine->supports(cand.key, QStringLiteral("comment"))) {
                (*step)();
                return;
            }
            QVariantMap info;
            info.insert(QStringLiteral("musicInfo"), musicInfoFor(state->song, cand.key));
            info.insert(QStringLiteral("page"), state->page);
            engine->callAction(cand.key, QStringLiteral("comment"), info,
                [state, step](const ScriptActionResult &r) {
                    QVector<Comment> out;
                    int total = -1;
                    if (r.ok && r.data.canConvert<QVariantMap>()) {
                        const QVariantMap m = r.data.toMap();
                        QVariant v = m.value(QStringLiteral("comments"));
                        if (!v.canConvert<QVariantList>()) v = m.value(QStringLiteral("list"));
                        total = m.value(QStringLiteral("total")).toInt();
                        if (total == 0) total = m.value(QStringLiteral("count")).toInt();
                        const QVariantList arr = v.toList();
                        for (const QVariant &rv : arr) {
                            const QVariantMap c = rv.toMap();
                            Comment cm;
                            cm.source = QStringLiteral("script");
                            cm.id = firstVariantString(c, {QStringLiteral("id"), QStringLiteral("commentId")});
                            cm.text = firstVariantString(c, {QStringLiteral("text"), QStringLiteral("content"),
                                                             QStringLiteral("msg")});
                            cm.userName = firstVariantString(c, {QStringLiteral("userName"), QStringLiteral("name"),
                                                                 QStringLiteral("nick"), QStringLiteral("user_name")});
                            cm.avatar = firstVariantString(c, {QStringLiteral("avatar"), QStringLiteral("avatarUrl"),
                                                               QStringLiteral("user_pic")});
                            const QVariant t = c.value(QStringLiteral("time"));
                            qint64 ms = t.toLongLong();
                            if (ms > 0 && ms < 100000000000LL) ms *= 1000;   // 秒 → 毫秒
                            cm.time = ms;
                            cm.timeText = c.value(QStringLiteral("timeText")).toString();
                            if (cm.timeText.isEmpty())
                                cm.timeText = firstVariantString(c, {QStringLiteral("timeStr"),
                                                                     QStringLiteral("addtime")});
                            if (cm.timeText.isEmpty()) cm.timeText = commentTimeText(cm.time);
                            cm.likedCount = c.value(QStringLiteral("likedCount")).toInt();
                            if (cm.likedCount == 0)
                                cm.likedCount = c.value(QStringLiteral("likeNum")).toInt();
                            cm.replyCount = c.value(QStringLiteral("replyCount")).toInt();
                            if (cm.replyCount == 0)
                                cm.replyCount = c.value(QStringLiteral("reply_num")).toInt();
                            cm.replyText = firstVariantString(c, {QStringLiteral("replyText"),
                                                                   QStringLiteral("replyContent")});
                            if (!cm.text.isEmpty()) out << cm;
                        }
                    }
                    if (!out.isEmpty()) {
                        if (state->cb) state->cb(out, total);
                        return;
                    }
                    (*step)();
                }, 15000);
        });
    };
    (*next)();
}

} // namespace Furui
