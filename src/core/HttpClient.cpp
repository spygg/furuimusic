#include "core/QtCompat.h"
#include "HttpClient.h"

#include <QElapsedTimer>
#include <QDebug>
#include <QNetworkCookie>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSaveFile>
#include <QTimer>
#include <QUrlQuery>

#include <cstdio>
#include <memory>

#include "JsonMapper.h"
#include "Settings.h"

namespace Furui {

HttpClient *HttpClient::instance()
{
    static HttpClient *c = new HttpClient();
    return c;
}

HttpClient::HttpClient(QObject *parent)
    : QObject(parent)
{
    setProxyFromSettings();
}

QNetworkProxy HttpClient::proxyFromSettings()
{
    const QString proxy = Settings::string(QStringLiteral("proxy")).trimmed();
    if (proxy.isEmpty())
        return QNetworkProxy(QNetworkProxy::NoProxy);
    QUrl u(proxy);
    if (u.host().isEmpty()) u = QUrl(QStringLiteral("http://") + proxy);
    QNetworkProxy p(QNetworkProxy::HttpProxy, u.host(), u.port(8080));
    if (!u.userName().isEmpty()) {
        p.setUser(u.userName());
        p.setPassword(u.password());
    }
    return p;
}

void HttpClient::applyApplicationProxyFromSettings()
{
    // setApplicationProxy 同时会关掉系统代理探测：Android 上部分设备会返回
    // 无效的 ProxyInfo，导致封面/音频请求全部报 “Proxy connection refused”。
    QNetworkProxy::setApplicationProxy(proxyFromSettings());
}

void HttpClient::setProxyFromSettings()
{
    const QNetworkProxy p = proxyFromSettings();
    m_nam.setProxy(p);
    // 用户改完设置即时生效：QML 侧后续新建的请求也会走同一策略
    QNetworkProxy::setApplicationProxy(p);
}

void HttpClient::request(const QString &method, const QUrl &url, const QVariantMap &headers,
                         const QByteArray &body, const Callback &cb, int timeoutMs,
                         bool followRedirects, const QString &peerVerifyName, qint64 bodyLimit)
{
    QNetworkRequest req(url);
    // 只改「证书应该签给谁」这一项：证书链、有效期、根 CA 校验全部照常。
    // 酷狗 login.user.kugou.com 全 IP 都下发 CN=*.kugou.com 的单级通配证书，
    // 按 RFC 6125 不匹配二级子域，Qt 会直接 SSL handshake failed。连接本身仍走
    // 原域名（Host/SNI 不变），这里把校验名降到证书确实覆盖的 kugou.com。
    // setPeerVerifyName 是 Qt 5.14+ API：老 Qt 5 只能按原域名校验，
    // 酷狗那个 CN=*.kugou.com 的端点在 Qt 5.7 上会 SSL 失败并有明确报错。
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    if (!peerVerifyName.isEmpty() && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0)
        req.setPeerVerifyName(peerVerifyName);
#else
    Q_UNUSED(peerVerifyName);
#endif
    // Qt 6.11 已移除 FollowRedirectsAttribute，RedirectPolicyAttribute 在 Qt 5.9+ / Qt 6 均可用；
    // Qt 5.7/5.8 只有布尔版 FollowRedirectsAttribute，语义等价地降级。
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     followRedirects ? QNetworkRequest::NoLessSafeRedirectPolicy
                                     : QNetworkRequest::ManualRedirectPolicy);
#else
    req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, followRedirects);
