#include "AuthManager.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QDateTime>
#include <QRandomGenerator>
#include <QUrl>
#include <functional>
#include <memory>

#include "JsonMapper.h"
#include "KugouProtocol.h"
#include "Library.h"
#include "MusicSource.h"
#include "NeteaseCrypto.h"
#include "QqProtocol.h"
#include "Settings.h"
#include "SourceConfig.h"
#include "SourceManager.h"
#include "UserApi.h"
#include "3rd/qrcodegen.hpp"
#include <QImage>
#include <QPainter>
#include <QCryptographicHash>

namespace Furui {

static QStringList listOf(const QVariant &v)
{
    QStringList out;
    for (const QVariant &x : v.toList()) out << x.toString();
    return out;
}

static QVariantMap browserHeaders(const QString &referer)
{
    QVariantMap h;
    h.insert(QStringLiteral("Referer"), referer);
    h.insert(QStringLiteral("User-Agent"),
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) FuruiMusic/1.0"));
    return h;
}

// 网页版扫码会在二维码内容里带 chainId，缺了它手机端授权后服务端可能直接按
// 环境风险拒绝（8821「请切换其他登录方式或升级新版本再试」）。
// 规则与官方 web 端一致：v1_{deviceId}_web_login_{毫秒时间戳}；deviceId 持久化复用。
static QString neteaseDeviceId()
{
    QString did = Settings::string(QStringLiteral("neteaseDeviceId"));
    if (!did.isEmpty()) return did;
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 52; ++i)
        did.append(QChar(hex[QRandomGenerator::global()->bounded(16)]));
    Settings::set(QStringLiteral("neteaseDeviceId"), did);
    return did;
}

static QString neteaseChainId()
{
    return QStringLiteral("v1_%1_web_login_%2")
        .arg(neteaseDeviceId()).arg(QDateTime::currentMSecsSinceEpoch());
}

// Qt 的网络错误串带完整请求 URL（含签名参数），直接贴到界面上既难看又多余，
// 只保留 " - " 之后的服务端回复部分。
static QString shortNetworkError(const QString &error)
{
    const int sep = error.lastIndexOf(QStringLiteral(" - "));
    return sep > 0 ? error.mid(sep + 3) : error;
}

// FURUI_DIAG_HTTP 排障日志脱敏：登录响应里的 Set-Cookie / token 是长期有效的账号凭据，
// 明文写到本地文件等于把账号交给任何拿到该文件的人。这里只保留「键名 + 长度 + 短指纹」，
// 足以判断「有没有下发 Cookie」「两次是不是同一个值」，但无法还原凭据。
static bool isSensitiveKey(const QString &key)
{
    static const QSet<QString> keys = {
        QStringLiteral("cookie"), QStringLiteral("set-cookie"), QStringLiteral("token"),
        QStringLiteral("music_u"), QStringLiteral("__csrf"), QStringLiteral("csrf"),
        QStringLiteral("sessionid"), QStringLiteral("session"), QStringLiteral("password"),
        QStringLiteral("passwd"), QStringLiteral("unikey"), QStringLiteral("codekey"),
        QStringLiteral("qrsig"), QStringLiteral("ptqrtoken"), QStringLiteral("access_token"),
        QStringLiteral("refresh_token"), QStringLiteral("secretkey"), QStringLiteral("encseckey"),
        QStringLiteral("phone"), QStringLiteral("cellphone"), QStringLiteral("email"),
        QStringLiteral("url"), QStringLiteral("play_url"), QStringLiteral("listenurl"),
        QStringLiteral("lyric"), QStringLiteral("lrc")
    };
    return keys.contains(key.toLower());
}

static QByteArray shortFingerprint(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex().left(8);
}

static QString redactCookies(const QStringList &cookies)
{
    if (cookies.isEmpty()) return QStringLiteral("(none)");
    QStringList out;
    for (const QString &c : cookies) {
        const QString kv = c.section(QLatin1Char(';'), 0, 0).trimmed();
        const QString name = kv.section(QLatin1Char('='), 0, 0);
        const QString val = kv.section(QLatin1Char('='), 1);
        out << QStringLiteral("%1=<redacted len=%2 sha256:%3>")
               .arg(name.isEmpty() ? QStringLiteral("?") : name)
               .arg(val.size())
               .arg(QString::fromLatin1(shortFingerprint(val.toUtf8())));
    }
    return out.join(QStringLiteral(" || "));
}

static QString redactBody(const QByteArray &body)
{
    static const int kMax = 4096;
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(body.left(kMax), &pe);
    if (pe.error != QJsonParseError::NoError || !(doc.isObject() || doc.isArray())) {
        // HTML / 加密串 / 非 UTF-8：只留长度与指纹，绝不写原文
        return QStringLiteral("<non-json bytes=%1 sha256:%2>")
            .arg(body.size()).arg(QString::fromLatin1(shortFingerprint(body)));
    }
    std::function<QJsonValue(const QJsonValue &)> scrub = [&scrub](const QJsonValue &v) -> QJsonValue {
        if (v.isObject()) {
            QJsonObject o;
            for (auto it = v.toObject().constBegin(); it != v.toObject().constEnd(); ++it) {
                if (isSensitiveKey(it.key())) {
                    const QByteArray raw = it.value().isString()
                        ? it.value().toString().toUtf8() : it.value().toVariant().toString().toUtf8();
                    o.insert(it.key(), QStringLiteral("<redacted len=%1 sha256:%2>")
                        .arg(raw.size()).arg(QString::fromLatin1(shortFingerprint(raw))));
                } else {
                    o.insert(it.key(), scrub(it.value()));
                }
            }
            return o;
        }
        if (v.isArray()) {
            QJsonArray a;
            for (const QJsonValue &x : v.toArray()) a.append(scrub(x));
            return a;
        }
        return v;
    };
    const QJsonValue safe = scrub(doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(doc.array()));
    QJsonDocument out;
    if (safe.isArray()) out.setArray(safe.toArray());
    else out.setObject(safe.toObject());
    return QString::fromUtf8(out.toJson(QJsonDocument::Compact).left(kMax));
}

// 统一的脱敏日志片段，避免两处 HTTP 包装各自复制一套格式
static QByteArray diagHttpLine(const QString &tag, const QString &path, const HttpClient::Reply &r)
{
    return QStringLiteral("[%1 %2] status=%3 ok=%4 error=%5\nsetcookies=%6\nbody=%7\n\n")
        .arg(tag).arg(path).arg(r.status).arg(int(r.ok))
        .arg(shortNetworkError(r.error))
        .arg(redactCookies(r.setCookies))
        .arg(redactBody(r.body))
        .toUtf8();
}

// 二维码本身的有效期就是几分钟。没有这个硬过期，服务端一直回「等待扫码」
// 或 ptlogin 持续回空串时，2 秒一次的轮询会永远跑下去。
static const qint64 kQrSessionMs = 4 * 60 * 1000;

