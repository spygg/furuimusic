// 福瑞音乐 - 在线检测更新（参照洛雪音乐 publish/version.json 机制）
#include "UpdateChecker.h"

#include "AppController.h"
#include "HttpClient.h"
#include "Version.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QJniObject>
// QNativeInterface::QAndroidApplication 的真实头文件；QNativeInterface/QAndroidApplication
// 这个前向路径 Qt 6.9+ 才有，6.8.3 的 android kit 里没有（CI 实测 fatal error）
#include <QtCore/qcoreapplication_platform.h>
#endif

namespace Furui {

namespace {

// 多镜像依次尝试：GitHub raw 直连失败时退回 jsDelivr CDN（与洛雪 update.js 的镜像链同思路）
QStringList versionMirrors()
{
    return QStringList{
        QStringLiteral("https://raw.githubusercontent.com/spygg/furuimusic/main/publish/version.json"),
        QStringLiteral("https://cdn.jsdelivr.net/gh/spygg/furuimusic@main/publish/version.json"),
        QStringLiteral("https://fastly.jsdelivr.net/gh/spygg/furuimusic@main/publish/version.json"),
        QStringLiteral("https://gcore.jsdelivr.net/gh/spygg/furuimusic@main/publish/version.json"),
    };
}

// CI（.github/workflows/release.yml）上传到 Release 的产物命名，下载直链按它拼
QString releaseAssetFor(const QString &platformKey)
{
    if (platformKey == QLatin1String("windows"))
        return QStringLiteral("FuruiMusic-windows-x64.zip");
    if (platformKey == QLatin1String("android"))
        return QStringLiteral("FuruiMusic-android-arm64-v8a.apk");
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
// Android 7+ 不允许 file:// 直接跨应用共享，必须经 FileProvider 转 content:// URI。
// 工程的 AndroidManifest 已声明 androidx FileProvider（authority 见下），
// paths 覆盖 files-path 根目录，APK 放在 AppDataLocation 下即可共享。
bool androidInstallApk(const QString &path)
{
    QJniObject jpath = QJniObject::fromString(path);
    QJniObject file("java/io/File", "(Ljava/lang/String;)V", jpath.object());
    if (!file.isValid())
        return false;
    QJniObject activity = QNativeInterface::QAndroidApplication::context();
    if (!activity.isValid())
        return false;
    QJniObject uri = QJniObject::callStaticObjectMethod(
        "androidx/core/content/FileProvider", "getUriForFile",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/io/File;)Landroid/net/Uri;",
        activity.object(),
        QJniObject::fromString(QStringLiteral("cn.furui.music.qtprovider")).object(),
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
    intent.callObjectMethod("addFlags", "(I)Landroid/content/Intent;", jint(1));
    // FLAG_ACTIVITY_NEW_TASK：从非 Activity 上下文启动必须加
    intent.callObjectMethod("addFlags", "(I)Landroid/content/Intent;", jint(0x10000000));
    activity.callMethod<void>("startActivity", "(Landroid/content/Intent;)V", intent.object());
    return true;
}
#endif

} // namespace

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

void UpdateChecker::checkForUpdates()
{
    if (m_checking || m_downloading)
        return;
    m_checking = true;
    m_progress = 0;
    emit stateChanged();
    fetchMirror(0);
}

void UpdateChecker::fetchMirror(int index)
{
    const QStringList mirrors = versionMirrors();
    if (index >= mirrors.size()) {
        m_checking = false;
        emit stateChanged();
        emit checkFinished(false, QStringLiteral("无法获取更新信息（网络不可达或全部镜像失败）"));
        return;
    }
    HttpClient::instance()->request(QStringLiteral("GET"), QUrl(mirrors.at(index)), QVariantMap(),
                                    QByteArray(),
                                    [this, index](const HttpClient::Reply &r) {
        const QJsonObject obj = r.json.object();
        const QString remote = obj.value(QLatin1String("version")).toString();
        if (!r.ok || remote.isEmpty()) {
            fetchMirror(index + 1);
            return;
        }
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
        emit stateChanged();
        emit checkFinished(hasUpdate,
                           hasUpdate ? QString() : QStringLiteral("已是最新版本"));
    });
}

QString UpdateChecker::downloadUrlFor(const QString &version) const
{
    // manifest 里声明了当前平台直链则优先（可发版时远程换镜像），否则按 CI 产物名拼默认地址
    const QString fromManifest = m_downloads.value(platformKey()).toString();
    if (!fromManifest.isEmpty())
        return fromManifest;
    return QStringLiteral("https://github.com/spygg/furuimusic/releases/download/v%1/%2")
        .arg(version, releaseAssetFor(platformKey()));
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
    const QUrl url = downloadUrlFor(m_remoteVersion);
    if (!url.isValid() || url.scheme() != QLatin1String("https")) {
        emit downloadFailed(QStringLiteral("未找到当前平台的更新包下载地址"));
        return;
    }
    m_downloading = true;
    m_progress = 0;
    m_downloadedPath.clear();
    emit stateChanged();

    const QString path = targetPath();
    // APK/zip 体积大且用户网络可能较慢：超时放宽到 30 分钟（HttpClient 的超时是整请求硬超时）
    HttpClient::instance()->download(url, QVariantMap(), path,
                                     [this, path](bool ok, const QString &error) {
        m_downloading = false;
        if (!ok) {
            m_progress = 0;
            emit stateChanged();
            emit downloadFailed(error);
            return;
        }
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

void UpdateChecker::installUpdate()
{
    if (m_downloadedPath.isEmpty() || !QFile::exists(m_downloadedPath)) {
        emit downloadFailed(QStringLiteral("更新包不存在，请重新下载"));
        return;
    }
#if defined(Q_OS_ANDROID)
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (androidInstallApk(m_downloadedPath))
        return;
    AppController::instance()->notify(QStringLiteral("拉起安装器失败，请手动安装：%1").arg(m_downloadedPath),
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

} // namespace Furui
