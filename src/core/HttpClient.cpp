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
                         bool followRedirects, const QString &peerVerifyName)
{
    QNetworkRequest req(url);
    // 只改「证书应该签给谁」这一项：证书链、有效期、根 CA 校验全部照常。
    // 酷狗 login.user.kugou.com 全 IP 都下发 CN=*.kugou.com 的单级通配证书，
    // 按 RFC 6125 不匹配二级子域，Qt 会直接 SSL handshake failed。连接本身仍走
    // 原域名（Host/SNI 不变），这里把校验名降到证书确实覆盖的 kugou.com。
    if (!peerVerifyName.isEmpty() && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0)
        req.setPeerVerifyName(peerVerifyName);
    // Qt 6.11 已移除 FollowRedirectsAttribute，RedirectPolicyAttribute 在 Qt 5.9+ / Qt 6 均可用
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     followRedirects ? QNetworkRequest::NoLessSafeRedirectPolicy
                                     : QNetworkRequest::ManualRedirectPolicy);
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

    QPointer<QNetworkReply> guard(reply);
    if (timeoutMs < 0) timeoutMs = Settings::integer(QStringLiteral("timeout"), 12) * 1000;
    QTimer::singleShot(timeoutMs, this, [guard, finish]() {
        if (!guard) return;
        guard->abort();
        Reply r;
        r.error = QStringLiteral("请求超时");
        finish(r);
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, finish]() {
        Reply r;
        r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r.body = reply->readAll();
        // FURUI_PERF=1：把 GUI 线程上的解析耗时打出来。要不要挪去线程池取决于这条数据，
        // 不靠猜——几十 KB 的搜索结果解析不到 1ms，为它引入跨线程回调生命周期风险不值。
        QElapsedTimer parseClock;
        const bool perfLog = qEnvironmentVariableIsSet("FURUI_PERF");
        if (perfLog) parseClock.start();
        r.json = Json::document(r.body);   // JsonMapper 通过 Types.h 间接可见
        if (perfLog && parseClock.elapsed() >= 8)
            qWarning().noquote() << QStringLiteral("[perf] json parse %1ms body=%2KB %3")
                                        .arg(parseClock.elapsed())
                                        .arg(r.body.size() / 1024)
                                        .arg(reply->url().host());
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
        // rawHeader 与 SetCookieHeader 是同一批 Cookie 的两种视图，去重后再交给业务层，
        // 避免拼接头里出现重复键（网易对重复/空值 Cookie 会直接风控 -462）
        r.setCookies.removeDuplicates();
        r.error = (reply->error() == QNetworkReply::NoError)
                      ? QString() : reply->errorString();
        r.ok = (reply->error() == QNetworkReply::NoError) && (r.status == 0 || r.status < 400);
        reply->deleteLater();
        finish(r);
    });
}

void HttpClient::download(const QUrl &url, const QVariantMap &headers,
                          const QString &destPath, const DownloadCallback &cb,
                          int timeoutMs)
{
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
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
        return;
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
        timeoutMs = Settings::integer(QStringLiteral("timeout"), 12) * 1000;
    QTimer::singleShot(timeoutMs, this, [guard, finish]() {
        if (!guard) return;
        guard->abort();
        finish(false, QStringLiteral("下载超时"));
    });

    connect(reply, &QNetworkReply::readyRead, this, [reply, file]() {
        file->write(reply->readAll());
    });

    connect(reply, &QNetworkReply::finished, this, [reply, finish, file]() {
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
}

} // namespace Furui
