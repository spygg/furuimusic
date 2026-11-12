#include "core/QtCompat.h"
#include "SourceManager.h"

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
#include "Settings.h"
#include "SourceConfig.h"

namespace Furui {

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
    if (!s || id.isEmpty()) { finish(QVector<Song>()); return; }
    if (playlist.type == QLatin1String("user"))
        s->userPlaylistDetail(id, finish);
    else if (playlist.type == QLatin1String("rank"))
        s->rankDetail(id, Settings::integer(QStringLiteral("pageSize"), 30) * 5, 0, finish);
    else
        s->playlistDetail(id, Settings::integer(QStringLiteral("pageSize"), 30) * 5, 0, finish);
}

void SourceManager::resolve(const Song &song, const QString &quality, PlayableCallback cb)
{
    if (song.source == QLatin1String("custom")) {
        CustomSourceManager::instance()->resolve(song, quality, cb);
        return;
    }
    MusicSource *s = source(song.source);
    if (!s) { CustomSourceManager::instance()->resolve(song, quality, cb); return; }

    s->resolveUrl(song, quality, [this, song, quality, cb](const Playable &original) {
        if (original.ok) { if (cb) cb(original); return; }
        if (!Settings::boolean(QStringLiteral("vipSubstitute"), true)) {
            if (cb) cb(original);
            return;
        }
        CustomSourceManager::instance()->resolve(song, quality,
            [this, song, quality, original, cb](const Playable &sub) {
                if (sub.ok) {
                    Playable r = sub;
                    r.substituted = true;
                    r.needSubstitute = false;
                    r.message = QStringLiteral("已由自定义音源「%1」替代播放").arg(r.substituteSource);
                    if (cb) cb(r);
                    return;
                }
                Playable r = original;
                // 把「原平台失败」与「替代音源为什么也没成」分开说清楚，
                // 否则用户只会看到一句「需要会员」，误以为导入的音源没生效。
                r.message = original.message + QStringLiteral("；") + sub.message
                          + QStringLiteral("。可在「设置 → 在线音源」检查状态或换用其它音源");
                if (Settings::boolean(QStringLiteral("autoToggleSource"), true)) {
                    tryCrossSource(song, quality, r, cb);
                } else if (cb) {
                    cb(r);
                }
            });
    });
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
                                   const Playable &original, PlayableCallback cb)
{
    const QString keyword = (song.title + QLatin1Char(' ') + song.artist).trimmed();
    search(keyword, QString(), 25, [this, song, quality, original, cb](const QVector<Song> &list) {
        qreal best = 0.55;
        Song matched;
        for (const Song &c : list) {
            const qreal score = candidateScore(song, c);
            if (score > best) { best = score; matched = c; }
        }
        MusicSource *target = matched.title.isEmpty() ? nullptr : source(matched.source);
        if (!target) { if (cb) cb(original); return; }
        target->resolveUrl(matched, quality, [this, matched, original, cb](const Playable &p) {
            if (p.ok) {
                Playable r = p;
                r.substituted = true;
                r.needSubstitute = false;
                r.source = matched.source;
                r.substituteSource = SourceConfig::instance()->nameOf(matched.source) + QStringLiteral("（自动换源）");
                r.message = QStringLiteral("原曲不可播放，已自动换源到 %1").arg(r.substituteSource);
                promoteSource(matched.source);   // 解析阶段换源成功也置顶，保持"成功源第一位"
                if (cb) cb(r);
            } else if (cb) {
                cb(original);
            }
        });
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

void SourceManager::resolveAlternate(const Song &song, const QStringList &exclude,
                                     const QString &quality, PlayableCallback cb)
{
    // 候选顺序：自定义音源最优先（VIP 替代），随后按 m_sourceOrder 优先级遍历内置平台。
    // exclude 里的（已失败的原平台 + 本次已尝试过的候选）不再试，避免重复与死循环。
    QStringList candidates;
    if (!exclude.contains(QStringLiteral("custom")))
        candidates << QStringLiteral("custom");
    for (const QString &id : m_sourceOrder)
        if (!exclude.contains(id) && source(id)) candidates << id;
    for (MusicSource *s : m_sources)
        if (!candidates.contains(s->id()) && !exclude.contains(s->id()))
            candidates << s->id();
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
                    (*attempt)(next);
                }
            });
        });
    };
    (*attempt)(0);
}

void SourceManager::lyric(const Song &song, StringCallback cb)
{
    MusicSource *s = source(song.source);
    if (!s) { CustomSourceManager::instance()->lyric(song, cb); return; }
    s->lyric(song, [song, cb](const QString &text) {
        if (!text.trimmed().isEmpty()) { if (cb) cb(text); return; }
        CustomSourceManager::instance()->lyric(song, cb);
    });
}

void SourceManager::cover(const Song &song, StringCallback cb)
{
    MusicSource *s = source(song.source);
    if (!s) { if (cb) cb(QString()); return; }
    s->cover(song, cb);
}

void SourceManager::mv(const Song &song, MvCallback cb)
{
    MusicSource *s = source(song.source);
    if (!s) {
        if (cb) cb(false, QUrl(), QStringLiteral("未知音源，无法播放 MV"));
        return;
    }
    s->mv(song, std::move(cb));
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
