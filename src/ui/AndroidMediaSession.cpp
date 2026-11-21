#include "AndroidMediaSession.h"

#include "../core/HttpClient.h"
#include "../core/Logger.h"
#include "../core/PlayerController.h"
#include "../core/Settings.h"
#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include "../core/AppController.h"
#endif

#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
// QNativeInterface::QAndroidApplication 的真实头文件（Qt 6.9+ 才有前向路径，
// 6.8.3 没有；与 UpdateChecker.cpp 保持一致，显式 include 以保证兼容）。
#include <QtCore/qcoreapplication_platform.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJniEnvironment>
#include <QJniObject>
#include <QMetaObject>
#include <QMutex>
#include <QOperatingSystemVersion>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVector>
#include <functional>
#include <jni.h>
#include <memory>
#endif

namespace Furui {

#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
namespace {

constexpr const char *kServiceClass = "cn/furui/music/MediaSessionService";

// 与 MediaSessionService.java 中的 CMD_* 一一对应
enum Command {
    CmdPlay = 1, CmdPause = 2, CmdNext = 3, CmdPrev = 4, CmdSeek = 5, CmdStop = 6,
    // 音频焦点回调：其它应用开始播放时由 Java 监听器回传
    CmdDuck = 7, CmdUnduck = 8,
    // 车机歌单/模式类口令（只有 BYD 特供版会发）
    CmdPlayLike = 9, CmdPlayLately = 10, CmdRandom = 11,
    CmdModeOrder = 12, CmdModeCycle = 13, CmdModeLoop = 14, CmdModeRandom = 15,
    CmdCollect = 16, CmdUncollect = 17
};

AndroidMediaSession *g_instance = nullptr;
bool g_serviceStarted = false;
bool g_permissionRequested = false;
QMutex g_commandMutex;

// 语音点歌的多轮搜索状态：先按歌名搜，没结果再拼歌手，最后只用歌手兜底。
struct VoiceSearchCtx {
    QVector<QString> queries;
    int index = -1;
    QMetaObject::Connection conn;
    QTimer *timer = nullptr;
    bool done = false;
};

// Android 运行时权限申请：
// - API 33+ 的 POST_NOTIFICATIONS：通知栏条目（前台服务本身不依赖它）
// - API <=28 的 WRITE_EXTERNAL_STORAGE：老机/车机把更新 APK 复制到公共 Download
//   （API 29+ 走 MediaStore 不需要该权限）。
void requestNotificationPermission()
{
    if (g_permissionRequested)
        return;
    g_permissionRequested = true;

    QJniEnvironment env;
    JNIEnv *jni = env.jniEnv();
    if (!jni)
        return;
    // QtNative.activity() 在 Java 侧不是 public，但 JNI 按名称/签名查找不受可见性限制
    QJniObject activity = QJniObject::callStaticObjectMethod(
        "org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
    if (!activity.isValid() || !activity.object())
        return;

    // 读 Build.VERSION.SDK_INT，按版本决定申请哪些权限
    jclass buildClass = jni->FindClass("android/os/Build$VERSION");
    if (!buildClass) return;
    jfieldID sdkField = jni->GetStaticFieldID(buildClass, "SDK_INT", "I");
    const int sdk = sdkField ? jni->GetStaticIntField(buildClass, sdkField) : 0;
    jni->DeleteLocalRef(buildClass);

    QStringList perms;
    if (sdk >= 33) perms << QStringLiteral("android.permission.POST_NOTIFICATIONS");
    if (sdk <= 28) perms << QStringLiteral("android.permission.WRITE_EXTERNAL_STORAGE");
    if (perms.isEmpty()) return;

    jclass stringClass = jni->FindClass("java/lang/String");
    if (!stringClass) return;
    jobjectArray permissions = jni->NewObjectArray(perms.size(), stringClass, nullptr);
    for (int i = 0; i < perms.size(); ++i) {
        jstring name = jni->NewStringUTF(perms.at(i).toUtf8().constData());
        jni->SetObjectArrayElement(permissions, i, name);
        jni->DeleteLocalRef(name);
    }
    jni->DeleteLocalRef(stringClass);

    jclass activityClass = jni->FindClass("android/app/Activity");
    if (activityClass) {
        jmethodID request = jni->GetMethodID(activityClass, "requestPermissions",
                                             "([Ljava/lang/String;I)V");
        if (request)
            jni->CallVoidMethod(activity.object(), request, permissions, jint(0x4652));
        jni->DeleteLocalRef(activityClass);
    }
    jni->DeleteLocalRef(permissions);
    if (jni->ExceptionCheck())
        jni->ExceptionClear();
}

// 前台播放时保持屏幕常亮：用官方推荐的 WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON，
// 无需 WAKE_LOCK 权限，系统会在窗口销毁时自动清理。关闭开关或暂停播放时清掉 flag。
// playing 由调用方传入，是否启用读设置（keepScreenOn，默认开）。
//
// 注意：Qt 的事件循环跑在 qtMainLoopThread，不是 Android 的 UI 线程；Window.addFlags()
// 会触发 view layout 更新，必须从 Android UI 线程调用，否则抛 CalledFromWrongThreadException
// 且 flag 不生效。这里用 runOnAndroidMainThread 把 JNI 操作切到 UI 线程。
void applyKeepScreenOn(bool playing)
{
    const bool enabled = playing
                         && Settings::boolean(QStringLiteral("keepScreenOn"), true);
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([enabled]() {
        QJniObject activity = QJniObject::callStaticObjectMethod(
            "org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
        if (!activity.isValid() || !activity.object())
            return;
        QJniObject window = activity.callObjectMethod("getWindow", "()Landroid/view/Window;");
        if (!window.isValid() || !window.object())
            return;
        // WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON = 0x00000080
        if (enabled)
            window.callMethod<void>("addFlags", "(I)V", jint(0x80));
        else
            window.callMethod<void>("clearFlags", "(I)V", jint(0x80));
    });
}

// Java 侧静态 native 方法：通知/锁屏/耳机/车机按键。
//
// 这里刻意【同步直接调用】handleCommand，不走 QueuedConnection 投递到 Qt 事件循环。
// 原因：锁屏息屏时 Qt 主线程事件循环会被系统挂起数秒（实测收到 onPause/onPlay 后
// 要 5 秒才被处理），用户点暂停/播放会觉得「毫无反应」。媒体按键必须即时响应。
// QMediaPlayer 的 play/pause/seek 在 Android 底层是线程安全的 Binder 调用；
// handleCommand 内发出的 Qt 信号由 AutoConnection 自动排队到接收者线程，不会
// 在被占用的线程里直接执行槽函数。命令本身用 g_commandMutex 串行化，避免
// 连点时多线程同时操作播放器。
// 音频焦点：开始播放时向系统申请独占焦点（USAGE_MEDIA / AUDIOFOCUS_GAIN）。
// 其它应用随后开始播放（视频、导航、来电、通知音）时，Java 侧监听器按焦点变化
// 回传命令：LOSS/LOSS_TRANSIENT → 暂停；LOSS_TRANSIENT_CAN_DUCK → 压低音量继续播
//（地图导航只压低不暂停的主流行为）；焦点回到本应用时恢复/解除压低。
void acquireAudioFocus()
{
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid())
        return;
    QJniObject::callStaticMethod<void>(kServiceClass, "acquireFocus",
        "(Landroid/content/Context;)V", context.object());
}

void releaseAudioFocus()
{
    QJniObject::callStaticMethod<void>(kServiceClass, "releaseFocus", "()V");
}

void jniCommand(JNIEnv *, jclass, jint action, jlong position)
{
    if (!g_instance)
        return;
    QMutexLocker lock(&g_commandMutex);
    AndroidMediaSession::handleCommand(int(action), qint64(position));
}

// 车机语音点歌：MediaSessionService.dispatchVoiceSearch() 回传歌名/歌手。
// 与 jniCommand 一样跑在 Android 主线程，真正的工作排到 Qt 主线程做。
static QString jniString(JNIEnv *env, jstring s)
{
    if (!s) return QString();
    const jchar *chars = env->GetStringChars(s, nullptr);
    if (!chars) return QString();
    const jsize len = env->GetStringLength(s);
    const QString out = QString::fromUtf16(reinterpret_cast<const char16_t *>(chars), len);
    env->ReleaseStringChars(s, chars);
    return out;
}

void jniSearch(JNIEnv *env, jclass, jstring keyword)
{
    if (!g_instance) return;
    AndroidMediaSession::handleVoiceSearch(jniString(env, keyword));
}

void jniSearchEx(JNIEnv *env, jclass, jstring keyword, jstring artist)
{
    if (!g_instance) return;
    AndroidMediaSession::handleVoiceSearch(jniString(env, keyword), jniString(env, artist));
}

} // namespace

void AndroidMediaSession::install()
{
    if (g_instance)
        return;

    QJniEnvironment env;
    if (env.jniEnv()) {
        // 注册 native 必须早于 Java 服务里的任何回调；install() 在 QML 加载前执行，
        // 而服务只会在首次播放时启动，顺序天然满足。
        jclass clazz = env.jniEnv()->FindClass(kServiceClass);
        if (clazz) {
            static const JNINativeMethod methods[] = {
                { const_cast<char *>("onNativeCommand"), const_cast<char *>("(IJ)V"),
                  reinterpret_cast<void *>(&jniCommand) },
                // 车机语音点歌（主流版无人调用，注册无害）
                { const_cast<char *>("onNativeSearch"), const_cast<char *>("(Ljava/lang/String;)V"),
                  reinterpret_cast<void *>(&jniSearch) },
                { const_cast<char *>("onNativeSearchEx"),
                  const_cast<char *>("(Ljava/lang/String;Ljava/lang/String;)V"),
                  reinterpret_cast<void *>(&jniSearchEx) }
            };
            env.jniEnv()->RegisterNatives(clazz, methods, 3);
            env.jniEnv()->DeleteLocalRef(clazz);
        }
    }

    g_instance = new AndroidMediaSession(QCoreApplication::instance());

    // 冷启动时 PlayerController 可能已经恢复了上次的歌曲，那次 mediaChanged 发生在
    // install() 连接信号之前，会错过；这里补推一次，避免通知/锁屏显示空标题。
    PlayerController *player = PlayerController::instance();
    if (!player->current().title.isEmpty()) {
        g_instance->syncCover(player->current());
        g_instance->pushMetadata();
        g_instance->pushPlayback();
        if (player->playing()) {
            g_instance->ensureService();
            acquireAudioFocus();
        }
    }
    // 冷启动就常在播放（续播）：补一次常亮状态，避免要等下一次 stateChanged
    applyKeepScreenOn(player->playing());
}

AndroidMediaSession::AndroidMediaSession(QObject *parent) : QObject(parent)
{
    PlayerController *player = PlayerController::instance();

    // 换歌：元数据 + 播放态一起推，服务尚未启动时顺手拉起
    connect(player, &PlayerController::mediaChanged, this, [this] {
        syncCover(PlayerController::instance()->current());
        pushMetadata();
        pushPlayback();
        if (!PlayerController::instance()->current().title.isEmpty())
            ensureService();
    });

    // 播放/暂停/播完：刷新播放态、同步屏幕常亮；开始播放时确保服务已在前台，
    // 并向系统申请音频焦点（别的应用开始播放时本 App 会收到暂停/压低回调）
    connect(player, &PlayerController::stateChanged, this, [this] {
        const bool playing = PlayerController::instance()->playing();
        pushPlayback();
        applyKeepScreenOn(playing);
        if (playing) {
            ensureService();
            acquireAudioFocus();
        }
    });

    // 设置页切换「播放时常亮」后立即生效（不必等下一次播放/暂停）
    connect(AppController::instance(), &AppController::settingsChanged, this, [this] {
        applyKeepScreenOn(PlayerController::instance()->playing());
    });
}

void AndroidMediaSession::syncCover(const Song &song)
{
    const QString cover = song.cover;
    if (cover == m_coverRemote)
        return;
    m_coverRemote = cover;
    m_coverPath.clear();
    if (cover.isEmpty())
        return;

    // 已经是本地文件（下载目录里的内嵌封面/用户自选封面）就直接交给系统
    if (!cover.startsWith(QLatin1String("http"))) {
        m_coverPath = cover.startsWith(QLatin1String("file:")) ? QUrl(cover).toLocalFile() : cover;
        return;
    }

    // 网络封面：下载到 cache 目录，文件名取 URL 摘要，避免同名冲突与非法字符。
    // Java 侧只解码本地文件，不做网络请求。
    const QString dir = Settings::filePath(QStringLiteral("cache"));
    QDir().mkpath(dir);
    const QString digest = QString::fromLatin1(
        QCryptographicHash::hash(cover.toUtf8(), QCryptographicHash::Md5).toHex());
    const QString local = dir + QStringLiteral("/cover_") + digest + QStringLiteral(".img");
    if (QFileInfo(local).size() > 0) {
        m_coverPath = local;
        return;
    }

    const QString wanted = cover;
    // 封面 CDN 部分会校验 Referer，带常规 UA 通常即可；失败就保持无封面，不影响播放
    HttpClient::instance()->download(
        QUrl(cover),
        QVariantMap { { QStringLiteral("User-Agent"), QStringLiteral("Mozilla/5.0") } }, local,
        [this, wanted, local](bool ok, const QString &) {
            if (!ok || m_coverRemote != wanted)
                return; // 下载期间已切歌，丢弃这次结果
            m_coverPath = local;
            pushMetadata(); // 重新推元数据，Java 侧解码后刷新通知/锁屏封面
        });
}

void AndroidMediaSession::pushMetadata()
{
    const Song song = PlayerController::instance()->current();
    const QJniObject title = QJniObject::fromString(song.title);
    const QJniObject artist = QJniObject::fromString(song.artist);
    const QJniObject album = QJniObject::fromString(song.album);
    const QJniObject cover = QJniObject::fromString(m_coverPath);

    QJniObject::callStaticMethod<void>(
        kServiceClass, "updateMetadata",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;J)V",
        title.object<jstring>(), artist.object<jstring>(), album.object<jstring>(),
        cover.object<jstring>(), jlong(song.duration));
}

void AndroidMediaSession::pushPlayback()
{
    PlayerController *player = PlayerController::instance();
    QJniObject::callStaticMethod<void>(kServiceClass, "updatePlayback", "(ZJ)V",
                                       jboolean(player->playing()), jlong(player->position()));
}

void AndroidMediaSession::ensureService()
{
    if (g_serviceStarted)
        return;
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid())
        return;
    QJniObject::callStaticMethod<void>(kServiceClass, "start", "(Landroid/content/Context;)V",
                                       context.object());
    g_serviceStarted = true;
    // 首次真正开始播放时才申请通知权限：此时用户刚点了播放，能理解弹窗用途
    requestNotificationPermission();
}