// Cookie 串里可能混着 Set-Cookie 的属性字段（Max-Age/Path/Domain/Secure…），
// 直接存下来会污染后续请求的 Cookie 头，这里只保留真正的键值对。
static QString onlyCookiePairs(const QString &raw)
{
    static const QStringList attrs = {
        QStringLiteral("max-age"), QStringLiteral("expires"), QStringLiteral("path"),
        QStringLiteral("domain"), QStringLiteral("secure"), QStringLiteral("httponly"),
        QStringLiteral("samesite"), QStringLiteral("version"), QStringLiteral("comment"),
        QStringLiteral("commenturl"), QStringLiteral("partitioned")};
    QStringList out;
    for (const QString &part : raw.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QString kv = part.trimmed();
        const int eq = kv.indexOf(QLatin1Char('='));
        if (eq <= 0) continue;                                   // Secure 这类没有值的属性
        const QString name = kv.left(eq).trimmed().toLower();
        if (attrs.contains(name) || name.contains(QLatin1Char(' '))) continue;
        if (!out.contains(kv)) out << kv;
    }
    return out.join(QStringLiteral("; "));
}

static QString cookiesFromReply(const HttpClient::Reply &r, const QVariantMap &loginQr,
                                const QJsonValue &root)
{
    const QString cookiePath = loginQr.value(QStringLiteral("cookiePath")).toString();
    QString direct = cookiePath.isEmpty() ? QString() : Json::stringAt(root, cookiePath);
    if (!direct.isEmpty()) return onlyCookiePairs(direct);
    QStringList parts;
    for (const QString &c : r.setCookies) {
        const QString kv = c.section(QLatin1Char(';'), 0, 0).trimmed();
        if (!kv.isEmpty() && !parts.contains(kv)) parts << kv;
    }
    return parts.join(QStringLiteral("; "));
}

AuthManager *AuthManager::instance()
{
    static AuthManager *a = new AuthManager();
    return a;
}

AuthManager::AuthManager(QObject *parent)
    : QObject(parent)
{
    m_poll.setInterval(2000);
    connect(&m_poll, &QTimer::timeout, this, &AuthManager::pollQr);
    refresh();
}

QStringList AuthManager::methods(const QString &sourceId) const
{ return UserApi::instance()->methods(sourceId); }

QString AuthManager::apiStatus(const QString &sourceId) const
{ return UserApi::instance()->statusOf(sourceId); }

QVector<Account> AuthManager::accounts() const
{ return Library::instance()->accounts(); }

Account AuthManager::account(const QString &sourceId) const
{
    for (const Account &a : accounts())
        if (a.source == sourceId) return a;
    Account empty;
    empty.source = sourceId;
    empty.status = QStringLiteral("未登录");
    return empty;
}

void AuthManager::refresh()
{
    SourceManager::instance()->refreshSessions();
    emit accountsChanged();
}

void AuthManager::startQrLogin(const QString &sourceId)
{
    // QQ 音乐原生扫码（ptlogin 二维码 + QQLogin 换登录态，应用内直连）
    if (sourceId == QLatin1String("qq")) {
        cancelQrLogin();
        startQqQr();
        return;
    }
    // 酷狗原生扫码（login-user.kugou.com 二维码接口 + web 签名，应用内直连）
    if (sourceId == QLatin1String("kugou")) {
        cancelQrLogin();
        startKgQr();
        return;
    }
    // 网易云原生二维码登录（应用内直连，无需外部服务）
    if (UserApi::instance()->config(sourceId).driver == QLatin1String("netease")) {
        cancelQrLogin();
        m_qrMode = QStringLiteral("netease");
        m_qrSource = sourceId;
        m_qrKey.clear();
        m_qrImagePath.clear();
        m_neeCookies.clear();
        m_qrHint = QStringLiteral("正在获取二维码…");
        m_qrStatus = QStringLiteral("准备中");
        emit qrChanged();
        // 2026-11-08 实测：/weapi 二维码路径在手机确认后会回 8821（风控要求行为验证码），
        // 永远拿不到 803 的 MUSIC_U；官方网页用的是明文 /api/login/qrcode/unikey，
        // 回复形如 {"code":200,"unikey":"…"}，配合 Cookie 与 chainId 才不会被判成异常环境。
        neteaseWebPost(QStringLiteral("/api/login/qrcode/unikey"), QByteArrayLiteral("type=1"),
            [this, sourceId](const HttpClient::Reply &r) {
                if (m_qrSource != sourceId) return;
                absorbNeteaseCookies(r);
                if (!r.ok) { m_qrStatus = QStringLiteral("二维码获取失败：") + shortNetworkError(r.error); emit qrChanged(); return; }
                const QJsonValue root = Json::rootOf(r.json);
                const QString key = Json::firstString(root, {
                    QStringLiteral("unikey"), QStringLiteral("data.unikey"), QStringLiteral("key")});
                if (key.isEmpty()) {
                    const QString msg = Json::firstString(root, {QStringLiteral("message"),
                                                                QStringLiteral("msg")});
                    m_qrStatus = QStringLiteral("二维码获取失败：") +
                                 (msg.isEmpty() ? QStringLiteral("未返回 unikey") : msg);
                    emit qrChanged();
                    return;
                }
                m_qrKey = key;
                generateQrImage(QStringLiteral("https://music.163.com/login?codekey=") + key
                                + QStringLiteral("&chainId=") + neteaseChainId(), sourceId);
                m_qrHint = QStringLiteral("请用网易云 App 扫码");
                m_qrStatus = QStringLiteral("等待扫码");
                emit qrChanged();
                startQrPolling();
            });
        return;
    }
    cancelQrLogin();
    MusicSource *s = SourceManager::instance()->source(sourceId);
    if (!s) return;
    const LoginConfig &login = s->desc().login;
    if (!login.qrCreate.valid()) {
        emit toast(QStringLiteral("该平台未配置二维码登录，请使用 Cookie 登录；"
                                  "如需扫码，请部署实现了二维码端点的账号 API 服务"));
        return;
    }
    m_qrMode = QStringLiteral("service");
    m_qrSource = sourceId;
    m_qrImagePath.clear();
    m_qrHint = QStringLiteral("正在获取二维码…");
    m_qrStatus = QStringLiteral("准备中");
    emit qrChanged();

    QVariantMap vars = s->baseVars();
    s->callLogin(QStringLiteral("qrCreate"), vars,
        [this, sourceId, s](const HttpClient::Reply &r) {
            if (m_qrSource != sourceId) return;
            const QJsonValue root = Json::rootOf(r.json);
            const QVariantMap qr = s->desc().login.qr;
            m_qrKey = Json::firstString(root, {
                qr.value(QStringLiteral("keyPath")).toString(), QStringLiteral("unikey"),
                QStringLiteral("key"), QStringLiteral("data.unikey"), QStringLiteral("data.key")
            });
            const QString image = Json::firstString(root, {
                qr.value(QStringLiteral("imagePath")).toString(), QStringLiteral("data.qrurl"),
                QStringLiteral("data.qrcode"), QStringLiteral("qrurl"), QStringLiteral("image")
            });
            m_qrHint = m_qrKey.isEmpty() ? QString() : QStringLiteral("登录 Key：") + m_qrKey;
            m_qrStatus = QStringLiteral("等待扫码");
            if (!r.ok) m_qrStatus = QStringLiteral("二维码获取失败：") + shortNetworkError(r.error);

            auto saveImage = [this, sourceId](const QByteArray &bytes) {
                if (bytes.isEmpty()) return;
                const QString path = Settings::filePath(QStringLiteral("qr_") + sourceId + QStringLiteral(".png"));
                QFile f(path);
                if (f.open(QIODevice::WriteOnly)) { f.write(bytes); f.close(); }
                m_qrImagePath = path;
                emit qrChanged();
            };

            if (image.startsWith(QStringLiteral("data:image"), Qt::CaseInsensitive)) {
                const QByteArray b64 = image.mid(image.indexOf(QLatin1Char(',')) + 1).toUtf8();
                saveImage(QByteArray::fromBase64(b64));
            } else if (image.startsWith(QStringLiteral("http"), Qt::CaseInsensitive)) {
                HttpClient::instance()->request(QStringLiteral("GET"), QUrl(image), QVariantMap(),
                                                QByteArray(), [saveImage](const HttpClient::Reply &ir) {
                    if (ir.ok) saveImage(ir.body);
                });
            } else if (!image.isEmpty()) {
                m_qrHint = QStringLiteral("二维码内容：") + image;
            }
            emit qrChanged();
            if (r.ok && !m_qrKey.isEmpty()) {
                startQrPolling();
            }
        });
}

