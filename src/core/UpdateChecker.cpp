// 福瑞音乐 - 在线检测更新（参照洛雪音乐 version.json 机制）
// 清单的唯一事实来源是 src/version.json；src/publish/version.json 只是为已发布的老客户端
// 保留的兼容副本，由 CI 校验两者内容一致（见 .github/workflows/release.yml）。
// 注意：CDN（jsDelivr）对 main 分支的缓存滞后可达数小时，甚至各节点版本不一致，
// 所以这里先用 GitHub API 拿最新 tag，再按 tag 取清单，并取所有镜像里最大的版本号。
#include "UpdateChecker.h"

#include "AppController.h"
#include "HttpClient.h"
#include "Logger.h"
#include "Version.h"

#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>

#include <atomic>
#include <climits>
#include <algorithm>

#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QJniObject>
#include <QJniEnvironment>
// QNativeInterface::QAndroidApplication 的真实头文件；QNativeInterface/QAndroidApplication
// 这个前向路径 Qt 6.9+ 才有，6.8.3 的 android kit 里没有（CI 实测 fatal error）
#include <QtCore/qcoreapplication_platform.h>
#endif

namespace Furui {

struct UpdateVerification {
    QString path;
    QByteArray expectedHash;
    qint64 expectedSize = 0;
    std::atomic<bool> cancelled{false};
};

namespace {

class PackageVerifier : public QObject {
    Q_OBJECT
public:
    explicit PackageVerifier(const std::shared_ptr<UpdateVerification> &state)
        : m_state(state) {}

