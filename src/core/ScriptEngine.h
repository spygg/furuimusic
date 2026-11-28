#pragma once
// 福瑞音乐 - 在线 JS 音源宿主（洛雪 LX Music 用户脚本运行时）
//
// 用内嵌的 QuickJS 执行用户导入的 LX 风格音源脚本，并向脚本注入 `globalThis.lx`：
//   lx.EVENT_NAMES / lx.on / lx.send / lx.request(url, options, cb)
//   lx.utils（crypto: md5/sha1/sha256/randomBytes；buffer: from/bufToStr/concat/alloc）
//   lx.env / lx.version / lx.currentScriptInfo
//
// 脚本能力在运行期通过 send(EVENT_NAMES.inited, {status, sources}) 声明；
// 宿主只负责转发与超时隔离，不替脚本伪造结果。
#include <QDateTime>
#include <QObject>
#include <QHash>
#include <QSemaphore>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

#include "HttpClient.h"

namespace Furui {

struct ScriptSourceInfo {
    QString key;        // kw / kg / tx / wy / mg
    QString name;
    QString type;       // music
    QStringList actions;   // search / musicUrl / lyric ...
    QStringList qualities; // 128k / 320k / flac / flac24bit ...
};

struct ScriptInitResult {
    bool ok = false;
    QString errorType;   // syntax / runtime / init / timeout / http / network / tls / empty
    QString errorMsg;
    QVector<ScriptSourceInfo> sources;
};

struct ScriptActionResult {
    bool ok = false;
    QVariant data;
    QString error;
    QString errorType;
};

class ScriptEngine : public QObject {
    Q_OBJECT
public:
    using InitCallback = std::function<void(const ScriptInitResult &)>;
    using ActionCallback = std::function<void(const ScriptActionResult &)>;

    explicit ScriptEngine(QString name, QObject *parent = nullptr);
    ~ScriptEngine() override;

    // 执行脚本；脚本异步 send(inited) 时回调。超时视为初始化失败。
    // 本对象始终留在 GUI 线程，start 只发布快照并向固定 worker 投递初始化。
    void start(const QString &script, const QVariantMap &meta,
               InitCallback cb, int timeoutMs = 30000);
    bool ready() const { return m_ready; }
    QVector<ScriptSourceInfo> sources() const { return m_sources; }
    bool supports(const QString &sourceKey, const QString &action) const;

    // 调用脚本注册的 request 处理器；异步返回，内部有超时隔离。
    // 业务回调只在 GUI 线程执行。
    void callAction(const QString &sourceKey, const QString &action, const QVariantMap &info,
                    ActionCallback cb, int timeoutMs = 20000);
    void stop();

    QString name() const { return m_name; }
    QString version() const { return m_version; }

signals:
    // worker 已完成 teardown 并销毁 workerContext 后，以 QueuedConnection 通知 GUI。
    void workerStopped();

public:
    // aboutToQuit：取消全部引擎、等待 worker 清理，再退出并 join 固定线程。
    static void shutdownWorkers();

private:
    friend struct ScriptEngineHost;

    struct Lifetime {
        std::atomic<bool> cancelled{false};
        QSemaphore stopped{0};
    };

    // 给一段 JS 执行加硬超时（QuickJS 中断处理器读 m_execDeadline）。
    // 保存/恢复旧值以支持嵌套；离开作用域自动撤销，避免任何 JS_Call 裸奔。
    class ExecDeadlineGuard {
    public:
        explicit ExecDeadlineGuard(ScriptEngine *engine, int timeoutMs)
            : m_engine(engine), m_saved(engine ? engine->m_execDeadline : 0)
        {
            if (m_engine)
                m_engine->m_execDeadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
        }
        ~ExecDeadlineGuard() { if (m_engine) m_engine->m_execDeadline = m_saved; }
        ExecDeadlineGuard(const ExecDeadlineGuard &) = delete;
        ExecDeadlineGuard &operator=(const ExecDeadlineGuard &) = delete;
    private:
        ScriptEngine *m_engine;
        qint64 m_saved;
    };

    // 以下方法均由 workerContext 的事件在固定 worker 线程执行。
    void initializeWorker(const QString &script, InitCallback cb, int timeoutMs,
                          const std::shared_ptr<Lifetime> &lifetime, quint64 generation);
    void executeAction(const QString &sourceKey, const QString &action,
                       const QVariantMap &info, ActionCallback cb, int timeoutMs,
                       const std::shared_ptr<Lifetime> &lifetime, quint64 generation);
    void stopWorker(const std::shared_ptr<Lifetime> &lifetime, quint64 generation);
    void createContext();
    void evalScript(const QString &script);
    void drainJobs();
    void onInited(const QVariant &data);
    void finishInit(bool ok, const QString &type, const QString &msg);
    void completeAction(int tag, const ScriptActionResult &result);
    void postInitResult(const std::shared_ptr<Lifetime> &lifetime, quint64 generation,
                        InitCallback cb, const ScriptInitResult &result);
    void postActionResult(const std::shared_ptr<Lifetime> &lifetime, quint64 generation,
                          ActionCallback cb, const ScriptActionResult &result);
    void reportException(const QString &where);
    void teardownContext();

    // ---- GUI-only facade 状态 ----
    QString m_name;
    QString m_version;                    // start 投递前写一次，worker 只读
    QVariantMap m_meta;                   // start 投递前写一次，worker 只读
    bool m_ready = false;
    QVector<ScriptSourceInfo> m_sources;   // GUI 快照
    bool m_workerActive = false;          // GUI-only；workerStopped 后清零
    bool m_workerStoppedNotified = false; // GUI-only；兼容未启动引擎的 stop
    quint64 m_generation = 0;             // GUI-only；任务只捕获快照
    std::shared_ptr<Lifetime> m_lifetime;
    QObject *m_workerContext = nullptr;    // GUI 创建并发布；active 期间地址不变

    // ---- worker-only JS 状态 ----
    std::atomic<bool> m_stopped{false};   // GUI 原子取消，worker 中断处理读取
    void *m_rt = nullptr;
    void *m_ctx = nullptr;
    QString m_lastRejection;
    bool m_inited = false;
    bool m_initDone = false;
    bool m_draining = false;
    bool m_workerStopping = false;
    quint64 m_workerGeneration = 0;       // initialize 任务发布的代次快照
    QVector<ScriptSourceInfo> m_initSources;
    InitCallback m_initCb;
    QTimer *m_initTimer = nullptr;
    QHash<QString, void *> m_handlers;    // event -> JSValue holder
    QHash<int, void *> m_timers;          // timer id -> JSValue holder
    QHash<int, QTimer *> m_intervals;     // setInterval 的周期定时器（id 复用 m_nextTimer）
    qint64 m_execDeadline = 0;            // 正在执行的 JS 的硬超时（毫秒）
    struct Pending { ActionCallback cb; QTimer *timer = nullptr; };
    QHash<int, Pending> m_pending;
    int m_nextTag = 0;
    int m_nextTimer = 0;
};

} // namespace Furui