void AndroidMediaSession::handleCommand(int action, qint64 position)
{
    PlayerController *player = PlayerController::instance();
    switch (action) {
    case CmdPlay:
        if (!player->playing())
            player->togglePause();
        break;
    case CmdPause:
        if (player->playing())
            player->togglePause();
        // 焦点被其它应用抢走（或通知栏统一暂停）时，MV 沉浸页一起停
        emit AppController::instance()->mvPauseRequested();
        break;
    case CmdDuck:
        // 导航播报等 CAN_DUCK 场景：只把音量压到 30%，不暂停。
        // setDucked 会调 QAudioOutput::setVolume，跨线程无保障 → 排队到主线程执行
        QMetaObject::invokeMethod(player, [player] { player->setDucked(true); },
                                  Qt::QueuedConnection);
        break;
    case CmdUnduck:
        QMetaObject::invokeMethod(player, [player] { player->setDucked(false); },
                                  Qt::QueuedConnection);
        break;
    case CmdNext:
        player->next();
        break;
    case CmdPrev:
        player->previous();
        break;
    case CmdSeek:
        player->seek(position);
        break;
    case CmdStop:
        player->stop();
        g_serviceStarted = false;
        // 与 CmdPause 一样：MV 沉浸页有自己的播放器，停止命令必须一起停，
        // 否则「小迪停止播放」后音乐停了、MV 还在响。
        emit AppController::instance()->mvPauseRequested();
        // 主动清空队列/停止时把焦点还给系统
        releaseAudioFocus();
        break;
    default:
        handleVoiceListCommand(action);
        break;
    }
}