    void run()
    {
        QString error;
        QFile file(m_state->path);
        if (!file.open(QIODevice::ReadOnly)) {
            error = QStringLiteral("无法读取更新包进行 SHA-256 校验");
        } else {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            QByteArray buffer;
            buffer.reserve(1024 * 1024);
            while (!m_state->cancelled.load()) {
                const QByteArray chunk = file.read(1024 * 1024);
                if (chunk.isEmpty())
                    break;
                hash.addData(chunk);
            }
            if (m_state->cancelled.load()) {
                error = QStringLiteral("更新包校验已取消");
            } else if (file.error() != QFileDevice::NoError) {
                error = QStringLiteral("读取更新包失败：%1").arg(file.errorString());
            } else if (file.size() != m_state->expectedSize) {
                error = QStringLiteral("更新包大小与官方发布记录不一致");
            } else if (hash.result().toHex().compare(m_state->expectedHash, Qt::CaseInsensitive) != 0) {
                error = QStringLiteral("更新包 SHA-256 校验失败，可能已损坏或被篡改");
            }
        }
        emit finished(error);
        QThread::currentThread()->quit();
    }

signals:
    void finished(const QString &error);

private:
    std::shared_ptr<UpdateVerification> m_state;
};

QString normalizedSha256(const QString &digest)
{
    static const QRegularExpression valid(QStringLiteral("\\Asha256:([0-9a-fA-F]{64})\\z"));
    const QRegularExpressionMatch match = valid.match(digest.trimmed());
    return match.hasMatch() ? match.captured(1).toLower() : QString();
}

// 清单拉取的两级来源见 probeLatestRelease()：这一级是 GitHub API，不走任何 CDN
QString latestReleaseApiUrl()
{
    return QStringLiteral("https://api.github.com/repos/spygg/furuimusic/releases/latest");
}

// 某个 ref（不可变 tag 或 main）下的清单地址。
// jsDelivr 对 @main 这类可变分支的缓存各节点进度不一致（实测同一时刻分别返回
// 1.4.13 / 1.4.12 / 1.4.9），所以拿到 tag 就用 tag 取清单：不可变 ref 不会串到旧缓存。
QStringList manifestUrls(const QString &ref)
{
    // src/publish/version.json 是迁移后的兼容副本；末尾的 publish/version.json 保留给
    // 更早 tag（当时副本还在仓库根）的老客户端，找不到会自然跳过。
    static const char *const paths[] = {"src/version.json", "src/publish/version.json",
                                        "publish/version.json"};
    QStringList urls;
    for (const char *path : paths) {
        const QString p = QString::fromLatin1(path);
        const QString raw = QStringLiteral("https://raw.githubusercontent.com/spygg/furuimusic/%1/").arg(ref) + p;
        urls << raw
             << QStringLiteral("https://ghfast.top/") + raw
             << QStringLiteral("https://cdn.jsdelivr.net/gh/spygg/furuimusic@%1/").arg(ref) + p
             << QStringLiteral("https://fastly.jsdelivr.net/gh/spygg/furuimusic@%1/").arg(ref) + p
             << QStringLiteral("https://gcore.jsdelivr.net/gh/spygg/furuimusic@%1/").arg(ref) + p;
    }
    return urls;
}

// 单条清单最多等 8s：10 个地址是并行发的，整体上限也就是这一个超时
constexpr int kManifestTimeoutMs = 8000;

// CI（.github/workflows/release.yml）上传到 Release 的产物命名，下载直链按它拼。
// 车机兼容版（FURUI_CAR_COMPAT_EDITION，CMake 在 FURUI_ANDROID_CAR_COMPAT=ON 时
// 注入）必须下特供包：包名是网易云车机版 com.netease.cloudmusic.iot，主流版
// APK 装不上（装上也会变成第二个应用），绝不能把主流版推给它。
QString androidAssetName()
{
#ifdef FURUI_CAR_COMPAT_EDITION
    return QStringLiteral("FuruiMusic-carcompat-netease-arm64-v8a.apk");
#else
    return QStringLiteral("FuruiMusic-android-arm64-v8a.apk");
#endif
}

QString releaseAssetFor(const QString &platformKey)
{
    if (platformKey == QLatin1String("windows"))
        return QStringLiteral("FuruiMusic-windows-x64.zip");
    if (platformKey == QLatin1String("android"))
        return androidAssetName();
    if (platformKey == QLatin1String("macos"))
        return QStringLiteral("FuruiMusic-macos-x86_64.zip");
    return QStringLiteral("FuruiMusic-linux-x86_64.AppImage");
}

QString platformKey()
{
#ifdef Q_OS_ANDROID
    return QStringLiteral("android");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS) || defined(Q_OS_MAC)
    return QStringLiteral("macos");
#else
    return QStringLiteral("linux");
#endif
}

// GitHub Releases 在国内直连经常慢/超时/打不开。这里维护一组公开可用的
// 反向代理前缀，下载前用 Range 探测挑选当前网络下最快且返回真 APK 的一个。
// 约定用法是「前缀 + 完整原始 github.com URL」（不是替换 github.com 主机）。
// 2026-11-20 实测可用：gh-proxy.com / gh-proxy.org / ghproxy.net / ghfast.top /
// githubproxy.cc；ghproxy.cn / ghproxylist.com 返回 HTML 错误页（探测时靠 PK
// 文件头挡掉），故未收录。gh.llkk.cc / mirror.ghproxy.com 在部分网络下被代理
// 拒绝但在别处可用，保留为候选。探测是运行时做的，某条失效不影响其它条，
// 也不写死顺序。
QStringList githubProxyPrefixes()
{
    return {
        QStringLiteral("https://gh-proxy.com/"),
        QStringLiteral("https://gh-proxy.org/"),
        QStringLiteral("https://ghproxy.net/"),
        QStringLiteral("https://ghfast.top/"),
        QStringLiteral("https://githubproxy.cc/"),
        QStringLiteral("https://gh.llkk.cc/"),
        QStringLiteral("https://mirror.ghproxy.com/"),
        QStringLiteral("https://github.moeyy.xyz/"),
        QStringLiteral("https://ghp.ci/"),
    };
}

// 点分版本号比较：<0 a 更旧，0 相同，>0 a 更新
int compareVersion(const QString &a, const QString &b)
{
    const QStringList pa = a.split(QLatin1Char('.'));
    const QStringList pb = b.split(QLatin1Char('.'));
    const int n = qMax(pa.size(), pb.size());
    for (int i = 0; i < n; ++i) {
        const int va = i < pa.size() ? pa.at(i).toInt() : 0;
        const int vb = i < pb.size() ? pb.at(i).toInt() : 0;
        if (va != vb)
            return va < vb ? -1 : 1;
    }
    return 0;
}

bool isValidVersion(const QString &version)
{
    static const QRegularExpression valid(QStringLiteral("\\A[0-9]+(\\.[0-9]+)*\\z"));
    return valid.match(version).hasMatch();
}

#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
// 拉起安装器之前先判断这个 APK 到底装不装得上。国产 ROM 对
// INSTALL_FAILED_ALREADY_EXISTS / INSTALL_FAILED_UPDATE_INCOMPATIBLE 的提示语
// 都是「同签名应用已安装 / 应用已安装」，用户完全看不出真正原因，所以这里
// 把两种根因分开拦下并给出可操作的提示：
//   0 可安装；1 与已装应用签名不同；2 versionCode 不高于已装版本；-1 判断不了（放行）
// 签名检查用 PackageInfo.signatures[0].toCharsString()，V1/V2/V3 都体现在这里。
int androidApkInstallability(const QString &path)
{
    QJniEnvironment env;
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) return -1;
    QJniObject pm = context.callObjectMethod("getPackageManager",
                                            "()Landroid/content/pm/PackageManager;");
    if (!pm.isValid()) return -1;
    const QJniObject jPath = QJniObject::fromString(path);
    // 当前包名必须运行时取：车机特供版的 applicationId 是 com.netease.cloudmusic.iot，
    // 写死 cn.furui.music 会让 getPackageInfo 抛 NameNotFoundException，版本/签名
    // 两项检查全部退化成「判断不了」，最终仍拉起安装器并给出含糊的失败提示。
    const QJniObject jPkg = context.callObjectMethod("getPackageName",
                                                     "()Ljava/lang/String;");
    const jint flags = 64; // PackageManager.GET_SIGNATURES
    QJniObject archive = pm.callObjectMethod(
        "getPackageArchiveInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;",
        jPath.object<jstring>(), flags);
    QJniObject current = pm.callObjectMethod(
        "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;",
        jPkg.object<jstring>(), flags);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return -1; }
    if (!archive.isValid() || !current.isValid()) return -1;

    const jint apkCode = archive.getField<jint>("versionCode");
    const jint curCode = current.getField<jint>("versionCode");
    // 相同 versionCode 装上去只会得到「应用已安装」；旧版本更是会被系统直接拒绝
    if (apkCode > 0 && apkCode <= curCode) return 2;

    const QJniObject apkSigs = archive.getObjectField(
        "signatures", "[Landroid/content/pm/Signature;");
    const QJniObject curSigs = current.getObjectField(
        "signatures", "[Landroid/content/pm/Signature;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return -1; }
    if (!apkSigs.isValid() || !curSigs.isValid()) return -1;
    jobjectArray a = static_cast<jobjectArray>(apkSigs.object<jobject>());
    jobjectArray c = static_cast<jobjectArray>(curSigs.object<jobject>());
    if (!a || !c || env->GetArrayLength(a) == 0 || env->GetArrayLength(c) == 0) return -1;
    jobject sa = env->GetObjectArrayElement(a, 0);
    jobject sc = env->GetObjectArrayElement(c, 0);
    QJniObject sigA(sa), sigC(sc);
    const QString hexA = sigA.callObjectMethod("toCharsString", "()Ljava/lang/String;").toString();
    const QString hexC = sigC.callObjectMethod("toCharsString", "()Ljava/lang/String;").toString();
    env->DeleteLocalRef(sa);
    env->DeleteLocalRef(sc);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return -1; }
    if (hexA.isEmpty() || hexC.isEmpty()) return -1;
    return hexA.compare(hexC, Qt::CaseInsensitive) == 0 ? 0 : 1;
}

