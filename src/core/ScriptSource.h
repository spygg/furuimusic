#pragma once
// 福瑞音乐 - 在线 JS 音源管理（导入 / 更新 / 删除 / 启用禁用 / 状态 / 聚合）
//
// 只做三件事：管理脚本生命周期、持久化其能力声明、把脚本提供的
// search / musicUrl / lyric 接到聚合搜索与播放；不随安装包内置任何脚本，
// 首次启动仅从公开仓库页面按需发现并导入。
#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <functional>

#include "ScriptEngine.h"
#include "Types.h"

namespace Furui {

class ScriptSourceManager : public QObject {
    Q_OBJECT
public:
    using DoneCallback = std::function<void(bool ok, const QString &message)>;
    using SongsCallback = std::function<void(const QVector<Song> &)>;
    using PlayableCallback = std::function<void(const Playable &)>;
    using StringCallback = std::function<void(const QString &)>;
    using LyricCallback = Furui::LyricCallback;   // (lrc, 翻译轨)

    static ScriptSourceManager *instance();

    QVariantList items() const;          // 供 QML 直接展示（含状态/能力/版本）
    bool hasEnabled() const;
    bool hasSearch() const;
    bool hasMusicUrl() const;

    // url 导入：下载 → 解析元信息 → 落库 → 立即初始化并记录真实状态。
    // silent = 内置音源自动预置用：不弹任何提示，失败也不打扰（用户可在设置页手动重试）
    void importUrl(const QString &url, DoneCallback cb = {}, bool silent = false);
    // 重新下载并初始化（脚本更新）
    void update(const QString &id, DoneCallback cb = {});
    // 用本地已存的脚本重新初始化（不改 URL）
    void check(const QString &id, DoneCallback cb = {});
    void checkAll();
    void remove(const QString &id);
    void setEnabled(const QString &id, bool enabled);
    // 手动重新解析仓库页面（忽略每日节流），用于发现仓库新增的音源
    void rescanRepository();
    // 动态权重：成功取链的源置顶，失败的源后移一位（顺序持久化到 scriptSourceOrder）。
    void promoteSource(const QString &id);
    void demoteSource(const QString &id);

    // 聚合入口
    void search(const QString &keyword, int limit, SongsCallback cb);
    void resolve(const Song &song, const QString &quality, PlayableCallback cb);
    void lyric(const Song &song, LyricCallback cb);

    static QString platformToLx(const QString &platform);
    static QString qualityToLx(const QString &quality);

signals:
    void changed();
    void message(const QString &id, bool ok, const QString &text);

private:
    explicit ScriptSourceManager(QObject *parent = nullptr);

    struct Item {
        QString id;
        QString name;
        QString url;
        QString version;
        QString script;
        QString hash;
        bool enabled = true;
        QString status = QStringLiteral("pending");  // ok / error / pending / disabled
        QString errorType;
        QString errorMsg;
        QStringList actions;
        QStringList qualities;
        QJsonArray sources;   // [{key,name,actions,qualities}]
        qint64 updated = 0;
        qint64 checked = 0;
        bool ready = false;
    };

    void load();
    void seedBuiltinSources();           // 首次启动预置内置在线音源（用户删除过的不再补回）
    void importWithFallback(const QStringList &mirrorUrls, int index,
                            std::function<void()> done = {});
    struct DiscoveredSource {
        QString key;     // 仓库目录名，健康状态用它标记
        QString name;    // README 小节名
        QString url;     // raw 脚本地址
        QStringList mirrors;  // 小节里给出的全部地址（原始 + 加速，按页面顺序）
    };
    // 解析仓库页面（README 原文）：按「### 名称」小节收集该节的全部 latest.js 地址
    static QVector<DiscoveredSource> parseRepoPage(const QString &text);
    // 原始 raw 地址 -> 多条加速镜像（含原始地址本身）
    static QStringList mirrorUrlsFor(const QString &rawUrl);
    void seedDiscovered(const QVector<DiscoveredSource> &list, std::function<void()> done = {});
    bool m_seeding = false;
    static QString scriptIdForUrl(const QString &url);
    static bool isBuiltinUrl(const QString &url);
    int rankOf(const QString &id) const;
    QStringList orderedIds() const;      // 按权重排好序的音源 id
    void saveOrder();
    void persist(const Item &item);
    Item *find(const QString &id);
    void ensureEngine(const QString &id, std::function<void(bool)> cb);
    void startEngine(Item &item, DoneCallback cb, bool persistResult);
    void updateFromInit(Item &item, const ScriptInitResult &r);
    void fillMeta(Item &item, const QString &script);
    QVariantMap itemToVariant(const Item &item) const;
    QVector<ScriptEngine *> enginesFor(const QString &lxKey, const QString &action);
    QVariantMap musicInfoFor(const Song &song, const QString &lxKey) const;
    static QString classifyNetworkError(const QString &error, int status);
    static QString metaLine(const QString &script, const QString &key);

    QHash<QString, Item> m_items;
    // 音源使用顺序（权重从高到低）：取链成功置顶、失败后移一位
    QStringList m_order;
    QHash<QString, QPointer<ScriptEngine>> m_engines;
    QHash<QString, QVector<std::function<void(bool)>>> m_waiters;
    QSet<QString> m_starting;
};

} // namespace Furui
