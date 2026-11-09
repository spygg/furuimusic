#pragma once
// 福瑞音乐 - 异步 HTTP 客户端（全平台共用）
#include <QByteArray>
#include <QJsonDocument>
#include <QMap>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <functional>

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

    static HttpClient *instance();

    // headers 支持 {placeholder}，由调用方渲染后传入；body 已是最终字节
    // followRedirects=false 时 3xx 原样返回，调用方从 headers["location"] 自行跳转
    // （QQ 扫码登录需要用这条链路把 graph.qq.com 的重定向一路追到带 code 的回调页）
    void request(const QString &method, const QUrl &url, const QVariantMap &headers,
                 const QByteArray &body, const Callback &cb, int timeoutMs = -1,
                 bool followRedirects = true);
    void setProxyFromSettings();

private:
    explicit HttpClient(QObject *parent = nullptr);
    QNetworkAccessManager m_nam;
};

} // namespace Furui