// Android 7+ 不允许 file:// 直接跨应用共享，必须经 FileProvider 转 content:// URI。
// 工程的 AndroidManifest 已声明 androidx FileProvider（authority 见下），
// paths 覆盖 files-path 根目录，APK 放在 AppDataLocation 下即可共享。
bool androidInstallApk(const QString &path)
{
    QJniObject jpath = QJniObject::fromString(path);
    QJniObject file("java/io/File", "(Ljava/lang/String;)V", jpath.object());
    if (!file.isValid())
        return false;
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid())
        return false;
    // manifest 里 authority = ${applicationId}.qtprovider。车机特供版 applicationId 是
    // com.netease.cloudmusic.iot，写死 cn.furui.music.qtprovider 会让 getUriForFile
    // 找不到 provider 抛异常 → 更新包装不上。按运行时包名拼，主流/特供版都正确。
    const QJniObject pkgObj = context.callObjectMethod("getPackageName", "()Ljava/lang/String;");
    const QString pkg = pkgObj.toString();
    const QString authority = (pkg.isEmpty() ? QStringLiteral("cn.furui.music") : pkg)
                              + QStringLiteral(".qtprovider");
    QJniObject uri = QJniObject::callStaticObjectMethod(
        "androidx/core/content/FileProvider", "getUriForFile",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/io/File;)Landroid/net/Uri;",
        context.object(),
        QJniObject::fromString(authority).object(),
        file.object());
    if (!uri.isValid())
        return false;
    QJniObject intent("android/content/Intent", "()V");
    if (!intent.isValid())
        return false;
    intent.callObjectMethod("setAction", "(Ljava/lang/String;)Landroid/content/Intent;",
                            QJniObject::fromString(QStringLiteral("android.intent.action.VIEW")).object());
    intent.callObjectMethod("setDataAndType", "(Landroid/net/Uri;Ljava/lang/String;)Landroid/content/Intent;",
                            uri.object(),
                            QJniObject::fromString(QStringLiteral("application/vnd.android.package-archive")).object());
    // FLAG_GRANT_READ_URI_PERMISSION：允许安装器临时读这个 URI
    intent.callObjectMethod("addFlags", "(I)Landroid/content/Intent;", jint(0x00000001));
    // FLAG_ACTIVITY_NEW_TASK：从非 Activity 上下文启动必须加。
    // 之前这里误把一串版本号样式的数字当 flag 传（0x10000000），里面混进了
    // 一堆无关的 Activity 标志，部分 ROM 的安装器会因此走异常分支。
    intent.callObjectMethod("addFlags", "(I)Landroid/content/Intent;", jint(0x10000000));
    intent.callObjectMethod("putExtra", "(Ljava/lang/String;Z)Landroid/content/Intent;",
                            QJniObject::fromString(
                                QStringLiteral("android.intent.extra.NOT_UNKNOWN_SOURCE")).object<jstring>(),
                            jboolean(true));
    context.callMethod<void>("startActivity", "(Landroid/content/Intent;)V", intent.object());
    return true;
}

// 把私有目录里的 APK 复制到公共 Download（MediaStore），供车机系统文件管理器手动安装。
// 车机上 startActivity 可能「假成功」——不弹安装界面却返回 true，所以调用方在车机版
// 下载完成后无条件复制一份。返回展示名，空串失败。
QString androidCopyToDownloads(const QString &path, const QString &displayName)
{
    const QJniObject activity = QJniObject::callStaticObjectMethod(
        "org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
    if (!activity.isValid()) return QString();
    const QJniObject result = QJniObject::callStaticObjectMethod(
        "cn/furui/music/UpdateInstaller", "copyToDownloads",
        "(Landroid/app/Activity;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
        activity.object(),
        QJniObject::fromString(path).object<jstring>(),
        QJniObject::fromString(displayName).object<jstring>());
    return result.toString();
}
#endif

} // namespace

// 一轮检查的并行拉取状态：pending 归零时用 best 出结论
struct UpdateChecker::FetchState {
    int pending = 0;
    QString apiVersion;     // API 给出的 tag（仅 API 路径有值）
    QJsonObject release;    // GitHub 官方 release 元数据，作为校验值信任根
    QString bestVersion;    // 已拿到的最高版本
    QJsonObject best;       // 与 bestVersion 对应的清单
};

// 下载前并行探测镜像的共享状态：pending 归零时挑最快且返回真 APK 的地址。
// valid=true 表示 HTTP 2xx 且响应体开头是 ZIP 魔数 "PK"（ghproxy.cn 之类
// 返回 HTML 错误页的会被挡掉）；elapsed 是探测耗时，越小越快。
struct UpdateChecker::DownloadProbeState {
    int pending = 0;
    struct Candidate {
        QString url;
        bool valid = false;
        qint64 elapsedMs = 0;
    };
    QList<Candidate> results;
};

UpdateChecker *UpdateChecker::instance()
{
    static UpdateChecker inst;
    return &inst;
}

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent)
{
}

UpdateChecker::~UpdateChecker()
{
    ++m_downloadGen;
    if (m_downloadReply)
        m_downloadReply->abort();
    cancelVerification(true);
}

QString UpdateChecker::localVersion() const
{
    return QStringLiteral(FURUI_APP_VERSION);
}

bool UpdateChecker::hasUpdate() const
{
    return !m_remoteVersion.isEmpty()
           && compareVersion(localVersion(), m_remoteVersion) < 0;
}

void UpdateChecker::checkForUpdates()
{
    if (m_checking || m_downloading)
        return;
    ++m_downloadGen;
    cancelVerification(true);
    m_checking = true;
    m_progress = 0;
    m_lastResult.clear();
    // 新一轮检测必须先作废上一轮的更新状态：否则这次网络失败后 hasUpdate() 仍为 true
    m_remoteVersion.clear();
    m_changelog.clear();
    m_downloads.clear();
    m_history.clear();
    m_downloadedPath.clear();
    resetTrustedRelease();
    emit stateChanged();
    probeLatestRelease();
}

