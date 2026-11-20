#include "core/QtCompat.h"
#include "SourceManager.h"

#include <QDebug>
#include <QSet>
#include <QPair>
#include <QTimer>
#include <algorithm>
#include <functional>
#include <memory>

#include "CustomSource.h"
#include "JsonMapper.h"
#include "Library.h"
#include "MusicSource.h"
#include "ScriptSource.h"
#include "Settings.h"
#include "SourceConfig.h"

namespace Furui {

namespace {

// 打开一个歌单时要多少首。
// - 只有声明了「trackIds + 批量详情」补全能力的平台（网易云）才一次要全量：它一个响应就
//   给出完整 id 列表，代价只是多几次 song/detail 批量请求；
// - 其余平台沿用 150 首：把 limit 提到上千后，不少网关会直接回 code:-1 / 0 首，
//   反而比现在更差。
int detailSongLimit(const Playlist &p, MusicSource *s)
{
    const int base = qMax(30, Settings::integer(QStringLiteral("pageSize"), 30) * 5);
    if (!s || !s->supportsFullPlaylist()) return base;
    const int cap = qBound(base, Settings::integer(QStringLiteral("playlistMaxSongs"), 1000), 5000);
    return p.count > 0 ? qBound(base, p.count, cap) : cap;
}

} // namespace

SourceManager *SourceManager::instance()
{
    static SourceManager *m = new SourceManager();
    return m;
}

SourceManager::SourceManager(QObject *parent)
    : QObject(parent)
{
    const QVector<SourceDesc> descs = SourceConfig::instance()->sources();
    for (const SourceDesc &d : descs)
        m_sources.append(new MusicSource(d, this));
    // 音源优先级：优先用持久化的覆盖默认顺序（避免每次启动都从默认序重来）。
    const QString saved = Settings::string(QStringLiteral("sourceOrder"));
    if (!saved.isEmpty())
        m_sourceOrder = saved.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (MusicSource *s : m_sources)
        if (!m_sourceOrder.contains(s->id())) m_sourceOrder.append(s->id());
    refreshSessions();
}

MusicSource *SourceManager::source(const QString &id) const
{
    for (MusicSource *s : m_sources)
        if (s->id() == id) return s;
    return nullptr;
}

void SourceManager::refreshSessions()
{
    const QVector<Account> accounts = Library::instance()->accounts();
    for (MusicSource *s : m_sources) {
        Account empty;
        empty.source = s->id();
        for (const Account &a : accounts)
            if (a.source == s->id() && a.logged) { empty = a; break; }
        s->setSession(empty);
    }
}

void SourceManager::search(const QString &keyword, const QString &sourceId, int limit, SongsCallback cb)
{
    QVector<MusicSource *> targets;
    if (sourceId.isEmpty()) targets = m_sources;
    else if (MusicSource *s = source(sourceId)) targets << s;

    const bool includeCustom = sourceId.isEmpty() || sourceId == QLatin1String("custom");
    int pending = targets.size() + (includeCustom ? 1 : 0);
    if (pending == 0) { if (cb) cb(QVector<Song>()); return; }

    auto remaining = std::make_shared<int>(pending);
    auto collected = std::make_shared<QVector<Song>>();
    auto finished = std::make_shared<bool>(false);
    auto lastCount = std::make_shared<int>(-1);
    const QString kw = keyword.trimmed();

    // 相关度排序：歌名完全一致 > 歌名以关键词开头 > 歌名包含关键词 > 其余；
    // 用 stable_sort 保证同一档内仍按平台返回顺序排列。
    auto sorted = [kw](const QVector<Song> &in) {
        // 分数先算好再排序：以前比较器里现算，每次比较都要 trimmed() 复制一遍标题再做
        // 三趟大小写无关匹配，n·log n 次比较全落在 GUI 线程上，几百条结果就能卡一下。
        auto scoreOf = [&kw](const Song &s) {
            const QString title = s.title.trimmed();
            if (kw.isEmpty()) return 3;
            if (title.compare(kw, Qt::CaseInsensitive) == 0) return 0;
            if (title.startsWith(kw, Qt::CaseInsensitive)) return 1;
            if (title.contains(kw, Qt::CaseInsensitive)) return 2;
            return 3;
        };
        QVector<QPair<int, int>> ranked;
        ranked.reserve(in.size());
        for (int i = 0; i < in.size(); ++i)
            ranked.append(qMakePair(scoreOf(in.at(i)), i));
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const QPair<int, int> &a, const QPair<int, int> &b) {
                             return a.first < b.first;
                         });
        QVector<Song> out;
        out.reserve(ranked.size());
        for (const QPair<int, int> &r : ranked) out.append(in.at(r.second));
        return out;
    };

    auto deliver = [collected, cb, sorted, finished, lastCount](bool final) {
        if (*finished) return;
        // 软刷新和最终结果内容相同就不要重复推给界面（避免整表 reset 两次）
        if (collected->size() != *lastCount) {
            *lastCount = collected->size();
            if (cb) cb(sorted(*collected));
        }
        if (final) *finished = true;
    };

    // 软超时：某个平台挂起时不能让整页空等到 12s 超时。700ms 先把已到的结果给界面，
    // 全部返回后再补一次最终（完整排序）结果。
    QTimer::singleShot(700, this, [collected, deliver]() {
        if (!collected->isEmpty()) deliver(false);
    });

    // 硬上限：个别慢源（脚本 30s+ 超时、失效接口等 TCP 重试）会拖住「最终结果」
    // 的交付，实测换源场景干等 31 秒才跳歌。到点不管还剩几个平台没回来，
    // 都用已有结果收尾；迟到源的回调被 finished 闸门丢弃。
    QTimer::singleShot(8000, this, [collected, deliver]() { deliver(true); });

    auto done = [remaining, deliver]() {
        if (--(*remaining) > 0) return;
        deliver(true);
    };

    for (MusicSource *s : targets) {
        s->search(keyword, limit, 0, [collected, done](const QVector<Song> &songs) {
            collected->append(songs);
            done();
        });
    }
    if (includeCustom) {
        CustomSourceManager::instance()->search(keyword, limit, [collected, done](const QVector<Song> &songs) {
            collected->append(songs);
            done();
        });
    }
}