#endif
    // Cookie 全部由业务层手动管理：AuthManager/MusicSource 会从 Set-Cookie 解析并
    // 自己拼 "Cookie" 头。QNAM 默认 cookieJar 会再自动附加一份，形成重复/畸形 Cookie 头，
    // 网易云对这种请求直接回 -462「检测到您的网络环境存在风险」（Android/Qt 6 尤其明显）。
    // 关掉自动加载/保存后，手动头是唯一来源，Set-Cookie 响应头仍照常可读。
    req.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                     QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                     QNetworkRequest::Manual);
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        req.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());

    const QString m = method.toUpper();
    QNetworkReply *reply = nullptr;
    if (m == QLatin1String("POST")) {
        if (!req.hasRawHeader("Content-Type"))
            req.setRawHeader("Content-Type", "application/x-www-form-urlencoded");
        reply = m_nam.post(req, body);
    } else if (m == QLatin1String("PUT")) {
        reply = m_nam.put(req, body);
    } else {
        reply = m_nam.get(req);
    }

    auto done = std::make_shared<bool>(false);
    auto finish = [done, cb](Reply r) {
        if (*done) return;
        *done = true;
        if (cb) cb(r);
    };

    // bodyLimit 命中时要在 readyRead 里提前投递结果，和 finished 走同一套封装，
    // 保证 headers/setCookies/json/ok 语义与普通响应完全一致。
    auto bodyBuf = std::make_shared<QByteArray>();
    auto makeReply = [](QNetworkReply *reply, const QByteArray &bytes) {
        Reply r;
        r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r.body = bytes;
        r.json = Json::document(r.body);
        for (const QByteArray &h : reply->rawHeaderList()) {
            const QString key = QString::fromUtf8(h).toLower();
            r.headers.insert(key, QString::fromUtf8(reply->rawHeader(h)));
            if (key == QLatin1String("set-cookie"))
                r.setCookies << QString::fromUtf8(reply->rawHeader(h));
        }
        const QVariant cookieVar = reply->header(QNetworkRequest::SetCookieHeader);
        if (cookieVar.isValid()) {
            for (const QNetworkCookie &c : cookieVar.value<QList<QNetworkCookie>>())
                r.setCookies << QString::fromUtf8(c.toRawForm());
        }
        r.setCookies.removeDuplicates();
        r.error = (reply->error() == QNetworkReply::NoError)
                      ? QString() : reply->errorString();
        r.ok = (reply->error() == QNetworkReply::NoError) && (r.status == 0 || r.status < 400);
        return r;
    };

    // FURUI_HTTP_TRACE=1：把每个请求的发出/结束/超时打到 stderr，用于定位
    // 「请求发出去了但一直不回包」这类问题（默认完全不输出）。
    const bool trace = qEnvironmentVariableIntValue("FURUI_HTTP_TRACE") > 0;
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    if (trace)
        std::fprintf(stderr, "[http] -> %s %s body=%lld headers=%d\n",
                     qPrintable(m), qPrintable(url.toString(QUrl::RemoveQuery)),
                     (long long)body.size(), headers.size());

    QPointer<QNetworkReply> guard(reply);
    // 设置里填 0/负数会让 QTimer 变成 0ms 定时器，请求瞬间「超时」；至少给 1s。
    if (timeoutMs < 0) timeoutMs = qMax(1, Settings::integer(QStringLiteral("timeout"), 12)) * 1000;
    QTimer::singleShot(timeoutMs, this, [guard, finish, trace, clock]() {
        if (!guard) return;
        if (trace)
            std::fprintf(stderr, "[http] !! timeout elapsed=%lldms\n",
                         (long long)clock->elapsed());
        // 先投递结果再 abort：abort() 可能同步触发 finished，若先 abort，
        // 回调会带 "Operation canceled" 抢先占住 done，真正的「请求超时」被吞掉。
        Reply r;
        r.error = QStringLiteral("请求超时");
        finish(r);
        guard->abort();
    });

    if (bodyLimit >= 0) {
        connect(reply, &QNetworkReply::readyRead, this,
                [reply, guard, done, finish, makeReply, bodyBuf, bodyLimit]() {
            if (*done || !guard) return;
            const qint64 remaining = bodyLimit - bodyBuf->size();
            if (remaining > 0)
                bodyBuf->append(reply->read(remaining));
            if (bodyBuf->size() >= bodyLimit) {
                // 先投递截断结果再 abort：abort 触发的 finished 会在 done 门禁处直接返回
                finish(makeReply(reply, *bodyBuf));
                guard->abort();
            }
        });
    }

    connect(reply, &QNetworkReply::finished, this,
            [reply, done, finish, makeReply, bodyBuf, bodyLimit, trace, clock]() {
        // 超时/限流已经投递过结果，abort() 又同步触发了 finished；此时再 readAll()
        // 会打印 "QIODevice::read (QNetworkReply): device not open"，且可能覆盖已投递的结果。
        if (*done || reply->error() == QNetworkReply::OperationCanceledError) {
            reply->deleteLater();
            return;
        }
        QByteArray bytes;
        if (bodyLimit >= 0) {
            const qint64 remaining = bodyLimit - bodyBuf->size();
            if (remaining > 0) bodyBuf->append(reply->read(remaining));
            bytes = *bodyBuf;
        } else {
            bytes = reply->readAll();
        }
        // FURUI_PERF=1：把 GUI 线程上的解析耗时打出来。要不要挪去线程池取决于这条数据，
        // 不靠猜——几十 KB 的搜索结果解析不到 1ms，为它引入跨线程回调生命周期风险不值。
        QElapsedTimer parseClock;
        const bool perfLog = qEnvironmentVariableIsSet("FURUI_PERF");
        if (perfLog) parseClock.start();
        Reply r = makeReply(reply, bytes);   // JsonMapper 通过 Types.h 间接可见
        if (trace)
            std::fprintf(stderr, "[http] <- elapsed=%lldms status=%d error=%d %s body=%lld\n",
                         (long long)clock->elapsed(), r.status, int(reply->error()),
                         qPrintable(reply->errorString()), (long long)r.body.size());
        if (perfLog && parseClock.elapsed() >= 8)
            qWarning().noquote() << QStringLiteral("[perf] json parse %1ms body=%2KB %3")
                                        .arg(parseClock.elapsed())
                                        .arg(r.body.size() / 1024)
                                        .arg(reply->url().host());
        reply->deleteLater();
        finish(r);
    });
}