void UpdateChecker::resetTrustedRelease()
{
    m_releaseTag.clear();
    m_packageAsset.clear();
    m_packageUrl.clear();
    m_packageSha256.clear();
    m_packageSize = 0;
    m_releaseMetadataTrusted = false;
}

void UpdateChecker::probeLatestRelease()
{
    // 第一级：GitHub API 的 /releases/latest 不走 CDN，tag 就是当前最新发布版本
    QVariantMap headers;
    headers.insert(QStringLiteral("User-Agent"), QStringLiteral("FuruiMusic"));
    headers.insert(QStringLiteral("Accept"), QStringLiteral("application/vnd.github+json"));
    HttpClient::instance()->request(QStringLiteral("GET"), QUrl(latestReleaseApiUrl()), headers,
                                    QByteArray(),
                                    [this](const HttpClient::Reply &r) {
        QString tag;
        QJsonObject release;
        QString ref;
        if (r.ok) {
            release = r.json.object();
            ref = release.value(QLatin1String("tag_name")).toString().trimmed();
            tag = ref;
            if (tag.startsWith(QLatin1Char('v')))
                tag.remove(0, 1);
            if (isValidVersion(tag)) {
                fetchManifests(ref, tag, release);
                return;
            }
        }
        // API 限流或不可达：退回 main 分支多镜像；由 finishCheck 取其中最大版本。
        // 这条链路只能提示更新，下载仍要求官方 release 的资产 digest，不能信任镜像自报哈希。
        fetchManifests(QStringLiteral("main"), QString(), QJsonObject());
    }, kManifestTimeoutMs);
}

void UpdateChecker::fetchManifests(const QString &ref, const QString &apiVersion,
                                   const QJsonObject &release)
{
    const QStringList urls = manifestUrls(ref);
    auto state = std::make_shared<FetchState>();
    state->pending = urls.size();
    state->apiVersion = apiVersion;
    state->release = release;
    for (const QString &url : urls) {
        // 有官方 tag 时只接受同一不可变版本的清单，避免缓存或恶意镜像报出更高版本。
        // API 不可达时才在 main 的镜像清单中取最高版本，且该结果仅用于提示。
        HttpClient::instance()->request(QStringLiteral("GET"), QUrl(url), QVariantMap(), QByteArray(),
                                        [this, state](const HttpClient::Reply &r) {
            const QString v = r.ok ? r.json.object().value(QLatin1String("version")).toString()
                                   : QString();
            if (!v.isEmpty()
                && (!state->apiVersion.isEmpty() && v != state->apiVersion)) {
                if (--state->pending > 0)
                    return;
                finishCheck(state);
                return;
            }
            if (!v.isEmpty()
                && (state->bestVersion.isEmpty() || compareVersion(state->bestVersion, v) < 0)) {
                state->bestVersion = v;
                state->best = r.json.object();
            }
            if (--state->pending > 0)
                return;
            finishCheck(state);
        }, kManifestTimeoutMs);
    }
}

void UpdateChecker::finishCheck(const std::shared_ptr<FetchState> &state)
{
    if (!state->bestVersion.isEmpty() && applyManifest(state->best, state->release))
        return;
    if (!state->apiVersion.isEmpty()) {
        // 清单一条都没拉到，但 API 已经明确了最新 tag：至少要让用户知道有更新
        QJsonObject fallback;
        fallback.insert(QStringLiteral("version"), state->apiVersion);
        if (applyManifest(fallback, state->release))
            return;
    }
    // 清单版本号非法（如 ../ 穿越）也要落到这里，不能让 m_checking 永远挂着
    // 以前这里回退到打包进 APK 的 version.json，它与本地版本同源，必然谎报「已是最新」；
    // 网络失败就必须如实说拿不到更新信息
    m_checking = false;
    m_remoteVersion.clear();
    m_downloads.clear();
    m_downloadedPath.clear();
    resetTrustedRelease();
    m_lastResult = QStringLiteral("无法获取更新信息（网络不可达或全部镜像失败）");
    emit stateChanged();
    emit checkFinished(false, m_lastResult);
}

bool UpdateChecker::applyManifest(const QJsonObject &obj, const QJsonObject &release)
{
    const QString remote = obj.value(QLatin1String("version")).toString();
    // 版本号会拼进下载 URL（{version} 模板）和本地保存路径，必须是纯数字点分格式；
    // 否则清单被篡改/镜像被污染时用 "../" 就能写到下载目录之外。
    if (!isValidVersion(remote))
        return false;
    resetTrustedRelease();
    m_checking = false;
    m_remoteVersion = remote;
    m_changelog = obj.value(QLatin1String("desc")).toString();
    m_history.clear();
    const QJsonArray hist = obj.value(QLatin1String("history")).toArray();
    for (const QJsonValue &v : hist) {
        const QJsonObject e = v.toObject();
        QVariantMap entry;
        entry.insert(QStringLiteral("version"), e.value(QLatin1String("version")).toString());
        entry.insert(QStringLiteral("desc"), e.value(QLatin1String("desc")).toString());
        m_history.append(entry);
    }
    m_downloads.clear();
    const QJsonObject dl = obj.value(QLatin1String("downloads")).toObject();
    for (auto it = dl.constBegin(); it != dl.constEnd(); ++it)
        m_downloads.insert(it.key(), it.value().toString());

    m_packageAsset = releaseAssetFor(platformKey());
    QString releaseTag = release.value(QLatin1String("tag_name")).toString().trimmed();
    if (releaseTag.startsWith(QLatin1Char('v')))
        releaseTag.remove(0, 1);
    if (!release.isEmpty() && releaseTag == remote) {
        const QJsonArray assets = release.value(QLatin1String("assets")).toArray();
        for (const QJsonValue &value : assets) {
            const QJsonObject asset = value.toObject();
            if (asset.value(QLatin1String("name")).toString() != m_packageAsset)
                continue;
            const QUrl assetUrl(asset.value(QLatin1String("browser_download_url")).toString());
            const QString hash = normalizedSha256(asset.value(QLatin1String("digest")).toString());
            const qint64 size = asset.value(QLatin1String("size")).toDouble(-1);
            if (assetUrl.isValid() && assetUrl.scheme() == QLatin1String("https")
                && assetUrl.host() == QLatin1String("github.com")
                && !hash.isEmpty() && size > 0) {
                m_releaseTag = releaseTag;
                m_packageUrl = assetUrl.toString();
                m_packageSha256 = hash;
                m_packageSize = size;
                m_releaseMetadataTrusted = true;
            }
            break;
        }
    }

    const bool hasUpdate = compareVersion(localVersion(), remote) < 0;
    if (hasUpdate && !m_releaseMetadataTrusted)
        m_lastResult = QStringLiteral("发现新版本，但官方未提供可信的 SHA-256 校验信息，已禁止下载");
    else
        m_lastResult = hasUpdate ? QString() : QStringLiteral("已是最新版本");
    emit stateChanged();
    emit checkFinished(hasUpdate, hasUpdate ? QString() : m_lastResult);
    return true;
}

