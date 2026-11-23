#pragma once
// 福瑞音乐 - 开放 API（对标洛雪 lx-music-desktop 的 openApi 模块）
// 本机 HTTP/JSON 服务，供第三方脚本/工具读取播放器状态与控制播放。
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include "Types.h"

class QTcpServer;
class QTcpSocket;
class QTimer;
class QNetworkReply;
class QNetworkRequest;

namespace Furui {

class OpenApiServer : public QObject {
    Q_OBJECT
public:
    static OpenApiServer *instance();

    void start(int port = 23330, bool localOnly = false);
    void stop();
    bool running() const;
    // 给用户看的主地址：优先局域网 IP（手机/平板/车机上别的设备能直接打开），
    // 仅本机访问或找不到局域网地址时回退 127.0.0.1
    QString address() const;
    QString localAddress() const;
    QString lanAddress() const;
    int port() const { return m_port; }

signals:
    void stateChanged();
    void toast(const QString &message);

private:
    explicit OpenApiServer(QObject *parent = nullptr);
    void onNewConnection();
    void onReadyRead();
    void handleRequest(QTcpSocket *socket, const QByteArray &request);
    void respond(QTcpSocket *socket, int code, const QString &contentType, const QByteArray &body,
                 const QByteArray &extraHeaders = QByteArray());
    QByteArray statusJson() const;
    void pushSubscribe();
    // 音频流代理：按 uid 找到歌曲后实时取新鲜直链再拉流，字节转发给浏览器 <audio>。
    // 不再复用 App 当前播放的直链——那会随 CDN 防盗链过期/切歌空窗而频繁 403/503。
    void handleStream(QTcpSocket *socket, const QByteArray &request);
    // /stream 内部：异步取链（resolve）→ 成功后进入 startStreamFromPlayable
    void resolveStream(QTcpSocket *socket, const QByteArray &request, const Song &song);
    // /stream 内部：用取到的直链 + 防盗链头拉上游并转发；fromCache=true 时
    // 上游 >=400 视为缓存直链失效，清缓存自动重取一次
    void startStreamFromPlayable(QTcpSocket *socket, const QByteArray &request,
                                 const Song &song, const Playable &p, bool fromCache);
    // 歌曲下载：网页控制台点歌曲行尾的下载按钮，按 列表+下标 取 Song → resolve 取链
    // → 流式转发给浏览器并带 Content-Disposition: attachment（大文件不落内存）。
    void handleDownload(QTcpSocket *socket, const QByteArray &request);
    // 下载专用字节转发：与 /stream 同构但带 Content-Disposition: attachment
    void streamWithAttachment(QTcpSocket *socket, const QNetworkRequest &req,
                              const QString &fileName);
    void abortStreamsForSocket(QTcpSocket *socket);

    QTcpServer *m_server = nullptr;
    // 每个 /stream 长连接对应一个上游 reply；socket 断开时 abort 对应请求。
    QHash<QTcpSocket *, QNetworkReply *> m_streams;
    // 直链缓存：uid → resolve 结果 + 过期时间戳。浏览器拖进度/重连不再反复取链，
    // TTL 10 分钟（CDN 防盗链直链一般 20~30 分钟有效，留足余量）。
    struct CachedPlayable { Playable p; qint64 expiresAt = 0; };
    QHash<QString, CachedPlayable> m_streamCache;
    QTimer *m_pushTimer = nullptr;
    QSet<QTcpSocket *> m_subscribers;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QHash<QTcpSocket *, QByteArray> m_origins;
    int m_port = 23330;
    bool m_localOnly = false;
};

} // namespace Furui
