#pragma once
// 福瑞音乐 - 多平台聚合 + VIP 音源替代链
#include <QObject>
#include <QVector>
#include <functional>

#include "Types.h"

namespace Furui {

class MusicSource;

class SourceManager : public QObject {
    Q_OBJECT
public:
    using SongsCallback = std::function<void(const QVector<Song> &)>;
    using PlaylistsCallback = std::function<void(const QVector<Playlist> &)>;
    using PlayableCallback = std::function<void(const Playable &)>;
    using StringCallback = std::function<void(const QString &)>;
    using MvCallback = std::function<void(bool ok, const QUrl &url, const QString &error)>;
    using MvListCallback = std::function<void(const QVector<QVariantMap> &items, bool hasMore)>;

    static SourceManager *instance();

    QVector<MusicSource *> sources() const { return m_sources; }
    MusicSource *source(const QString &id) const;
    void refreshSessions();

    void search(const QString &keyword, const QString &sourceId, int limit, SongsCallback cb);
    void searchPlaylists(const QString &keyword, const QString &sourceId, int limit, PlaylistsCallback cb);
    void rank(const QString &sourceId, int limit, PlaylistsCallback cb);
    void playlistDetail(const Playlist &playlist, SongsCallback cb);
    void resolve(const Song &song, const QString &quality, PlayableCallback cb);
    // 播放失败后的换源：依次尝试 custom 与各内置平台（按 m_sourceOrder 优先级），
    // 跳过 exclude 中的音源（通常是已失败的原平台 + 已尝试过的候选）。找到可播直链即回调。
    void resolveAlternate(const Song &song, const QStringList &exclude,
                         const QString &quality, PlayableCallback cb);
    // 把成功播放的音源置顶：后续换源/自动换源优先尝试它。custom 始终置顶，无需排序。
    void promoteSource(const QString &id);
    void lyric(const Song &song, StringCallback cb);
    void cover(const Song &song, StringCallback cb);
    void mv(const Song &song, MvCallback cb);
    void mvList(const QString &sourceId, const QString &area, int limit, int offset,
                MvListCallback cb);

private:
    explicit SourceManager(QObject *parent = nullptr);
    void tryCrossSource(const Song &song, const QString &quality, const Playable &original,
                        PlayableCallback cb);
    static qreal candidateScore(const Song &target, const Song &candidate);
    QVector<MusicSource *> m_sources;
    // 音源优先级（内置平台 id 顺序）；custom 永远排在最前，不在本表里。
    // 由 promoteSource 在成功播放后置顶，并持久化到 Settings，跨重启保持"成功源第一位"。
    QStringList m_sourceOrder;
};

} // namespace Furui