QString UpdateChecker::downloadUrlFor(const QString &version) const
{
    if (m_releaseMetadataTrusted && version == m_remoteVersion)
        return m_packageUrl;
#ifdef FURUI_CAR_COMPAT_EDITION
    // 特供版：优先读清单里的 android-carcompat 直链；清单没给就忽略主流版
    // android 直链（那是 cn.furui.music 的包，装到特供版上等于装第二个应用），
    // 按 CI 产物名拼特供默认地址。
    return downloadUrlForKey(version, QStringLiteral("android-carcompat"),
                             QStringLiteral("FuruiMusic-carcompat-netease-arm64-v8a.apk"));
#else
    return downloadUrlForKey(version, platformKey(), releaseAssetFor(platformKey()));
#endif
}

// downloadUrlFor 的公共体：key = 清单 downloads 里查的键，fallbackAsset =
// 清单没给直链时按 CI 产物名拼的默认资产名。
QString UpdateChecker::downloadUrlForKey(const QString &version, const QString &key,
                                         const QString &fallbackAsset) const
{
    // manifest 里声明了当前平台直链则优先（可发版时远程换镜像），否则按 CI 产物名拼默认地址
    const QString fromManifest = m_downloads.value(key).toString();
    if (!fromManifest.isEmpty()) {
        QString url = fromManifest;
        // 清单直链支持 {version}/{v} 占位符：模板写一次，发版不再需要逐条改 URL
        if (url.contains(QLatin1Char('{'))) {
            url.replace(QLatin1String("{version}"), version);
            url.replace(QLatin1String("{v}"), version);
        }
        // 兜底：直链里写死的版本目录与本次检测到的最新版本不符时（发版脚本漏同步
        // downloads 就会出现——v1.4.24 实际发生过：清单 version=1.4.24 但下载地址
        // 还指 v1.4.23，用户「检查到新版本却下载到上一个版本」），按最新版本重写。
        // 这条同时救了手里还拿着旧硬编码清单的存量老客户端。
        static const QRegularExpression re(
            QStringLiteral("(/releases/download/v)(\\d+(?:\\.\\d+)+)(/)"));
        const QRegularExpressionMatch m = re.match(url);
        if (m.hasMatch() && m.captured(2) != version)
            url.replace(m.capturedStart(0), m.capturedLength(0),
                        m.captured(1) + version + m.captured(3));
        return url;
    }
    return QStringLiteral("https://github.com/spygg/furuimusic/releases/download/v%1/%2")
        .arg(version, fallbackAsset);
}

// 下载候选地址：清单/默认拼出的 GitHub 原链放首位兜底，再追加各加速前缀 +
// 完整原链。去重，且只加速 github.com 直链（第三方 CDN 直链不加前缀）。
QStringList UpdateChecker::downloadCandidates(const QString &version) const
{
    const QString original = downloadUrlFor(version);
    QStringList urls;
    urls << original;
    if (original.startsWith(QStringLiteral("https://github.com/"))) {
        for (const QString &prefix : githubProxyPrefixes()) {
            const QString mirrored = prefix + original;
            if (!urls.contains(mirrored))
                urls << mirrored;
        }
    }
    return urls;
}

// 探测下载地址时校验响应体是不是真的发布包。Windows/macOS 的资产是 zip（"PK"），
// Linux 的 AppImage 是 ELF（"\x7fELF"）；只认 zip 会把 Linux 的每条候选都判成
// BAD，镜像加速形同虚设、只能退回 GitHub 原链。
static bool looksLikeReleaseAsset(const QByteArray &head)
{
    if (head.size() >= 2 && head.at(0) == 'P' && head.at(1) == 'K')
        return true;
    if (head.size() >= 4 && static_cast<unsigned char>(head.at(0)) == 0x7f
        && head.at(1) == 'E' && head.at(2) == 'L' && head.at(3) == 'F')
        return true;
    return false;
}