void SourceManager::searchPlaylists(const QString &keyword, const QString &sourceId,
                                    int limit, PlaylistsCallback cb)
{
    QVector<MusicSource *> targets;
    if (sourceId.isEmpty()) targets = m_sources;
    else if (MusicSource *s = source(sourceId)) targets << s;
    if (targets.isEmpty()) { if (cb) cb(QVector<Playlist>()); return; }

    auto remaining = std::make_shared<int>(targets.size());
    auto collected = std::make_shared<QVector<Playlist>>();
    for (MusicSource *s : targets) {
        s->searchPlaylist(keyword, limit, 0,
            [collected, remaining, cb](const QVector<Playlist> &list) {
                collected->append(list);
                if (--(*remaining) == 0 && cb) cb(*collected);
            });
    }
}

void SourceManager::rank(const QString &sourceId, int limit, PlaylistsCallback cb)
{
    QVector<MusicSource *> targets;
    if (sourceId.isEmpty()) targets = m_sources;
    else if (MusicSource *s = source(sourceId)) targets << s;
    if (targets.isEmpty()) { if (cb) cb(QVector<Playlist>()); return; }

    // 每个平台占一个槽位，全部回来后按「平台轮转」交错展开。
    // 直接按完成顺序拼接会让榜单数量多的平台霸屏（实测酷狗 57 个、网易云 65 个，
    // QQ 的 25 个和酷我的 6 个全被压到列表很后面，看起来像「只有酷狗有榜单」）。
    // 注意：变量名不能叫 slots —— 那是 Qt 的关键字宏（qobjectdefs.h），会被展开成空
    auto perSource = std::make_shared<QVector<QVector<Playlist>>>(targets.size());
    auto remaining = std::make_shared<int>(targets.size());
    for (int i = 0; i < targets.size(); ++i) {
        MusicSource *s = targets.at(i);
        s->rank(limit, 0, [perSource, remaining, cb, i](const QVector<Playlist> &list) {
            (*perSource)[i] = list;
            if (--(*remaining) == 0 && cb) {
                int rows = 0;
                for (const QVector<Playlist> &v : *perSource) rows = qMax(rows, int(v.size()));
                QVector<Playlist> out;
                out.reserve(rows * int(perSource->size()));
                for (int row = 0; row < rows; ++row)
                    for (const QVector<Playlist> &v : *perSource)
                        if (row < v.size()) out << v.at(row);
                cb(out);
            }
        });
    }
}

QVector<Playlist> SourceManager::defaultBoards(const QString &sourceId) const
{
    MusicSource *s = source(sourceId);
    return s ? s->defaultBoards() : QVector<Playlist>();
}