void AuthManager::cancelQrLogin()
{
    m_poll.stop();
    m_polling = false;
    m_qrDeadline = 0;
    m_qrMode.clear();
    m_qqQrsig.clear();
    m_qqLoginSig.clear();
    m_kgQrcode.clear();
    m_neeCookies.clear();
    m_qrKey.clear();
    m_qrSource.clear();
    m_qrImagePath.clear();
    m_qrHint = QStringLiteral("请在官方客户端扫码，或使用 Cookie 方式登录");
    m_qrStatus = QStringLiteral("未开始");
    emit qrChanged();
}

void AuthManager::startQrPolling()
{
    m_qrDeadline = QDateTime::currentMSecsSinceEpoch() + kQrSessionMs;
    m_qrEmptyStreak = 0;
    m_polling = true;
    m_poll.start();
}

// 会话超时后统一收尾：不清 m_qrSource，界面还要在原平台页上把「已过期」讲清楚
bool AuthManager::qrTimedOut()
{
    if (m_qrDeadline == 0 || QDateTime::currentMSecsSinceEpoch() <= m_qrDeadline) return false;
    m_poll.stop();
    m_polling = false;
    m_qrImagePath.clear();
    m_qrStatus = QStringLiteral("二维码已过期，请重新获取");
    emit qrChanged();
    return true;
}

void AuthManager::pollQr()
{
    if (qrTimedOut()) return;
    if (m_qrMode == QLatin1String("qq")) {
        pollQqQr();
        return;
    }
    if (m_qrMode == QLatin1String("kugou")) {
        pollKgQr();
        return;
    }
    // 网易云原生二维码轮询（官方网页同款明文接口，Cookie 与出码时同一个会话）
    if (m_qrMode == QLatin1String("netease")) {
        if (m_qrSource.isEmpty() || m_qrKey.isEmpty()) { m_poll.stop(); return; }
        const QString sourceId = m_qrSource;
        neteaseWebPost(QStringLiteral("/api/login/qrcode/client/login"),
                       QByteArray("key=") + QUrl::toPercentEncoding(m_qrKey) + "&type=1",
            [this, sourceId](const HttpClient::Reply &r) {
                if (!m_polling || m_qrSource != sourceId) return;
                absorbNeteaseCookies(r);
                if (!r.ok) { m_qrStatus = QStringLiteral("轮询失败：") + shortNetworkError(r.error); emit qrChanged(); return; }
                const QJsonValue root = Json::rootOf(r.json);
                const QString code = Json::stringAt(root, QStringLiteral("code"));
                if (code.isEmpty()) {
                    // 空回复/无法解析绝不能当成"等待扫码"：历史上 weapi 密文里的 '+' 未转义时
                    // 服务端会返回空 body，导致扫码授权成功后界面永远停在未登录状态。
                    if (++m_qrEmptyStreak >= 6) {
                        m_poll.stop();
                        m_polling = false;
                        m_qrStatus = QStringLiteral("轮询接口连续返回空内容，请重新获取二维码");
                        emit qrChanged();
                        emit toast(m_qrStatus);
                        return;
                    }
                    m_qrStatus = QStringLiteral("轮询异常，正在重试…");
                    emit qrChanged();
                    return;
                }
                m_qrEmptyStreak = 0;
                if (code == QLatin1String("800")) {
                    m_poll.stop(); m_polling = false;
                    m_qrStatus = QStringLiteral("二维码已过期，请重新获取"); emit qrChanged();
                } else if (code == QLatin1String("803")) {
                    m_poll.stop(); m_polling = false;
                    // 登录态可能在回复体的 cookie 字段里，也可能在本次会话的 Set-Cookie 头里
                    QString cookie = cookiesFromReply(
                        r, QVariantMap{{QStringLiteral("cookiePath"), QStringLiteral("cookie")}}, root);
                    if (cookieValue(cookie, {QStringLiteral("MUSIC_U")}).isEmpty())
                        cookie = neteaseCookieHeader();
                    if (cookieValue(cookie, {QStringLiteral("MUSIC_U")}).isEmpty()) {
                        m_qrStatus = QStringLiteral("授权成功但未取到 MUSIC_U，请改用 Cookie 登录");
                        emit qrChanged();
                        emit toast(m_qrStatus);
                        return;
                    }
                    m_qrStatus = QStringLiteral("授权成功，正在同步账号…"); emit qrChanged();
                    const QJsonValue profile = root.toObject().value(QStringLiteral("profile"));
                    const QString uid = Json::firstString(profile, {QStringLiteral("userId"),
                                                                  QStringLiteral("id")});
                    const QString nick = Json::firstString(profile, {QStringLiteral("nickname")});
                    const QString avatar = Json::firstString(profile, {QStringLiteral("avatarUrl")});
                    saveSession(sourceId, cookie, uid, nick, avatar,
                                QStringLiteral("二维码登录成功"));
                    syncUserLibrary(sourceId);
                } else if (code == QLatin1String("802")) {
                    m_qrStatus = QStringLiteral("已扫码，请在手机上确认登录"); emit qrChanged();
                } else if (code == QLatin1String("801")) {
                    m_qrStatus = QStringLiteral("等待扫码"); emit qrChanged();
                } else if (code == QLatin1String("8821")) {
                    // 风控终态：服务端要求行为验证码，继续轮询也不会变成 803
                    m_poll.stop();
                    m_polling = false;
                    m_qrStatus = QStringLiteral("网易云风控拦截（8821），扫码登录被拒绝，请改用 Cookie 登录");
                    emit qrChanged();
                    emit toast(m_qrStatus);
                } else {
                    m_qrStatus = QStringLiteral("等待扫码（接口返回 %1）").arg(code); emit qrChanged();
                }
            });
        return;
    }
    if (!m_polling || m_qrSource.isEmpty()) { m_poll.stop(); return; }
    MusicSource *s = SourceManager::instance()->source(m_qrSource);
    if (!s || !s->desc().login.qrCheck.valid()) { m_poll.stop(); return; }
    const QVariantMap qr = s->desc().login.qr;
    QVariantMap vars = s->baseVars();
    vars.insert(QStringLiteral("key"), m_qrKey);
    const QString sourceId = m_qrSource;
    s->callLogin(QStringLiteral("qrCheck"), vars,
        [this, sourceId, qr](const HttpClient::Reply &r) {
            if (!m_polling || m_qrSource != sourceId) return;
            const QJsonValue root = Json::rootOf(r.json);
            const QString status = Json::firstString(root, {
                qr.value(QStringLiteral("statusPath")).toString(), QStringLiteral("code"),
                QStringLiteral("status"), QStringLiteral("data.state")
            });
            const QVariantMap statusMap = qr.value(QStringLiteral("statusMap")).toMap();
            if (statusMap.contains(status)) m_qrStatus = statusMap.value(status).toString();
            else m_qrStatus = QStringLiteral("状态：") + status;
            const QStringList success = listOf(qr.value(QStringLiteral("successValues")));
            const QStringList expired = listOf(qr.value(QStringLiteral("expiredValues")));
            if (success.contains(status)) {
                m_poll.stop();
                m_polling = false;
                m_qrStatus = QStringLiteral("授权成功，正在同步账号…");
                emit qrChanged();
                const QString cookie = cookiesFromReply(r, qr, root);
                const QString uidPath = qr.value(QStringLiteral("uidPath")).toString();
                const QString nickPath = qr.value(QStringLiteral("nickPath")).toString();
                const QString uid = uidPath.isEmpty() ? QString() : Json::stringAt(root, uidPath);
                const QString nick = nickPath.isEmpty() ? QString() : Json::stringAt(root, nickPath);
                saveSession(sourceId, cookie, uid, nick, QString(), QStringLiteral("二维码登录成功"));
                syncUserLibrary(sourceId);
            } else if (expired.contains(status)) {
                m_poll.stop();
                m_polling = false;
                m_qrStatus = QStringLiteral("二维码已过期，请重新获取");
                emit qrChanged();
            } else {
                emit qrChanged();
            }
        });
}

