#include "HttpClient.h"

#include <QNetworkCookie>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrlQuery>

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

void HttpClient::setProxyFromSettings()
{
    const QString proxy = Settings::string(QStringLiteral("proxy"));
    if (proxy.trimmed().isEmpty()) {
        m_nam.setProxy(QNetworkProxy::NoProxy);
        return;
    }
    QUrl u(proxy);
    if (u.host().isEmpty()) u = QUrl(QStringLiteral("http://") + proxy);
    QNetworkProxy p(QNetworkProxy::HttpProxy, u.host(), u.port(8080));
    if (!u.userName().isEmpty()) {
        p.setUser(u.userName());
        p.setPassword(u.password());
    }
    m_nam.setProxy(p);
}

void HttpClient::request(const QString &method, const QUrl &url, const QVariantMap &headers,
                         const QByteArray &body, const Callback &cb, int timeoutMs,
                         bool followRedirects)
{
    QNetworkRequest req(url);
    // Qt 6.11 已移除 FollowRedirectsAttribute，RedirectPolicyAttribute 在 Qt 5.9+ / Qt 6 均可用
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     followRedirects ? QNetworkRequest::NoLessSafeRedirectPolicy
                                     : QNetworkRequest::ManualRedirectPolicy);
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
        r.json = Json::document(r.body);   // JsonMapper 通过 Types.h 间接可见
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
        r.error = (reply->error() == QNetworkReply::NoError)
                      ? QString() : reply->errorString();
        r.ok = (reply->error() == QNetworkReply::NoError) && (r.status == 0 || r.status < 400);
        reply->deleteLater();
        finish(r);
    });
}

} // namespace Furui
