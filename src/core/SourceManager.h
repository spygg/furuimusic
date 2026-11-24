#pragma once
// 福瑞音乐 - 多平台聚合 + VIP 音源替代链
#include <QObject>
#include <QSet>
#include <QVector>
#include <functional>
#include <memory>

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
    using LyricCallback = Furui::LyricCallback;   // (lrc, 翻译轨)
    using MvCallback = std::function<void(bool ok, const QUrl &url, const QString &error)>;
    using MvListCallback = std::function<void(const QVector<QVariantMap> &items, bool hasMore)>;

    static SourceManager *instance();

    QVector<MusicSource *> sources() const { return m_sources; }
    MusicSource *source(const QString &id) const;
    void refreshSessions();

    void search(const QString &keyword, const QString &sourceId, int limit, SongsCallback cb);
    void searchPlaylists(const QString &keyword, const QString &sourceId, int limit, PlaylistsCallback cb);
    void rank(const QString &sourceId, int limit, PlaylistsCallback cb);
    // 某个平台的静态默认榜单：网络/登录态导致 rank 为空时作本地兜底
    QVector<Playlist> defaultBoards(const QString &sourceId) const;
    void playlistDetail(const Playlist &playlist, SongsCallback cb);
    // quiet=true：后台预取链用，跳过 demoteSource/noteNeedsSubstitute 等排名副作用
    void resolve(const Song &song, const QString &quality, PlayableCallback cb,
                 bool quiet = false);
    // 播放失败后的换源：依次尝试 custom 与各内置平台（按 m_sourceOrder 优先级），
    // 跳过 exclude 中的音源（通常是已失败的原平台 + 已尝试过的候选）。找到可播直链即回调。
    void resolveAlternate(const Song &song, const QStringList &exclude,
                         const QString &quality, PlayableCallback cb);
    // 把成功播放的音源置顶：后续换源/自动换源优先尝试它。custom 始终置顶，无需排序。
    void promoteSource(const QString &id);
    // 取链/播放失败：后移一位，降低后续优先度（顺序持久化，跨重启保持）。
    void demoteSource(const QString &id);
    // 曲中断流：该源给的直链播到一半死了，沉底到候选末位（promoteSource 可拉回）。
    void penalizeSource(const QString &id);
    void lyric(const Song &song, LyricCallback cb);
    // 歌曲评论（洛雪式）：按平台取评论，page 从 1 起
    void comments(const Song &song, int page, CommentCallback cb);
    // 歌词跨源智能匹配（对标洛雪 findMusic）：当前平台歌词无效（空/HTML 反爬页/
    // 错误体）时，按歌名+歌手跨源搜索，用歌名/歌手/专辑/时长打分选出最佳候选，
    // 逐个候选源取歌词直到拿到有效时间轴歌词。内部有 generation 守卫防切歌竞态。
    void lyricCrossSource(const Song &song, LyricCallback cb);
    // 「下一首 / 音乐推荐」：以当前歌曲为种子取相似歌曲（目前汽水已实现）。
    void related(const Song &song, int limit, SongsCallback cb);
    void cover(const Song &song, StringCallback cb);
    void mv(const Song &song, MvCallback cb);
    // 本平台没有 MV 时跨源匹配：按「歌名+歌手」直接搜各平台 MV 库（网易云 type=1004、
    // QQ search_type=4），用歌名/歌手/时长打分选最佳，再走对应平台的 mvUrl 取地址。
    // 比「搜歌曲再取 mvid」命中率高得多：歌曲回包 mvid 常为 0，Netease 缺版权的歌
    // 甚至搜不到歌，但 MV 库/QQ 仍能命中。
    void mvCrossSource(const Song &song, MvCallback cb);
    void mvList(const QString &sourceId, const QString &area, int limit, int offset,
                MvListCallback cb);

private:
    explicit SourceManager(QObject *parent = nullptr);
    void tryCrossSource(const Song &song, const QString &quality, const Playable &original,
                        PlayableCallback cb, bool quiet = false);
    static qreal candidateScore(const Song &target, const Song &candidate);
    // MV 跨源链式尝试：逐个平台搜 MV，命中即取址；done 保证「命中/超时」只回调一次。
    void mvSearchNext(const Song &song, const QString &keyword, QStringList candidates,
                      const std::shared_ptr<bool> &done, MvCallback cb);
    // 平台已经明确说过「这首要会员 / 没版权」的歌，本会话内记住：
    // 下次同一首直接跳过原平台直链，省掉一次注定失败的请求。
    void noteNeedsSubstitute(const Song &song);
    bool isKnownUnplayable(const Song &song) const;
    QSet<QString> m_knownUnplayable;
    // 歌词跨源匹配的代次守卫：切歌/重新取词时 ++，旧的异步回调直接丢弃。
    quint64 m_lyricGen = 0;
    QVector<MusicSource *> m_sources;
    // 音源优先级（内置平台 id 顺序）；custom 永远排在最前，不在本表里。
    // 由 promoteSource 在成功播放后置顶，并持久化到 Settings，跨重启保持"成功源第一位"。
    QStringList m_sourceOrder;
};

} // namespace Furui