// 并行探测所有候选：Range 取前 2KB，HTTP 2xx 且文件头匹配才可用。
// 探测超时 6s（只是取 2KB，6s 还没回包的线路对整包下载也没意义）。
// 探测期间 progress=-1，UI 显示不确定进度；选出最快可用地址后开始真正下载。
void UpdateChecker::probeAndDownload(const QStringList &candidates, quint64 gen)
{
    auto state = std::make_shared<DownloadProbeState>();
    state->pending = candidates.size();
    m_progress = -1;
    emit stateChanged();

    QVariantMap headers;
    headers.insert(QStringLiteral("User-Agent"), QStringLiteral("Mozilla/5.0"));
    headers.insert(QStringLiteral("Range"), QStringLiteral("bytes=0-2047"));

    for (const QString &url : candidates) {
        // 每个候选各自计时，选中耗时最短的可用线路
        auto clock = std::make_shared<QElapsedTimer>();
        clock->start();
        HttpClient::instance()->request(
            QStringLiteral("GET"), QUrl(url), headers, QByteArray(),
            [this, state, url, clock, candidates, gen](const HttpClient::Reply &r) {
                if (gen != m_downloadGen) return;   // 已被「重新下载」作废
                DownloadProbeState::Candidate c;
                c.url = url;
                c.valid = r.ok && (r.status == 200 || r.status == 206)
                          && looksLikeReleaseAsset(r.body);
                c.elapsedMs = clock->elapsed();
                state->results.append(c);
                const QString verdict = c.valid ? QStringLiteral("OK")
                                                : QStringLiteral("BAD(status=%1,head=%2)")
                                                      .arg(r.status)
                                                      .arg(QString::fromLatin1(r.body.left(4).toHex()));
                Logger::write(QStringLiteral("I"),
                              QStringLiteral("[更新探测] %1 -> %2").arg(url, verdict));
                if (--state->pending > 0)
                    return;
                // 全部探测完：挑最快且有效的；没有有效的就退回 GitHub 原链试一次
                QString best;
                qint64 bestMs = LLONG_MAX;
                for (const DownloadProbeState::Candidate &x : state->results) {
                    if (x.valid && x.elapsedMs < bestMs) {
                        best = x.url;
                        bestMs = x.elapsedMs;
                    }
                }
                // 全部探测完：按耗时排序所有可用线路；没有可用的就退回 GitHub 原链试一次。
                // 下载中途失败时 downloadNextCandidate 会顺着这个队列逐条回退。
                m_downloadQueue.clear();
                QList<QPair<qint64, QString>> ranked;
                for (const DownloadProbeState::Candidate &x : state->results)
                    if (x.valid)
                        ranked.append(qMakePair(x.elapsedMs, x.url));
                std::sort(ranked.begin(), ranked.end(),
                          [](const QPair<qint64, QString> &a, const QPair<qint64, QString> &b) {
                              return a.first < b.first;
                          });
                for (const QPair<qint64, QString> &x : ranked) {
                    if (!m_downloadQueue.contains(x.second))
                        m_downloadQueue << x.second;
                }
                if (m_downloadQueue.isEmpty())
                    m_downloadQueue << (candidates.isEmpty() ? QString() : candidates.first());
                Logger::write(QStringLiteral("I"),
                              QStringLiteral("[更新探测] 线路优先级: %1").arg(m_downloadQueue.join(QStringLiteral(" | "))));
                m_downloadIndex = 0;
                startDownload(m_downloadQueue.first(), gen);
            }, 6000, true, QString(), 2048);   // 只要 2KB 文件头；截断了也不会 whole-file 进内存
    }
}

void UpdateChecker::startDownload(const QString &urlString, quint64 gen)
{
    if (gen != m_downloadGen) return;   // 旧请求迟到，别再写文件/回调
    const QUrl url(urlString);
    if (!url.isValid() || url.scheme() != QLatin1String("https")) {
        m_downloading = false;
        m_progress = 0;
        emit stateChanged();
        emit downloadFailed(QStringLiteral("未找到当前平台的更新包下载地址"));
        return;
    }
    const QString path = targetPath();
    Logger::write(QStringLiteral("I"), QStringLiteral("[更新下载] 使用线路: %1").arg(urlString));
    // APK/zip 体积大且用户网络可能较慢：超时放宽到 30 分钟（HttpClient 的超时是整请求硬超时）
    m_downloadReply = HttpClient::instance()->download(url, QVariantMap(), path,
                                     [this, path, gen](bool ok, const QString &error) {
        if (gen != m_downloadGen) return;   // 「重新下载」已开启新请求，丢弃旧结果
        if (!ok) {
            Logger::write(QStringLiteral("W"),
                          QStringLiteral("[更新下载] 线路失败: %1，尝试下一条").arg(error));
            downloadNextCandidate(true);
            return;
        }
        verifyPackage(path, gen, [this, path] {
            m_downloading = false;
            m_progress = 100;
            m_downloadedPath = path;
            m_lastResult = QStringLiteral("更新包已通过 SHA-256 校验");
            emit stateChanged();
            emit downloadFinished(path);
        });
    }, 30 * 60 * 1000, [this, gen](qint64 received, qint64 total) {
        if (gen != m_downloadGen) return;
        const int p = total > 0 ? int(received * 100 / total) : -1;
        if (p != m_progress) {
            m_progress = p;
            emit stateChanged();
        }
    });
}

// 下载失败后回退到下一条候选；全部失败才真正报错。
void UpdateChecker::downloadNextCandidate(bool previousFailed)
{
    Q_UNUSED(previousFailed);
    if (m_downloadIndex + 1 < m_downloadQueue.size()) {
        ++m_downloadIndex;
        m_progress = -1;
        emit stateChanged();
        startDownload(m_downloadQueue.at(m_downloadIndex), m_downloadGen);
        return;
    }
    m_downloading = false;
    m_progress = 0;
    m_downloadedPath.clear();
    emit stateChanged();
    emit downloadFailed(QStringLiteral("所有下载线路均失败，请检查网络后重试"));
}

void UpdateChecker::verifyPackage(const QString &path, quint64 gen, std::function<void()> done)
{
    if (m_verifying)
        return;
    if (!m_releaseMetadataTrusted || m_packageSha256.isEmpty() || m_packageSize <= 0
        || !QFile::exists(path)) {
        m_downloading = false;
        m_downloadedPath.clear();
        emit stateChanged();
        emit downloadFailed(QStringLiteral("缺少可信的更新包校验信息，已拒绝安装"));
        return;
    }
    auto state = std::make_shared<UpdateVerification>();
    state->path = path;
    state->expectedHash = m_packageSha256.toUtf8();
    state->expectedSize = m_packageSize;
    m_verification = state;
    m_verificationDone = std::move(done);
    m_verifying = true;
    emit stateChanged();

    QThread *thread = new QThread(this);
    auto *verifier = new PackageVerifier(state);
    verifier->moveToThread(thread);
    QPointer<QThread> threadGuard(thread);
    connect(thread, &QThread::started, verifier, [verifier] { verifier->run(); });
    connect(verifier, &PackageVerifier::finished, this,
            [this, state, path, gen, threadGuard](const QString &error) {
        QThread *thread = threadGuard.data();
        if (!thread || m_verificationThread != thread)
            return;
        m_verificationThread.clear();
        m_verifying = false;
        thread->deleteLater();
        if (state->cancelled.load() || gen != m_downloadGen)
            return;
        if (!error.isEmpty()) {
            m_downloading = false;
            m_progress = 0;
            m_downloadedPath.clear();
            QFile::remove(path);
            m_verificationDone = {};
            emit stateChanged();
            emit downloadFailed(error);
            return;
        }
        std::function<void()> done = m_verificationDone;
        m_verificationDone = {};
        emit stateChanged();
        if (done)
            done();
    }, Qt::QueuedConnection);
    connect(verifier, &PackageVerifier::finished, thread, &QThread::quit);
    connect(thread, &QThread::finished, verifier, &QObject::deleteLater);
    m_verificationThread = thread;
    thread->start();
}