// 车机歌单/模式类口令：loadFavorites/loadHistory 同步落模型（m_results），
// 之后 playResult(0) 等价「从第一条开始播，整队列替换为该列表」。
// 这些命令都来自 JNI 线程，会触碰 UI 模型/数据库 → 统一排队到 Qt 主线程。
void AndroidMediaSession::handleVoiceListCommand(int action)
{
    AppController *app = AppController::instance();
    QMetaObject::invokeMethod(app, [app, action] {
        switch (action) {
        case CmdPlayLike:
            Logger::write(QStringLiteral("I"), QStringLiteral("[语音点歌] 播放我喜欢的歌"));
            app->loadFavorites();
            app->playResult(0);
            break;
        case CmdPlayLately:
            Logger::write(QStringLiteral("I"), QStringLiteral("[语音点歌] 播放最近在听"));
            app->loadHistory();
            app->playResult(0);
            break;
        case CmdRandom:
        case CmdModeRandom:
            PlayerController::instance()->setMode(3);
            break;
        case CmdModeOrder:
            PlayerController::instance()->setMode(0);
            break;
        case CmdModeCycle:
            PlayerController::instance()->setMode(1);
            break;
        case CmdModeLoop:
            PlayerController::instance()->setMode(2);
            break;
        case CmdCollect:
            // 小迪只会发一个收藏口令，是否已收藏由这里判：未收藏才切，
            // 避免重复口令把已收藏的歌又取消掉
            if (!app->isFavoriteCurrent())
                app->toggleFavoriteCurrent();
            break;
        case CmdUncollect:
            if (app->isFavoriteCurrent())
                app->toggleFavoriteCurrent();
            break;
        default:
            break;
        }
    }, Qt::QueuedConnection);
}

