#include "core/QtCompat.h"
#include "OpenApiServer.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVector>
#include <algorithm>

#include "AppController.h"
#include "Library.h"
#include "Logger.h"
#include "OpenApiWeb.h"

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
//
// 本机服务自己托管的网页（可能是局域网 IP 打开，如 http://192.168.1.7:23330）
// 属于同源请求，必须放行，否则网页控制台里的播放控制会一律 403。
QString hostHeaderOf(const QByteArray &request)
{
    return QString::fromUtf8(headerValue(request, "Host")).trimmed();
}

bool sameHostAsRequest(const QByteArray &urlLike, const QByteArray &request)
{
    if (urlLike.isEmpty()) return false;
    const QUrl u(QString::fromUtf8(urlLike));
    if (!u.isValid() || u.host().isEmpty()) return false;
    QString want = u.host();
    const int port = u.port(-1);
    if (port > 0) want += QLatin1Char(':') + QString::number(port);
    const QString host = hostHeaderOf(request);
    return !host.isEmpty() && want.compare(host, Qt::CaseInsensitive) == 0;
}

bool trustedOrigin(const QByteArray &value, const QByteArray &request)
{
    if (value.isEmpty()) return true;
    if (value == "null") return true;             // file:// 页面
    if (isLoopbackOrigin(value)) return true;
    return sameHostAsRequest(value, request);     // 本服务自己托管的网页
}

bool untrustedBrowserRequest(const QByteArray &request)
{
    const QByteArray origin = headerValue(request, "Origin");
    const QByteArray referer = headerValue(request, "Referer");
    if (origin.isEmpty() && referer.isEmpty()) return false;
    if (!origin.isEmpty() && !trustedOrigin(origin, request)) return true;
    if (!referer.isEmpty() && !trustedOrigin(referer, request)) return true;
    return false;
}

bool isControlPath(const QString &path)
{
    static const QSet<QString> controls {
        QStringLiteral("/play"), QStringLiteral("/pause"), QStringLiteral("/toggle"),
        QStringLiteral("/skip-next"), QStringLiteral("/skip-prev"), QStringLiteral("/play-index"),
        QStringLiteral("/seek"), QStringLiteral("/volume"), QStringLiteral("/search"),
        // 网页控制台的曲库操作：点歌、打开歌单/榜单、刷新歌单
        QStringLiteral("/play-result"), QStringLiteral("/open-playlist"),
        QStringLiteral("/open-discovery"), QStringLiteral("/refresh"),
        QStringLiteral("/refresh-discovery")
    };
    return controls.contains(path);
}

// ---- 网页控制台的数据组装 ----
QJsonObject songObject(AppController *app, const Song &s, int index = -1)
{
    QJsonObject o;
    if (index >= 0) o.insert(QStringLiteral("index"), index);
    o.insert(QStringLiteral("name"), s.title);
    o.insert(QStringLiteral("singer"), s.artist);
    o.insert(QStringLiteral("albumName"), s.album);
    o.insert(QStringLiteral("cover"), s.cover);
    o.insert(QStringLiteral("source"), s.source);
    o.insert(QStringLiteral("sourceName"), app->sourceName(s.source));
    o.insert(QStringLiteral("duration"), double(s.duration));
    o.insert(QStringLiteral("vip"), s.vip);
    return o;
}

QJsonArray playlistArray(AppController *app, const QVector<Playlist> &list)
{
    QJsonArray arr;
    for (const Playlist &p : list) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), p.uid);
        o.insert(QStringLiteral("name"), p.title);
        o.insert(QStringLiteral("count"), p.count);
        o.insert(QStringLiteral("source"), p.source);
        o.insert(QStringLiteral("sourceName"), app->sourceName(p.source));
        o.insert(QStringLiteral("cover"), p.cover);
        o.insert(QStringLiteral("owner"), p.owner);
        arr.append(o);
    }
    return arr;
}