void UpdateChecker::cancelVerification(bool wait)
{
    if (m_verification)
        m_verification->cancelled.store(true);
    m_verificationDone = {};
    m_verifying = false;
    QPointer<QThread> thread = m_verificationThread;
    m_verificationThread.clear();
    m_verification.reset();
    if (thread) {
        thread->quit();
        if (wait)
            thread->wait();
        thread->deleteLater();
    }
}

QString UpdateChecker::targetPath() const
{
    const QString suffix = QFileInfo(releaseAssetFor(platformKey())).suffix();
    QDir dir;
#ifdef Q_OS_ANDROID
    // AppDataLocation 对应 getFilesDir()，FileProvider 的 files-path 覆盖它，
    // 才能转 content:// URI 给系统安装器
    dir = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
               + QStringLiteral("/update"));
#else
    QString base = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    dir = QDir(base + QStringLiteral("/FuruiMusicUpdate"));
#endif
    if (!dir.exists())
        dir.mkpath(QStringLiteral("."));
    // 双保险：即使 m_remoteVersion 被别的路径写入异常值，也不允许它带路径分隔符。
    QString version = m_remoteVersion;
    static const QRegularExpression validVersion(QStringLiteral("\\A[0-9]+(\\.[0-9]+)*\\z"));
    if (!validVersion.match(version).hasMatch()) version = QStringLiteral("unknown");
    return dir.filePath(QStringLiteral("FuruiMusic-v%1-%2.%3")
                            .arg(version, platformKey(), suffix));
}

void UpdateChecker::downloadUpdate()
{
    if (m_checking || m_remoteVersion.isEmpty())
        return;
    // 每次下载都开启新代次：旧探测、下载、校验回调均不能改写本次结果。
    ++m_downloadGen;
    if (m_downloadReply) {
        m_downloadReply->abort();
        m_downloadReply.clear();
    }
    cancelVerification(true);
    m_downloadQueue.clear();
    m_downloadIndex = 0;
    m_downloadedPath.clear();
    if (!m_releaseMetadataTrusted || m_packageSha256.isEmpty() || m_packageSize <= 0
        || m_packageAsset != releaseAssetFor(platformKey()) || !QUrl(m_packageUrl).isValid()) {
        m_downloading = false;
        m_progress = 0;
        m_lastResult = QStringLiteral("官方 release 缺少可信的 SHA-256 校验信息，已禁止下载");
        emit stateChanged();
        emit downloadFailed(m_lastResult);
        return;
    }
    Logger::write(QStringLiteral("I"), QStringLiteral("[更新] 用户触发下载，作废旧请求"));
    m_downloading = true;
    m_progress = -1;
    emit stateChanged();

    // 下载前先并行探测 GitHub 原链 + 各加速镜像，选最快且返回真 APK 的线路。
    // 镜像只传输字节，最终必须匹配官方 asset digest 与 size。
    const QStringList candidates = downloadCandidates(m_remoteVersion);
    Logger::write(QStringLiteral("I"),
                  QStringLiteral("[更新探测] 开始，%1 条候选").arg(candidates.size()));
    probeAndDownload(candidates, m_downloadGen);
}

// 取消当前下载并复位状态：gen 递增作废一切迟到回调（成功/失败/进度都不再落地），
// abort 在途连接。「稍后」关闭弹层时 QML 不调用它（下载转后台继续）；
// 它只留给将来需要真正中止的场景（如退出应用前的清理）。
void UpdateChecker::cancelDownload()
{
    ++m_downloadGen;
    if (m_downloadReply) {
        m_downloadReply->abort();   // 迟到的回调被 gen 闸门丢弃
        m_downloadReply.clear();
    }
    cancelVerification(true);
    m_downloading = false;
    m_verifying = false;
    m_progress = 0;
    m_downloadedPath.clear();
    m_downloadQueue.clear();
    emit stateChanged();
    Logger::write(QStringLiteral("I"), QStringLiteral("[更新] 已取消下载"));
}

void UpdateChecker::installUpdate()
{
    if (m_downloadedPath.isEmpty() || !QFile::exists(m_downloadedPath)) {
        emit downloadFailed(QStringLiteral("更新包不存在，请重新下载"));
        return;
    }
    verifyPackage(m_downloadedPath, m_downloadGen, [this] { installVerifiedPackage(); });
}