void SourceManager::playlistDetail(const Playlist &playlist, SongsCallback cb)
{
    MusicSource *s = source(playlist.source);
    const QString id = playlist.sourceId;
    SongsCallback finish = [playlist, cb](const QVector<Song> &songs) {
        if (!songs.isEmpty()) {
            Library::instance()->setPlaylistSongs(playlist.uid, songs);
            if (cb) cb(songs);
        } else {
            // 离线/接口不可用时回退到本地缓存
            const QVector<Song> local = Library::instance()->playlistSongs(playlist.uid);
            if (cb) cb(local);
        }
    };
    if (!s || id.isEmpty()) {
        qWarning().noquote() << "[source] playlistDetail bailed, source=" << playlist.source
                             << "id=" << id << "type=" << playlist.type;
        finish(QVector<Song>());
        return;
    }
    if (playlist.type == QLatin1String("user"))
        s->userPlaylistDetail(id, finish);
    else if (playlist.type == QLatin1String("rank"))
        s->rankDetail(id, detailSongLimit(playlist, s), 0, finish);
    else
        s->playlistDetail(id, detailSongLimit(playlist, s), 0, finish);
}

// 一首歌的稳定标识：「这个平台的这首歌唱不了」按它记忆
static QString songKeyOf(const Song &song)
{
    if (!song.uid.isEmpty()) return song.uid;
    if (song.sourceId.isEmpty()) return QString();
    return song.source + QLatin1Char(':') + song.sourceId;
}

void SourceManager::noteNeedsSubstitute(const Song &song)
{
    const QString k = songKeyOf(song);
    if (k.isEmpty() || m_knownUnplayable.contains(k)) return;
    // 一次进程内通常只有几十首；给个上限，避免长时间挂着无限增长
    if (m_knownUnplayable.size() > 500) m_knownUnplayable.clear();
    m_knownUnplayable.insert(k);
}

bool SourceManager::isKnownUnplayable(const Song &song) const
{
    const QString k = songKeyOf(song);
    return !k.isEmpty() && m_knownUnplayable.contains(k);
}

void SourceManager::resolve(const Song &song, const QString &quality, PlayableCallback cb,
                            bool quiet)
{
    if (song.source == QLatin1String("custom")) {
        CustomSourceManager::instance()->resolve(song, quality, cb);
        return;
    }
    MusicSource *s = source(song.source);
    if (!s) { CustomSourceManager::instance()->resolve(song, quality, cb); return; }

    // 取链顺序（设置项 sourceFirst）：
    //   platform —— 先原平台直链，失败才换源（老行为）
    //   vip      —— VIP / 平台已判定不可播的歌先走自定义音源（默认）
    //   custom   —— 所有歌曲都先走自定义音源
    // 走原平台直链的意义是「正版 + 最高音质」；但 VIP / 独家歌它必然回一句
    // 「需要会员或暂不可播放」，白等一个来回还把它降权一次，所以默认让这类歌
    // 直接从自定义音源起步。
    const bool substituteOn = Settings::boolean(QStringLiteral("vipSubstitute"), true);
    const QString order = Settings::string(QStringLiteral("sourceFirst"), QStringLiteral("vip"));
    const bool customFirst = substituteOn
            && (order == QLatin1String("custom")
                || (order == QLatin1String("vip")
                    && (song.vip || isKnownUnplayable(song))));

    if (customFirst) {
        qInfo().noquote() << QStringLiteral("[取链] 「%1 - %2」按「%3」策略先走自定义音源")
                                 .arg(song.title, song.artist, order);
    }

    // 两条路都走完仍失败时的统一收尾：内置跨源 → 否则把两边的失败原因并起来报给用户
    auto bothFailed = [this, song, quality, cb, quiet](const Playable &platformRes, const Playable &sub) {
        Playable r = platformRes;
        // 把「原平台失败」与「替代音源为什么也没成」分开说清楚，
        // 否则用户只会看到一句「需要会员」，误以为导入的音源没生效。
        r.message = platformRes.message;
        if (!sub.message.isEmpty()) {
            if (!r.message.isEmpty()) r.message += QStringLiteral("；");
            r.message += sub.message;
        }
        if (!r.message.isEmpty()) r.message += QStringLiteral("。");
        r.message += QStringLiteral("可在「设置 → 在线音源」检查状态或换用其它音源");
        // 「全部自定义」模式下不做内置跨源：用户明确要求所有取链只走自定义音源
        const bool customOnly = Settings::boolean(QStringLiteral("vipSubstitute"), true)
                && Settings::string(QStringLiteral("sourceFirst"), QStringLiteral("vip"))
                   == QLatin1String("custom");
        if (!customOnly && Settings::boolean(QStringLiteral("autoToggleSource"), true))
            tryCrossSource(song, quality, r, cb, quiet);
        else if (cb)
            cb(r);
    };

    // 走平台直链；失败的下游是「自定义音源 → 内置跨源」
    auto byPlatformFirst = [this, song, quality, cb, bothFailed, quiet](const Playable &original) {
        if (original.ok) { if (cb) cb(original); return; }
        if (!quiet && (original.needSubstitute || original.reason == QLatin1String("vip")))
            noteNeedsSubstitute(song);
        // 底层排查用：原平台为什么取不到链（会员/版权/接口变更），后面的换源都是它的下游
        qWarning().noquote() << QStringLiteral("[取链] 原平台 %1 失败：%2")
                                    .arg(song.source, original.message.isEmpty()
                                             ? original.reason : original.message);
        if (!quiet) demoteSource(song.source);   // 原平台取链失败 → 动态降权（后台预取不改排名）
        if (!Settings::boolean(QStringLiteral("vipSubstitute"), true)) {
            if (cb) cb(original);
            return;
        }
        CustomSourceManager::instance()->resolve(song, quality,
            [cb, bothFailed, original](const Playable &sub) {
                if (sub.ok) {
                    Playable r = sub;
                    r.substituted = true;
                    r.needSubstitute = false;
                    r.message = QStringLiteral("已由自定义音源「%1」替代播放").arg(r.substituteSource);
                    if (cb) cb(r);
                    return;
                }
                bothFailed(original, sub);
            });
    };

    // 走自定义音源；失败的下游是「平台直链 → 内置跨源」
    auto byCustomFirst = [this, song, quality, s, cb, bothFailed, quiet](const Playable &sub) {
        if (sub.ok) {
            Playable r = sub;
            r.substituted = true;
            r.needSubstitute = false;
            if (r.message.isEmpty() && !r.substituteSource.isEmpty())
                r.message = song.vip
                        ? QStringLiteral("VIP 歌曲，已由自定义音源「%1」播放").arg(r.substituteSource)
                        : QStringLiteral("已由自定义音源「%1」播放").arg(r.substituteSource);
            if (cb) cb(r);
            return;
        }
        qWarning().noquote() << QStringLiteral("[取链] 自定义音源未取到「%1」，回落原平台 %2")
                                    .arg(song.title, song.source);
        s->resolveUrl(song, quality, [this, song, sub, cb, bothFailed, quiet](const Playable &original) {
            if (original.ok) { if (cb) cb(original); return; }
            if (!quiet && (original.needSubstitute || original.reason == QLatin1String("vip")))
                noteNeedsSubstitute(song);
            if (!quiet) demoteSource(song.source);
            bothFailed(original, sub);
        });
    };

    if (customFirst)
        CustomSourceManager::instance()->resolve(song, quality, byCustomFirst);
    else
        s->resolveUrl(song, quality, byPlatformFirst);
}