QNetworkReply *HttpClient::download(const QUrl &url, const QVariantMap &headers,
                          const QString &destPath, const DownloadCallback &cb,
                          int timeoutMs, const DownloadProgressCallback &progress)
{
    QNetworkRequest req(url);
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
#else
    req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
#endif
    // Cookie 全部由业务层手动管理：AuthManager/MusicSource 会从 Set-Cookie 解析并
    // 自己拼 "Cookie" 头。QNAM 默认 cookieJar 会再自动附加一份，形成重复/畸形 Cookie 头，
    // 网易云对这种请求直接回 -462「检测到您的网络环境存在风险」（Android/Qt 6 尤其明显）。
    // 关掉自动加载/保存后，手动头是唯一来源，Set-Cookie 响应头仍照常可读。
    req.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                     QNetworkRequest::Manual);
    req.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                     QNetworkRequest::Manual);
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        req.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());

    auto file = std::make_shared<QSaveFile>(destPath);
    if (!file->open(QIODevice::WriteOnly)) {
        if (cb)
            cb(false, QStringLiteral("无法写入下载文件"));
        return nullptr;
    }

    QNetworkReply *reply = m_nam.get(req);

    auto done = std::make_shared<bool>(false);
    auto finish = [done, cb, file](bool ok, const QString &error) {
        if (*done) return;
        *done = true;
        if (!ok) {
            file->cancelWriting();
            if (cb) cb(false, error);
            return;
        }
        // commit() 成功才代表文件真正替换到位，中途失败不会留下半个文件
        if (!file->commit()) {
            if (cb) cb(false, QStringLiteral("写入下载文件失败"));
            return;
        }
        if (cb) cb(true, QString());
    };

    QPointer<QNetworkReply> guard(reply);
    if (timeoutMs < 0)
        timeoutMs = qMax(1, Settings::integer(QStringLiteral("timeout"), 12)) * 1000;
    QTimer::singleShot(timeoutMs, this, [guard, finish]() {
        if (!guard) return;
        // 与 request() 一致：先投递结果再 abort。abort() 可能同步触发 finished，
        // 若先 abort，回调会带 "Operation canceled" 抢先占住 done，真正的「下载超时」被吞掉。
        finish(false, QStringLiteral("下载超时"));
        guard->abort();
    });

    connect(reply, &QNetworkReply::readyRead, this, [reply, file, done]() {
        // 超时已经 cancelWriting() 后 abort() 仍可能送来 readyRead/finished，
        // 往已取消的 QSaveFile 里写只会浪费磁盘 I/O
        if (*done) return;
        file->write(reply->readAll());
    });

    if (progress) {
        connect(reply, &QNetworkReply::downloadProgress, this,
                [progress](qint64 received, qint64 total) { progress(received, total); });
    }

    connect(reply, &QNetworkReply::finished, this, [reply, finish, file, done]() {
        if (!*done)
            file->write(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError
                        && (status == 0 || status < 400);
        QString error;
        if (!ok) {
            error = reply->error() == QNetworkReply::NoError
                        ? QStringLiteral("HTTP %1").arg(status)
                        : reply->errorString();
        }
        reply->deleteLater();
        finish(ok, error);
    });
    return reply;
}

QNetworkReply *HttpClient::openStream(const QNetworkRequest &req)
{
    // 与 request()/download() 共用 m_nam（已按设置配好代理），只裸转发：
    // 不设 body 缓冲、不挂超时定时器、不碰 cookie jar（防盗链头由 OpenApiServer 预先
    // 放进 req）。调用方（/stream）全权管理 reply 生命周期与背压。
    return m_nam.get(req);
}

} // namespace Furui