// ---- QQ 音乐原生扫码：ptlogin 二维码 → 重定向链取授权码 → musicu QQLogin 换登录态 ----

namespace {
const char kQqAppid[] = "716027609";
const char kQqDaid[] = "383";
const char kQqThirdAid[] = "100497308";
const char kQqJump[] = "https%3A%2F%2Fgraph.qq.com%2Foauth2.0%2Flogin_jump";

// 注意：这些 URL 里带 %3A/%2F 之类的转义序列，只能用拼接构造。
// QString::arg 会把已插入值中的 %2、%3 再当成占位符替换一遍，会把 u1 拼坏。
QString qqXloginUrl()
{
    return QStringLiteral("https://xui.ptlogin2.qq.com/cgi-bin/xlogin?appid=")
           + QLatin1String(kQqAppid)
           + QStringLiteral("&daid=") + QLatin1String(kQqDaid)
           + QStringLiteral("&style=33&target=self&s_url=") + QLatin1String(kQqJump)
           + QStringLiteral("&pt_3rd_aid=") + QLatin1String(kQqThirdAid)
           + QStringLiteral("&theme=2");
}

QString qqQrShowUrl()
{
    return QStringLiteral("https://xui.ptlogin2.qq.com/ssl/ptqrshow?appid=")
           + QLatin1String(kQqAppid)
           + QStringLiteral("&e=2&l=M&s=3&d=72&v=4&t=0.")
           + QString::number(QRandomGenerator::global()->bounded(1000000, 9999999))
           + QStringLiteral("&daid=") + QLatin1String(kQqDaid)
           + QStringLiteral("&pt_3rd_aid=") + QLatin1String(kQqThirdAid)
           + QStringLiteral("&u1=") + QLatin1String(kQqJump);
}

QString qqQrPollUrl(const QString &loginSig, const QString &qrToken)
{
    return QStringLiteral("https://xui.ptlogin2.qq.com/ssl/ptqrlogin?u1=")
           + QLatin1String(kQqJump)
           + QStringLiteral("&ptqrtoken=") + qrToken
           + QStringLiteral("&ptredirect=0&h=1&t=1&g=1&from_ui=1&ptlang=2052&action=0-0-")
           + QString::number(QDateTime::currentMSecsSinceEpoch())
           + QStringLiteral("&js_ver=26100915&js_type=1&login_sig=") + loginSig
           + QStringLiteral("&pt_uistyle=40&aid=") + QLatin1String(kQqAppid)
           + QStringLiteral("&daid=") + QLatin1String(kQqDaid)
           + QStringLiteral("&pt_3rd_aid=") + QLatin1String(kQqThirdAid);
}

} // namespace

void AuthManager::startQqQr()
{
    m_qrMode = QStringLiteral("qq");
    m_qrSource = QStringLiteral("qq");
    m_qqQrsig.clear();
    m_qqLoginSig.clear();
    m_qrImagePath.clear();
    m_qrHint = QStringLiteral("正在获取二维码…");
    m_qrStatus = QStringLiteral("准备中");
    emit qrChanged();

    const QString xlogin = qqXloginUrl();
    HttpClient::instance()->request(QStringLiteral("GET"), QUrl(xlogin),
                                    browserHeaders(QStringLiteral("https://y.qq.com/")),
                                    QByteArray(),
        [this, xlogin](const HttpClient::Reply &r) {
            if (m_qrMode != QLatin1String("qq")) return;
            m_qqLoginSig = cookieValue(cookiesFromReply(r, QVariantMap(), QJsonValue()),
                                           {QStringLiteral("pt_login_sig")});
            // 二维码图片接口对参数与 Referer 都敏感，必须和 ptlogin iframe 里抓到的一致
            HttpClient::instance()->request(QStringLiteral("GET"), QUrl(qqQrShowUrl()),
                                            browserHeaders(xlogin), QByteArray(),
                [this](const HttpClient::Reply &ir) {
                    if (m_qrMode != QLatin1String("qq")) return;
                    m_qqQrsig = cookieValue(cookiesFromReply(ir, QVariantMap(), QJsonValue()),
                                                 {QStringLiteral("qrsig")});
                    if (!ir.ok || ir.body.isEmpty() || m_qqQrsig.isEmpty()) {
                        m_qrStatus = QStringLiteral("二维码获取失败：ptlogin 未返回二维码");
                        emit qrChanged();
                        return;
                    }
                    const QString path = Settings::filePath(QStringLiteral("qr_qq.png"));
                    QFile f(path);
                    if (!f.open(QIODevice::WriteOnly)) {
                        m_qrStatus = QStringLiteral("二维码保存失败");
                        emit qrChanged();
                        return;
                    }
                    f.write(ir.body);
                    f.close();
                    m_qrImagePath = path;
                    m_qrHint = QStringLiteral("请使用手机 QQ 或 TIM 扫码");
                    m_qrStatus = QStringLiteral("等待扫码");
                    emit qrChanged();
                    startQrPolling();
                });
        });
}

