#pragma once
// 福瑞音乐 - 在线 JS 音源管理（导入 / 更新 / 删除 / 启用禁用 / 状态 / 聚合）
//
// 只做三件事：管理脚本生命周期、持久化其能力声明、把脚本提供的
// search / musicUrl / lyric 接到聚合搜索与播放；不内置任何第三方源。
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

    static ScriptSourceManager *instance();

    QVariantList items() const;          // 供 QML 直接展示（含状态/能力/版本）
    bool hasEnabled() const;
    bool hasSearch() const;
    bool hasMusicUrl() const;

    // url 导入：下载 → 解析元信息 → 落库 → 立即初始化并记录真实状态
    void importUrl(const QString &url, DoneCallback cb = {});
    // 重新下载并初始化（脚本更新）
    void update(const QString &id, DoneCallback cb = {});
    // 用本地已存的脚本重新初始化（不改 URL）
    void check(const QString &id, DoneCallback cb = {});
    void checkAll();
    void remove(const QString &id);
    void setEnabled(const QString &id, bool enabled);

    // 聚合入口
    void search(const QString &keyword, int limit, SongsCallback cb);
    void resolve(const Song &song, const QString &quality, PlayableCallback cb);
    void lyric(const Song &song, StringCallback cb);

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
    void importWithFallback(const QStringList &mirrorUrls, int index);
    static QString scriptIdForUrl(const QString &url);
    static bool isBuiltinUrl(const QString &url);
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
    QHash<QString, QPointer<ScriptEngine>> m_engines;
    QHash<QString, QVector<std::function<void(bool)>>> m_waiters;
    QSet<QString> m_starting;
};

} // namespace Furui
