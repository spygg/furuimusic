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
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

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

namespace {

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
#elif defined(Q_OS_MACOS)
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
    const QJniObject jPkg = QJniObject::fromString(QStringLiteral("cn.furui.music"));
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
    m_checking = true;
    m_progress = 0;
    m_lastResult.clear();
    emit stateChanged();
    probeLatestRelease();
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
        if (r.ok)
            tag = r.json.object().value(QLatin1String("tag_name")).toString().trimmed();
        if (tag.startsWith(QLatin1Char('v')))
            tag.remove(0, 1);
        if (!tag.isEmpty()) {
            fetchManifests(tag, tag);
            return;
        }
        // API 限流或不可达：退回 main 分支多镜像；由 finishCheck 取其中最大版本
        fetchManifests(QStringLiteral("main"), QString());
    }, kManifestTimeoutMs);
}

void UpdateChecker::fetchManifests(const QString &ref, const QString &apiVersion)
{
    const QStringList urls = manifestUrls(ref);
    auto state = std::make_shared<FetchState>();
    state->pending = urls.size();
    state->apiVersion = apiVersion;
    for (const QString &url : urls) {
        // 并行拉全部镜像后取最高版本：只要有一个节点是旧缓存（或者镜像没同步），
        // 也不会像以前那样「第一个返回合法 JSON 就采纳」把结果带偏
        HttpClient::instance()->request(QStringLiteral("GET"), QUrl(url), QVariantMap(), QByteArray(),
                                        [this, state](const HttpClient::Reply &r) {
            const QString v = r.ok ? r.json.object().value(QLatin1String("version")).toString()
                                   : QString();
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
    if (!state->bestVersion.isEmpty()) {
        applyManifest(state->best);
        return;
    }
    if (!state->apiVersion.isEmpty()) {
        // 清单一条都没拉到，但 API 已经明确了最新 tag：至少要让用户知道有更新
        QJsonObject fallback;
        fallback.insert(QStringLiteral("version"), state->apiVersion);
        applyManifest(fallback);
        return;
    }
    // 以前这里回退到打包进 APK 的 version.json，它与本地版本同源，必然谎报「已是最新」；
    // 网络失败就必须如实说拿不到更新信息
    m_checking = false;
    m_lastResult = QStringLiteral("无法获取更新信息（网络不可达或全部镜像失败）");
    emit stateChanged();
    emit checkFinished(false, m_lastResult);
}

bool UpdateChecker::applyManifest(const QJsonObject &obj)
{
    const QString remote = obj.value(QLatin1String("version")).toString();
    if (remote.isEmpty())
        return false;
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
    const bool hasUpdate = compareVersion(localVersion(), remote) < 0;
    m_lastResult = hasUpdate ? QString() : QStringLiteral("已是最新版本");
    emit stateChanged();
    emit checkFinished(hasUpdate, hasUpdate ? QString() : m_lastResult);
    return true;
}

QString UpdateChecker::downloadUrlFor(const QString &version) const
{
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

// 并行探测所有候选：Range 取前 2KB，HTTP 2xx 且开头是 "PK" 才算可用。
// 探测超时 6s（只是取 2KB，6s 还没回包的线路对整包下载也没意义）。
// 探测期间 progress=-1，UI 显示不确定进度；选出最快可用地址后开始真正下载。
void UpdateChecker::probeAndDownload(const QStringList &candidates)
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
            [this, state, url, clock, candidates](const HttpClient::Reply &r) {
                DownloadProbeState::Candidate c;
                c.url = url;
                c.valid = r.ok && (r.status == 200 || r.status == 206)
                          && r.body.size() >= 2 && r.body.at(0) == 'P' && r.body.at(1) == 'K';
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
                startDownload(m_downloadQueue.first());
            }, 6000);
    }
}

void UpdateChecker::startDownload(const QString &urlString)
{
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
    HttpClient::instance()->download(url, QVariantMap(), path,
                                     [this, path](bool ok, const QString &error) {
        if (!ok) {
            Logger::write(QStringLiteral("W"),
                          QStringLiteral("[更新下载] 线路失败: %1，尝试下一条").arg(error));
            downloadNextCandidate(true);
            return;
        }
        m_downloading = false;
        m_downloadedPath = path;
        emit stateChanged();
        emit downloadFinished(path);
    }, 30 * 60 * 1000, [this](qint64 received, qint64 total) {
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
        startDownload(m_downloadQueue.at(m_downloadIndex));
        return;
    }
    m_downloading = false;
    m_progress = 0;
    m_downloadedPath.clear();
    emit stateChanged();
    emit downloadFailed(QStringLiteral("所有下载线路均失败，请检查网络后重试"));
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
    return dir.filePath(QStringLiteral("FuruiMusic-v%1-%2.%3")
                            .arg(m_remoteVersion, platformKey(), suffix));
}

void UpdateChecker::downloadUpdate()
{
    if (m_downloading || m_checking || m_remoteVersion.isEmpty())
        return;
    m_downloading = true;
    m_progress = -1;
    m_downloadedPath.clear();
    emit stateChanged();

    // 下载前先并行探测 GitHub 原链 + 各加速镜像，选最快且返回真 APK 的线路。
    // 探测完自动开始真正下载（成功/失败仍走原来的 downloadFinished/downloadFailed）。
    const QStringList candidates = downloadCandidates(m_remoteVersion);
    Logger::write(QStringLiteral("I"),
                  QStringLiteral("[更新探测] 开始，%1 条候选").arg(candidates.size()));
    probeAndDownload(candidates);
}

void UpdateChecker::installUpdate()
{
    if (m_downloadedPath.isEmpty() || !QFile::exists(m_downloadedPath)) {
        emit downloadFailed(QStringLiteral("更新包不存在，请重新下载"));
        return;
    }
#if defined(Q_OS_ANDROID)
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // 车机特供版：比亚迪 DiLink 禁止应用内拉起安装器，startActivity 常「假成功」
    // （不弹安装界面却返回 true）。下载完成后无条件复制一份到公共 Download，
    // 保证系统文件管理器一定能找到 APK；随后仍尝试拉起安装器（个别 ROM 允许）。
    // 复制是整包 Binder 写入（耗时），全流程只做一次——用 copied 闸门收敛，
    // 避免车机版无条件复制 + verdict==1 + 安装失败三处叠加成 2~3 次拷贝与重复 toast。
    bool copied = false;
#ifdef FURUI_CAR_COMPAT_EDITION
    copied = !copyToDownloads().isEmpty();
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
        if (!copied) copied = !copyToDownloads().isEmpty();
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
    if (!copied) copied = !copyToDownloads().isEmpty();
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
    // 桌面：exe 安装器直接运行；zip/AppImage 打开所在文件夹由用户解压/安装
    const QFileInfo fi(m_downloadedPath);
    if (fi.suffix().compare(QLatin1String("exe"), Qt::CaseInsensitive) == 0
        && QProcess::startDetached(m_downloadedPath, QStringList()))
        return;
    QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
#endif
}

void UpdateChecker::openDownloadFolder()
{
    if (m_downloadedPath.isEmpty() || !QFile::exists(m_downloadedPath)) {
        emit downloadFailed(QStringLiteral("更新包不存在，请重新下载"));
        return;
    }
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
