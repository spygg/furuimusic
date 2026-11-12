#include "core/QtCompat.h"
#include "OpenApiServer.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include "AppController.h"
#include "Library.h"

namespace Furui {

namespace {

QByteArray headerValue(const QByteArray &request, const QByteArray &name)
{
    const QList<QByteArray> lines = request.split('\n');
    for (const QByteArray &raw : lines) {
        const QByteArray line = raw.trimmed();
        const int colon = line.indexOf(':');
        if (colon <= 0) continue;
        if (furuiByteArrayEqualsIC(line.left(colon).trimmed(), name))
            return line.mid(colon + 1).trimmed();
    }
    return QByteArray();
}

// 只有本机页面（或 file:// 的 null origin）才允许被回显 CORS 头
bool isLoopbackOrigin(const QByteArray &value)
{
    if (value.isEmpty() || value == "null") return true;
    const QString host = QUrl(QString::fromUtf8(value)).host();
    if (host.isEmpty()) return false;
    if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0) return true;
    const QHostAddress addr(host);
    return !addr.isNull() && (addr == QHostAddress(QHostAddress::LocalHost) ||
                              addr == QHostAddress(QHostAddress::LocalHostIPv6));
}

// 浏览器发出的跨站请求会带外部 Origin / Referer；纯本地工具（curl、洛雪兼容脚本）
// 通常两者皆无，保持放行以免破坏既有客户端。
bool untrustedBrowserRequest(const QByteArray &request)
{
    const QByteArray origin = headerValue(request, "Origin");
    const QByteArray referer = headerValue(request, "Referer");
    if (origin.isEmpty() && referer.isEmpty()) return false;
    if (!origin.isEmpty() && !isLoopbackOrigin(origin)) return true;
    if (!referer.isEmpty() && !isLoopbackOrigin(referer)) return true;
    return false;
}

bool isControlPath(const QString &path)
{
    static const QSet<QString> controls {
        QStringLiteral("/play"), QStringLiteral("/pause"), QStringLiteral("/toggle"),
        QStringLiteral("/skip-next"), QStringLiteral("/skip-prev"),
        QStringLiteral("/seek"), QStringLiteral("/volume"), QStringLiteral("/search")
    };
    return controls.contains(path);
}

} // namespace

OpenApiServer *OpenApiServer::instance()
{
    static OpenApiServer *s = new OpenApiServer();
    return s;
}

OpenApiServer::OpenApiServer(QObject *parent)
    : QObject(parent)
{
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &OpenApiServer::onNewConnection);
    m_pushTimer = new QTimer(this);
    m_pushTimer->setInterval(1000);
    connect(m_pushTimer, &QTimer::timeout, this, &OpenApiServer::pushSubscribe);
}

bool OpenApiServer::running() const { return m_server && m_server->isListening(); }

QString OpenApiServer::address() const
{
    if (!running()) return QString();
    return QStringLiteral("http://127.0.0.1:%1").arg(m_port);
}

void OpenApiServer::start(int port, bool localOnly)
{
    if (port <= 0) port = 23330;
    if (running() && m_port == port && m_localOnly == localOnly) return;
    stop();
    m_port = port;
    m_localOnly = localOnly;
    const QHostAddress addr = localOnly ? QHostAddress::LocalHost : QHostAddress::Any;
    if (!m_server->listen(addr, quint16(port))) {
        emit toast(QStringLiteral("开放 API 启动失败：") + m_server->errorString());
        return;
    }
    m_pushTimer->start();
    emit stateChanged();
    emit toast(QStringLiteral("开放 API 已启动：%1").arg(address()));
    if (!localOnly)
        emit toast(QStringLiteral("警告：当前监听所有网卡，局域网设备可无鉴权控制播放；建议开启「仅本机访问」"));
}

void OpenApiServer::stop()
{
    if (m_pushTimer) m_pushTimer->stop();
    for (QTcpSocket *s : m_subscribers) { s->disconnectFromHost(); s->deleteLater(); }
    m_subscribers.clear();
    m_buffers.clear();
    if (m_server && m_server->isListening()) m_server->close();
    emit stateChanged();
}

void OpenApiServer::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        m_buffers.insert(socket, QByteArray());
        connect(socket, &QTcpSocket::readyRead, this, &OpenApiServer::onReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            m_subscribers.remove(socket);
            m_buffers.remove(socket);
            m_origins.remove(socket);
            socket->deleteLater();
        });
    }
}