QByteArray jsonArrayBody(const QJsonArray &arr)
{
    return QJsonDocument(arr).toJson(QJsonDocument::Compact);
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

QString OpenApiServer::localAddress() const
{
    if (!running()) return QString();
    return QStringLiteral("http://127.0.0.1:%1").arg(m_port);
}

QString OpenApiServer::lanAddress() const
{
    if (!running() || m_localOnly) return QString();

    // 只按枚举顺序取第一个可用地址会挑到虚拟网卡（VMware / Hyper-V / WSL / Docker
    // 的 192.168.x.1），那个地址局域网里别的设备根本连不上。这里按「像不像真实
    // 局域网」打分：常见私网段优先，虚拟网卡降权，无线/有线物理网卡加权。
    struct Candidate { QString ip; int score; };
    QVector<Candidate> candidates;
    const QList<QNetworkInterface> ifaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &ni : ifaces) {
        const QNetworkInterface::InterfaceFlags flags = ni.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)) continue;
        if (flags & QNetworkInterface::IsLoopBack) continue;
        const QString name = ni.name().toLower();
        const QString human = ni.humanReadableName().toLower();
        static const char *kVirtualHints[] = {
            "vethernet", "vmware", "virtualbox", "hyper-v", "docker", "wsl",
            "br-", "veth", "virbr", "tap", "tun", "tailscale", "zerotier", "npcap"
        };
        bool virtualIf = false;
        for (const char *hint : kVirtualHints) {
            const QString h = QLatin1String(hint);
            if (name.contains(h) || human.contains(h)) { virtualIf = true; break; }
        }
        for (const QNetworkAddressEntry &entry : ni.addressEntries()) {
            const QHostAddress addr = entry.ip();
            if (addr.protocol() != QAbstractSocket::IPv4Protocol || addr.isLoopback()) continue;
            const QString ip = addr.toString();
            if (ip.startsWith(QLatin1String("169.254."))) continue;   // 链路本地，连不到
            int score = 10;
            if (ip.startsWith(QLatin1String("192.168."))) score = 40;
            else if (ip.startsWith(QLatin1String("10."))) score = 30;
            else if (ip.startsWith(QLatin1String("172."))) score = 20;
            if (virtualIf) score -= 25;
            if (name.startsWith(QLatin1String("wlan")) || name.startsWith(QLatin1String("wlp"))
                || name.startsWith(QLatin1String("wl")) || name.startsWith(QLatin1String("ap"))
                || name.startsWith(QLatin1String("eth")) || name.startsWith(QLatin1String("en")))
                score += 15;
            candidates.append({ ip, score });
        }
    }
    if (candidates.isEmpty()) return QString();
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate &a, const Candidate &b) { return a.score > b.score; });
    return QStringLiteral("http://%1:%2").arg(candidates.first().ip).arg(m_port);
}