// 对标洛雪 musicSdk/index.js 的 findMusic：跨源按歌名/歌手/专辑/时长模糊匹配后换源
qreal SourceManager::candidateScore(const Song &target, const Song &candidate)
{
    const qreal t = similarity(target.title, candidate.title);
    if (t < 0.6) return -1.0;
    const qreal a = similarity(target.artist, candidate.artist);
    const qreal b = (!target.album.isEmpty() && !candidate.album.isEmpty())
                        ? similarity(target.album, candidate.album) : 0.0;
    qreal d = 0.0;
    if (target.duration > 0 && candidate.duration > 0) {
        const qint64 diff = qAbs(target.duration - candidate.duration);
        d = qMax(0.0, 1.0 - qreal(diff) / qreal(qMax<qint64>(target.duration, 10000)));
    }
    qreal score = t * 0.55 + a * 0.25 + b * 0.10 + d * 0.10;
    if (candidate.source == target.source && candidate.sourceId == target.sourceId) score -= 1.0;
    return score;
}

void SourceManager::tryCrossSource(const Song &song, const QString &quality,
                                   const Playable &original, PlayableCallback cb, bool quiet)
{
    const QString keyword = (song.title + QLatin1Char(' ') + song.artist).trimmed();
    // search() 会多次回调（700ms 软刷新 / 8s 硬超时收尾 / 全部返回的最终结果）：
    // 一旦已经拿到匹配并开始取链，后续回调一律丢弃，否则会对同一首歌重复取链甚至重复 playUrl。
    const auto started = std::make_shared<bool>(false);
    search(keyword, QString(), 25, [this, song, quality, original, cb, started, quiet](const QVector<Song> &list) {
        if (*started) return;
        qreal best = 0.55;
        Song matched;
        for (const Song &c : list) {
            const qreal score = candidateScore(song, c);
            if (score > best) { best = score; matched = c; }
        }
        qInfo().noquote() << QStringLiteral("[取链] 内置跨源搜索「%1 %2」→ %3 条，最佳：%4 - %5（%6）")
                                 .arg(song.title, song.artist).arg(int(list.size()))
                                 .arg(matched.title, matched.artist).arg(best, 0, 'f', 2);
        MusicSource *target = matched.title.isEmpty() ? nullptr : source(matched.source);
        if (!target) {
            // 本轮没匹配上：软刷新批次里可能只是结果不全，等下一个批次；最终批次自然收尾
            return;
        }
        *started = true;
        target->resolveUrl(matched, quality, [this, matched, original, cb, quiet](const Playable &p) {
            if (p.ok) {
                Playable r = p;
                r.substituted = true;
                r.needSubstitute = false;
                r.source = matched.source;
                r.substituteSource = SourceConfig::instance()->nameOf(matched.source) + QStringLiteral("（自动换源）");
                r.message = QStringLiteral("原曲不可播放，已自动换源到 %1").arg(r.substituteSource);
                if (!quiet) promoteSource(matched.source);   // 解析阶段换源成功也置顶，保持"成功源第一位"
                if (cb) cb(r);
            } else {
                if (!quiet) demoteSource(matched.source);    // 换源候选也取链失败 → 降权
                if (cb) cb(original);
            }
        });
    });
    // 兜底收尾：search 的多次交付里始终没有匹配（小众歌）或一个批次都没触发
    // （聚合被硬超时提前掐断且 collected 为空）时，按「跨源失败」回调，避免取链流程挂起。
    QTimer::singleShot(8500, this, [song, original, cb, started]() {
        if (!*started && cb) {
            qWarning().noquote() << QStringLiteral("[取链] 内置跨源搜索「%1 %2」无匹配，放弃换源")
                                        .arg(song.title, song.artist);
            cb(original);
        }
    });
}

