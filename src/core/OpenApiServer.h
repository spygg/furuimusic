#pragma once
// 福瑞音乐 - 开放 API（对标洛雪 lx-music-desktop 的 openApi 模块）
// 本机 HTTP/JSON 服务，供第三方脚本/工具读取播放器状态与控制播放。
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

class QTcpServer;
class QTcpSocket;
class QTimer;

namespace Furui {

class OpenApiServer : public QObject {
    Q_OBJECT
public:
    static OpenApiServer *instance();

    void start(int port = 23330, bool localOnly = true);
    void stop();
    bool running() const;
    QString address() const;
    int port() const { return m_port; }

signals:
    void stateChanged();
    void toast(const QString &message);

private:
    explicit OpenApiServer(QObject *parent = nullptr);
    void onNewConnection();
    void onReadyRead();
    void handleRequest(QTcpSocket *socket, const QByteArray &request);
    void respond(QTcpSocket *socket, int code, const QString &contentType, const QByteArray &body);
    QByteArray statusJson() const;
    void pushSubscribe();

    QTcpServer *m_server = nullptr;
    QTimer *m_pushTimer = nullptr;
    QSet<QTcpSocket *> m_subscribers;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QHash<QTcpSocket *, QByteArray> m_origins;
    int m_port = 23330;
    bool m_localOnly = true;
};

} // namespace Furui
