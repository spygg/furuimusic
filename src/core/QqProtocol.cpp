#include "QqProtocol.h"

#include <QRegularExpression>
#include <QJsonArray>
#include <QUrl>
#include <QUrlQuery>

#include "Types.h"

namespace Furui {
namespace Qq {

quint32 hash33(const QString &text)
{
    // 与前端 e += (e << 5) + charCode 等价：移位与累加都按 32 位取模，最后取低 31 位
    quint32 e = 0;
    for (const QChar c : text) {
        const quint32 shifted = e << 5;
        e = e + shifted + quint32(c.unicode());
    }
    return e & 0x7fffffffu;
}

QString gTk(const QString &cookie)
{
    const QString key = cookieValue(cookie, {QStringLiteral("qqmusic_key"),
                                             QStringLiteral("qm_keyst"),
                                             QStringLiteral("p_skey"),
                                             QStringLiteral("skey")});
    if (key.isEmpty()) return QStringLiteral("5381");
    return QString::number(hash33(key));
}

QString numericUin(const QString &uin)
{
    QString digits;
    for (const QChar c : uin)
        if (c.isDigit()) digits.append(c);
    while (digits.size() > 1 && digits.startsWith(QLatin1Char('0'))) digits.remove(0, 1);
    return digits;
}

QStringList ptuiCbFields(const QByteArray &body)
{
    static const QRegularExpression call(QStringLiteral("ptuiCB\\s*\\((.*)\\)\\s*;?\\s*$"),
                                         QRegularExpression::DotMatchesEverythingOption);
    const QString text = QString::fromUtf8(body).trimmed();
    const QRegularExpressionMatch m = call.match(text);
    if (!m.hasMatch()) return QStringList();
    QStringList out;
    static const QRegularExpression quoted(QStringLiteral("'([^']*)'"));
    QRegularExpressionMatchIterator it = quoted.globalMatch(m.captured(1));
    while (it.hasNext()) out << it.next().captured(1);
    return out;
}

QString authCodeFromUrl(const QUrl &url)
{
    static const QStringList names = {QStringLiteral("code"), QStringLiteral("authorization_code")};
    const QList<QPair<QString, QString>> items = QUrlQuery(url).queryItems();
    for (const QString &name : names) {
        for (const QPair<QString, QString> &kv : items) {
            if (kv.first.compare(name, Qt::CaseInsensitive) == 0
                && !kv.second.trimmed().isEmpty())
                return kv.second.trimmed();
        }
    }
    return QString();
}

QJsonObject loginByCodeRequest(const QString &code)
{
    return QJsonObject{
        {QStringLiteral("comm"), QJsonObject{
            {QStringLiteral("g_tk"), 5381},
            {QStringLiteral("format"), QStringLiteral("json")},
            {QStringLiteral("inCharset"), QStringLiteral("utf-8")},
            {QStringLiteral("outCharset"), QStringLiteral("utf-8")},
            {QStringLiteral("notice"), 0},
            {QStringLiteral("platform"), QStringLiteral("yqq.json")},
            {QStringLiteral("needNewCode"), 1},
            {QStringLiteral("ct"), 24},
            {QStringLiteral("cv"), 0}}},
        {QStringLiteral("req"), QJsonObject{
            {QStringLiteral("module"), QStringLiteral("QQConnectLogin.LoginServer")},
            {QStringLiteral("method"), QStringLiteral("QQLogin")},
            {QStringLiteral("param"), QJsonObject{{QStringLiteral("code"), code}}}}}};
}

QJsonObject refreshKeyRequest(const QString &musicid, const QString &musickey)
{
    return QJsonObject{
        {QStringLiteral("comm"), QJsonObject{
            {QStringLiteral("g_tk"), 5381},
            {QStringLiteral("uin"), musicid},
            {QStringLiteral("format"), QStringLiteral("json")},
            {QStringLiteral("inCharset"), QStringLiteral("utf-8")},
            {QStringLiteral("outCharset"), QStringLiteral("utf-8")},
            {QStringLiteral("notice"), 0},
            {QStringLiteral("platform"), QStringLiteral("yqq.json")},
            {QStringLiteral("needNewCode"), 1},
            {QStringLiteral("ct"), 24},
            {QStringLiteral("cv"), 0}}},
        {QStringLiteral("req"), QJsonObject{
            {QStringLiteral("module"), QStringLiteral("QQConnectLogin.LoginServer")},
            {QStringLiteral("method"), QStringLiteral("QQLogin")},
            {QStringLiteral("param"), QJsonObject{
                {QStringLiteral("musicid"), musicid},
                {QStringLiteral("musickey"), musickey},
                {QStringLiteral("expired_in"), 7776000}}}}}};
}

QString sessionCookie(const QString &musicid, const QString &musickey,
                      const QString &openid, const QString &accessToken)
{
    QStringList parts;
    parts << QStringLiteral("uin=%1").arg(musicid);
    parts << QStringLiteral("qqmusic_key=%1").arg(musickey);
    parts << QStringLiteral("qm_keyst=%1").arg(musickey);
    if (!accessToken.isEmpty()) parts << QStringLiteral("qm_third_token=%1").arg(accessToken);
    if (!openid.isEmpty()) parts << QStringLiteral("qm_third_openid=%1").arg(openid);
    parts << QStringLiteral("QQMusic_login_type=1");
    return parts.join(QStringLiteral("; "));
}

} // namespace Qq
} // namespace Furui
