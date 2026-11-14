#pragma once
// 福瑞音乐 - 在线检测更新（参照洛雪音乐 version.json 机制）
// 流程：
//   1) 从多个镜像依次拉取 src/version.json（失败才回退 publish/version.json 兼容副本）；
//   2) 与本地版本号比较，有新版本时通知 QML（设置页弹层 + 全局 Toast）；
//   3) 用户同意后从 GitHub Releases 直接下载当前平台的安装包
//      （Windows: FuruiMusic-windows-x64.zip / Android: FuruiMusic-android-arm64-v8a.apk），
//      不跳转外部网页；
//   4) 下载完成后 Android 拉起系统安装器（FileProvider），桌面打开安装包所在文件夹。
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace Furui {

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
    // 远端版本是否真的比本地新（QML 的「下载/安装」按钮据此禁用，避免重复安装同版本）
    Q_PROPERTY(bool hasUpdate READ hasUpdate NOTIFY stateChanged)
    // 下载进度 0..100，总大小未知时为 -1
    Q_PROPERTY(int progress READ progress NOTIFY stateChanged)
    // 下载完成后安装包落盘路径
    Q_PROPERTY(QString downloadedPath READ downloadedPath NOTIFY stateChanged)

public:
    static UpdateChecker *instance();
    explicit UpdateChecker(QObject *parent = nullptr);

    QString localVersion() const;
    QString remoteVersion() const { return m_remoteVersion; }
    QString changelog() const { return m_changelog; }
    QVariantList history() const { return m_history; }
    bool checking() const { return m_checking; }
    bool downloading() const { return m_downloading; }
    bool hasUpdate() const;
    int progress() const { return m_progress; }
    QString downloadedPath() const { return m_downloadedPath; }

    // 拉取 version.json 并与本地版本比较；结果经 checkFinished 通知
    Q_INVOKABLE void checkForUpdates();
    // 下载当前平台的更新包；进度经 stateChanged 展示，结果经 downloadFinished/downloadFailed 通知
    Q_INVOKABLE void downloadUpdate();
    // 下载完成后触发：Android 拉起系统安装器；桌面打开安装包所在文件夹
    Q_INVOKABLE void installUpdate();

signals:
    void stateChanged();
    // message 为空表示成功；无更新时是「已是最新版本」，拉取失败时是错误描述
    void checkFinished(bool hasUpdate, const QString &message);
    void downloadFinished(const QString &path);
    void downloadFailed(const QString &error);

private:
    void fetchMirror(int index);
    bool applyManifest(const QJsonObject &obj);
    QString downloadUrlFor(const QString &version) const;
    QString targetPath() const;

    QString m_remoteVersion;
    QString m_changelog;
    QVariantList m_history;
    QVariantMap m_downloads;   // manifest 里的 downloads 映射（平台 -> 直链，可远程改镜像）
    bool m_checking = false;
    bool m_downloading = false;
    int m_progress = 0;
    QString m_downloadedPath;
};

} // namespace Furui