void AuthManager::pollQqQr()
{
    if (m_qrSource.isEmpty() || m_qqQrsig.isEmpty()) { m_poll.stop(); m_polling = false; return; }
    const QString sourceId = m_qrSource;
    const QString token = QString::number(Qq::hash33(m_qqQrsig));
    // ptqrtoken = hash33(qrsig)，与服务端校验不一致时 ptlogin 直接返回空响应
    const QString poll = qqQrPollUrl(m_qqLoginSig, token);
    QVariantMap headers = browserHeaders(qqXloginUrl());
    headers.insert(QStringLiteral("Cookie"),
                   QStringLiteral("pt_login_sig=%1; qrsig=%2").arg(m_qqLoginSig, m_qqQrsig));

    HttpClient::instance()->request(QStringLiteral("GET"), QUrl(poll), headers, QByteArray(),
        [this, sourceId](const HttpClient::Reply &r) {
            if (!m_polling || m_qrSource != sourceId || m_qrMode != QLatin1String("qq")) return;
            if (!r.ok) {
                m_qrStatus = QStringLiteral("轮询失败：") + shortNetworkError(r.error);
                emit qrChanged();
                return;
            }
            const QStringList f = Qq::ptuiCbFields(r.body);
            if (f.isEmpty()) return;      // ptlogin 偶发空响应，保持状态继续轮询
            const QString code = f.value(0);
            if (code == QLatin1String("0")) {
                m_poll.stop();
                m_polling = false;
                m_qrStatus = QStringLiteral("授权成功，正在同步账号…");
                emit qrChanged();
                qqFollowGrantChain(f.value(2), 0);
            } else if (code == QLatin1String("66")) {
                m_qrStatus = QStringLiteral("等待扫码");
                emit qrChanged();
            } else if (code == QLatin1String("67")) {
                m_qrStatus = QStringLiteral("已扫码，请在手机上确认登录");
                emit qrChanged();
            } else if (code == QLatin1String("65")) {
                m_poll.stop();
                m_polling = false;
                m_qrStatus = QStringLiteral("二维码已过期，请重新获取");
                emit qrChanged();
            } else {
                m_poll.stop();
                m_polling = false;
                m_qrStatus = QStringLiteral("登录失败（ptqrlogin %1）").arg(code);
                emit qrChanged();
                emit toast(m_qrStatus);
            }
        });
}

// 授权码不在 ptqrlogin 的响应里，要顺着 ptuiCB 给出的地址一跳跳重定向才能拿到，
// 因此这里必须关掉 HttpClient 的自动跳转，自己从 Location 里找 code。
void AuthManager::qqFollowGrantChain(const QString &url, int hop)
{
    if (m_qrMode != QLatin1String("qq") || m_qrSource.isEmpty()) return;
    const QString sourceId = m_qrSource;
    const auto fail = [this](const QString &reason) {
        m_qrStatus = QStringLiteral("登录失败：%1").arg(reason);
        emit qrChanged();
        emit toast(m_qrStatus);
    };
    if (hop > 8) { fail(QStringLiteral("授权跳转链路过长")); return; }
    const QUrl here(url);
    const QString code = Qq::authCodeFromUrl(here);
    if (!code.isEmpty()) { qqExchange(code); return; }
    if (url.isEmpty()) { fail(QStringLiteral("未取得授权跳转地址")); return; }

    QVariantMap headers = browserHeaders(QStringLiteral("https://y.qq.com/"));
    headers.insert(QStringLiteral("Cookie"),
                   QStringLiteral("pt_login_sig=%1; qrsig=%2").arg(m_qqLoginSig, m_qqQrsig));
    HttpClient::instance()->request(QStringLiteral("GET"), here, headers, QByteArray(),
        [this, sourceId, here, hop](const HttpClient::Reply &r) {
            // 用户可能已经关掉登录页或改扫其他平台，这条链路必须整个丢弃
            if (m_qrMode != QLatin1String("qq") || m_qrSource != sourceId) return;
            const QString location = r.headers.value(QStringLiteral("location"));
            if (location.isEmpty()) {
                qqFollowGrantChain(QString(), hop + 1);
                return;
            }
            qqFollowGrantChain(here.resolved(QUrl(location)).toString(), hop + 1);
        }, -1, false);
}

void AuthManager::qqExchange(const QString &code)
{
    const QString sourceId = m_qrSource;
    QVariantMap headers = browserHeaders(QStringLiteral("https://y.qq.com/portal/wx_redirect.html"));
    headers.insert(QStringLiteral("Origin"), QStringLiteral("https://y.qq.com"));
    headers.insert(QStringLiteral("Content-Type"), QStringLiteral("application/json"));
    const QByteArray body = QJsonDocument(Qq::loginByCodeRequest(code)).toJson(QJsonDocument::Compact);
    HttpClient::instance()->request(QStringLiteral("POST"),
                                    QUrl(QStringLiteral("https://u.y.qq.com/cgi-bin/musicu.fcg")),
                                    headers, body,
        [this, sourceId](const HttpClient::Reply &r) {
            if (m_qrSource != sourceId || m_qrMode != QLatin1String("qq")) return;
            const auto fail = [this](const QString &reason) {
                m_qrStatus = QStringLiteral("登录失败：%1").arg(reason);
                emit qrChanged();
                emit toast(m_qrStatus);
            };
            if (!r.ok) { fail(r.error.isEmpty() ? QStringLiteral("换取登录态失败") : r.error); return; }
            const QJsonValue root = Json::rootOf(r.json);
            const QJsonValue data = Json::valueAt(root, QStringLiteral("req.data"));
            const QString inner = Json::stringAt(root, QStringLiteral("req.code"));
            const QString musicid = Qq::numericUin(Json::firstString(
                data, {QStringLiteral("str_musicid"), QStringLiteral("musicid")}));
            const QString key = Json::firstString(data, {QStringLiteral("musickey"),
                                                          QStringLiteral("music_key")});
            if (musicid.isEmpty() || key.isEmpty()) {
                const QString msg = Json::firstString(root, {QStringLiteral("req.msg"),
                                                              QStringLiteral("req.message")});
                fail(msg.isEmpty() ? QStringLiteral("未取得登录凭证（req.code %1）").arg(inner) : msg);
                return;
            }
            const QString nick = Json::firstString(data, {QStringLiteral("nick"),
                                                           QStringLiteral("nickname")});
            const QString avatar = Json::firstString(data, {QStringLiteral("logo"),
                                                             QStringLiteral("figureurl")});
            saveSession(sourceId,
                        Qq::sessionCookie(musicid, key,
                                          Json::firstString(data, {QStringLiteral("openid")}),
                                          Json::firstString(data, {QStringLiteral("access_token")})),
                        musicid, nick, avatar, QStringLiteral("二维码登录成功"));
            syncUserLibrary(sourceId);
        });
}

// ---- 酷狗原生扫码：login-user.kugou.com 二维码 → web 签名轮询 → token/userid 组登录 Cookie ----

namespace {
// 创建与轮询共用的公共参数；appid 只有创建二维码时才是 1014
QVariantMap kgQrParams()
{
    QVariantMap p = Kugou::baseParams();
    p.insert(QStringLiteral("plat"), 4);
    p.insert(QStringLiteral("srcappid"), 2919);
    return p;
}
} // namespace