void AndroidMediaSession::handleVoiceSearch(const QString &keyword, const QString &artist)
{
    const QString kw = keyword.trimmed();
    const QString ar = artist.trimmed();
    if (kw.isEmpty() && ar.isEmpty())
        return;

    // JNI 回调来自 Android 主线程：search/playResult 会操作 UI 模型，必须排到 Qt 主线程。
    QMetaObject::invokeMethod(AppController::instance(), [kw, ar] {
        AppController *app = AppController::instance();
        auto ctx = std::make_shared<VoiceSearchCtx>();
        if (!kw.isEmpty()) ctx->queries.append(kw);
        if (!ar.isEmpty() && !kw.isEmpty()) ctx->queries.append(ar + QLatin1Char(' ') + kw);
        // 小迪常把整句「周杰伦的晴天」塞进关键词；去掉「的」再搜一次，命中率更高
        if (kw.contains(QStringLiteral("的"))) {
            QString alt = kw;
            alt.replace(QStringLiteral("的"), QStringLiteral(" "));
            alt = alt.simplified();
            if (!alt.isEmpty() && !ctx->queries.contains(alt)) ctx->queries.append(alt);
        }
        if (!ar.isEmpty() && kw.isEmpty()) ctx->queries.append(ar);
        ctx->timer = new QTimer(app);
        ctx->timer->setSingleShot(true);

        const auto finish = [ctx] {
            if (ctx->done) return;
            ctx->done = true;
            QObject::disconnect(ctx->conn);
            ctx->timer->stop();
            ctx->timer->deleteLater();
        };

        auto tryNext = std::make_shared<std::function<void()>>();
        *tryNext = [ctx, app, finish, tryNext, ar] {
            if (ctx->done) return;
            QObject::disconnect(ctx->conn);
            ctx->timer->stop();
            ++ctx->index;
            if (ctx->index >= ctx->queries.size()) { finish(); return; }

            const QString q = ctx->queries.at(ctx->index);
            Logger::write(QStringLiteral("I"),
                          QStringLiteral("[语音点歌] 搜索尝试 %1/%2：%3")
                              .arg(ctx->index + 1).arg(ctx->queries.size()).arg(q));
            app->setSearchType(QStringLiteral("song"));
            ctx->conn = QObject::connect(app, &AppController::searchFinished, app,
                [ctx, app, finish, tryNext, ar](int count, const QString &kind) {
                    if (ctx->done) return;
                    if (count < 0) { finish(); return; }        // 搜索被取消
                    if (kind != QLatin1String("song")) return;  // 等歌曲搜索的结果
                    if (count <= 0) { (*tryNext)(); return; }   // 这一轮没结果，换下一种拼法

                    // 有歌手信息时优先挑「歌手匹配」的行，而不是无脑播第一条
                    int row = 0;
                    if (!ar.isEmpty()) {
                        const QVector<Song> songs = app->results()->songs();
                        const QString needle = normalize(ar);
                        for (int i = 0; i < songs.size(); ++i) {
                            if (songs.at(i).normalizedKey().contains(needle)) { row = i; break; }
                        }
                    }
                    row = qBound(0, row, app->results()->rowCount() - 1);
                    Logger::write(QStringLiteral("I"),
                                  QStringLiteral("[语音点歌] 命中 %1 条，播放第 %2 行")
                                      .arg(count).arg(row));
                    app->playResult(row);
                    finish();
                });
            ctx->timer->start(12000);   // 单轮 12s 超时，避免语音点歌永久挂住
            app->search(q);
        };
        // 超时只接一次：放在循环外，避免每轮重试都叠一个连接而跳过候选查询
        QObject::connect(ctx->timer, &QTimer::timeout, app, [tryNext] { (*tryNext)(); });
        (*tryNext)();
    }, Qt::QueuedConnection);
}
#else
void AndroidMediaSession::install() {}
void AndroidMediaSession::handleCommand(int, qint64) {}
void AndroidMediaSession::handleVoiceSearch(const QString &, const QString &) {}
void AndroidMediaSession::handleVoiceListCommand(int) {}
#endif

} // namespace Furui