QString OpenApiServer::address() const
{
    const QString lan = lanAddress();
    return lan.isEmpty() ? localAddress() : lan;
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
    // 不再弹「已启动 + 无鉴权警告」：地址在设置页常驻显示，网页里也能看到，
    // 每次开关都弹一遍纯属噪音
    Logger::write(QStringLiteral("I"),
                  QStringLiteral("开放 API 已启动：%1（%2）")
                      .arg(address(), localOnly ? QStringLiteral("仅本机")
                                                : QStringLiteral("局域网可访问")));
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
    o.insert(QStringLiteral("cover"), app->currentCover());
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
                            const QByteArray &body, const QByteArray &extraHeaders)
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
    if (!extraHeaders.isEmpty()) {
        header += extraHeaders;
        if (!extraHeaders.endsWith("\r\n")) header += "\r\n";
    }
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
    // 网页控制台：局域网设备（手机 / 平板 / 电脑）直接打开 http://<IP>:23330 即可
    if (path == QLatin1String("/") || path == QLatin1String("/index.html")) {
        respond(socket, 200, QStringLiteral("text/html; charset=utf-8"), openApiIndexHtml());
        return;
    }
    if (path == QLatin1String("/favicon.ico")) {
        respond(socket, 204, QStringLiteral("text/plain"), QByteArray());
        return;
    }
    if (path == QLatin1String("/logs")) {
        int lines = query.queryItemValue(QStringLiteral("lines")).toInt();
        if (lines <= 0) lines = 400;
        lines = qBound(20, lines, 2000);
        QByteArray body = Logger::instance()->recentText(lines).toUtf8();
        if (body.isEmpty()) {
            body = QStringLiteral("（暂无日志：在设置页打开「写入日志文件」可长期保留日志）").toUtf8();
        }
        // download=1 走附件下载：手机上直接把日志存成文件发出去更方便
        const bool download = query.queryItemValue(QStringLiteral("download")) == QLatin1String("1");
        respond(socket, 200, QStringLiteral("text/plain; charset=utf-8"), body,
                download ? QByteArray("Content-Disposition: attachment; filename=\"furui-logs.txt\"\r\n")
                         : QByteArray());
        return;
    }
    if (path == QLatin1String("/status")) {
        respond(socket, 200, QStringLiteral("application/json; charset=utf-8"), statusJson());
        return;
    }
    // ---- 网页控制台用的读接口 ----
    if (path == QLatin1String("/playlists") || path == QLatin1String("/discovery")) {
        const QVector<Playlist> &list = (path == QLatin1String("/discovery"))
                                            ? app->discovery()->playlists()
                                            : app->playlists()->playlists();
        respond(socket, 200, QStringLiteral("application/json; charset=utf-8"),
                jsonArrayBody(playlistArray(app, list)));
        return;
    }
    if (path == QLatin1String("/results")) {
        QJsonObject o;
        o.insert(QStringLiteral("count"), app->results()->songs().size());
        QJsonArray songs;
        const QVector<Song> &list = app->results()->songs();
        for (int i = 0; i < list.size(); ++i) songs.append(songObject(app, list.at(i), i));
        o.insert(QStringLiteral("songs"), songs);
        respond(socket, 200, QStringLiteral("application/json; charset=utf-8"),
                QJsonDocument(o).toJson(QJsonDocument::Compact));
        return;
    }
    if (path == QLatin1String("/accounts")) {
        QJsonArray arr;
        for (const Account &a : app->accounts()->accounts()) {
            QJsonObject o;
            o.insert(QStringLiteral("source"), a.source);
            o.insert(QStringLiteral("sourceName"), app->sourceName(a.source));
            o.insert(QStringLiteral("nickname"), a.nickname);
            o.insert(QStringLiteral("avatar"), a.avatar);
            o.insert(QStringLiteral("logged"), a.logged);
            o.insert(QStringLiteral("status"), a.status);
            arr.append(o);
        }
        respond(socket, 200, QStringLiteral("application/json; charset=utf-8"), jsonArrayBody(arr));
        return;
    }
    if (path == QLatin1String("/settings")) {
        QJsonObject o;
        o.insert(QStringLiteral("theme"), app->theme());
        o.insert(QStringLiteral("quality"), qualityName(qualityFromKey(app->quality())));
        o.insert(QStringLiteral("playMode"), app->playModeName());
        o.insert(QStringLiteral("vipSubstitute"), app->vipSubstituteEnabled());
        o.insert(QStringLiteral("autoToggle"), app->autoToggleSource());
        o.insert(QStringLiteral("openApi"), running());
        o.insert(QStringLiteral("address"), address());
        o.insert(QStringLiteral("localOnly"), app->openApiLocalOnly());
        o.insert(QStringLiteral("logToFile"), app->logToFile());
        o.insert(QStringLiteral("logPath"), app->logFilePath());
        o.insert(QStringLiteral("appName"), app->appName());
        o.insert(QStringLiteral("version"), app->version());
        o.insert(QStringLiteral("sources"), app->sourceNames().join(QStringLiteral(" / ")));
        respond(socket, 200, QStringLiteral("application/json; charset=utf-8"),
                QJsonDocument(o).toJson(QJsonDocument::Compact));
        return;
    }
    if (path == QLatin1String("/lyrics")) {
        // 带时间轴的歌词给网页播放页做逐行高亮；没有时间轴时前端只显示静态文本
        QJsonObject o;
        const QStringList texts = app->lyricLines();
        const QVariantList times = app->lyricTimes();
        o.insert(QStringLiteral("timed"), app->lyricsTimed());
        o.insert(QStringLiteral("index"), app->currentLyricIndex());
        o.insert(QStringLiteral("current"), app->currentLyricLine());
        QJsonArray lines;
        for (int i = 0; i < texts.size(); ++i) {
            QJsonObject l;
            l.insert(QStringLiteral("t"), i < times.size() ? double(times.at(i).toLongLong()) : 0.0);
            l.insert(QStringLiteral("text"), texts.at(i));
            lines.append(l);
        }
        o.insert(QStringLiteral("lines"), lines);
        respond(socket, 200, QStringLiteral("application/json; charset=utf-8"),
                QJsonDocument(o).toJson(QJsonDocument::Compact));
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
            QStringLiteral("/queue"), QStringLiteral("/playlist"), QStringLiteral("/favorites"),
            QStringLiteral("/play-index?index=<n>"), QStringLiteral("/logs?lines=<n>&download=1"),
            QStringLiteral("/playlists"), QStringLiteral("/discovery"), QStringLiteral("/results"),
            QStringLiteral("/accounts"), QStringLiteral("/settings"), QStringLiteral("/lyrics"),
            QStringLiteral("/history"), QStringLiteral("/play-result?index=<n>"),
            QStringLiteral("/open-playlist?id=<uid>"), QStringLiteral("/open-discovery?id=<uid>"),
            QStringLiteral("/refresh"), QStringLiteral("/refresh-discovery"),
            QStringLiteral("/")
        }));
        respond(socket, 200, QStringLiteral("application/json; charset=utf-8"),
                QJsonDocument(h).toJson(QJsonDocument::Compact));
        return;
    }
    if (path == QLatin1String("/queue") || path == QLatin1String("/playlist") ||
        path == QLatin1String("/favorites") || path == QLatin1String("/history")) {
        if (path == QLatin1String("/playlist")) {
            respond(socket, 200, QStringLiteral("application/json; charset=utf-8"),
                    jsonArrayBody(playlistArray(app, app->playlists()->playlists())));
            return;
        }
        QVector<Song> songs;
        if (path == QLatin1String("/queue")) songs = app->queue()->songs();
        else if (path == QLatin1String("/favorites")) songs = Library::instance()->favorites();
        else songs = Library::instance()->history(200);
        QJsonArray arr;
        const int current = path == QLatin1String("/queue") ? app->currentQueueIndex() : -1;
        for (int i = 0; i < songs.size(); ++i) {
            QJsonObject o = songObject(app, songs.at(i), i);
            if (path == QLatin1String("/queue")) o.insert(QStringLiteral("current"), i == current);
            arr.append(o);
        }
        respond(socket, 200, QStringLiteral("application/json; charset=utf-8"), jsonArrayBody(arr));
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
    } else if (path == QLatin1String("/play-index")) {
        // 网页队列里点某一行：按队列下标播放，越界直接拒绝（不要静默播第 0 首）
        const int index = query.queryItemValue(QStringLiteral("index")).toInt();
        if (index < 0 || index >= app->queue()->songs().size()) {
            respond(socket, 400, QStringLiteral("application/json; charset=utf-8"),
                    "{\"error\":\"index out of range\"}");
            return;
        }
        app->playQueue(index);
        msg = QStringLiteral("playing index ") + QString::number(index);
    } else if (path == QLatin1String("/play-result")) {
        // 网页搜索结果 / 歌单详情里点某一行：按当前结果集下标播放
        const int index = query.queryItemValue(QStringLiteral("index")).toInt();
        if (index < 0 || index >= app->results()->songs().size()) {
            respond(socket, 400, QStringLiteral("application/json; charset=utf-8"),
                    "{\"error\":\"index out of range\"}");
            return;
        }
        app->playResult(index);
        msg = QStringLiteral("playing result ") + QString::number(index);
    } else if (path == QLatin1String("/open-playlist") ||
               path == QLatin1String("/open-discovery")) {
        const QString uid = query.queryItemValue(QStringLiteral("id"));
        if (uid.isEmpty()) {
            respond(socket, 400, QStringLiteral("application/json; charset=utf-8"),
                    "{\"error\":\"missing id\"}");
            return;
        }
        if (path == QLatin1String("/open-playlist")) app->loadPlaylist(uid);
        else app->openDiscoveryPlaylist(uid);
        msg = QStringLiteral("loading playlist");
    } else if (path == QLatin1String("/refresh") ||
               path == QLatin1String("/refresh-discovery")) {
        if (path == QLatin1String("/refresh")) app->refreshPlaylists();
        else app->loadDiscovery(QString());
        msg = QStringLiteral("refreshing");
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