void AuthManager::startKgQr()
{
    m_qrMode = QStringLiteral("kugou");
    m_qrSource = QStringLiteral("kugou");
    m_kgQrcode.clear();
    m_qrImagePath.clear();
    m_qrHint = QStringLiteral("正在获取二维码…");
    m_qrStatus = QStringLiteral("准备中");
    emit qrChanged();

    QVariantMap p = kgQrParams();
    p.insert(QStringLiteral("appid"), 1014);
    p.insert(QStringLiteral("type"), 1);
    p.insert(QStringLiteral("qrcode_txt"),
             QStringLiteral("https://h5.kugou.com/apps/loginQRCode/html/index.html?appid=1005&"));
    const QString url = Kugou::signedUrl(QStringLiteral("https://login-user.kugou.com/v2/qrcode"), p);

    HttpClient::instance()->request(QStringLiteral("GET"), QUrl(url),
                                    browserHeaders(QStringLiteral("https://www.kugou.com/")),
                                    QByteArray(),
        [this](const HttpClient::Reply &r) {
            if (m_qrMode != QLatin1String("kugou")) return;
            if (!r.ok) {
                m_qrStatus = QStringLiteral("二维码获取失败：") + shortNetworkError(r.error);
                emit qrChanged();
                return;
            }
            const QJsonValue root = Json::rootOf(r.json);
            m_kgQrcode = Json::firstString(root, {QStringLiteral("data.qrcode"),
                                                  QStringLiteral("qrcode")});
            const QString img = Json::firstString(root, {QStringLiteral("data.qrcode_img"),
                                                         QStringLiteral("qrcode_img")});
            if (m_kgQrcode.isEmpty() || !img.startsWith(QStringLiteral("data:image"),
                                                       Qt::CaseInsensitive)) {
                m_qrStatus = QStringLiteral("二维码获取失败：酷狗未返回二维码");
                emit qrChanged();
                return;
            }
            const QByteArray png = QByteArray::fromBase64(
                img.mid(img.indexOf(QLatin1Char(',')) + 1).toUtf8());
            const QString path = Settings::filePath(QStringLiteral("qr_kugou.png"));
            QFile f(path);
            if (png.isEmpty() || !f.open(QIODevice::WriteOnly)) {
                m_qrStatus = QStringLiteral("二维码保存失败");
                emit qrChanged();
                return;
            }
            f.write(png);
            f.close();
            m_qrImagePath = path;
            m_qrHint = QStringLiteral("请使用酷狗 App 扫码");
            m_qrStatus = QStringLiteral("等待扫码");
            emit qrChanged();
            startQrPolling();
        });
}

void AuthManager::pollKgQr()
{
    if (m_qrSource.isEmpty() || m_kgQrcode.isEmpty()) { m_poll.stop(); m_polling = false; return; }
    const QString sourceId = m_qrSource;
    QVariantMap p = kgQrParams();
    p.insert(QStringLiteral("qrcode"), m_kgQrcode);
    const QString url = Kugou::signedUrl(
        QStringLiteral("https://login-user.kugou.com/v2/get_userinfo_qrcode"), p);

    HttpClient::instance()->request(QStringLiteral("GET"), QUrl(url),
                                    browserHeaders(QStringLiteral("https://www.kugou.com/")),
                                    QByteArray(),
        [this, sourceId](const HttpClient::Reply &r) {
            if (!m_polling || m_qrMode != QLatin1String("kugou") || m_qrSource != sourceId) return;
            if (!r.ok) {
                m_qrStatus = QStringLiteral("轮询失败：") + shortNetworkError(r.error);
                emit qrChanged();
                return;
            }
            const QJsonValue root = Json::rootOf(r.json);
            const QString st = Json::firstString(root, {QStringLiteral("data.status"),
                                                        QStringLiteral("status")});
            if (st == QLatin1String("4")) {
                m_poll.stop();
                m_polling = false;
                m_qrStatus = QStringLiteral("授权成功，正在同步账号…");
                emit qrChanged();
                const QJsonValue data = Json::valueAt(root, QStringLiteral("data"));
                const QString token = Json::firstString(data, {QStringLiteral("token")});
                const QString userid = Json::firstString(data, {QStringLiteral("userid"),
                                                                QStringLiteral("user_id")});
                if (token.isEmpty()) {
                    m_qrStatus = QStringLiteral("登录失败：酷狗未返回 token");
                    emit toast(m_qrStatus);
                    return;
                }
                // 酷狗的用户信息接口要 RSA 加密，昵称留给后续账号接口同步时补
                saveSession(sourceId, Kugou::sessionCookie(token, userid), userid,
                            QString(), QString(), QStringLiteral("二维码登录成功"));
                syncUserLibrary(sourceId);
            } else if (st == QLatin1String("2")) {
                m_qrStatus = QStringLiteral("已扫码，请在手机上确认登录");
                emit qrChanged();
            } else if (st == QLatin1String("1")) {
                m_qrStatus = QStringLiteral("等待扫码");
                emit qrChanged();
            } else if (st == QLatin1String("0")) {
                m_poll.stop();
                m_polling = false;
                m_qrStatus = QStringLiteral("二维码已过期，请重新获取");
                emit qrChanged();
            } else {
                emit qrChanged();
            }
        });
}

void AuthManager::saveSession(const QString &sourceId, const QString &cookie,
                              const QString &uid, const QString &nickname,
                              const QString &avatar, const QString &message)
{
    Account a = account(sourceId);
    a.source = sourceId;
    a.cookie = cookie;
    a.uid = uid;
    a.nickname = nickname;
    a.avatar = avatar;
    a.logged = !cookie.trimmed().isEmpty();
    a.status = a.logged ? (nickname.isEmpty() ? QStringLiteral("已登录") : nickname)
                        : QStringLiteral("登录态无效");
    Library::instance()->saveAccount(a);
    refresh();
    emit loginStateChanged(sourceId, a.logged, message);
    emit toast(message);
}

// 预留扩展点：config-driven 的用户资料补全。sources.json 里给某个平台配
// login.userInfo 端点后即可启用（当前所有平台都未配置，所以这里会立即返回）。
// 不是死代码，别删——QQ/酷狗等账号 API 落地时直接靠它补昵称/头像。
void AuthManager::fetchUserInfo(const QString &sourceId, const QString &cookie, const QString &uid)
{
    MusicSource *s = SourceManager::instance()->source(sourceId);
    if (!s || !s->desc().login.userInfo.valid()) return;
    QVariantMap vars = s->baseVars();
    vars.insert(QStringLiteral("cookie"), cookie);
    vars.insert(QStringLiteral("uid"), uid);
    const Endpoint ep = s->desc().login.userInfo;
    s->call(ep, vars, [this, sourceId, cookie, ep](const HttpClient::Reply &r) {
        if (!r.ok) return;
        const QJsonValue root = Json::rootOf(r.json);
        const QString infoUid = Json::firstString(root, {
            ep.fields.value(QStringLiteral("uid")).toString(), QStringLiteral("uid"), QStringLiteral("data.uid")
        });
        const QString nick = Json::firstString(root, {
            ep.fields.value(QStringLiteral("nickname")).toString(), QStringLiteral("nickname"), QStringLiteral("data.nick")
        });
        const QString avatar = Json::firstString(root, {
            ep.fields.value(QStringLiteral("avatar")).toString(), QStringLiteral("avatar"), QStringLiteral("data.avatar")
        });
        Account a = account(sourceId);
        if (!infoUid.isEmpty()) a.uid = infoUid;
        if (!nick.isEmpty()) a.nickname = nick;
        if (!avatar.isEmpty()) a.avatar = avatar;
        a.cookie = cookie;
        a.logged = true;
        a.status = a.nickname.isEmpty() ? QStringLiteral("已登录") : a.nickname;
        Library::instance()->saveAccount(a);
        refresh();
        emit loginStateChanged(sourceId, true, QStringLiteral("账号信息已更新"));
    });
}