void OpenApiServer::onReadyRead()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket) return;
    m_buffers[socket].append(socket->readAll());
    const QByteArray &buf = m_buffers[socket];
    const int end = buf.indexOf("\r\n\r\n");
    if (end < 0) {
        if (buf.size() > 65536) socket->disconnectFromHost();
        return;
    }
    const QByteArray request = buf.left(end + 4);
    m_buffers[socket].clear();
    m_origins.insert(socket, headerValue(request, "Origin"));
    handleRequest(socket, request);
}

QByteArray OpenApiServer::statusJson() const
{
    AppController *app = AppController::instance();
    QJsonObject o;
    o.insert(QStringLiteral("status"), app->playing() ? QStringLiteral("playing") : QStringLiteral("paused"));
    o.insert(QStringLiteral("name"), app->currentTitle());
    o.insert(QStringLiteral("singer"), app->currentArtist());
    o.insert(QStringLiteral("albumName"), QString());
    o.insert(QStringLiteral("source"), app->currentSourceLabel());
    o.insert(QStringLiteral("substitute"), app->substituteBadge());
    o.insert(QStringLiteral("lyricLineText"), app->currentLyricLine());
    o.insert(QStringLiteral("duration"), app->duration());
    o.insert(QStringLiteral("progress"), app->position());
    o.insert(QStringLiteral("volume"), app->volume());
    o.insert(QStringLiteral("playbackRate"), 1.0);
    o.insert(QStringLiteral("playMode"), app->playModeName());
    o.insert(QStringLiteral("appName"), app->appName());
    o.insert(QStringLiteral("version"), app->version());
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

void OpenApiServer::respond(QTcpSocket *socket, int code, const QString &contentType,
                            const QByteArray &body)
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) return;
    // 状态行的 reason 必须跟着状态码走：写成 "404 OK" 时，按状态行解析的客户端会误判
    QByteArray reason;
    switch (code) {
    case 200: reason = "OK"; break;
    case 204: reason = "No Content"; break;
    case 400: reason = "Bad Request"; break;
    case 403: reason = "Forbidden"; break;
    case 404: reason = "Not Found"; break;
    case 405: reason = "Method Not Allowed"; break;
    default:  reason = "Error"; break;
    }
    QByteArray header;
    header += "HTTP/1.1 " + QByteArray::number(code) + " " + reason + "\r\n";
    header += "Content-Type: " + contentType.toUtf8() + "\r\n";
    header += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    // 不再放通任意站点：只回显本机来源，外部网页读不到响应
    const QByteArray origin = m_origins.value(socket);
    if (!origin.isEmpty() && isLoopbackOrigin(origin))
        header += "Access-Control-Allow-Origin: " + origin + "\r\n";
    header += "Access-Control-Allow-Methods: GET, OPTIONS\r\n";
    header += "Cache-Control: no-cache\r\n";
    header += "Connection: close\r\n\r\n";
    socket->write(header);
    socket->write(body);
    socket->flush();
    socket->disconnectFromHost();
}