void SourceManager::promoteSource(const QString &id)
{
    if (id.isEmpty() || id == QLatin1String("custom"))
        return;     // 自定义音源永远优先，无需排序
    m_sourceOrder.removeAll(id);
    m_sourceOrder.prepend(id);
    Settings::set(QStringLiteral("sourceOrder"), m_sourceOrder.join(QLatin1Char(',')));
}

void SourceManager::demoteSource(const QString &id)
{
    if (id.isEmpty() || id == QLatin1String("custom"))
        return;     // 自定义音源永远优先，无需排序
    const int index = m_sourceOrder.indexOf(id);
    if (index < 0) {
        m_sourceOrder.append(id);
        Settings::set(QStringLiteral("sourceOrder"), m_sourceOrder.join(QLatin1Char(',')));
        return;
    }
    // 一次失败（版权/临时 5xx）只后移一位，避免直接沉底导致好源被长期冷落
    if (index + 1 >= m_sourceOrder.size()) return;
    m_sourceOrder.move(index, index + 1);
    Settings::set(QStringLiteral("sourceOrder"), m_sourceOrder.join(QLatin1Char(',')));
}

// 曲中断流 = 这个源给的直链播到一半死了，比单纯取链失败严重得多：
// 直接沉底，让后续换源优先选没出过这种事的源。播成功了 promoteSource 还能拉回来。
void SourceManager::penalizeSource(const QString &id)
{
    if (id.isEmpty() || id == QLatin1String("custom"))
        return;
    if (!m_sourceOrder.contains(id)) m_sourceOrder.append(id);
    const int index = m_sourceOrder.indexOf(id);
    if (index >= m_sourceOrder.size() - 1) return;   // 已在最后，不再动
    m_sourceOrder.move(index, m_sourceOrder.size() - 1);
    Settings::set(QStringLiteral("sourceOrder"), m_sourceOrder.join(QLatin1Char(',')));
    qWarning().noquote() << QStringLiteral("[取链] 音源 %1 曲中断流，降级到候选末位").arg(id);
}