void UpdateChecker::installVerifiedPackage()
{
#if defined(Q_OS_ANDROID)
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // 车机特供版：比亚迪 DiLink 禁止应用内拉起安装器，startActivity 常「假成功」
    // （不弹安装界面却返回 true）。下载完成后无条件复制一份到公共 Download，
    // 保证系统文件管理器一定能找到 APK；随后仍尝试拉起安装器（个别 ROM 允许）。
    // 复制是整包 Binder 写入（耗时），全流程只做一次——用 copied 闸门收敛，
    // 避免车机版无条件复制 + verdict==1 + 安装失败三处叠加成 2~3 次拷贝与重复 toast。
    bool copied = false;
#ifdef FURUI_CAR_COMPAT_EDITION
    copied = !copyVerifiedToDownloads().isEmpty();
#endif
    // 系统安装器对「同签名已安装 / 应用已安装」这类失败只给一句含糊提示，
    // 先自己把 versionCode 与签名核对清楚，能装的才拉起安装器。
    const int verdict = androidApkInstallability(m_downloadedPath);
    if (verdict == 2) {
        const QString msg = QStringLiteral("更新包与当前版本相同（v%1），无需重复安装")
                                .arg(localVersion());
        AppController::instance()->notify(msg, QStringLiteral("info"));
        emit downloadFailed(msg);
        return;
    }
    if (verdict == 1) {
        // 签名不同：本机装的是调试包、在线包是发布签名，强制给用户留一份可手动安装的包
        if (!copied) copied = !copyVerifiedToDownloads().isEmpty();
        const QString msg = copied
            ? QStringLiteral("更新包与当前应用签名不同（常见原因：本机装的是本地调试包，在线包是发布签名）。"
                             "已复制到系统「下载」目录，请用文件管理器安装")
            : QStringLiteral("更新包与当前应用签名不同，且复制到下载目录失败，请检查存储权限后手动安装：%1")
                  .arg(m_downloadedPath);
        AppController::instance()->notify(msg, QStringLiteral("warning"));
        emit downloadFailed(msg);
        return;
    }
    if (androidInstallApk(m_downloadedPath))
        return;
    // 拉起安装器失败（部分 ROM 禁止应用内安装）：复制到公共 Download 供文件管理器手动装
    if (!copied) copied = !copyVerifiedToDownloads().isEmpty();
    AppController::instance()->notify(
        copied ? QStringLiteral("拉起安装器失败，已复制到系统「下载」目录，请用文件管理器安装")
               : QStringLiteral("拉起安装器失败，且复制到下载目录失败，请检查存储权限后手动安装：%1")
                     .arg(m_downloadedPath),
        QStringLiteral("warning"));
#else
    // Qt5 Android 构建未链接 JNI 助手，退化为提示路径由用户手动安装
    AppController::instance()->notify(QStringLiteral("请手动安装已下载的更新包：%1").arg(m_downloadedPath),
                                      QStringLiteral("warning"));
#endif
#else
    // 桌面：exe 直接运行；AppImage 补执行位后启动；zip 打开所在文件夹由用户解压/安装
    const QFileInfo fi(m_downloadedPath);
    const QString suffix = fi.suffix().toLower();
    if (suffix == QLatin1String("exe")
        && QProcess::startDetached(m_downloadedPath, QStringList()))
        return;
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    // Linux 发布资产是 AppImage，下载后没有执行位（双击也起不来）。
    // 先补 +x 再启动；系统缺 FUSE 时启动会失败，退回打开所在文件夹。
    if (suffix == QLatin1String("appimage")) {
        QFile::setPermissions(m_downloadedPath,
                              QFile::permissions(m_downloadedPath)
                                  | QFileDevice::ExeOwner | QFileDevice::ExeUser
                                  | QFileDevice::ExeGroup | QFileDevice::ExeOther);
        if (QProcess::startDetached(m_downloadedPath, QStringList()))
            return;
    }
#endif
    QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
#endif
}

void UpdateChecker::openDownloadFolder()
{
    if (m_downloadedPath.isEmpty() || !QFile::exists(m_downloadedPath)) {
        emit downloadFailed(QStringLiteral("更新包不存在，请重新下载"));
        return;
    }
    verifyPackage(m_downloadedPath, m_downloadGen, [this] { openVerifiedDownloadFolder(); });
}

void UpdateChecker::openVerifiedDownloadFolder()
{
#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // 车机/部分 ROM 禁止应用直接拉起安装器，改由系统文件管理器打开下载目录，用户手动安装。
    const QJniObject activity = QJniObject::callStaticObjectMethod(
        "org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
    if (!activity.isValid()) {
        AppController::instance()->notify(QStringLiteral("无法打开下载目录：%1").arg(m_downloadedPath),
                                          QStringLiteral("warning"));
        return;
    }
    const bool ok = QJniObject::callStaticMethod<jboolean>(
        "cn/furui/music/UpdateInstaller", "openFolder",
        "(Landroid/app/Activity;Ljava/lang/String;)Z",
        activity.object(), QJniObject::fromString(m_downloadedPath).object<jstring>());
    if (!ok)
        AppController::instance()->notify(QStringLiteral("请到文件管理器手动打开：%1").arg(m_downloadedPath),
                                          QStringLiteral("warning"));
#else
    // 桌面/Qt5 Android：直接打开所在文件夹
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_downloadedPath).absolutePath()));
#endif
}

QString UpdateChecker::copyToDownloads()
{
    if (m_downloadedPath.isEmpty() || !QFile::exists(m_downloadedPath)) {
        AppController::instance()->notify(QStringLiteral("更新包不存在，请重新下载"),
                                          QStringLiteral("warning"));
        return QString();
    }
    verifyPackage(m_downloadedPath, m_downloadGen, [this] { copyVerifiedToDownloads(); });
    return QString();
}

QString UpdateChecker::copyVerifiedToDownloads()
{
#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QString name = QFileInfo(m_downloadedPath).fileName();
    const QString shown = androidCopyToDownloads(m_downloadedPath, name);
    if (!shown.isEmpty()) {
        const QString msg = QStringLiteral("已复制到系统「下载」目录：%1，请到文件管理器点此文件安装").arg(shown);
        AppController::instance()->notify(msg, QStringLiteral("success"));
        return shown;
    }
    AppController::instance()->notify(QStringLiteral("复制到下载目录失败，请检查存储权限或空间"),
                                      QStringLiteral("warning"));
    return QString();
#else
    // 桌面：更新包本就在 Download/FuruiMusicUpdate，直接打开所在文件夹即可
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_downloadedPath).absolutePath()));
    return QFileInfo(m_downloadedPath).fileName();
#endif
}

} // namespace Furui

#include "UpdateChecker.moc"
