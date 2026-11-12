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
};

} // namespace Furui