void AuthManager::loginByCookie(const QString &sourceId, const QString &cookie)
{
    const QString value = cookie.trimmed();
    if (value.isEmpty()) {
        emit loginStateChanged(sourceId, false, QStringLiteral("Cookie 不能为空"));
        return;
    }
    // 进度提示不能走 loginStateChanged：那是"登录结果"，ok=false 会被界面当成"登录失败"
    emit toast(QStringLiteral("正在验证 %1 登录态…")
                   .arg(SourceConfig::instance()->nameOf(sourceId)));
    UserApi::instance()->verify(sourceId, value,
        [this, sourceId, value](const UserProfile &p) {
            if (p.noAccountApi) {
                // 这类平台（酷我/咪咕/汽水）的 Cookie 只影响播放鉴权，存下来就有用
                Account a = account(sourceId);
                a.cookie = value;
                a.logged = true;
                a.status = p.message;
                Library::instance()->saveAccount(a);
                refresh();
                emit loginStateChanged(sourceId, true, p.message);
                emit toast(p.message);
                return;
            }
            if (!p.ok) {
                const QString msg = p.message.isEmpty()
                    ? QStringLiteral("登录态无效") : p.message;
                Account a = account(sourceId);
                // 断网、账号服务没配这类失败判不了登录态，把用户原来的 Cookie 抹掉
                // 等于一次网络抖动就让人重新扫码，只有服务端明确说不认才清除
                if (!p.networkError) {
                    a.cookie.clear();
                    a.logged = false;
                }
                a.status = msg;
                Library::instance()->saveAccount(a);
                refresh();
                emit loginStateChanged(sourceId, a.logged, msg);
                emit toast(msg);
                return;
            }
            saveSession(sourceId, value, p.uid, p.nickname, p.avatar, p.message);
            syncUserLibrary(sourceId);
        });
}

void AuthManager::sendPhoneCode(const QString &sourceId, const QString &phone)
{
    MusicSource *s = SourceManager::instance()->source(sourceId);
    if (!s || !s->desc().login.phoneSend.valid()) {
        emit toast(QStringLiteral("该平台未配置验证码发送端点"));
        return;
    }
    QVariantMap vars = s->baseVars();
    vars.insert(QStringLiteral("phone"), phone);
    s->callLogin(QStringLiteral("phoneSend"), vars, [this](const HttpClient::Reply &r) {
        emit toast(r.ok ? QStringLiteral("验证码已发送") : QStringLiteral("发送失败：") + r.error);
    });
}

void AuthManager::loginByPhone(const QString &sourceId, const QString &phone, const QString &code)
{
    MusicSource *s = SourceManager::instance()->source(sourceId);
    if (!s || !s->desc().login.phoneLogin.valid()) {
        emit loginStateChanged(sourceId, false, QStringLiteral("该平台未配置手机号登录端点"));
        return;
    }
    QVariantMap vars = s->baseVars();
    vars.insert(QStringLiteral("phone"), phone);
    vars.insert(QStringLiteral("code"), code);
    s->callLogin(QStringLiteral("phoneLogin"), vars,
        [this, sourceId](const HttpClient::Reply &r) {
            if (!r.ok) {
                emit loginStateChanged(sourceId, false, QStringLiteral("登录失败：") + r.error);
                return;
            }
            const QJsonValue root = Json::rootOf(r.json);
            const QString cookie = cookiesFromReply(r, QVariantMap(), root);
            const QString uid = Json::firstString(root, {QStringLiteral("uid"), QStringLiteral("data.uid"), QStringLiteral("userId")});
            const QString nick = Json::firstString(root, {QStringLiteral("nickname"), QStringLiteral("data.nickname")});
            saveSession(sourceId, cookie, uid, nick, QString(), QStringLiteral("手机号登录成功"));
            syncUserLibrary(sourceId);
        });
}

void AuthManager::weapiPost(const QString &path, const QJsonObject &payload,
                            const QString &cookie, const HttpClient::Callback &cb)
{
    const QUrl url(QStringLiteral("https://music.163.com") + path);
    QVariantMap headers;
    headers.insert(QStringLiteral("Referer"), QStringLiteral("https://music.163.com/"));
    headers.insert(QStringLiteral("Origin"), QStringLiteral("https://music.163.com"));
    headers.insert(QStringLiteral("User-Agent"),
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                       "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"));
    headers.insert(QStringLiteral("Content-Type"),
        QStringLiteral("application/x-www-form-urlencoded"));
    if (!cookie.isEmpty())
        headers.insert(QStringLiteral("Cookie"), cookie);
    QByteArray body = Netease::weapiForm(payload);
    HttpClient::Callback handler = cb;
    if (!qEnvironmentVariableIsEmpty("FURUI_DIAG_HTTP")) {
        const QString logPath = qEnvironmentVariable("FURUI_DIAG_HTTP");
        handler = [logPath, path, cb](const HttpClient::Reply &r) {
            QFile f(logPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
                f.write(diagHttpLine(QStringLiteral("weapi"), path, r));
                f.close();
            }
            if (cb) cb(r);
        };
    }
    HttpClient::instance()->request(QStringLiteral("POST"), url, headers, body, handler);
}

void AuthManager::neteaseWebPost(const QString &path, const QByteArray &form,
                                 const HttpClient::Callback &cb)
{
    const QUrl url(QStringLiteral("https://music.163.com") + path);
    QVariantMap headers;
    headers.insert(QStringLiteral("Referer"), QStringLiteral("https://music.163.com/"));
    headers.insert(QStringLiteral("Origin"), QStringLiteral("https://music.163.com"));
    // 与真实浏览器一致的 UA：带自定义后缀的 UA 会提高风控判定风险
    headers.insert(QStringLiteral("User-Agent"),
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                       "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"));
    headers.insert(QStringLiteral("Content-Type"),
        QStringLiteral("application/x-www-form-urlencoded"));
    const QString cookie = neteaseCookieHeader();
    if (!cookie.isEmpty())
        headers.insert(QStringLiteral("Cookie"), cookie);
    HttpClient::Callback handler = cb;
    if (!qEnvironmentVariableIsEmpty("FURUI_DIAG_HTTP")) {
        // 排查用：把请求回复原样写入日志（仅本地排障）
        const QString logPath = qEnvironmentVariable("FURUI_DIAG_HTTP");
        handler = [logPath, path, cb](const HttpClient::Reply &r) {
            QFile f(logPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
                f.write(diagHttpLine(QStringLiteral("netease"), path, r));
                f.close();
            }
            if (cb) cb(r);
        };
    }
    HttpClient::instance()->request(QStringLiteral("POST"), url, headers, form, handler);
}

