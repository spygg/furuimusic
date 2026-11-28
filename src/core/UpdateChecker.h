#pragma once
// 福瑞音乐 - 在线检测更新（参照洛雪音乐 version.json 机制）
// 流程：
//   1) 先问 GitHub API /releases/latest 拿最新 tag（不走 CDN），再按该不可变 tag
//      并行拉多镜像清单并取最大版本号；API 不可达时才退回 main 分支多镜像；
//   2) 与本地版本号比较，有新版本时通知 QML（设置页弹层 + 全局 Toast）；
//   3) 用户同意后从 GitHub Releases 直接下载当前平台的安装包；Android 按已安装证书
//      选择新签名包或旧 debug 签名兼容包，不跳转外部网页；
//   4) 下载完成后 Android 拉起系统安装器（FileProvider），桌面打开安装包所在文件夹。
#include "UpdatePackage.h"

#include <QJsonObject>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
#include <memory>

class QThread;

namespace Furui {

struct UpdateVerification;

class UpdateChecker : public QObject {
    Q_OBJECT
    // 本地版本（Version.h 的 FURUI_APP_VERSION）
    Q_PROPERTY(QString localVersion READ localVersion CONSTANT)
    // 远端最新版本号（检查成功后有效）
    Q_PROPERTY(QString remoteVersion READ remoteVersion NOTIFY stateChanged)
    // 最新版本更新说明
    Q_PROPERTY(QString changelog READ changelog NOTIFY stateChanged)
    // 历史版本 [{version, desc}, ...]
    Q_PROPERTY(QVariantList history READ history NOTIFY stateChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY stateChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY stateChanged)
    Q_PROPERTY(bool verifying READ verifying NOTIFY stateChanged)
    // 远端版本是否真的比本地新（QML 的「下载/安装」按钮据此禁用，避免重复安装同版本）
    Q_PROPERTY(bool hasUpdate READ hasUpdate NOTIFY stateChanged)
    // 下载进度 0..100，总大小未知时为 -1
    Q_PROPERTY(int progress READ progress NOTIFY stateChanged)
    // 下载完成后安装包落盘路径
    Q_PROPERTY(QString downloadedPath READ downloadedPath NOTIFY stateChanged)
    // 检查结论的补充说明：无更新时是「已是最新版本」，拉取失败时是失败原因，检查中为空。
    // 「拉不到清单」和「确实是最新」以前在界面上都显示成「已是最新版本」，必须区分开
    Q_PROPERTY(QString lastResult READ lastResult NOTIFY stateChanged)

public:
    static UpdateChecker *instance();
    explicit UpdateChecker(QObject *parent = nullptr);
    ~UpdateChecker() override;

    QString localVersion() const;
    QString remoteVersion() const { return m_remoteVersion; }
    QString changelog() const { return m_changelog; }
    QVariantList history() const { return m_history; }
    bool checking() const { return m_checking; }
    bool downloading() const { return m_downloading; }
    bool verifying() const { return m_verifying; }
    bool hasUpdate() const;
    int progress() const { return m_progress; }
    QString downloadedPath() const { return m_downloadedPath; }
    QString lastResult() const { return m_lastResult; }

    // 拉取 version.json 并与本地版本比较；结果经 checkFinished 通知
    Q_INVOKABLE void checkForUpdates();
    // 下载当前平台的更新包；下载中再调用会取消旧请求并重新探测/下载（「重新下载」）
    Q_INVOKABLE void downloadUpdate();
    // 取消当前下载并复位状态（关闭更新弹层时可调用）
    Q_INVOKABLE void cancelDownload();
    // 下载完成后触发：Android 拉起系统安装器；桌面打开安装包所在文件夹
    Q_INVOKABLE void installUpdate();
    // 打开下载结果所在目录（系统文件管理器），供车机等「只允许系统文件管理安装」的环境手动安装
    Q_INVOKABLE void openDownloadFolder();
    // 把已下载的 APK 复制到公共 Download 目录（MediaStore），车机禁应用内安装时
    // 供系统文件管理器找到并手动安装。返回展示名，空串表示失败；结果经 toast 提示。
    Q_INVOKABLE QString copyToDownloads();

signals:
    void stateChanged();
    // message 为空表示成功；无更新时是「已是最新版本」，拉取失败时是错误描述
    void checkFinished(bool hasUpdate, const QString &message);
    void downloadFinished(const QString &path);
    void downloadFailed(const QString &error);

private:
    // 一轮检查里并行拉取清单的共享状态（定义在 .cpp）
    struct FetchState;
    // 下载前并行探测各镜像的共享状态（定义在 .cpp）
    struct DownloadProbeState;

    void probeLatestRelease();
    void fetchManifests(const QString &ref, const QString &apiVersion,
                        const QJsonObject &release);
    void finishCheck(const std::shared_ptr<FetchState> &state);
    bool applyManifest(const QJsonObject &obj, const QJsonObject &release);
    void resetTrustedRelease();
    void verifyPackage(const QString &path, quint64 gen, std::function<void()> done);
    void cancelVerification(bool wait);
    void installVerifiedPackage();
    void openVerifiedDownloadFolder();
    QString copyVerifiedToDownloads();
    QString downloadUrlFor(const QString &version) const;
    // 按平台、车机变体和签名通道选择清单键及默认资产名。
    QString downloadUrlForKey(const QString &version, AndroidUpdateChannel channel) const;
    QStringList downloadCandidates(const QString &version) const;
    void probeAndDownload(const QStringList &candidates, quint64 gen);
    void startDownload(const QString &url, quint64 gen);
    void downloadNextCandidate(bool previousFailed);
    QString targetPath() const;

    QString m_remoteVersion;
    QString m_changelog;
    QVariantList m_history;
    QVariantMap m_downloads;   // manifest 里的 downloads 映射（平台 -> 直链，可远程改镜像）
    QString m_releaseTag;
    QString m_packageAsset;
    AndroidUpdateChannel m_androidUpdateChannel = AndroidUpdateChannel::Release;
    QString m_packageUrl;
    QString m_packageSha256;
    qint64 m_packageSize = 0;
    bool m_releaseMetadataTrusted = false;
    QStringList m_downloadQueue;  // 探测后按耗时排序的可用线路，失败时依次回退
    int m_downloadIndex = 0;
    // 「重新下载」时递增：让旧探测/旧下载/校验的异步回调全部作废，避免旧请求抢答或写同一个文件
    quint64 m_downloadGen = 0;
    QPointer<QNetworkReply> m_downloadReply;
    std::shared_ptr<UpdateVerification> m_verification;
    QPointer<QThread> m_verificationThread;
    std::function<void()> m_verificationDone;
    bool m_checking = false;
    bool m_downloading = false;
    bool m_verifying = false;
    int m_progress = 0;
    QString m_downloadedPath;
    QString m_lastResult;
};

} // namespace Furui