void OpenApiServer::handleRequest(QTcpSocket *socket, const QByteArray &request)
{
    const QString firstLine = QString::fromUtf8(request.left(request.indexOf('\r')));
    const QStringList parts = firstLine.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() < 2) { respond(socket, 400, QStringLiteral("text/plain"), "Bad Request"); return; }
    const QString method = parts.at(0);
    const QUrl url(parts.at(1));
    const QString path = QStringLiteral("/") + url.path().section(QLatin1Char('/'), -1);
    const QUrlQuery query(url);
    AppController *app = AppController::instance();

    if (method == QLatin1String("OPTIONS")) {
        respond(socket, 200, QStringLiteral("text/plain"), "OK");
        return;
    }
    // 浏览器跨站保护：控制类端点不接受外部 Origin/Referer；
    // 不带这两个头的本地工具（curl / 洛雪兼容脚本）行为完全不变。
    if (isControlPath(path) && untrustedBrowserRequest(request)) {
        respond(socket, 403, QStringLiteral("application/json; charset=utf-8"),
                "{\"error\":\"cross-origin control rejected\"}");
        return;
    }
    if (path == QLatin1String("/status")) {
        respond(socket, 200, QStringLiteral("application/json; charset=utf-8"), statusJson());
        return;
    }
    if (path == QLatin1String("/lyric")) {
        respond(socket, 200, QStringLiteral("text/plain; charset=utf-8"), app->lyricsText().toUtf8());
        return;
    }
    if (path == QLatin1String("/subscribe-player-status")) {
        QByteArray header;
        header += "HTTP/1.1 200 OK\r\n";
        header += "Content-Type: text/event-stream; charset=utf-8\r\n";
        header += "Cache-Control: no-cache\r\n";
        header += "Connection: keep-alive\r\n";
        const QByteArray sseOrigin = m_origins.value(socket);
        if (!sseOrigin.isEmpty() && isLoopbackOrigin(sseOrigin))
            header += "Access-Control-Allow-Origin: " + sseOrigin + "\r\n";
        header += "\r\n";
        socket->write(header);
        socket->write("event: status\ndata: " + statusJson() + "\n\n");
        socket->flush();
        m_subscribers.insert(socket);
        return;
    }
    if (path == QLatin1String("/help")) {
        QJsonObject h;
        h.insert(QStringLiteral("endpoints"), QJsonArray::fromStringList({
            QStringLiteral("/status"), QStringLiteral("/lyric"), QStringLiteral("/subscribe-player-status"),
            QStringLiteral("/play"), QStringLiteral("/pause"), QStringLiteral("/toggle"),
            QStringLiteral("/skip-next"), QStringLiteral("/skip-prev"),
            QStringLiteral("/seek?time=<ms>"), QStringLiteral("/volume?value=<0-100>"),
            QStringLiteral("/search?keyword=<text>&source=<id>"),
            QStringLiteral("/queue"), QStringLiteral("/playlist"), QStringLiteral("/favorites")
        }));
        respond(socket, 200, QStringLiteral("application/json; charset=utf-8"),
                QJsonDocument(h).toJson(QJsonDocument::Compact));
        return;
    }
    if (path == QLatin1String("/queue") || path == QLatin1String("/playlist") ||
        path == QLatin1String("/favorites")) {
        QJsonArray arr;
        const QVector<Song> *songs = nullptr;
        if (path == QLatin1String("/queue")) songs = &app->queue()->songs();
        if (songs) {
            for (const Song &s : *songs) {
                QJsonObject o;
                o.insert(QStringLiteral("name"), s.title);
                o.insert(QStringLiteral("singer"), s.artist);
                o.insert(QStringLiteral("albumName"), s.album);
                o.insert(QStringLiteral("source"), s.source);
                o.insert(QStringLiteral("duration"), s.duration);
                arr.append(o);
            }
        } else if (path == QLatin1String("/playlist")) {
            for (const Playlist &p : app->playlists()->playlists()) {
                QJsonObject o;
                o.insert(QStringLiteral("id"), p.uid);
                o.insert(QStringLiteral("name"), p.title);
                o.insert(QStringLiteral("count"), p.count);
                o.insert(QStringLiteral("source"), p.source);
                arr.append(o);
            }
        } else {
            for (const Song &s : Library::instance()->favorites()) {
                QJsonObject o;
                o.insert(QStringLiteral("name"), s.title);
                o.insert(QStringLiteral("singer"), s.artist);
                arr.append(o);
            }
        }
        respond(socket, 200, QStringLiteral("application/json; charset=utf-8"),
                QJsonDocument(arr).toJson(QJsonDocument::Compact));
        return;
    }

    QString msg = QStringLiteral("OK");
    if (path == QLatin1String("/play")) {
        if (!app->playing()) app->togglePause();
    } else if (path == QLatin1String("/pause")) {
        if (app->playing()) app->togglePause();
    } else if (path == QLatin1String("/toggle")) {
        app->togglePause();
    } else if (path == QLatin1String("/skip-next")) {
        app->next();
    } else if (path == QLatin1String("/skip-prev")) {
        app->previous();
    } else if (path == QLatin1String("/seek")) {
        app->seek(query.queryItemValue(QStringLiteral("time")).toLongLong());
    } else if (path == QLatin1String("/volume")) {
        app->setVolume(query.queryItemValue(QStringLiteral("value")).toInt());
    } else if (path == QLatin1String("/search")) {
        app->search(query.queryItemValue(QStringLiteral("keyword")),
                    query.queryItemValue(QStringLiteral("source")));
        msg = QStringLiteral("search started");
    } else {
        respond(socket, 404, QStringLiteral("application/json; charset=utf-8"),
                "{\"error\":\"unknown endpoint, see /help\"}");
        return;
    }
    QJsonObject result;
    result.insert(QStringLiteral("result"), msg);
    respond(socket, 200, QStringLiteral("application/json; charset=utf-8"),
            QJsonDocument(result).toJson(QJsonDocument::Compact));
}

void OpenApiServer::pushSubscribe()
{
    if (m_subscribers.isEmpty()) return;
    const QByteArray data = "event: status\ndata: " + statusJson() + "\n\n";
    for (auto it = m_subscribers.begin(); it != m_subscribers.end();) {
        QTcpSocket *s = *it;
        if (!s || s->state() != QAbstractSocket::ConnectedState) {
            it = m_subscribers.erase(it);
            continue;
        }
        s->write(data);
        s->flush();
        ++it;
    }
}

} // namespace Furui