void SourceManager::resolveAlternate(const Song &song, const QStringList &exclude,
                                     const QString &quality, PlayableCallback cb)
{
    // 候选顺序：自定义音源最优先（VIP 替代），随后按 m_sourceOrder 优先级遍历内置平台。
    // exclude 里的（已失败的原平台 + 本次已尝试过的候选）不再试，避免重复与死循环。
    QStringList candidates;
    if (!exclude.contains(QStringLiteral("custom")))
        candidates << QStringLiteral("custom");

    // 「取链顺序 = 全部自定义」（sourceFirst=custom）时，用户明确要求所有取链只走
    // 自定义/在线音源。此前换源只读 sourceOrder、无视该设置，导致断流后仍跨到咪咕等
    // 内置平台——与设置字面含义冲突，也是「设了全部自定义却还跨源」的根因。
    // 这里让换源在该模式下严格只试自定义音源；自定义全失败则回调失败，由上层提示用户，
    // 不再偷偷回落到内置平台。vip / platform 模式保持原有跨源行为不变。
    const QString firstOrder = Settings::string(QStringLiteral("sourceFirst"), QStringLiteral("vip"));
    const bool customOnly = Settings::boolean(QStringLiteral("vipSubstitute"), true)
                            && firstOrder == QLatin1String("custom");
    if (customOnly) {
        qInfo().noquote() << QStringLiteral("[换源] 取链顺序=全部自定义，仅尝试自定义音源，不跨内置平台");
        if (candidates.isEmpty()) { if (cb) cb(Playable()); return; }
    } else {
        for (const QString &id : m_sourceOrder)
            if (!exclude.contains(id) && source(id)) candidates << id;
        for (MusicSource *s : m_sources)
            if (!candidates.contains(s->id()) && !exclude.contains(s->id()))
                candidates << s->id();
    }
    if (candidates.isEmpty()) { if (cb) cb(Playable()); return; }

    auto attempt = std::make_shared<std::function<void(int)>>();
    *attempt = [this, song, quality, candidates, cb, attempt](int idx) {
        if (idx >= candidates.size()) { if (cb) cb(Playable()); return; }
        const QString id = candidates.at(idx);
        const int next = idx + 1;
        if (id == QLatin1String("custom")) {
            CustomSourceManager::instance()->resolve(song, quality,
                [this, song, quality, cb, attempt, next](const Playable &sub) {
                    if (sub.ok) {
                        Playable r = sub;
                        r.source = QStringLiteral("custom");
                        // 与内置平台换源分支保持一致：补 substituted 标识，否则
                        // AppController 据此计算的「VIP 替代 / 在线音源」徽标不显示
                        r.substituted = true;
                        r.needSubstitute = false;
                        if (r.substituteSource.isEmpty())
                            r.substituteSource = QStringLiteral("在线音源");
                        if (cb) cb(r);
                        return;
                    }
                    (*attempt)(next);
                });
            return;
        }
        MusicSource *target = source(id);
        if (!target) { (*attempt)(next); return; }
        const QString keyword = (song.title + QLatin1Char(' ') + song.artist).trimmed();
        target->search(keyword, 10, 0, [this, song, quality, id, target, cb, attempt, next](const QVector<Song> &list) {
            Song matched; qreal best = 0.55;
            for (const Song &c : list) {
                const qreal sc = candidateScore(song, c);
                if (sc > best) { best = sc; matched = c; }
            }
            if (matched.title.isEmpty()) { (*attempt)(next); return; }
            target->resolveUrl(matched, quality, [this, id, cb, attempt, next](const Playable &p) {
                if (p.ok) {
                    Playable r = p;
                    r.substituted = true;
                    r.needSubstitute = false;
                    r.source = id;
                    r.substituteSource = SourceConfig::instance()->nameOf(id) + QStringLiteral("（自动换源）");
                    r.message = QStringLiteral("原曲播放失败，已自动换源到 ") + r.substituteSource;
                    promoteSource(id);
                    if (cb) cb(r);
                } else {
                    demoteSource(id);            // 本候选取链失败 → 降权后再试下一个
                    (*attempt)(next);
                }
            });
        });
    };
    (*attempt)(0);
}

// 「下一首 / 音乐推荐」：把请求转给它所属的平台适配器；没有 implemented 的平台
// 直接回调空列表，AppController 会退回原队列顺序。
void SourceManager::related(const Song &song, int limit, SongsCallback cb)
{
    MusicSource *s = source(song.source);
    if (!s) {
        if (cb) cb(QVector<Song>());
        return;
    }
    s->related(song, limit, std::move(cb));
}

