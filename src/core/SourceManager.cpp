#include "SourceManager.h"

#include <QSet>
#include <algorithm>

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
    auto done = [remaining, collected, cb, keyword]() {
        if (--(*remaining) > 0) return;
        // 相关度排序：歌名完全一致 > 歌名以关键词开头 > 歌名包含关键词 > 其余；
        // 用 stable_sort 保证同一档内仍按平台返回顺序排列。
        // （原来按 normalizedKey() 长度升序，等于「标题越短越靠前」，和关键词无关）
        const QString kw = keyword.trimmed();
        auto score = [&kw](const Song &s) {
            const QString title = s.title.trimmed();
            if (kw.isEmpty()) return 3;
            if (title.compare(kw, Qt::CaseInsensitive) == 0) return 0;
            if (title.startsWith(kw, Qt::CaseInsensitive)) return 1;
            if (title.contains(kw, Qt::CaseInsensitive)) return 2;
            return 3;
        };
        std::stable_sort(collected->begin(), collected->end(), [&score](const Song &a, const Song &b) {
            return score(a) < score(b);
        });
        if (cb) cb(*collected);
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

    auto remaining = std::make_shared<int>(targets.size());
    auto collected = std::make_shared<QVector<Playlist>>();
    for (MusicSource *s : targets) {
        s->rank(limit, 0, [collected, remaining, cb](const QVector<Playlist> &list) {
            collected->append(list);
            if (--(*remaining) == 0 && cb) cb(*collected);
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
                r.message = original.message + QStringLiteral("；") + sub.message;
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
        target->resolveUrl(matched, quality, [matched, original, cb](const Playable &p) {
            if (p.ok) {
                Playable r = p;
                r.substituted = true;
                r.needSubstitute = false;
                r.substituteSource = SourceConfig::instance()->nameOf(matched.source) + QStringLiteral("（自动换源）");
                r.message = QStringLiteral("原曲不可播放，已自动换源到 %1").arg(r.substituteSource);
                if (cb) cb(r);
            } else if (cb) {
                cb(original);
            }
        });
    });
}

void SourceManager::lyric(const Song &song, StringCallback cb)
{
    MusicSource *s = source(song.source);
    if (!s) { CustomSourceManager::instance()->lyric(song, cb); return; }
    s->lyric(song, [song, cb](const QString &text) {
        if (!text.isEmpty()) { if (cb) cb(text); return; }
        CustomSourceManager::instance()->lyric(song, cb);
    });
}

} // namespace Furui