void AuthManager::absorbNeteaseCookies(const HttpClient::Reply &r)
{
    for (const QString &c : r.setCookies) {
        const QString kv = c.section(QLatin1Char(';'), 0, 0).trimmed();
        const int eq = kv.indexOf(QLatin1Char('='));
        if (eq <= 0) continue;
        m_neeCookies.insert(kv.left(eq).trimmed(), kv.mid(eq + 1).trimmed());
    }
}

QString AuthManager::neteaseCookieHeader() const
{
    QStringList parts;
    for (auto it = m_neeCookies.constBegin(); it != m_neeCookies.constEnd(); ++it)
        parts << it.key() + QLatin1Char('=') + it.value();
    // deviceId 与二维码里的 chainId 同源，浏览器也是靠这个 Cookie 绑定环境
    parts << QStringLiteral("deviceId=") + neteaseDeviceId();
    return parts.join(QStringLiteral("; "));
}

void AuthManager::generateQrImage(const QString &content, const QString &sourceId)
{
    using namespace qrcodegen;
    QrCode qr = QrCode::encodeText(content.toUtf8().constData(), QrCode::Ecc::MEDIUM);
    const int size = qr.getSize();
    const int scale = 8;
    const int border = 4;
    const int dim = (size + border * 2) * scale;
    QImage img(dim, dim, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter painter(&img);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (qr.getModule(x, y))
                painter.drawRect((x + border) * scale, (y + border) * scale, scale, scale);
        }
    }
    const QString path = Settings::filePath(QStringLiteral("qr_") + sourceId + QStringLiteral(".png"));
    if (img.save(path, "PNG")) {
        m_qrImagePath = path;
        emit qrChanged();
    }
}

void AuthManager::loginByPassword(const QString &sourceId, const QString &account, const QString &password)
{
    const QString acc = account.trimmed();
    if (acc.isEmpty() || password.isEmpty()) {
        emit loginStateChanged(sourceId, false, QStringLiteral("账号和密码都不能为空"));
        return;
    }
    // 进度提示只走 toast：loginStateChanged 是「登录结果」，ok=false 会被界面当成「登录失败」闪一下
    emit toast(QStringLiteral("正在登录 %1…")
                   .arg(SourceConfig::instance()->nameOf(sourceId)));

    const QString driver = UserApi::instance()->config(sourceId).driver;
    if (driver != QLatin1String("netease")) {
        emit loginStateChanged(sourceId, false,
            QStringLiteral("该平台未内置密码登录，请改用二维码或 Cookie，或配置账号 API 服务"));
        return;
    }

    const QString md5 = QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Md5).toHex();
    const bool isEmail = acc.contains(QLatin1Char('@'));
    QJsonObject payload;
    if (isEmail) {
        payload.insert(QStringLiteral("username"), acc);
        payload.insert(QStringLiteral("password"), md5);
        payload.insert(QStringLiteral("rememberLogin"), QStringLiteral("true"));
    } else {
        payload.insert(QStringLiteral("phone"), acc);
        payload.insert(QStringLiteral("password"), md5);
        payload.insert(QStringLiteral("rememberLogin"), QStringLiteral("true"));
    }
    const QString endpoint = isEmail
        ? QStringLiteral("/weapi/login")
        : QStringLiteral("/weapi/login/cellphone");
    weapiPost(endpoint, payload, QString(),
        [this, sourceId](const HttpClient::Reply &r) {
            if (!r.ok) {
                emit loginStateChanged(sourceId, false, QStringLiteral("登录失败：") + r.error);
                emit toast(QStringLiteral("登录失败：") + r.error);
                return;
            }
            const QJsonValue root = Json::rootOf(r.json);
            const QString code = Json::stringAt(root, QStringLiteral("code"));
            if (code != QLatin1String("200")) {
                const QString msg = Json::firstString(root, {QStringLiteral("message"),
                                                            QStringLiteral("msg")});
                const QString err = msg.isEmpty() ? QStringLiteral("账号或密码错误") : msg;
                emit loginStateChanged(sourceId, false, err);
                emit toast(err);
                return;
            }
            const QString cookie = cookiesFromReply(r, QVariantMap(), root);
            const QJsonValue profile = root.toObject().value(QStringLiteral("profile"));
            const QString uid = Json::firstString(profile, {QStringLiteral("userId"),
                                                          QStringLiteral("id")});
            const QString nick = Json::firstString(profile, {QStringLiteral("nickname")});
            const QString avatar = Json::firstString(profile, {QStringLiteral("avatarUrl")});
            saveSession(sourceId, cookie, uid, nick, avatar, QStringLiteral("密码登录成功"));
            syncUserLibrary(sourceId);
        });
}

void AuthManager::logout(const QString &sourceId)
{
    Library::instance()->removeAccount(sourceId);
    refresh();
    emit loginStateChanged(sourceId, false, QStringLiteral("已退出登录"));
    emit toast(QStringLiteral("已退出登录"));
}

void AuthManager::syncUserLibrary(const QString &sourceId)
{
    const Account a = account(sourceId);
    if (!a.logged || a.cookie.isEmpty()) {
        emit toast(QStringLiteral("请先通过 Cookie 登录该平台"));
        return;
    }
    emit toast(QStringLiteral("正在同步 %1 的歌单与收藏…")
                   .arg(SourceConfig::instance()->nameOf(sourceId)));

    UserApi::instance()->fetchLibrary(sourceId, a.cookie,
        [this, sourceId](const UserLibraryData &data) {
            Library *lib = Library::instance();
            if (!data.playlists.isEmpty()) lib->savePlaylists(data.playlists);
            // 每个歌单的歌曲一并落库，点开「我的歌单」即可直接播放
            for (const auto &pair : data.playlistSongs) {
                if (pair.second.isEmpty()) continue;
                lib->saveSongs(pair.second);
                lib->setPlaylistSongs(pair.first, pair.second);
            }
            if (!data.favorites.isEmpty()) {
                Playlist fav;
                fav.uid = sourceId + QStringLiteral(":user_favorite");
                fav.source = sourceId;
                fav.sourceId = QStringLiteral("user_favorite");
                fav.title = QStringLiteral("我喜欢的音乐（")
                            + SourceConfig::instance()->nameOf(sourceId) + QStringLiteral("）");
                fav.type = QStringLiteral("favorite");
                fav.count = data.favorites.size();
                lib->savePlaylist(fav);
                lib->saveSongs(data.favorites);
                lib->setPlaylistSongs(fav.uid, data.favorites);
                for (const Song &song : data.favorites) lib->addFavorite(song);
            }
            emit libraryUpdated();
            // 歌单同步失败不等于登录失败：登录态已经落库，错误只通过 syncFinished 提示，
            // 避免界面把网络抖动说成"登录失败"
            emit librarySynced(sourceId, data.ok, data.message);
            emit toast(data.message);
        });
}

void AuthManager::syncAll()
{
    for (const Account &a : accounts())
        if (a.logged) syncUserLibrary(a.source);
}

} // namespace Furui