void SourceManager::lyric(const Song &song, LyricCallback cb)
{
    const quint64 gen = ++m_lyricGen;
    MusicSource *s = source(song.source);

    // 取词渠道顺序跟随「取链顺序」设置（sourceFirst）：
    //   custom / vip —— 优先自定义（在线）音源取词，其次原平台，最后跨源匹配；
    //   platform     —— 先原平台，其次自定义，最后跨源。
    // 这一点很关键：播放走自定义音源替代时，原平台歌词接口（如咪咕）常被反爬返回
    // HTML 页，若仍按 song.source 先取原平台歌词就会拿到「乱码」。自定义优先能从
    // 根源避开；任何一层拿到有效歌词即止，无效则自动进入下一层。
    const QString order = Settings::string(QStringLiteral("sourceFirst"), QStringLiteral("vip"));
    const bool customFirst = Settings::boolean(QStringLiteral("vipSubstitute"), true)
                             && (order == QLatin1String("custom")
                                 || (order == QLatin1String("vip") && song.vip));
    QVector<QString> steps;
    if (customFirst) {
        steps << QStringLiteral("custom");
        if (s) steps << QStringLiteral("platform");
    } else {
        if (s) steps << QStringLiteral("platform");
        steps << QStringLiteral("custom");
    }
    steps << QStringLiteral("cross");   // 最后总是跨源智能匹配兜底

    auto accept = [this, gen, cb](const QString &text, const QString &trans) -> bool {
        if (gen != m_lyricGen) return true;              // 已切歌，停掉整条链
        if (!isLyricsUsable(text)) return false;         // HTML 反爬页/错误体/空 → 下一层
        if (cb) cb(text, trans);
        return true;
    };
    auto run = std::make_shared<std::function<void(int)>>();
    *run = [this, gen, song, cb, steps, run, accept](int i) {
        if (gen != m_lyricGen) return;
        if (i >= steps.size()) return;   // cross 是最后一步，它自己负责回调，不重复
        const QString step = steps.at(i);
        auto next = [run, i]() { (*run)(i + 1); };
        if (step == QLatin1String("platform")) {
            MusicSource *ps = source(song.source);
            if (!ps) { next(); return; }
            ps->lyric(song, [this, gen, accept, next](const QString &text, const QString &trans) {
                if (gen != m_lyricGen) return;
                if (!accept(text, trans)) next();
            });
        } else if (step == QLatin1String("custom")) {
            CustomSourceManager::instance()->lyric(song,
                [this, gen, accept, next](const QString &text, const QString &trans) {
                    if (gen != m_lyricGen) return;
                    if (!accept(text, trans)) next();
                });
        } else {   // cross
            qWarning().noquote() << QStringLiteral("[歌词] 「%1 - %2」各渠道均未拿到有效歌词，跨源搜索")
                                        .arg(song.title, song.artist);
            lyricCrossSource(song, cb);
        }
    };
    (*run)(0);
}

void SourceManager::lyricCrossSource(const Song &song, LyricCallback cb)
{
    const quint64 gen = m_lyricGen;
    // 对标洛雪 findMusic：用「歌名 + 歌手」跨全部平台搜索（限 25 条），
    // candidateScore 已含歌名相似度门槛 + 歌手/专辑/时长加权，是洛雪分层筛选的紧凑实现。
    const QString keyword = (song.title + QLatin1Char(' ') + song.artist).trimmed();
    const auto started = std::make_shared<bool>(false);
    search(keyword, QString(), 25, [this, gen, song, cb, started](const QVector<Song> &list) {
        if (*started || gen != m_lyricGen) return;
        // 打分排序，先跳过原平台自己的重复结果
        QVector<QPair<qreal, Song>> ranked;
        for (const Song &c : list) {
            if (c.source == song.source) continue;
            const qreal score = candidateScore(song, c);
            if (score > 0.55) ranked.append(qMakePair(score, c));
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const QPair<qreal, Song> &a, const QPair<qreal, Song> &b) {
                      return a.first > b.first;
                  });
        if (ranked.isEmpty()) return;   // 软刷新批次可能不全，等下一批
        *started = true;                // 已有候选，兜底定时器不再触发

        // 逐个候选源试歌词，拿到第一个带时间轴的有效歌词就收工。
        // 用共享游标递归，避免为同一首歌并发取多个源。
        struct Ctx {
            QVector<QPair<qreal, Song>> ranked;
            int index = 0;
            bool done = false;
        };
        auto ctx = std::make_shared<Ctx>();
        ctx->ranked = ranked;
        auto tryNext = std::make_shared<std::function<void()>>();
        *tryNext = [this, gen, song, cb, ctx, tryNext]() {
            if (gen != m_lyricGen || ctx->done) return;
            if (ctx->index >= ctx->ranked.size()) {
                ctx->done = true;
                if (cb) cb(QString(), QString());   // 全部候选都没有歌词
                return;
            }
            const Song cand = ctx->ranked.at(ctx->index++).second;
            MusicSource *target = source(cand.source);
            if (!target) { (*tryNext)(); return; }
            target->lyric(cand, [this, gen, song, cand, cb, ctx, tryNext](const QString &text,
                                                                          const QString &trans) {
                if (gen != m_lyricGen || ctx->done) return;
                if (isLyricsUsable(text)) {
                    ctx->done = true;
                    qInfo().noquote() << QStringLiteral("[歌词] 跨源命中「%1 - %2」→ %3（%4，%5）")
                                             .arg(song.title, song.artist, cand.title, cand.artist,
                                                  cand.source);
                    if (cb) cb(text, trans);
                    return;
                }
                (*tryNext)();
            });
        };
        (*tryNext)();
    });
    // 兜底收尾：search 各批次始终没匹配上时，明确回调空歌词，避免 UI 永远停在取词中
    QTimer::singleShot(9000, this, [this, gen, cb, started]() {
        if (!*started && gen == m_lyricGen && cb) cb(QString(), QString());
    });
}

