#pragma once
// 福瑞音乐 - 异步 HTTP 客户端（全平台共用）
#include <QByteArray>
#include <QJsonDocument>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <functional>

class QNetworkReply;

namespace Furui {

class HttpClient : public QObject {
    Q_OBJECT
public:
    struct Reply {
        bool ok = false;
        int status = 0;
        QByteArray body;
        QJsonDocument json;
        QString error;
        QMap<QString, QString> headers;   // 小写 header 名
        QStringList setCookies;
    };
    using Callback = std::function<void(const Reply &)>;
    // 下载专用：直接流式写入 QSaveFile，不把整首歌读进内存。
    // 回调 (ok, errorMessage)，成功后文件已原子落盘。
    using DownloadCallback = std::function<void(bool, const QString &)>;
    // 下载进度回调 (received, total)，total 未知时为 0（大文件下载如更新包需要进度展示）
    using DownloadProgressCallback = std::function<void(qint64, qint64)>;

    static HttpClient *instance();
    // 为当前线程创建独立 NAM（ScriptEngine worker 使用），不修改应用级代理。
    static HttpClient *createForCurrentThread();

    // headers 支持 {placeholder}，由调用方渲染后传入；body 已是最终字节
    // followRedirects=false 时 3xx 原样返回，调用方从 headers["location"] 自行跳转
    // （QQ 扫码登录需要用这条链路把 graph.qq.com 的重定向一路追到带 code 的回调页）
    // peerVerifyName 非空时，仅把「证书主机名校验」用的名字换成它，连接/SNI/Host 仍用 url 的域名。
    // 只给已知证书配错的站点用（如酷狗 login.user.kugou.com 拿到的是只覆盖 *.kugou.com 的通配证书）。
    // bodyLimit >=0 时最多保留这么多响应字节，达到上限立即 abort（用于只需要文件头的探测请求，
    // 避免上游忽略 Range 时把整包下载进内存）；默认 16MiB：业务响应（歌单/歌词/榜单 JSON）
    // 远达不到，但能挡住异常/恶意超大响应在 GUI 线程 readAll+解析导致 OOM（审查 PERF-02）。
    // 需要超大响应（如更新包探测）的调用点显式传 -1 或自己的上限。
    void request(const QString &method, const QUrl &url, const QVariantMap &headers,
                 const QByteArray &body, const Callback &cb, int timeoutMs = -1,
                 bool followRedirects = true, const QString &peerVerifyName = QString(),
                 qint64 bodyLimit = 16 * 1024 * 1024);
    // 返回本次下载的 QNetworkReply，调用方可在「重新下载」时 abort 旧请求；
    // 忽略返回值不影响原有调用点。
    QNetworkReply *download(const QUrl &url, const QVariantMap &headers, const QString &destPath,
                            const DownloadCallback &cb, int timeoutMs = -1,
                            const DownloadProgressCallback &progress = DownloadProgressCallback());
    // 流式打开（开放 API /stream 音频代理用）：复用本客户端的代理配置，返回裸 reply，
    // 由调用方负责读数据、abort 与 deleteLater。不缓存 body、不设超时（长音频流可能
    // 远超默认 12s；断连由调用方在 socket disconnected 时 abort）。
    QNetworkReply *openStream(const QNetworkRequest &req);
    void setProxyFromSettings();
    // 与 HttpClient 完全一致的代理判定：设置里为空就 NoProxy。
    // QML 的 Image / MediaPlayer 用自己的 QNetworkAccessManager，只认应用级代理，
    // 所以启动时也要把它设成同一份策略，避免出现“接口能取、封面却走系统代理失败”。
    static QNetworkProxy proxyFromSettings();
    static void applyApplicationProxyFromSettings();

private:
    explicit HttpClient(QObject *parent = nullptr);
    QNetworkAccessManager m_nam;
};

} // namespace Furui
