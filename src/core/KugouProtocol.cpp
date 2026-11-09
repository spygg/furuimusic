#include "KugouProtocol.h"

#include "Types.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QRandomGenerator>
#include <QUrl>
#include <QUrlQuery>

namespace Furui {
namespace Kugou {

namespace {

// 酷狗把 32 位十六进制当设备标识（mid/uuid），服务端不校验其来源，随进程生成一次即可
QString randomHex32()
{
    QString out;
    while (out.size() < 32)
        out += QStringLiteral("%1").arg(quint32(QRandomGenerator::global()->generate()), 8, 16,
                                        QLatin1Char('0'));
    return out.toLower();
}

QString joined(const QVariantMap &params)
{
    QStringList kv;
    for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
        if (it.key() == QLatin1String("signature")) continue;
        kv << (it.key() + QLatin1Char('=') + it.value().toString());
    }
    kv.sort();
    return kv.join(QString());
}

} // namespace

QString keyparam(Keyparam kp)
{
    return kp == Keyparam::App
        ? QStringLiteral("OIlwieks28dk2k092lksi2UIkp")
        : QStringLiteral("NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt");
}

QString webSalt()
{
    return keyparam(Keyparam::Web);
}

QVariantMap baseParams()
{
    static const QString mid = randomHex32();
    return {
        {QStringLiteral("dfid"), QStringLiteral("-")},
        {QStringLiteral("mid"), mid},
        {QStringLiteral("uuid"), QStringLiteral("-")},
        {QStringLiteral("appid"), 1005},
        {QStringLiteral("clientver"), 20489},
        {QStringLiteral("clienttime"), QDateTime::currentSecsSinceEpoch()},
    };
}

void addDeviceVars(QVariantMap *vars)
{
    const QVariantMap base = baseParams();
    for (auto it = base.constBegin(); it != base.constEnd(); ++it) {
        QString key = it.key();
        key[0] = key[0].toUpper();
        vars->insert(QStringLiteral("kg") + key, it.value().toString());
    }
}

void addAuthVars(QVariantMap *vars, const QString &cookie)
{
    vars->insert(QStringLiteral("kgToken"), cookieValue(cookie, {QStringLiteral("token")}));
    QString userid = cookieValue(cookie, {QStringLiteral("userid"), QStringLiteral("userId")});
    if (userid.isEmpty()) userid = vars->value(QStringLiteral("uid")).toString();
    vars->insert(QStringLiteral("kgUserid"), userid);
}

QString sign(const QVariantMap &params, const QByteArray &body, Keyparam kp)
{
    const QByteArray key = keyparam(kp).toUtf8();
    QByteArray raw = key + joined(params).toUtf8() + body + key;
    return QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Md5).toHex());
}

QString signedUrl(const QString &baseUrl, QVariantMap params, const QByteArray &body, Keyparam kp)
{
    params.insert(QStringLiteral("signature"), sign(params, body, kp));
    QUrl url(baseUrl);
    QUrlQuery query;
    for (auto it = params.constBegin(); it != params.constEnd(); ++it)
        query.addQueryItem(it.key(), it.value().toString());
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

QString sessionCookie(const QString &token, const QString &userid)
{
    QStringList parts;
    if (!userid.isEmpty()) parts << QStringLiteral("userid=%1").arg(userid);
    if (!token.isEmpty()) parts << QStringLiteral("token=%1").arg(token);
    return parts.join(QStringLiteral("; "));
}

} // namespace Kugou
} // namespace Furui
