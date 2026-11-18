#include "AndroidMediaSession.h"

#include "../core/HttpClient.h"
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
#include <QUrl>
#include <jni.h>
#endif

namespace Furui {

#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
namespace {

constexpr const char *kServiceClass = "cn/furui/music/MediaSessionService";

// 与 MediaSessionService.java 中的 CMD_* 一一对应
enum Command {
    CmdPlay = 1, CmdPause = 2, CmdNext = 3, CmdPrev = 4, CmdSeek = 5, CmdStop = 6,
    // 音频焦点回调：其它应用开始播放时由 Java 监听器回传
    CmdDuck = 7, CmdUnduck = 8
};

AndroidMediaSession *g_instance = nullptr;
bool g_serviceStarted = false;
bool g_permissionRequested = false;
QMutex g_commandMutex;

// Android 13+ 的 POST_NOTIFICATIONS 是运行时权限，Qt 6.11 的 QPermission
// 没有对应的封装类，只能直接向 Activity 申请。未授予时前台服务照常运行
// （锁屏/车机控制来自 MediaSession），仅通知栏不显示条目。
void requestNotificationPermission()
{
    if (g_permissionRequested)
        return;
    g_permissionRequested = true;
    if (QOperatingSystemVersion::current() < QOperatingSystemVersion::Android13)
        return;

    QJniEnvironment env;
    JNIEnv *jni = env.jniEnv();
    if (!jni)
        return;
    // QtNative.activity() 在 Java 侧不是 public，但 JNI 按名称/签名查找不受可见性限制
    QJniObject activity = QJniObject::callStaticObjectMethod(
        "org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
    if (!activity.isValid() || !activity.object())
        return;

    jclass stringClass = jni->FindClass("java/lang/String");
    if (!stringClass)
        return;
    jobjectArray permissions = jni->NewObjectArray(1, stringClass, nullptr);
    jstring name = jni->NewStringUTF("android.permission.POST_NOTIFICATIONS");
    jni->SetObjectArrayElement(permissions, 0, name);
    jni->DeleteLocalRef(name);
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
                  reinterpret_cast<void *>(&jniCommand) }
            };
            env.jniEnv()->RegisterNatives(clazz, methods, 1);
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
        // 主动清空队列/停止时把焦点还给系统
        releaseAudioFocus();
        break;
    default:
        break;
    }
}
#else
void AndroidMediaSession::install() {}
void AndroidMediaSession::handleCommand(int, qint64) {}
#endif

} // namespace Furui