void SourceManager::cover(const Song &song, StringCallback cb)
{
    MusicSource *s = source(song.source);
    if (!s) { if (cb) cb(QString()); return; }
    s->cover(song, cb);
}

void SourceManager::comments(const Song &song, int page, CommentCallback cb)
{
    // 脚本层搜出来的歌（带 scriptId）直接走脚本评论；内置平台先试自己的评论接口，
    // 取不到（不支持/被反爬/空）再让声明了 comment 能力的脚本兜底。
    MusicSource *s = source(song.source);
    const bool scriptSong = song.source == QLatin1String("custom")
            || song.extra.contains(QStringLiteral("scriptId"));
    if (s && !scriptSong) {
        s->comments(song, page, [song, page, cb](const QVector<Comment> &list, int total) {
            if (!list.isEmpty()) { if (cb) cb(list, total); return; }
            ScriptSourceManager::instance()->comments(song, page, cb);
        });
        return;
    }
    ScriptSourceManager::instance()->comments(song, page, std::move(cb));
}

void SourceManager::mv(const Song &song, MvCallback cb)
{
    MusicSource *s = source(song.source);
    if (!s) {
        if (cb) cb(false, QUrl(), QStringLiteral("未知音源，无法播放 MV"));
        return;
    }
    // 只有网易云配置了 mvUrl/mvInfo 端点；其它平台（酷狗/QQ/酷我…）直接查会被
    // 判定「该平台暂未配置 MV」。先试本平台，失败再按歌名+歌手去网易云跨源匹配。
    s->mv(song, [this, song, cb](bool ok, const QUrl &url, const QString &error) {
        if (ok) { if (cb) cb(true, url, error); return; }
        mvCrossSource(song, cb);
    });
}

void SourceManager::mvCrossSource(const Song &song, MvCallback cb)
{
    MusicSource *netease = source(QStringLiteral("netease"));
    if (!netease) {
        if (cb) cb(false, QUrl(), QStringLiteral("没有可用的 MV 匹配源"));
        return;
    }
    const QString keyword = (song.title + QLatin1Char(' ') + song.artist).trimmed();
    const auto started = std::make_shared<bool>(false);
    search(keyword, QStringLiteral("netease"), 20,
           [this, song, netease, cb, started](const QVector<Song> &list) {
        if (*started) return;
        qreal best = 0.6;
        QString mvid;
        Song matched;
        for (const Song &c : list) {
            // 网易云搜索把原始 JSON 存进了 extra，mvid 不在字段映射里，直接读原值
            const QString id = c.extra.value(QStringLiteral("mvid")).toString();
            if (id.isEmpty() || id == QLatin1String("0")) continue;
            const qreal sc = candidateScore(song, c);
            if (sc > best) { best = sc; mvid = id; matched = c; }
        }
        if (mvid.isEmpty() || matched.title.isEmpty()) return;
        *started = true;
        qInfo().noquote() << QStringLiteral("[MV] 跨源命中「%1 - %2」→ 网易云 MV %3（%4）")
                                 .arg(song.title, song.artist, mvid).arg(best, 0, 'f', 2);
        Song target = matched;
        target.source = QStringLiteral("netease");
        target.extra.insert(QStringLiteral("mvid"), mvid);
        netease->mv(target, cb);
    });
    // search 的多次软刷新回调都没有匹配时兜底收尾，避免 MV 按钮一直转圈
    QTimer::singleShot(8500, this, [song, cb, started]() {
        if (!*started && cb) {
            qInfo().noquote() << QStringLiteral("[MV] 跨源搜索「%1 %2」无匹配")
                                     .arg(song.title, song.artist);
            cb(false, QUrl(), QStringLiteral("没有找到匹配的 MV"));
        }
    });
}

void SourceManager::mvList(const QString &sourceId, const QString &area, int limit,
                           int offset, MvListCallback cb)
{
    MusicSource *s = source(sourceId);
    if (!s) {
        if (cb) cb(QVector<QVariantMap>(), false);
        return;
    }
    s->mvList(area, limit, offset, std::move(cb));
}

} // namespace Furui
