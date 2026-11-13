#include "AuthManager.h"

#include <QFile>
#include <QDebug>
#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QDateTime>
#include "core/QtCompat.h"
#include <memory>
#include <QUrl>
#include <QUrlQuery>
#include <functional>
#include <memory>

#include "JsonMapper.h"
#include "KugouLoginCrypto.h"
#include "KugouProtocol.h"
#include "Library.h"
#include "MusicSource.h"
#include "NeteaseCrypto.h"
#include "QqProtocol.h"
#include "RsaPkcs1.h"
#include "Settings.h"
#include "SourceConfig.h"
#include "SourceManager.h"
#include "UserApi.h"
#include "3rd/qrcodegen.hpp"
#include <QImage>
#include <QPainter>
#include <QCryptographicHash>
#include "core/QtCompat.h"

namespace Furui {

// 前置声明：定义在文件后半段的匿名命名空间里，二维码分支要用它拿浏览器常规头
namespace { QVariantMap neeWebHeaders(const QString &cookie = QString());
         QVariantMap qishuiPassportHeaders(const QString &cookie = QString()); }

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

// 持久化的设备标识，只用于密码登录时派生 csrf_token（浏览器登录也会带 deviceId Cookie）。
// 注意：不要把它拼进二维码内容里当 chainId —— 服务端不认自造的 chainId，
// 网易云 App 会直接把码判成无效，扫码授权就永远走不到 803。
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

// weapi 的 csrf_token 是「双提交」校验：body 里的 csrf_token 必须和 Cookie 里的
// __csrf 一致。浏览器登录后 Cookie 里一定有 __csrf，而应用内直连没有，历史上
// 一直传空串 —— 假账号会先被 502 挡掉所以测不出来，真实账号却可能被判成畸形请求。
// 这里自己生成一个稳定的随机值，Cookie 与 body 用同一个（与官方 API 实现一致）。
static QString neteaseCsrfToken()
{
    QString csrf = Settings::string(QStringLiteral("neteaseCsrf"));
    if (!csrf.isEmpty()) return csrf;
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i)
        csrf.append(QChar(hex[QRandomGenerator::global()->bounded(16)]));
    Settings::set(QStringLiteral("neteaseCsrf"), csrf);
    return csrf;
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
        QStringLiteral("mobile"), QStringLiteral("ticket"), QStringLiteral("assertion"),
        QStringLiteral("ssoticket"), QStringLiteral("qrc_sessionid"),
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

namespace {
// 与 weapi 辅助函数同处一个匿名命名空间，定义在文件后半段；扫码链路在前面先用
QString neeQrError(const QString &code, const QString &serverMessage);
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
    // 咪咕原生扫码（passport.migu.cn 门户同款 qrcWeb 接口，应用内直连）
    if (sourceId == QLatin1String("migu")) {
        cancelQrLogin();
        startMiguQr();
        return;
    }
    // 酷我：第三方里只有微信能纯 HTTP 走完（开放平台扫码 + i.kuwo.cn 回调换 Cookie）
    if (sourceId == QLatin1String("kuwo")) {
        cancelQrLogin();
        startKuwoWxQr();
        return;
    }
    // 汽水：登录闭环走官方 passport 出码链路（secsdk 设备签名由服务端完成），
    // 客户端按 GET /passport/web/* → GET /luna/pc/me 直接跑；也可配置自建账号 API 服务走其 /auth 契约。
    if (sourceId == QLatin1String("qishui")) {
        cancelQrLogin();
        startQishuiQr();
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
        // 与 AlgerMusicPlayer（内置 netease-cloud-music-api-alger）逐参数对齐，2026-11 实测：
        //   1) 出码端点 POST /api/login/qrcode/unikey，关键参数 type=3（官方新版流程）；
        //   2) 官方客户端走 eapi（AES-128-ECB + 设备 header），网页端走明文；
        //   3) 设备指纹 Cookie 与匿名令牌 MUSIC_A 有助于过确认阶段的风控（8821）；
        //   4) 二维码内容只有 https://music.163.com/login?codekey=<unikey>，不加 chainId。
        //
        // Android/模拟器上 eapi 或「带完整会话 Cookie」的请求偶尔会被服务端回
        // 「获取网络设备失败」，而裸明文请求实测始终 200。因此出码改为三级兜底：
        // eapi → 明文 + 精简设备指纹 → 无 Cookie 明文；匿名令牌改为后台并行获取，
        // 不再阻塞出码。轮询通道与最终成功的那一级保持一致（m_qrEapi）。
        neteaseBootstrap(nullptr);

        auto lastError = std::make_shared<QString>();
        auto stages = std::make_shared<std::function<void(int)>>();
        *stages = [this, sourceId, stages, lastError](int step) {
            if (m_qrSource != sourceId) return;
            if (step > 2) {
                m_qrStatus = QStringLiteral("二维码获取失败：") +
                             (lastError->isEmpty()
                                  ? QStringLiteral("请检查网络后重试，或改用密码登录")
                                  : *lastError);
                emit qrChanged();
                emit toast(m_qrStatus);
                return;
            }

            auto onReply = [this, sourceId, stages, lastError, step](const HttpClient::Reply &r) {
                if (m_qrSource != sourceId) return;
                const QJsonValue root = Json::rootOf(r.json);
                const QString code = Json::stringAt(root, QStringLiteral("code"));
                const QString key = Json::firstString(root, {
                    QStringLiteral("unikey"), QStringLiteral("data.unikey"),
                    QStringLiteral("key")});
                const QString msg = Json::firstString(root, {
                    QStringLiteral("message"), QStringLiteral("msg")});
                if (r.ok && (code.isEmpty() || code == QLatin1String("200")) && !key.isEmpty()) {
                    // 只在成功时串联会话 Cookie，失败请求不污染下一级兜底
                    absorbNeteaseCookies(r);
                    m_qrKey = key;
                    generateQrImage(
                        QStringLiteral("https://music.163.com/login?codekey=") + key, sourceId);
                    m_qrHint = QStringLiteral("请用网易云 App 扫码");
                    m_qrStatus = QStringLiteral("等待扫码");
                    emit qrChanged();
                    startQrPolling();
                    return;
                }
                *lastError = r.ok
                    ? (msg.isEmpty()
                           ? QStringLiteral("接口返回 %1")
                                 .arg(code.isEmpty() ? QStringLiteral("无数据") : code)
                           : msg)
                    : shortNetworkError(r.error);
                qDebug() << "FuruiQr unikey stage" << step << "failed:" << *lastError;
                if (code == QLatin1String("8821") || code == QLatin1String("8816")
                    || code == QLatin1String("460")) {
                    m_qrStatus = neeQrError(code, msg);
                    emit qrChanged();
                    emit toast(m_qrStatus);
                    return;
                }
                m_qrStatus = QStringLiteral("正在重试获取二维码（%1/3）…").arg(step + 2);
                emit qrChanged();
                (*stages)(step + 1);
            };

            switch (step) {
            case 0:  // 官方客户端通道：eapi + 设备 header + 匿名令牌
                m_qrEapi = true;
                neteaseEapiPost(QStringLiteral("/api/login/qrcode/unikey"),
                                QJsonObject{{QStringLiteral("type"), 3}}, onReply);
                break;
            case 1: {  // 网页通道：明文 + 精简设备指纹（不带任何会话 Cookie）
                m_qrEapi = false;
                QVariantMap headers = neeWebHeaders();
                headers.insert(QStringLiteral("Cookie"),
                               QStringLiteral("deviceId=") + neteaseDeviceId()
                                   + QStringLiteral("; os=pc; appver=3.1.17.204416"
                                                    "; channel=netease; __remember_me=true"));
                HttpClient::instance()->request(
                    QStringLiteral("POST"),
                    QUrl(QStringLiteral("https://music.163.com/api/login/qrcode/unikey")),
                    headers, QByteArrayLiteral("type=3"), onReply);
                break;
            }
            default: {  // 最后一级：完全不带 Cookie，实测服务端仍会下发 unikey
                m_qrEapi = false;
                HttpClient::instance()->request(
                    QStringLiteral("POST"),
                    QUrl(QStringLiteral("https://music.163.com/api/login/qrcode/unikey")),
                    neeWebHeaders(), QByteArrayLiteral("type=3"), onReply);
                break;
            }
            }
        };
        (*stages)(0);
        return;
    }
    cancelQrLogin();
    MusicSource *s = SourceManager::instance()->source(sourceId);
    if (!s) return;
    const LoginConfig &login = s->desc().login;
    if (!login.qrCreate.valid()) {
        emit toast(QStringLiteral("该平台未配置二维码登录，如需扫码，请部署实现了二维码端点的账号 API 服务"));
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
    m_mgSession.clear();
    m_mgCookie.clear();
    m_mgTicket.clear();
    m_kwUuid.clear();
    m_kwPollBusy = false;
    m_kwTm.clear();
    m_kwPhone.clear();
    m_neeCookies.clear();
    m_qrKey.clear();
    m_qrSource.clear();
    m_qrImagePath.clear();
    m_qrHint = QStringLiteral("请点击「获取二维码」后用官方 App 扫码");
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
    if (m_qrMode == QLatin1String("migu")) {
        pollMiguQr();
        return;
    }
    if (m_qrMode == QLatin1String("kuwo_wx")) {
        pollKuwoWx();
        return;
    }
    if (m_qrMode == QLatin1String("qishui")) {
        pollQishuiQr();
        return;
    }
    // 网易云原生二维码轮询（官方网页同款明文接口，Cookie 与出码时同一个会话）
    if (m_qrMode == QLatin1String("netease")) {
        if (m_qrSource.isEmpty() || m_qrKey.isEmpty()) { m_poll.stop(); return; }
        const QString sourceId = m_qrSource;
        // 出码与轮询必须是同一套协议：Cookie 会话就绑在这次请求链上
        auto onCheck = [this, sourceId](const HttpClient::Reply &r) {
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
                        m_qrStatus = QStringLiteral("授权成功但未取到 MUSIC_U，请改用密码方式登录或重新扫码");
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
                } else if (code == QLatin1String("8821") || code == QLatin1String("8816")
                           || code == QLatin1String("460")) {
                    // 账号级风控终态：继续轮询只会让风控更重。立刻停，并明确引导改用密码方式登录
                    m_poll.stop();
                    m_polling = false;
                    m_qrStatus = neeQrError(code, Json::firstString(root, {
                        QStringLiteral("message"), QStringLiteral("msg")}));
                    emit qrChanged();
                    emit toast(m_qrStatus);
                } else {
                    m_qrStatus = QStringLiteral("等待扫码（接口返回 %1）").arg(code); emit qrChanged();
                }
        };
        // 轮询必须与出码用同一条通道：eapi 出码就 eapi 轮询，退回明文时也一起退回。
        // type 必须与出码一致（3），否则服务端认不出这个二维码属于哪种登录流程
        if (m_qrEapi) {
            neteaseEapiPost(QStringLiteral("/api/login/qrcode/client/login"),
                            QJsonObject{{QStringLiteral("key"), m_qrKey},
                                        {QStringLiteral("type"), 3}},
                            onCheck);
            return;
        }
        const QString keyEnc = QString::fromLatin1(QUrl::toPercentEncoding(m_qrKey));
        neteaseWebPost(
            QStringLiteral("/api/login/qrcode/client/login?key=%1&type=3").arg(keyEnc),
            QByteArrayLiteral("key=") + keyEnc.toUtf8() + QByteArrayLiteral("&type=3"),
            onCheck);
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
            } else if (code == QLatin1String("68")) {
                // ptqrlogin 的 68 = 用户在手机上拒绝/取消，说成「登录失败」会把该做的事藏起来
                m_poll.stop();
                m_polling = false;
                m_qrStatus = QStringLiteral("已在手机上取消登录，请重新获取二维码");
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
                // 未识别的状态码（含「在手机取消」这种没有公开取值的情况）必须回显，
                // 否则界面会停在上一条「已扫码，请确认」，用户只能干等到会话超时，
                // 也不知道该不该重新扫码。取消的语义按服务端文案判断，不猜数字码。
                const QString msg = Json::firstString(root, {QStringLiteral("data.msg"),
                                                             QStringLiteral("msg"),
                                                             QStringLiteral("error_msg")});
                if (msg.contains(QStringLiteral("取消"))
                    || msg.contains(QStringLiteral("拒绝"))
                    || msg.contains(QStringLiteral("cancel"), Qt::CaseInsensitive)) {
                    m_poll.stop();
                    m_polling = false;
                    m_qrStatus = QStringLiteral("已在手机上取消登录，请重新获取二维码");
                } else {
                    m_qrStatus = QStringLiteral("等待扫码（接口返回 %1）")
                                     .arg(st.isEmpty() ? QStringLiteral("空") : st);
                }
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

void AuthManager::sendPhoneCode(const QString &sourceId, const QString &phone, const QString &captcha)
{
    if (sourceId == QLatin1String("kuwo")) {
        sendKuwoSms(phone.trimmed(), captcha);
        return;
    }
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
    if (sourceId == QLatin1String("kuwo")) {
        loginKuwoPhone(phone.trimmed(), code.trimmed());
        return;
    }
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

namespace {

// 与真实浏览器一致的常规请求头。网易云风控会把「只有 UA/Referer 的裸请求」
// 判成异常环境（460 / 8821），这里补齐浏览器必带的一组头。
QVariantMap neeWebHeaders(const QString &cookie)
{
    QVariantMap h;
    h.insert(QStringLiteral("User-Agent"),
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                       "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"));
    h.insert(QStringLiteral("Referer"), QStringLiteral("https://music.163.com/"));
    h.insert(QStringLiteral("Origin"), QStringLiteral("https://music.163.com"));
    h.insert(QStringLiteral("Accept"), QStringLiteral("*/*"));
    h.insert(QStringLiteral("Accept-Language"), QStringLiteral("zh-CN,zh;q=0.9,en;q=0.8"));
    h.insert(QStringLiteral("Content-Type"),
        QStringLiteral("application/x-www-form-urlencoded"));
    h.insert(QStringLiteral("Sec-Fetch-Dest"), QStringLiteral("empty"));
    h.insert(QStringLiteral("Sec-Fetch-Mode"), QStringLiteral("cors"));
    h.insert(QStringLiteral("Sec-Fetch-Site"), QStringLiteral("same-origin"));
    h.insert(QStringLiteral("sec-ch-ua"),
        QStringLiteral("\"Chromium\";v=\"124\", \"Google Chrome\";v=\"124\", \"Not-A.Brand\";v=\"99\""));
    h.insert(QStringLiteral("sec-ch-ua-mobile"), QStringLiteral("?0"));
    h.insert(QStringLiteral("sec-ch-ua-platform"), QStringLiteral("\"Windows\""));
    h.insert(QStringLiteral("Cache-Control"), QStringLiteral("no-cache"));
    // 可选：出口 IP 被判为机房/海外时，在设置里填一个国内住宅 IP（pyncm 同款绕法）
    const QString realIp = Settings::string(QStringLiteral("neteaseRealIp")).trimmed();
    if (!realIp.isEmpty()) h.insert(QStringLiteral("X-Real-IP"), realIp);
    if (!cookie.isEmpty()) h.insert(QStringLiteral("Cookie"), cookie);
    return h;
}

// 汽水 passport 类端点（出码 / check_qrconnect）的常规浏览器头：UA 用真实 Chrome 内核，
// 服务端出码接口对浏览器指纹（UA/头）有校验，裸 QNetworkAccessManager 请求会被拒。
QVariantMap qishuiPassportHeaders(const QString &cookie)
{
    QVariantMap h;
    h.insert(QStringLiteral("User-Agent"),
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                       "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"));
    h.insert(QStringLiteral("Accept"), QStringLiteral("application/json, text/plain, */*"));
    if (!cookie.isEmpty()) h.insert(QStringLiteral("Cookie"), cookie);
    return h;
}

// 把网易云的业务错误码翻译成能行动的提示，别把所有失败都叫「风控」
QString neeLoginError(const QString &code, const QString &serverMessage)
{
    QString hint;
    if (code == QLatin1String("502")) hint = QStringLiteral("账号或密码错误");
    else if (code == QLatin1String("501")) hint = QStringLiteral("账号不存在");
    else if (code == QLatin1String("509")) hint = QStringLiteral("密码错误次数超限，账号被临时锁定；请等 30~60 分钟，不要再连续重试");
    else if (code == QLatin1String("460")) hint = QStringLiteral("当前出口 IP 被判异常（机房/海外）；可在设置里填一个国内住宅 IP，或更换网络后重试");
    else if (code == QLatin1String("8821")) hint = QStringLiteral("网易云要求行为验证码：第三方客户端的密码/扫码登录被账号级风控拦截，请在浏览器登录官网后导入 Cookie（应用内无法完成滑块验证）");
    else if (code == QLatin1String("8816")) hint = QStringLiteral("账号存在风险，需先在手机客户端登录一次解除二次验证");
    else if (code == QLatin1String("8822")) hint = QStringLiteral("登录凭据被拒绝");
    else if (code == QLatin1String("8000")) hint = QStringLiteral("请求过于频繁，请稍后再试");
    else if (code == QLatin1String("400")) hint = QStringLiteral("请求参数有误");
    else hint = serverMessage.isEmpty() ? QStringLiteral("登录被拒绝") : serverMessage;

    QString out = QStringLiteral("登录失败（%1）：%2").arg(code, hint);
    if (!serverMessage.isEmpty() && serverMessage != hint)
        out += QStringLiteral(" ｜ 服务端：") + serverMessage;
    if (code == QLatin1String("460") || code == QLatin1String("8816")
        || code == QLatin1String("8000"))
        out += QStringLiteral("。建议改用二维码或稍后重试");
    return out;
}

QString neeQrError(const QString &code, const QString &serverMessage)
{
    if (code == QLatin1String("8821"))
        return QStringLiteral("网易云账号级风控（8821）：服务端拒绝向第三方客户端签发登录态，"
                              "换协议/UA/参数都绕不过。建议改用密码方式登录");
    if (code == QLatin1String("8816"))
        return QStringLiteral("账号需要二次验证（8816）：请先在手机客户端登录一次解除，再改用密码方式登录");
    if (code == QLatin1String("460"))
        return QStringLiteral("当前出口 IP 被判异常（460），请在设置里填国内住宅 IP 或更换网络后重试");
    return QStringLiteral("扫码登录被拒绝（%1）：%2")
        .arg(code, serverMessage.isEmpty() ? QStringLiteral("请改用密码方式登录") : serverMessage);
}

} // namespace

void AuthManager::weapiPost(const QString &path, const QJsonObject &payload,
                            const QString &cookie, const HttpClient::Callback &cb)
{
    const QUrl url(QStringLiteral("https://music.163.com") + path);
    QVariantMap headers = neeWebHeaders(cookie);
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
    // 允许直接传完整 URL（扫码接口要把 key / csrf_token 放 query 里）
    const QUrl url(path.startsWith(QLatin1String("http"))
                       ? path
                       : QStringLiteral("https://music.163.com") + path);
    QVariantMap headers = neeWebHeaders(neteaseCookieHeader());
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
    // 只带服务端真正下发过的 Cookie，再补上官方客户端必带的设备指纹字段。
    // 参考 NeteaseCloudMusicApi 的 processCookieObject：缺少 deviceId/os/appver/channel
    // 时，出码能成功，但手机上一确认就会被判异常环境（8821）。
    QMap<QString, QString> c = m_neeCookies;
    if (!c.contains(QStringLiteral("__csrf")))
        c.insert(QStringLiteral("__csrf"), neteaseCsrfToken());
    if (!c.contains(QStringLiteral("deviceId"))) c.insert(QStringLiteral("deviceId"), neteaseDeviceId());
    if (!c.contains(QStringLiteral("os"))) c.insert(QStringLiteral("os"), QStringLiteral("pc"));
    if (!c.contains(QStringLiteral("appver")))
        c.insert(QStringLiteral("appver"), QStringLiteral("3.1.17.204416"));
    if (!c.contains(QStringLiteral("osver")))
        c.insert(QStringLiteral("osver"),
                 QStringLiteral("Microsoft-Windows-10-Professional-build-19045-64bit"));
    if (!c.contains(QStringLiteral("channel")))
        c.insert(QStringLiteral("channel"), QStringLiteral("netease"));
    if (!c.contains(QStringLiteral("WEVNSM")))
        c.insert(QStringLiteral("WEVNSM"), QStringLiteral("1.0.0"));
    if (!c.contains(QStringLiteral("__remember_me")))
        c.insert(QStringLiteral("__remember_me"), QStringLiteral("true"));
    if (!c.contains(QStringLiteral("ntes_kaola_ad")))
        c.insert(QStringLiteral("ntes_kaola_ad"), QStringLiteral("1"));
    // _ntes_nuid/_ntes_nnid/WNMCID 是设备侧生成的稳定标识，持久化后复用
    QString nuid = Settings::string(QStringLiteral("neteaseNtesNuid"));
    if (nuid.isEmpty()) {
        static const char hexChars[] = "0123456789abcdef";
        for (int i = 0; i < 32; ++i)
            nuid.append(QLatin1Char(hexChars[QRandomGenerator::global()->bounded(16)]));
        Settings::set(QStringLiteral("neteaseNtesNuid"), nuid);
    }
    if (!c.contains(QStringLiteral("_ntes_nuid"))) c.insert(QStringLiteral("_ntes_nuid"), nuid);
    if (!c.contains(QStringLiteral("_ntes_nnid")))
        c.insert(QStringLiteral("_ntes_nnid"),
                 nuid + QLatin1Char(',') +
                     QString::number(QDateTime::currentMSecsSinceEpoch()));
    QString wnmcid = Settings::string(QStringLiteral("neteaseWnmcid"));
    if (wnmcid.isEmpty()) {
        static const char lower[] = "abcdefghijklmnopqrstuvwxyz";
        QString prefix;
        for (int i = 0; i < 6; ++i)
            prefix.append(QLatin1Char(lower[QRandomGenerator::global()->bounded(26)]));
        wnmcid = prefix + QLatin1Char('.') +
                 QString::number(QDateTime::currentMSecsSinceEpoch()) + QStringLiteral(".01.0");
        Settings::set(QStringLiteral("neteaseWnmcid"), wnmcid);
    }
    if (!c.contains(QStringLiteral("WNMCID"))) c.insert(QStringLiteral("WNMCID"), wnmcid);
    // 匿名令牌：没有 MUSIC_U 时用 MUSIC_A 兜底，服务端据此认定为「已注册的设备」
    if (!c.contains(QStringLiteral("MUSIC_U")) && !c.contains(QStringLiteral("MUSIC_A"))) {
        const QString ma = m_neeMusicA.isEmpty()
                               ? Settings::string(QStringLiteral("neteaseMusicA"))
                               : m_neeMusicA;
        if (!ma.isEmpty()) c.insert(QStringLiteral("MUSIC_A"), ma);
    }
    QStringList parts;
    for (auto it = c.constBegin(); it != c.constEnd(); ++it) {
        if (it.value().isEmpty()) continue;   // 空值 Cookie 会被判成畸形请求
        parts << it.key() + QLatin1Char('=') + it.value();
    }
    return parts.join(QStringLiteral("; "));
}

// 匿名设备令牌：官方客户端启动后第一件事就是 POST /weapi/register/anonimous，
// 拿一个 MUSIC_A 出来当「已注册设备」的身份，后续所有请求都带上它。
// username 的构造规则（NeteaseCloudMusicApi register_anonimous）：
//   base64( deviceId + " " + base64( md5( deviceId XOR "3go8&$8*3*3h0k(2)2" ) ) )
// 缺这一步时出码正常，但手机确认后服务端会回 8821（要求行为验证）。
void AuthManager::neteaseBootstrap(std::function<void(bool)> done)
{
    if (m_neeMusicA.isEmpty())
        m_neeMusicA = Settings::string(QStringLiteral("neteaseMusicA"));
    if (!m_neeMusicA.isEmpty()) {
        if (done) done(true);
        return;
    }
    m_neeBootWaiters.append(done);
    if (m_neeBootstrapping) return;
    m_neeBootstrapping = true;

    const QByteArray xorKey = QByteArrayLiteral("3go8&$8*3*3h0k(2)2");
    const QString did = neteaseDeviceId();
    QByteArray xored;
    for (int i = 0; i < did.size(); ++i)
        xored.append(char(did.at(i).toLatin1() ^ xorKey.at(i % xorKey.size())));
    const QByteArray digest = QCryptographicHash::hash(xored, QCryptographicHash::Md5).toBase64();
    const QString username = QString::fromLatin1((did.toUtf8() + QByteArrayLiteral(" ") + digest).toBase64());

    QJsonObject payload;
    payload.insert(QStringLiteral("username"), username);
    payload.insert(QStringLiteral("csrf_token"),
                   m_neeCookies.value(QStringLiteral("__csrf"), neteaseCsrfToken()));
    weapiPost(QStringLiteral("/weapi/register/anonimous"), payload, neteaseCookieHeader(),
        [this](const HttpClient::Reply &r) {
            const QJsonValue root = Json::rootOf(r.json);
            QString cookie = Json::stringAt(root, QStringLiteral("cookie"));
            if (cookie.isEmpty()) cookie = cookiesFromReply(r, QVariantMap(), root);
            const QString token = cookieValue(cookie, {QStringLiteral("MUSIC_A")});
            if (!token.isEmpty()) {
                m_neeMusicA = token;
                Settings::set(QStringLiteral("neteaseMusicA"), token);
            }
            m_neeBootstrapping = false;
            const bool ok = !m_neeMusicA.isEmpty();
            const QVector<std::function<void(bool)>> waiters = m_neeBootWaiters;
            m_neeBootWaiters.clear();
            for (const std::function<void(bool)> &w : waiters)
                if (w) w(ok);
        });
}

// eapi：官方客户端的加密通道。path 传 /api/xxx，实际请求 https://interface.music.163.com/eapi/xxx
// body 只有 params（AES-128-ECB），设备字段放在 body.header 里，同时原样作为 Cookie 发送。
void AuthManager::neteaseEapiPost(const QString &apiPath, const QJsonObject &payload,
                                  const HttpClient::Callback &cb)
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const QString requestId =
        QString::number(nowMs) + QLatin1Char('_') +
        QString::number(QRandomGenerator::global()->bounded(1000)).rightJustified(4, QLatin1Char('0'));

    QJsonObject header;
    header.insert(QStringLiteral("osver"),
                  QStringLiteral("Microsoft-Windows-10-Professional-build-19045-64bit"));
    header.insert(QStringLiteral("deviceId"), neteaseDeviceId());
    header.insert(QStringLiteral("os"), QStringLiteral("pc"));
    header.insert(QStringLiteral("appver"), QStringLiteral("3.1.17.204416"));
    header.insert(QStringLiteral("versioncode"), QStringLiteral("140"));
    header.insert(QStringLiteral("mobilename"), QString());
    header.insert(QStringLiteral("buildver"),
                  QString::number(nowMs / 1000).left(10));
    header.insert(QStringLiteral("resolution"), QStringLiteral("1920x1080"));
    header.insert(QStringLiteral("__csrf"), m_neeCookies.value(QStringLiteral("__csrf")));
    header.insert(QStringLiteral("channel"), QStringLiteral("netease"));
    header.insert(QStringLiteral("requestId"), requestId);
    const QString musicA = m_neeMusicA.isEmpty() ? Settings::string(QStringLiteral("neteaseMusicA"))
                                                 : m_neeMusicA;
    if (!musicA.isEmpty()) header.insert(QStringLiteral("MUSIC_A"), musicA);
    const QString musicU = m_neeCookies.value(QStringLiteral("MUSIC_U"));
    if (!musicU.isEmpty()) header.insert(QStringLiteral("MUSIC_U"), musicU);

    QJsonObject body = payload;
    body.insert(QStringLiteral("header"), header);
    body.insert(QStringLiteral("e_r"), false);

    QStringList cookieParts;
    for (auto it = header.constBegin(); it != header.constEnd(); ++it) {
        const QString v = it.value().toString();
        if (v.isEmpty()) continue;   // __csrf / mobilename / MUSIC_U 空值不要进 Cookie 头
        cookieParts << it.key() + QLatin1Char('=') + v;
    }
    for (auto it = m_neeCookies.constBegin(); it != m_neeCookies.constEnd(); ++it) {
        if (!header.contains(it.key()) && !it.value().isEmpty())
            cookieParts << it.key() + QLatin1Char('=') + it.value();
    }

    QVariantMap headers = neeWebHeaders();
    headers.insert(QStringLiteral("User-Agent"),
                   QStringLiteral("NeteaseMusic 9.0.90/5038 (iPhone; iOS 16.2; zh_CN)"));
    headers.insert(QStringLiteral("Cookie"), cookieParts.join(QStringLiteral("; ")));

    QString api = apiPath;
    if (!api.startsWith(QLatin1String("/api/"))) api = QStringLiteral("/api/") + api;
    const QUrl url(QStringLiteral("https://interface.music.163.com/eapi/") + api.mid(5));
    HttpClient::Callback handler = cb;
    if (!qEnvironmentVariableIsEmpty("FURUI_DIAG_HTTP")) {
        const QString logPath = qEnvironmentVariable("FURUI_DIAG_HTTP");
        handler = [logPath, api, cb](const HttpClient::Reply &r) {
            QFile f(logPath);
            if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
                f.write(diagHttpLine(QStringLiteral("eapi"), api, r));
                f.close();
            }
            if (cb) cb(r);
        };
    }
    HttpClient::instance()->request(QStringLiteral("POST"), url, headers,
                                    Netease::eapiForm(api, body), handler);
}

QString AuthManager::qishuiCookieHeader() const
{
    // Qt5 的 QMap range-for 只给 value，取键值对必须走迭代器
    QString cookie;
    for (auto it = m_qsCookies.constBegin(); it != m_qsCookies.constEnd(); ++it) {
        if (!cookie.isEmpty()) cookie += QLatin1Char(';');
        cookie += it.key() + QLatin1Char('=') + it.value();
    }
    return cookie;
}

void AuthManager::generateQrImage(const QString &content, const QString &sourceId)
{
    using namespace qrcodegen;
    // qrcodegen 对超长内容会抛 data_too_long / std::length_error（ECC MEDIUM 上限
    // 2953 字节），saveQrDataUrl 的兜底路径会把整条 data URL 喂进来，不接异常
    // 就是 std::terminate 直接崩掉整个进程。
    QrCode *qr = nullptr;
    try {
        if (content.size() > 2953)
            throw std::length_error("Data too long");
        qr = new QrCode(QrCode::encodeText(content.toUtf8().constData(), QrCode::Ecc::MEDIUM));
    } catch (const std::exception &) {
        delete qr;
        // 清掉上一张成功的图：saveQrDataUrl 用 m_qrImagePath 是否为空判定本次
        // 成败，不清的话失败仍返回 true，登录流程会误以为出码成功
        m_qrImagePath.clear();
        m_qrStatus = QStringLiteral("二维码生成失败：内容过长，请点击刷新重试");
        m_qrHint = QStringLiteral("生成失败，点击二维码区域刷新");
        emit toast(m_qrStatus);
        emit qrChanged();
        return;
    }
    const int size = qr->getSize();
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
            if (qr->getModule(x, y))
                painter.drawRect((x + border) * scale, (y + border) * scale, scale, scale);
        }
    }
    delete qr;
    const QString path = Settings::filePath(QStringLiteral("qr_") + sourceId + QStringLiteral(".png"));
    if (img.save(path, "PNG")) {
        m_qrImagePath = path;
        emit qrChanged();
    }
}

bool AuthManager::saveQrDataUrl(const QString &dataUrl, const QString &sourceId)
{
    // 汽水 passport 出码返回的 qrcode 是 data:image/png;base64,... 形态的完整二维码图片，
    // 不能拿它当二维码内容再用 QrCode 重新编码 —— 直接解码落盘即可。
    const int comma = dataUrl.indexOf(QLatin1Char(','));
    if (comma < 0) {
        generateQrImage(dataUrl, sourceId);
        return !m_qrImagePath.isEmpty();
    }
    const QByteArray img = QByteArray::fromBase64(dataUrl.mid(comma + 1).toUtf8());
    if (img.isEmpty()) {
        generateQrImage(dataUrl, sourceId);
        return !m_qrImagePath.isEmpty();
    }
    const QString path = Settings::filePath(QStringLiteral("qr_") + sourceId + QStringLiteral(".png"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(img);
    f.close();
    if (f.error() != QFileDevice::NoError) return false;
    m_qrImagePath = path;
    emit qrChanged();
    return true;
}

void AuthManager::loginKugouPassword(const QString &account, const QString &password)
{
    // 与 MakcRe/KuGouMusicApi module/login.js 逐参数对齐：
    //   POST https://login.user.kugou.com/v9/login_by_pwd（x-router 指定后端）
    //   params = AES-256-CBC({pwd,code:"",clienttime_ms}) 的 hex
    //   pk     = RSA_NO_PADDING({clienttime_ms,key:tempKey}) 的大写 hex
    //   query  = 设备参数 + signature（App 盐，body 原文参与签名）
    const QString sourceId = QStringLiteral("kugou");
    const QString tempKey = KugouLoginCrypto::randomTempKey();
    const QByteArray key = KugouLoginCrypto::keyFromTempKey(tempKey);
    const QByteArray iv = KugouLoginCrypto::ivFromKey(key);
    const qint64 ts = QDateTime::currentMSecsSinceEpoch();

    QJsonObject pwdObj;
    pwdObj.insert(QStringLiteral("pwd"), password);
    pwdObj.insert(QStringLiteral("code"), QStringLiteral(""));
    pwdObj.insert(QStringLiteral("clienttime_ms"), ts);
    const QString paramsHex = QString::fromLatin1(KugouLoginCrypto::toHexLower(
        KugouLoginCrypto::aes256CbcEncrypt(
            QJsonDocument(pwdObj).toJson(QJsonDocument::Compact), key, iv)));

    QJsonObject pkObj;
    pkObj.insert(QStringLiteral("clienttime_ms"), ts);
    pkObj.insert(QStringLiteral("key"), tempKey);
    const QString pkHex = KugouLoginCrypto::rsaNoPaddingHex(
        QJsonDocument(pkObj).toJson(QJsonDocument::Compact));

    if (paramsHex.isEmpty() || pkHex.isEmpty()) {
        const QString msg = QStringLiteral("酷狗登录参数加密失败，请重试");
        emit loginStateChanged(sourceId, false, msg);
        emit toast(msg);
        return;
    }

    QJsonObject bodyObj;
    bodyObj.insert(QStringLiteral("plat"), 1);
    bodyObj.insert(QStringLiteral("support_multi"), 1);
    bodyObj.insert(QStringLiteral("clienttime_ms"), ts);
    bodyObj.insert(QStringLiteral("t1"), 0);
    bodyObj.insert(QStringLiteral("t2"), 0);
    bodyObj.insert(QStringLiteral("t3"), QStringLiteral("MCwwLDAsMCwwLDAsMCwwLDA="));
    bodyObj.insert(QStringLiteral("username"), account);
    bodyObj.insert(QStringLiteral("params"), paramsHex);
    bodyObj.insert(QStringLiteral("pk"), pkHex);
    const QByteArray body = QJsonDocument(bodyObj).toJson(QJsonDocument::Compact);

    // 设备指纹：dfid/mid/uuid 三者在一次登录里保持同源，签名用 App 盐
    const QString dfid = QStringLiteral("%1").arg(quint64(QRandomGenerator::global()->generate64()),
                                                  16, 16, QLatin1Char('0'))
                       + QStringLiteral("%1").arg(quint64(QRandomGenerator::global()->generate64()),
                                                  16, 16, QLatin1Char('0'));
    const QString mid = QString::fromLatin1(
        QCryptographicHash::hash(dfid.toUtf8(), QCryptographicHash::Md5).toHex());
    const QString uuid = QString::fromLatin1(
        QCryptographicHash::hash((dfid + mid).toUtf8(), QCryptographicHash::Md5).toHex());
    QVariantMap p;
    p.insert(QStringLiteral("dfid"), dfid);
    p.insert(QStringLiteral("mid"), mid);
    p.insert(QStringLiteral("uuid"), uuid);
    p.insert(QStringLiteral("appid"), 1005);
    p.insert(QStringLiteral("clientver"), 20489);
    p.insert(QStringLiteral("userid"), 0);
    p.insert(QStringLiteral("clienttime"), furuiCurrentSecsSinceEpoch());
    const QString url = Kugou::signedUrl(
        QStringLiteral("https://login.user.kugou.com/v9/login_by_pwd"), p, body,
        Kugou::Keyparam::App);

    QVariantMap headers;
    headers.insert(QStringLiteral("Content-Type"), QStringLiteral("application/json"));
    headers.insert(QStringLiteral("x-router"), QStringLiteral("login.user.kugou.com"));
    headers.insert(QStringLiteral("User-Agent"),
        QStringLiteral("Android15-1070-11083-46-0-DiscoveryDRADProtocol-wifi"));
    headers.insert(QStringLiteral("dfid"), dfid);
    headers.insert(QStringLiteral("mid"), mid);
    headers.insert(QStringLiteral("clienttime"), p.value(QStringLiteral("clienttime")));

    HttpClient::instance()->request(QStringLiteral("POST"), QUrl(url), headers, body,
        [this, sourceId, key, iv](const HttpClient::Reply &r) {
            if (!r.ok) {
                const QString msg = QStringLiteral("酷狗登录请求失败：") + shortNetworkError(r.error);
                emit loginStateChanged(sourceId, false, msg);
                emit toast(msg);
                return;
            }
            const QJsonObject obj = r.json.object();
            const int status = obj.value(QStringLiteral("status")).toInt(-1);
            const int errCode = obj.value(QStringLiteral("error_code")).toInt(0);

            // 20028「请验证」：服务端要求人机验证（响应头 ssa-code）。
            // 按产品约定不伪造行为指纹绕过，如实告知用户可手动完成的路径。
            if (errCode == 20028) {
                const QString msg = QStringLiteral(
                    "酷狗要求人机验证（滑块）。本应用不内嵌网页验证："
                    "请先在酷狗官方 App/网页完成一次登录验证，再回来重试；"
                    "或改用同页的「二维码」登录。");
                emit loginStateChanged(sourceId, false, msg);
                emit toast(msg);
                return;
            }

            const QJsonObject data = obj.value(QStringLiteral("data")).toObject();
            if (status != 1) {
                const QString serverMsg = obj.value(QStringLiteral("data")).isString()
                    ? obj.value(QStringLiteral("data")).toString() : QString();
                const QString msg = serverMsg.isEmpty()
                    ? QStringLiteral("登录失败（酷狗返回 %1 / %2）").arg(status).arg(errCode)
                    : QStringLiteral("登录失败：%1（%2）").arg(serverMsg).arg(errCode);
                emit loginStateChanged(sourceId, false, msg);
                emit toast(msg);
                return;
            }

            // 成功：secu_params 用同一把 AES key 解密，里面是 token / userid / vip 等
            QString token = data.value(QStringLiteral("token")).toString();
            QString userid = data.value(QStringLiteral("userid")).toString();
            const QString secu = data.value(QStringLiteral("secu_params")).toString();
            if (!secu.isEmpty()) {
                const QByteArray plain = KugouLoginCrypto::aes256CbcDecrypt(
                    QByteArray::fromHex(secu.toLatin1()), key, iv);
                const QJsonDocument doc = QJsonDocument::fromJson(plain);
                if (doc.isObject()) {
                    const QJsonObject t = doc.object();
                    token = t.value(QStringLiteral("token")).toString(token);
                    userid = t.value(QStringLiteral("userid")).toString(userid);
                } else if (!plain.isEmpty()) {
                    token = QString::fromUtf8(plain);   // 旧版接口直接回一个裸 token 字符串
                }
            }
            if (token.isEmpty()) {
                const QString msg = QStringLiteral("登录失败：酷狗未返回 token");
                emit loginStateChanged(sourceId, false, msg);
                emit toast(msg);
                return;
            }
            QStringList cookieParts;
            cookieParts << QStringLiteral("token=%1").arg(token);
            cookieParts << QStringLiteral("userid=%1").arg(userid.isEmpty() ? QStringLiteral("0") : userid);
            for (const QString &k : {QStringLiteral("vip_type"), QStringLiteral("vip_token"),
                                     QStringLiteral("t1")}) {
                const QJsonValue v = data.value(k);
                if (!v.isUndefined() && !v.isNull())
                    cookieParts << QStringLiteral("%1=%2").arg(k, v.toVariant().toString());
            }
            saveSession(sourceId, cookieParts.join(QStringLiteral("; ")), userid,
                        QString(), QString(), QStringLiteral("密码登录成功"));
            syncUserLibrary(sourceId);
        }, -1, true, QStringLiteral("kugou.com"));
}

void AuthManager::loginByPassword(const QString &sourceId, const QString &account,
                                  const QString &password, const QString &captcha)
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
    if (sourceId == QLatin1String("migu")) {
        loginMiguPassword(sourceId, acc, password);
        return;
    }
    if (sourceId == QLatin1String("kuwo")) {
        loginKuwoPassword(acc, password, captcha);
        return;
    }
    if (sourceId == QLatin1String("kugou")) {
        loginKugouPassword(acc, password);
        return;
    }
    if (driver != QLatin1String("netease")) {
        // 汽水这类「网页端也被 SDK 签名挡住」的平台，按实际卡点给话，别用一句「未内置」糊过去；
        // 2026-11 已确认官方 passport 出码链路对浏览器常规头直连可用，密码登录仍无内置实现
        if (sourceId == QLatin1String("qishui")) {
            emit loginStateChanged(sourceId, false,
                QStringLiteral("汽水音乐仅支持扫码登录：客户端已内置官方 passport 出码链路，"
                               "请改用「二维码」登录。"));
            emit toast(QStringLiteral("汽水音乐仅支持扫码登录"));
            return;
        }
        emit loginStateChanged(sourceId, false,
            QStringLiteral("该平台未内置密码登录，请改用二维码或配置账号 API 服务"));
        return;
    }

    const QString md5 = QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Md5).toHex();
    const bool isEmail = acc.contains(QLatin1Char('@'));
    // 浏览器会在提交前把手机号规范化；应用内直连若原样把「+86 138 0013 8000」
    // 发给服务端，就只会拿到 502「账号或密码错误」，看起来像密码错了。
    QString phone = acc;
    if (!isEmail) {
        phone.remove(QRegularExpression(QStringLiteral("[\\s\\-()]")));
        if (phone.startsWith(QLatin1String("+86"))) phone = phone.mid(3);
        else if (phone.startsWith(QLatin1String("86")) && phone.size() > 11) phone = phone.mid(2);
    }

    // 与 AlgerMusicPlayer 内置的 netease-cloud-music-api-alger 的 login_cellphone 逐参数对齐：
    //   URL  : POST /weapi/w/login/cellphone（注意是 w/ 前缀；老的 /weapi/login/cellphone 用同一
    //          个密码会一直回 502「账号或密码错误」，这就是「密码登录也失败」的直接原因）
    //   body : type=1 / https=true / phone / countrycode=86 / password=md5 / remember=true
    //   Cookie: 设备指纹 + 匿名令牌 MUSIC_A + __csrf（与 body 的 csrf_token 一致）
    neteaseBootstrap([this, sourceId, acc, phone, md5, isEmail](bool bootOk) {
        if (!bootOk) {
            const QString msg = QStringLiteral("获取匿名设备令牌失败（网络异常），请稍后重试");
            emit loginStateChanged(sourceId, false, msg);
            emit toast(msg);
            return;
        }
        QJsonObject payload;
        const QString endpoint = isEmail ? QStringLiteral("/weapi/login")
                                         : QStringLiteral("/weapi/w/login/cellphone");
        if (isEmail) {
            payload.insert(QStringLiteral("username"), acc);
            payload.insert(QStringLiteral("rememberLogin"), QStringLiteral("true"));
        } else {
            payload.insert(QStringLiteral("type"), QStringLiteral("1"));
            payload.insert(QStringLiteral("https"), QStringLiteral("true"));
            payload.insert(QStringLiteral("phone"), phone);
            payload.insert(QStringLiteral("countrycode"), QStringLiteral("86"));
            payload.insert(QStringLiteral("remember"), QStringLiteral("true"));
        }
        payload.insert(QStringLiteral("password"), md5);
        // 双提交：与 neteaseCookieHeader() 里的 __csrf 保持同一个值
        payload.insert(QStringLiteral("csrf_token"),
                       m_neeCookies.value(QStringLiteral("__csrf"), neteaseCsrfToken()));

        // 主端点不通（个别账号/灰度环境仍走老路径）时自动回退一次，别让用户只看到一个 502
        auto send = std::make_shared<std::function<void(const QString &)>>();
        *send = [this, sourceId, payload, send](const QString &path) {
            weapiPost(path, payload, neteaseCookieHeader(),
            [this, sourceId, path, send](const HttpClient::Reply &r) {
                if (!r.ok) {
                    const QString msg = QStringLiteral("登录失败：") + shortNetworkError(r.error);
                    emit loginStateChanged(sourceId, false, msg);
                    emit toast(msg);
                    return;
                }
                const QJsonValue root = Json::rootOf(r.json);
                const QString code = Json::stringAt(root, QStringLiteral("code"));
                if (code != QLatin1String("200")) {
                    // 端点不存在/参数不被接受时换老路径再试一次，其余错误原样报给用户
                    if (path.endsWith(QStringLiteral("/w/login/cellphone"))
                        && (code == QLatin1String("400") || code == QLatin1String("404")
                            || code.isEmpty())) {
                        (*send)(QStringLiteral("/weapi/login/cellphone"));
                        return;
                    }
                    const QString msg = neeLoginError(code, Json::firstString(root, {
                        QStringLiteral("message"), QStringLiteral("msg")}));
                    emit loginStateChanged(sourceId, false, msg);
                    emit toast(msg);
                    return;
                }
                // 登录成功的响应体里带 cookie 字段（MUSIC_U），优先用它；没有再看 Set-Cookie
                QString cookie = cookiesFromReply(
                    r, QVariantMap{{QStringLiteral("cookiePath"), QStringLiteral("cookie")}}, root);
                if (cookie.trimmed().isEmpty())
                    cookie = neteaseCookieHeader();
                const QJsonValue profile = root.toObject().value(QStringLiteral("profile"));
                const QString uid = Json::firstString(profile, {QStringLiteral("userId"),
                                                                QStringLiteral("id")});
                const QString nick = Json::firstString(profile, {QStringLiteral("nickname")});
                const QString avatar = Json::firstString(profile, {QStringLiteral("avatarUrl")});
                saveSession(sourceId, cookie, uid, nick, avatar, QStringLiteral("密码登录成功"));
                syncUserLibrary(sourceId);
            });
        };
        (*send)(endpoint);
    });
}

// ---- 浏览器登录态导入 ---------------------------------------------------
// 2026-11 用真实账号实测：网易云对第三方客户端的 weapi 密码登录一律回 8821
// 「需要行为验证码验证」，换端点/UA/Cookie/加密都绕不过（浏览器里能完成滑块验证，
// 应用内没有 WebEngine 就做不了）。所以给一条确定能走的退路：用户在本机浏览器
// 登录官网后，把 Cookie（至少含 MUSIC_U）粘回来，由应用保存登录态并同步歌单。
void AuthManager::loginByCookie(const QString &sourceId, const QString &rawCookie)
{
    // 浏览器开发者工具既能「Copy value」也能「Copy as cURL / 复制整行」，
    // 后者会带 "Cookie: " 前缀；不剥掉的话 onlyCookiePairs 会因为键名含空格
    // 把第一对（往往正是 MUSIC_U）丢掉。
    QString raw = rawCookie.trimmed();
    if (raw.startsWith(QLatin1String("Cookie:"), Qt::CaseInsensitive))
        raw = raw.mid(7).trimmed();
    QString cookie = onlyCookiePairs(raw);
    if (cookie.isEmpty()) {
        const QString msg = QStringLiteral("Cookie 不能为空：请从浏览器开发者工具复制完整 Cookie");
        emit loginStateChanged(sourceId, false, msg);
        emit toast(msg);
        return;
    }
    if (sourceId == QLatin1String("netease")
        && cookieValue(cookie, {QStringLiteral("MUSIC_U")}).isEmpty()) {
        const QString msg = QStringLiteral("Cookie 里没有 MUSIC_U，这不是登录后的 Cookie；"
                                           "请在官网登录成功后再复制");
        emit loginStateChanged(sourceId, false, msg);
        emit toast(msg);
        return;
    }

    // 浏览器 Cookie 里通常自带 __csrf；从 document.cookie 复制时可能被 HttpOnly 挡掉。
    // 后续 weapi 请求要用它做 csrf_token 双提交，缺了会静默失败，这里补一个稳定的本地令牌。
    if (sourceId == QLatin1String("netease")
        && cookieValue(cookie, {QStringLiteral("__csrf")}).isEmpty()) {
        cookie += QStringLiteral("; __csrf=") + neteaseCsrfToken();
    }

    // 保存登录态不依赖资料接口：即使 /account/get 被风控挡了，Cookie 仍然是有效的，
    // 先落盘、再异步补昵称/头像，避免用户白等一次请求。
    auto commit = [this, sourceId, cookie](const QString &uid, const QString &nick,
                                           const QString &avatar) {
        saveSession(sourceId, cookie, uid, nick, avatar,
                    nick.isEmpty() ? QStringLiteral("浏览器登录态已导入")
                                   : QStringLiteral("浏览器登录成功：") + nick);
        syncUserLibrary(sourceId);
    };

    if (sourceId != QLatin1String("netease")) {
        commit(QString(), QString(), QString());
        return;
    }

    QJsonObject payload;
    payload.insert(QStringLiteral("csrf_token"),
                   cookieValue(cookie, {QStringLiteral("__csrf")}));
    weapiPost(QStringLiteral("/weapi/w/nuser/account/get"), payload, cookie,
        [this, sourceId, cookie, commit](const HttpClient::Reply &r) {
            QString uid, nick, avatar;
            if (r.ok) {
                const QJsonValue root = Json::rootOf(r.json);
                const QJsonValue profile = root.toObject().value(QStringLiteral("profile"));
                uid = Json::firstString(profile, {QStringLiteral("userId"), QStringLiteral("id")});
                nick = Json::firstString(profile, {QStringLiteral("nickname")});
                avatar = Json::firstString(profile, {QStringLiteral("avatarUrl")});
                if (uid.isEmpty()) uid = Json::firstString(root, {QStringLiteral("userId"),
                                                                  QStringLiteral("id")});
                if (nick.isEmpty()) nick = Json::firstString(root, {QStringLiteral("nickname")});
            }
            commit(uid, nick, avatar);
        });
}

// ---- 咪咕：passport.migu.cn 的网页端同款密码登录 ----
// 链路（2026-11 逐跳实测）：
//   1) POST /password/publickey  body: sourceID=208003&appType=0
//      → {"status":2000,"result":{"modulus":"0083...","publicExponent":"010001"}}（hex，1024 位）
//      注意必须 POST：GET 会被 302 到 /portal/error，这是很多人以为「咪咕没有登录接口」的原因
//   2) loginID / enpassword 各自做 RSA PKCS#1 v1.5 加密（hex 输出）
//   3) POST /authn  form: sourceID / appType / relayState / loginID / enpassword / rememberMeBox
//      → 成功 302 回 relayState，失败回 {"status":4001,"message":"帐号或密码错误"}
// 已用真实密钥对验证 RsaPkcs1 的密文可被标准 PKCS#1 私钥解密，且服务端对合法密文回的是
// 业务错误（4001）而不是解密失败，说明加密与参数都对了。
static QVariantMap miguHeaders(const QString &referer)
{
    QVariantMap h;
    h.insert(QStringLiteral("User-Agent"),
        QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                       "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36"));
    h.insert(QStringLiteral("Referer"), referer);
    h.insert(QStringLiteral("Origin"), QStringLiteral("https://passport.migu.cn"));
    h.insert(QStringLiteral("Accept"), QStringLiteral("application/json, text/javascript, */*; q=0.01"));
    h.insert(QStringLiteral("Accept-Language"), QStringLiteral("zh-CN,zh;q=0.9"));
    h.insert(QStringLiteral("X-Requested-With"), QStringLiteral("XMLHttpRequest"));
    h.insert(QStringLiteral("Content-Type"),
        QStringLiteral("application/x-www-form-urlencoded; charset=UTF-8"));
    return h;
}

static QString miguAuthError(const QString &code, const QString &message)
{
    QString hint;
    if (code == QLatin1String("4001")) hint = QStringLiteral("账号或密码错误");
    else if (code == QLatin1String("4002")) hint = QStringLiteral("账号不存在");
    else if (code == QLatin1String("4010")) hint = QStringLiteral("密码错误次数过多，账号已被临时锁定，请稍后再试");
    else if (code == QLatin1String("4020")) hint = QStringLiteral("需要图形验证码：请先在网页端登录一次解除，或稍后重试");
    else if (code == QLatin1String("5001")) hint = QStringLiteral("服务端要求人机校验（风控）");
    else hint = message.isEmpty() ? QStringLiteral("登录被拒绝") : message;
    if (code.isEmpty()) return QStringLiteral("登录失败：") + hint;
    return QStringLiteral("登录失败（%1）：%2").arg(code, hint);
}

// ---- 咪咕：门户同款扫码登录 ----
// 链路（2026-11 逐跳实测，与 passport.migu.cn/login 页面上的 cmcc-ajax / cmcc-qrcajax 完全一致）：
//   1) GET /api/qrcWeb/qrcLogin?sourceid=208003
//      → {"status":2000,"result":{"qrcUrl":"data:image/jpeg;base64,…",
//                                  "qrcWebtype":"1","qrc_sessionid":"<32 位>"}}
//      响应同时下发 Set-Cookie: JSESSIONID=…; Path=/normandy。这个 Path 让浏览器/curl 在
//      /api 路径下不会自动带上它，必须自己拼 Cookie 头 —— 否则轮询一律回 4072「二维码超时」。
//   2) GET /api/qrcWeb/qrcquery?qrc_sessionid=<id>   Cookie: JSESSIONID=…
//      status: 2000 成功 / 4073 已扫码待确认 / 4074 未扫码 / 4072 二维码超时
//      （状态码取自门户 login.js 里的 ajax.LOGIN 常量：QRCNOCODE / QRCCODEPOLLING）
//   3) 成功时记下门户凭据与整包 Cookie：卡片显示昵称，歌单同步仍走账号 API 服务。
static QString miguQrReferer()
{
    return QStringLiteral("https://passport.migu.cn/login?sourceid=208003&apptype=0");
}

// Cookie 罐合并：同名覆盖，不同名追加，避免每次轮询把 JSESSIONID 冲掉
static void mergeCookieJar(QString &jar, const QString &add)
{
    if (add.trimmed().isEmpty()) return;
    QStringList parts = jar.split(QStringLiteral("; "), Qt::SkipEmptyParts);
    const QStringList incoming = add.split(QStringLiteral("; "), Qt::SkipEmptyParts);
    for (const QString &kv : incoming) {
        const QString name = kv.section(QLatin1Char('='), 0, 0).trimmed();
        if (name.isEmpty()) continue;
        bool replaced = false;
        for (QString &p : parts) {
            if (p.section(QLatin1Char('='), 0, 0).trimmed() == name) { p = kv; replaced = true; break; }
        }
        if (!replaced) parts << kv;
    }
    jar = parts.join(QStringLiteral("; "));
}

void AuthManager::startMiguQr()
{
    m_qrMode = QStringLiteral("migu");
    m_qrSource = QStringLiteral("migu");
    m_mgSession.clear();
    m_mgCookie.clear();
    m_mgTicket.clear();
    m_qrImagePath.clear();
    m_qrHint = QStringLiteral("正在获取二维码…");
    m_qrStatus = QStringLiteral("准备中");
    emit qrChanged();

    QVariantMap h = miguHeaders(miguQrReferer());
    h.remove(QStringLiteral("Content-Type"));      // GET 不带表单体
    HttpClient::instance()->request(QStringLiteral("GET"),
        QUrl(QStringLiteral("https://passport.migu.cn/api/qrcWeb/qrcLogin?sourceid=208003")),
        h, QByteArray(),
        [this](const HttpClient::Reply &r) {
            if (m_qrMode != QLatin1String("migu")) return;
            if (!r.ok) {
                m_qrStatus = QStringLiteral("二维码获取失败：") + shortNetworkError(r.error);
                emit qrChanged();
                return;
            }
            const QJsonValue root = Json::rootOf(r.json);
            m_mgCookie = cookiesFromReply(r, QVariantMap(), QJsonValue());
            const QString status = Json::stringAt(root, QStringLiteral("status"));
            const QString dataUri = Json::firstString(root, {QStringLiteral("result.qrcUrl")});
            m_mgSession = Json::firstString(root, {QStringLiteral("result.qrc_sessionid")});
            if (status != QLatin1String("2000") || dataUri.isEmpty() || m_mgSession.isEmpty()) {
                const QString msg = Json::firstString(root, {QStringLiteral("message")});
                m_qrStatus = QStringLiteral("二维码获取失败：") +
                             (msg.isEmpty() ? QStringLiteral("门户未返回二维码") : msg);
                emit qrChanged();
                emit toast(m_qrStatus);
                return;
            }
            // 门户直接下发 data:image/jpeg;base64，转存成本地 PNG 交给 QML 显示
            const QByteArray raw = QByteArray::fromBase64(
                dataUri.mid(dataUri.indexOf(QLatin1Char(',')) + 1).toUtf8());
            QImage img;
            if (!img.loadFromData(raw)) {
                m_qrStatus = QStringLiteral("二维码图片解析失败");
                emit qrChanged();
                return;
            }
            const QString path = Settings::filePath(QStringLiteral("qr_migu.png"));
            if (!img.save(path, "PNG")) {
                m_qrStatus = QStringLiteral("二维码图片保存失败");
                emit qrChanged();
                return;
            }
            m_qrImagePath = path;
            m_qrHint = QStringLiteral("请用咪咕音乐 App 扫码");
            m_qrStatus = QStringLiteral("等待扫码");
            emit qrChanged();
            startQrPolling();
        });
}

void AuthManager::pollMiguQr()
{
    if (m_mgSession.isEmpty()) { m_poll.stop(); m_polling = false; return; }
    const QString sourceId = m_qrSource;
    QVariantMap h = miguHeaders(miguQrReferer());
    h.remove(QStringLiteral("Content-Type"));
    if (!m_mgCookie.isEmpty()) h.insert(QStringLiteral("Cookie"), m_mgCookie);
    const QUrl url(QStringLiteral("https://passport.migu.cn/api/qrcWeb/qrcquery?qrc_sessionid=")
                   + m_mgSession);

    HttpClient::instance()->request(QStringLiteral("GET"), url, h, QByteArray(),
        [this, sourceId](const HttpClient::Reply &r) {
            if (!m_polling || m_qrMode != QLatin1String("migu")) return;
            if (!r.ok) {
                m_qrStatus = QStringLiteral("轮询失败：") + shortNetworkError(r.error);
                emit qrChanged();
                return;
            }
            mergeCookieJar(m_mgCookie, cookiesFromReply(r, QVariantMap(), QJsonValue()));
            const QJsonValue root = Json::rootOf(r.json);
            if (!root.isObject()) {
                // 门户偶发空回复：不要把「等待扫码」误判成失败，连续失败再报
                if (++m_qrEmptyStreak < 3) return;
                m_poll.stop();
                m_polling = false;
                m_qrStatus = QStringLiteral("登录失败：门户返回了无法解析的响应");
                emit qrChanged();
                emit toast(m_qrStatus);
                return;
            }
            m_qrEmptyStreak = 0;
            const QString status = Json::stringAt(root, QStringLiteral("status"));

            if (status == QLatin1String("4074")) {
                // 未扫码：绝大多数轮询都停在这里。首次收到回复时把 hint 换成
                // 「已连上门户」，出码成功但轮询打不通的情况才不会看起来一切正常。
                if (m_qrHint != QStringLiteral("已连上咪咕门户，等待扫码")) {
                    m_qrHint = QStringLiteral("已连上咪咕门户，等待扫码");
                    if (m_qrStatus != QStringLiteral("等待扫码")) {
                        m_qrStatus = QStringLiteral("等待扫码");
                    }
                    emit qrChanged();
                }
                return;
            }
            if (status == QLatin1String("4073")) {
                if (m_qrStatus != QStringLiteral("已扫码，请在手机上确认登录")) {
                    m_qrStatus = QStringLiteral("已扫码，请在手机上确认登录");
                    emit qrChanged();
                }
                return;
            }
            if (status == QLatin1String("4072")) {
                m_poll.stop();
                m_polling = false;
                m_qrStatus = QStringLiteral("二维码已过期，请重新获取");
                emit qrChanged();
                return;
            }
            if (status != QLatin1String("2000")) {
                m_poll.stop();
                m_polling = false;
                const QString msg = Json::firstString(root, {QStringLiteral("message")});
                m_qrStatus = QStringLiteral("登录失败（%1）：%2")
                                 .arg(status, msg.isEmpty() ? QStringLiteral("门户拒绝了本次登录") : msg);
                emit qrChanged();
                emit toast(m_qrStatus);
                return;
            }

            // ---- 成功 ----
            m_poll.stop();
            m_polling = false;
            m_qrStatus = QStringLiteral("授权成功，正在同步账号…");
            emit qrChanged();

            const QString uid = Json::firstString(root, {
                QStringLiteral("result.userId"), QStringLiteral("result.uid"),
                QStringLiteral("result.userid"), QStringLiteral("result.account"),
                QStringLiteral("result.mobile")});
            const QString nick = Json::firstString(root, {
                QStringLiteral("result.nickName"), QStringLiteral("result.nickname"),
                QStringLiteral("result.userName"), QStringLiteral("result.name")});
            const QString avatar = Json::firstString(root, {
                QStringLiteral("result.userPicture"), QStringLiteral("result.avatar"),
                QStringLiteral("result.headPic"), QStringLiteral("result.portrait")});
            m_mgTicket = Json::firstString(root, {
                QStringLiteral("result.ticket"), QStringLiteral("result.token"),
                QStringLiteral("result.assertion"), QStringLiteral("result.ssoTicket")});

            // 把整包响应留一份：门户成功分支的字段名各家版本不一致，
            // 解析不到昵称时靠它定位，不用让用户重扫码复现。
            // 必须走 redactBody：ticket/token/mobile 是能直接复用登录态的凭据，
            // 原文落盘等于把账号交给任何能读到这文件的进程。
            QFile dump(Settings::filePath(QStringLiteral("migu_qr_last.json")));
            if (dump.open(QIODevice::WriteOnly)) {
                dump.write(redactBody(r.body).toUtf8());
                dump.close();
            }

            if (m_mgCookie.trimmed().isEmpty()) {
                emit loginStateChanged(sourceId, false,
                    QStringLiteral("扫码已确认，但门户没有下发登录态 Cookie，请改用密码登录"));
                emit toast(QStringLiteral("未取得咪咕登录态，请改用密码登录"));
                return;
            }
            saveSession(sourceId, m_mgCookie, uid, nick, avatar,
                        nick.isEmpty() ? QStringLiteral("咪咕已登录（扫码）")
                                       : QStringLiteral("咪咕登录成功：") + nick);
        });
}

void AuthManager::loginMiguPassword(const QString &sourceId, const QString &account,
                                    const QString &password)
{
    const QString referer = QStringLiteral(
        "https://passport.migu.cn/login?sourceid=208003&apptype=0"
        "&relayState=https%3A%2F%2Fmusic.migu.cn%2Fv3%2F");

    HttpClient::instance()->request(QStringLiteral("POST"),
        QUrl(QStringLiteral("https://passport.migu.cn/password/publickey")),
        miguHeaders(referer), QByteArray("sourceID=208003&appType=0"),
        [this, sourceId, account, password, referer](const HttpClient::Reply &pk) {
            if (!pk.ok) {
                const QString msg = QStringLiteral("取咪咕登录公钥失败：") + shortNetworkError(pk.error);
                emit loginStateChanged(sourceId, false, msg);
                emit toast(msg);
                return;
            }
            const QJsonValue root = Json::rootOf(pk.json);
            const QString mod = Json::stringAt(root, QStringLiteral("result.modulus"));
            const QString exp = Json::stringAt(root, QStringLiteral("result.publicExponent"));
            if (mod.isEmpty() || exp.isEmpty()) {
                const QString msg = QStringLiteral("咪咕登录公钥格式无法识别（modulus/exponent 缺失）");
                emit loginStateChanged(sourceId, false, msg);
                emit toast(msg);
                return;
            }
            const QString encId = RsaPkcs1::encryptHex(mod, exp, account.toUtf8());
            const QString encPw = RsaPkcs1::encryptHex(mod, exp, password.toUtf8());
            if (encId.isEmpty() || encPw.isEmpty()) {
                const QString msg = QStringLiteral("账号或密码过长，超出咪咕公钥可加密的长度");
                emit loginStateChanged(sourceId, false, msg);
                emit toast(msg);
                return;
            }

            QByteArray form;
            form += "sourceID=208003&appType=0&relayState="
                    "https%3A%2F%2Fmusic.migu.cn%2Fv3%2F&loginID=";
            form += encId.toUtf8();
            form += "&enpassword=";
            form += encPw.toUtf8();
            form += "&rememberMeBox=1&fingerPrint=&fingerPrintDetail=";

            // 手动跟 3xx：成功后咪咕会把会话 Cookie 分在 /authn 与后续跳转两次下发，
            // 交给 Qt 自动跟随只能拿到最后一跳，容易漏掉登录票据
            HttpClient::instance()->request(QStringLiteral("POST"),
                QUrl(QStringLiteral("https://passport.migu.cn/authn")), miguHeaders(referer), form,
                [this, sourceId](const HttpClient::Reply &r) {
                    const bool redirect = r.status >= 300 && r.status < 400;
                    QString cookie = cookiesFromReply(r, QVariantMap(), QJsonValue());
                    if (r.ok && r.json.isObject()) {
                        const QJsonValue root = Json::rootOf(r.json);
                        const QString code = Json::stringAt(root, QStringLiteral("status"));
                        const QString message = Json::stringAt(root, QStringLiteral("message"));
                        if (code != QLatin1String("2000")) {
                            const QString msg = miguAuthError(code, message);
                            emit loginStateChanged(sourceId, false, msg);
                            emit toast(msg);
                            return;
                        }
                    } else if (!redirect && !r.ok) {
                        const QString msg = QStringLiteral("登录失败：") + shortNetworkError(r.error);
                        emit loginStateChanged(sourceId, false, msg);
                        emit toast(msg);
                        return;
                    } else if (!redirect) {
                        const QString msg = QStringLiteral("咪咕登录没有返回可识别的结果（HTTP %1）")
                                                .arg(r.status);
                        emit loginStateChanged(sourceId, false, msg);
                        emit toast(msg);
                        return;
                    }

                    if (!redirect) {   // 异步接口直接回 2000，本地会话 Cookie 已经在手
                        saveSession(sourceId, cookie, QString(), QStringLiteral("咪咕用户"),
                                    QString(), QStringLiteral("密码登录成功"));
                        syncUserLibrary(sourceId);
                        return;
                    }
                    // 3xx：再走一跳把 music.migu.cn 侧的 Cookie 一并收下
                    const QString location = r.headers.value(QStringLiteral("location"));
                    if (location.isEmpty() || cookie.trimmed().isEmpty()) {
                        saveSession(sourceId, cookie, QString(), QStringLiteral("咪咕用户"),
                                    QString(), QStringLiteral("密码登录成功"));
                        syncUserLibrary(sourceId);
                        return;
                    }
                    QVariantMap h = miguHeaders(QStringLiteral("https://passport.migu.cn/"));
                    h.insert(QStringLiteral("Cookie"), cookie);
                    HttpClient::instance()->request(QStringLiteral("GET"), QUrl(location), h,
                        QByteArray(),
                        [this, sourceId, cookie](const HttpClient::Reply &fin) {
                            QString all = cookie;
                            for (const QString &c : fin.setCookies) {
                                const QString kv = c.section(QLatin1Char(';'), 0, 0).trimmed();
                                const QString name = kv.section(QLatin1Char('='), 0, 0);
                                if (name.isEmpty()) continue;
                                // 同名 Cookie 以最新一跳为准
                                QStringList parts = all.split(QStringLiteral("; "));
                                for (int i = parts.size() - 1; i >= 0; --i) {
                                    if (parts.at(i).section(QLatin1Char('='), 0, 0) == name)
                                        parts.removeAt(i);
                                }
                                parts << kv;
                                all = parts.join(QStringLiteral("; "));
                            }
                            saveSession(sourceId, all, QString(), QStringLiteral("咪咕用户"),
                                        QString(), QStringLiteral("密码登录成功"));
                            syncUserLibrary(sourceId);
                        },
                        -1, false);
                },
                -1, false);
        });
}

// ---- 酷我：wapi.kuwo.cn 的网页端同款登录链路（2026-11 逐条实测）----
// 站点前端（www.kuwo.cn 的登录弹层）实际打的是 wapi.kuwo.cn，四个接口全部可用：
//   GET  /api/common/captcha/getcode   → {"data":{"img":"data:image/png;base64,…","token":"…"}}
//   POST /api/www/login/loginByKw       body {uname,password,verifyCode,verifyCodeToken,userIp}
//   POST /api/sms/mobileLoginCode       body {mobile,verifyCode,verifyCodeToken,userIp} → data.tm
//   POST /api/www/login/loginByMobile   body {mobile,verifyCode,smsCode,tm}
// 三条硬规矩（少一条就只会看到 -102 / 415）：
//   1) 请求体必须是 JSON，form 提交直接 415 Unsupported Media Type；
//   2) 账号密码是明文（站点 v-model 直接送 normalLogin.password，没有 md5）；
//   3) 图形验证码是硬门槛，token 与图片一一对应，提交一次就得换一张（站点报错即 loginGetcode）。
// 另外别再试 www.kuwo.cn/api/www：那条路对非浏览器指纹一律回 "The request is illegal!"。
namespace {

QVariantMap kuwoApiHeaders(const QString &referer = QString())
{
    QVariantMap h;
    h.insert(QStringLiteral("Content-Type"), QStringLiteral("application/json;charset=UTF-8"));
    h.insert(QStringLiteral("Accept"), QStringLiteral("application/json, text/plain, */*"));
    h.insert(QStringLiteral("Origin"), QStringLiteral("https://www.kuwo.cn"));
    h.insert(QStringLiteral("Referer"),
             referer.isEmpty() ? QStringLiteral("https://www.kuwo.cn/") : referer);
    h.insert(QStringLiteral("User-Agent"),
             QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                            "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"));
    return h;
}

// 登录接口把登录态直接放在 data.cookies 里（userid / websid / kw_token / uname3 / pic3…），
// 站点前端就是把这些键值逐个写成 document.cookie，这里做同一件事。
QString kuwoCookies(const QJsonValue &root, const HttpClient::Reply &r)
{
    QMap<QString, QString> jar;
    const QJsonValue c = root.toObject().value(QStringLiteral("data"))
                             .toObject().value(QStringLiteral("cookies"));
    if (c.isObject()) {
        const QJsonObject o = c.toObject();
        for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
            const QJsonValue v = it.value();
            if (it.key().isEmpty()) continue;
            jar.insert(it.key(), v.isString() ? v.toString()
                                              : v.toVariant().toString());
        }
    }
    if (jar.isEmpty()) {
        for (const QString &sc : r.setCookies) {
            const QString kv = sc.section(QLatin1Char(';'), 0, 0).trimmed();
            const int eq = kv.indexOf(QLatin1Char('='));
            if (eq > 0) jar.insert(kv.left(eq), kv.mid(eq + 1));
        }
    }
    QStringList out;
    for (auto it = jar.constBegin(); it != jar.constEnd(); ++it)
        out << it.key() + QLatin1Char('=') + it.value();
    return out.join(QStringLiteral("; "));
}

QString kuwoLoginError(const QString &code, const QString &serverMessage, bool smsLogin)
{
    if (code == QLatin1String("-10001"))
        return QStringLiteral("账号或密码错误，请重新输入（验证码已刷新）");
    if (code == QLatin1String("-102"))
        return QStringLiteral("图形验证码不正确，已换一张，请重新输入");
    if (code == QLatin1String("1100"))
        return QStringLiteral("手机号格式不对：酷我只接受中国大陆手机号");
    if (code == QLatin1String("1107"))
        return QStringLiteral("短信验证码已过期，请重新获取");
    if (code == QLatin1String("1108"))
        return QStringLiteral("短信验证码不正确，请重新输入");
    if (code == QLatin1String("1157"))
        return QStringLiteral("该号码 30 天内注册过酷我，暂不支持再次注册式登录");
    if (code == QLatin1String("-150"))
        return QStringLiteral("该手机号已注册，请直接用密码登录");
    const QString msg = serverMessage.trimmed();
    if (!msg.isEmpty() && msg.compare(QLatin1String("system busy"), Qt::CaseInsensitive) != 0)
        return QStringLiteral("登录失败：") + msg;
    Q_UNUSED(smsLogin)
    return QStringLiteral("登录失败：酷我服务端返回系统繁忙，请稍后再试");
}

QString kuwoCookieText(const QMap<QString, QString> &jar)
{
    QStringList out;
    for (auto it = jar.constBegin(); it != jar.constEnd(); ++it)
        out << it.key() + QLatin1Char('=') + it.value();
    return out.join(QStringLiteral("; "));
}

} // namespace

void AuthManager::refreshKuwoCaptcha()
{
    // 登录页切换一次平台会连着触发三次取码（method 清空→回填→显式调用），
    // 回复是异步的，界面那句「路径为空才取」挡不住，等于白发两三个 getcode。
    if (m_captchaPending)
        return;
    m_captchaPending = true;
    QVariantMap h = kuwoApiHeaders();
    h.remove(QStringLiteral("Content-Type"));       // GET 不带请求体
    HttpClient::instance()->request(QStringLiteral("GET"),
        QUrl(QStringLiteral("https://wapi.kuwo.cn/api/common/captcha/getcode")), h, QByteArray(),
        [this](const HttpClient::Reply &r) {
            m_captchaPending = false;   // 失败也要放行下一次取码，否则界面再也自动取不到
            if (!r.ok) {
                emit toast(QStringLiteral("图形验证码获取失败：") + shortNetworkError(r.error));
                return;
            }
            const QJsonValue root = Json::rootOf(r.json);
            const QString dataUri = Json::stringAt(root, QStringLiteral("data.img"));
            const QString token = Json::stringAt(root, QStringLiteral("data.token"));
            if (!dataUri.startsWith(QStringLiteral("data:image"), Qt::CaseInsensitive)
                || token.isEmpty()) {
                emit toast(QStringLiteral("图形验证码格式无法识别（门户未返回图片或 token）"));
                return;
            }
            const QByteArray raw = QByteArray::fromBase64(
                dataUri.mid(dataUri.indexOf(QLatin1Char(',')) + 1).toUtf8());
            // QML 的 Image 按 URL 缓存，同名覆盖不会重绘：两个文件名交替写即可强制刷新，
            // 也不会在数据目录里堆一堆验证码图片
            const QString path = Settings::filePath(QStringLiteral("captcha_kuwo_%1.png")
                                                        .arg(++m_captchaSeq % 2));
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly)) {
                emit toast(QStringLiteral("图形验证码保存失败"));
                return;
            }
            f.write(raw);
            f.close();
            m_captchaToken = token;
            m_captchaImagePath = path;
            emit captchaChanged();
        });
}

// 统一收口：data.cookies → 本地登录态。昵称/头像站点也放在 Cookie 里（uname3 / pic3），
// 所以不必再补一次资料请求；酷我的 www 用户接口对非浏览器指纹是拒绝的。
void AuthManager::finishKuwoLogin(const HttpClient::Reply &r, const QString &okMessage)
{
    constexpr auto kSource = "kuwo";
    const QJsonValue root = Json::rootOf(r.json);
    const QString cookie = kuwoCookies(root, r);
    if (cookie.isEmpty()) {
        const QString msg = QStringLiteral("登录已通过，但服务端没有下发登录态 Cookie");
        emit loginStateChanged(QString::fromLatin1(kSource), false, msg);
        emit toast(msg);
        return;
    }
    m_captchaText.clear();
    m_captchaToken.clear();
    m_captchaImagePath.clear();     // 这张码已经消费掉了，界面下次进入要重新取
    m_kwTm.clear();
    emit captchaChanged();

    const QString uid = cookieValue(cookie, {QStringLiteral("userid")});
    QString nick = cookieValue(cookie, {QStringLiteral("uname3"), QStringLiteral("username"),
                                       QStringLiteral("UserName")});
    if (!nick.isEmpty()) nick = QUrl::fromPercentEncoding(nick.toUtf8());
    const QString avatar = cookieValue(cookie, {QStringLiteral("pic3"), QStringLiteral("pic")});
    saveSession(QString::fromLatin1(kSource), cookie, uid, nick, avatar, okMessage);
}

void AuthManager::loginKuwoPassword(const QString &account, const QString &password,
                                    const QString &captcha)
{
    constexpr auto kSource = "kuwo";
    const QString code = captcha.trimmed();
    if (code.isEmpty()) {
        emit loginStateChanged(QString::fromLatin1(kSource), false,
                               QStringLiteral("请输入图形验证码（点右侧图片可换一张）"));
        if (m_captchaToken.isEmpty()) refreshKuwoCaptcha();
        return;
    }
    // token 为空也照样提交：服务端会明确回 -102（图形验证码错误），比在本地拦住更容易定位问题
    m_captchaText = code;
    // 站点提交时会把 getcode 返回的对象整体并进 body（含 img），服务端只用这两项，
    // 这里只送必要字段，避免把整张 base64 图片再传一遍
    QJsonObject body{
        {QStringLiteral("uname"), account},
        {QStringLiteral("password"), password},
        {QStringLiteral("verifyCode"), code},
        {QStringLiteral("verifyCodeToken"), m_captchaToken},
        {QStringLiteral("userIp"), QString()}
    };
    HttpClient::instance()->request(QStringLiteral("POST"),
        QUrl(QStringLiteral("https://wapi.kuwo.cn/api/www/login/loginByKw")),
        kuwoApiHeaders(), QJsonDocument(body).toJson(QJsonDocument::Compact),
        [this](const HttpClient::Reply &r) {
            if (!r.ok) {
                const QString msg = QStringLiteral("登录失败：") + shortNetworkError(r.error);
                emit loginStateChanged(QStringLiteral("kuwo"), false, msg);
                emit toast(msg);
                refreshKuwoCaptcha();
                return;
            }
            const QJsonValue root = Json::rootOf(r.json);
            const QString c = Json::stringAt(root, QStringLiteral("code"));
            if (c == QLatin1String("200")) {
                finishKuwoLogin(r, QStringLiteral("酷我登录成功"));
                return;
            }
            m_captchaText.clear();
            m_captchaToken.clear();
            refreshKuwoCaptcha();       // 与站点一致：报错即换码
            const QString msg = kuwoLoginError(c, Json::stringAt(root, QStringLiteral("msg")), false);
            emit loginStateChanged(QStringLiteral("kuwo"), false, msg);
            emit toast(msg);
        });
}

void AuthManager::sendKuwoSms(const QString &phone, const QString &captcha)
{
    if (phone.size() != 11 || !phone.at(0).isDigit()) {
        const QString msg = QStringLiteral("请输入 11 位中国大陆手机号");
        emit loginStateChanged(QStringLiteral("kuwo"), false, msg);
        emit toast(msg);
        return;
    }
    if (m_captchaToken.isEmpty() || captcha.trimmed().isEmpty()) {
        const QString msg = QStringLiteral("请先填写图形验证码（点右侧图片可换一张）");
        emit loginStateChanged(QStringLiteral("kuwo"), false, msg);
        emit toast(msg);
        if (m_captchaToken.isEmpty()) refreshKuwoCaptcha();
        return;
    }
    m_captchaText = captcha.trimmed();
    m_kwPhone = phone;
    QJsonObject body{
        {QStringLiteral("mobile"), phone},
        {QStringLiteral("verifyCode"), m_captchaText},
        {QStringLiteral("verifyCodeToken"), m_captchaToken},
        {QStringLiteral("userIp"), QString()}
    };
    HttpClient::instance()->request(QStringLiteral("POST"),
        QUrl(QStringLiteral("https://wapi.kuwo.cn/api/sms/mobileLoginCode")),
        kuwoApiHeaders(), QJsonDocument(body).toJson(QJsonDocument::Compact),
        [this](const HttpClient::Reply &r) {
            if (!r.ok) {
                const QString msg = QStringLiteral("短信验证码发送失败：") + shortNetworkError(r.error);
                emit loginStateChanged(QStringLiteral("kuwo"), false, msg);
                emit toast(msg);
                return;
            }
            const QJsonValue root = Json::rootOf(r.json);
            const QString c = Json::stringAt(root, QStringLiteral("code"));
            if (c == QLatin1String("200")) {
                // tm 是这次发码的票据，登录时要原样带回；图形验证码也要保留到登录那一步
                m_kwTm = Json::stringAt(root, QStringLiteral("data.tm"));
                emit toast(QStringLiteral("短信验证码已发送"));
                return;
            }
            m_captchaText.clear();
            m_captchaToken.clear();
            refreshKuwoCaptcha();
            const QString msg = kuwoLoginError(c, Json::stringAt(root, QStringLiteral("msg")), true);
            emit loginStateChanged(QStringLiteral("kuwo"), false, msg);
            emit toast(msg);
        });
}

void AuthManager::loginKuwoPhone(const QString &phone, const QString &smsCode)
{
    if (phone.size() != 11) {
        const QString msg = QStringLiteral("请输入 11 位中国大陆手机号");
        emit loginStateChanged(QStringLiteral("kuwo"), false, msg);
        emit toast(msg);
        return;
    }
    if (smsCode.isEmpty()) {
        emit loginStateChanged(QStringLiteral("kuwo"), false, QStringLiteral("请输入短信验证码"));
        return;
    }
    if (m_captchaToken.isEmpty() || m_captchaText.isEmpty()) {
        const QString msg = QStringLiteral("图形验证码已失效，请重新获取验证码");
        emit loginStateChanged(QStringLiteral("kuwo"), false, msg);
        emit toast(msg);
        refreshKuwoCaptcha();
        return;
    }
    QJsonObject body{
        {QStringLiteral("mobile"), phone},
        {QStringLiteral("verifyCode"), m_captchaText},
        {QStringLiteral("smsCode"), smsCode},
        {QStringLiteral("tm"), m_kwTm}
    };
    HttpClient::instance()->request(QStringLiteral("POST"),
        QUrl(QStringLiteral("https://wapi.kuwo.cn/api/www/login/loginByMobile")),
        kuwoApiHeaders(), QJsonDocument(body).toJson(QJsonDocument::Compact),
        [this](const HttpClient::Reply &r) {
            if (!r.ok) {
                const QString msg = QStringLiteral("登录失败：") + shortNetworkError(r.error);
                emit loginStateChanged(QStringLiteral("kuwo"), false, msg);
                emit toast(msg);
                return;
            }
            const QJsonValue root = Json::rootOf(r.json);
            const QString c = Json::stringAt(root, QStringLiteral("code"));
            if (c == QLatin1String("200")) {
                finishKuwoLogin(r, QStringLiteral("酷我手机号登录成功"));
                return;
            }
            m_captchaText.clear();
            m_captchaToken.clear();
            refreshKuwoCaptcha();
            const QString msg = kuwoLoginError(c, Json::stringAt(root, QStringLiteral("msg")), true);
            emit loginStateChanged(QStringLiteral("kuwo"), false, msg);
            emit toast(msg);
        });
}

// ---- 汽水音乐：扫码登录（官方 passport 直连 / 可配置走自建账号 API 服务）----
// 2026-11 实测：汽水官方的出码/轮询/换 session 端点（passport/web/*，再换 luna/pc/me 拉账号）其实就是
// 普通浏览器形态的 passport 接口，带浏览器常规头即可直接拿到二维码与轮询链路，客户端不需要字节
// secsdk 设备签名。因此未配置自建账号 API 服务时，客户端即可「直接扫码」登录：
//   1) GET  https://api.qishui.com/passport/web/get_qrcode/ -> {data:{token,qrcode,...}}
//   2) POST https://api.qishui.com/passport/web/check_qrconnect/ -> {data:{status, auth:{sessionid}}}
//   3) GET  https://api.qishui.com/luna/pc/me?aid=386088          -> {data:{my_info:{uid,...}}}
// 与 src/tools/qishui_login_flow_test.py 的探针链路保持一致；用户在「设置 → 网络 → 账号 API 服务」
// 填了自建服务时，才走其 GET /auth/qrcode -> POST /auth/qrcode/status -> POST /auth/me 契约。
void AuthManager::startQishuiQr()
{
    const QString sourceId = QStringLiteral("qishui");
    m_qrMode = sourceId;
    m_qrSource = sourceId;
    m_qsToken.clear();
    m_qsCookies.clear();
    m_qrImagePath.clear();
    emit qrChanged();

    const UserApiConfig cfg = UserApi::instance()->config(sourceId);
    QString base = cfg.base.trimmed();
    if (base.isEmpty())
        base = Settings::string(QStringLiteral("accountApiBase")).trimmed();
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);

    if (base.isEmpty()) {
        // 未配置自建账号 API 服务：直接用官方 passport 端点完成扫码登录，无需任何外部服务
        m_qsBase.clear();
        m_qrHint = QStringLiteral("正在获取汽水二维码…");
        m_qrStatus = QStringLiteral("准备中");
        emit qrChanged();
        QUrl url(QStringLiteral("https://api.qishui.com/passport/web/get_qrcode/"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("passport_jssdk_version"), QStringLiteral("2.4.13"));
        query.addQueryItem(QStringLiteral("passport_jssdk_type"), QStringLiteral("normal"));
        query.addQueryItem(QStringLiteral("is_from_ttaccountsdk"), QStringLiteral("1"));
        query.addQueryItem(QStringLiteral("aid"), QStringLiteral("386088"));
        query.addQueryItem(QStringLiteral("next"), QStringLiteral("https://api.qishui.com"));
        query.addQueryItem(QStringLiteral("need_logo"), QStringLiteral("false"));
        query.addQueryItem(QStringLiteral("need_short_url"), QStringLiteral("false"));
        query.addQueryItem(QStringLiteral("is_new_login"), QStringLiteral("1"));
        url.setQuery(query);
        QVariantMap headers = qishuiPassportHeaders(QStringLiteral());
        headers.insert(QStringLiteral("Accept"), QStringLiteral("application/json, text/plain, */*"));
        HttpClient::instance()->request(
            QStringLiteral("GET"), url, headers, QByteArray(),
            [this, sourceId](const HttpClient::Reply &r) {
                if (m_qrMode != QLatin1String("qishui")) return;
                if (!r.ok) {
                    m_qrStatus = QStringLiteral("二维码获取失败：") + shortNetworkError(r.error);
                    emit qrChanged();
                    return;
                }
                // 出码响应里的 Set-Cookie（passport_csrf_token）要回灌进轮询的 Cookie 头
                QMap<QString, QString> cookies = m_qsCookies;
                for (const QString &c : r.setCookies) {
                    const QString kv = c.section(QLatin1Char(';'), 0, 0).trimmed();
                    const int eq = kv.indexOf(QLatin1Char('='));
                    if (eq <= 0) continue;
                    cookies.insert(kv.left(eq).trimmed().toLower(), kv.mid(eq + 1).trimmed());
                }
                m_qsCookies = cookies;
                QJsonObject root = Json::rootOf(r.json).toObject();
                const QJsonObject data = root.contains(QStringLiteral("data"))
                                             ? root.value(QStringLiteral("data")).toObject() : root;
                m_qsToken = Json::firstString(data, {QStringLiteral("token"),
                                                     QStringLiteral("qrcode_token")});
                const QString qr = Json::firstString(data, {
                    QStringLiteral("qrcode"), QStringLiteral("qrcode_url"),
                    QStringLiteral("login_url"), QStringLiteral("url")});
                if (m_qsToken.isEmpty() || qr.isEmpty()) {
                    m_qrStatus = QStringLiteral("账号接口未返回二维码（缺 token 或 qrcode 字段）");
                    emit qrChanged();
                    emit toast(m_qrStatus);
                    return;
                }
                m_qrHint = QStringLiteral("请用抖音 App 扫一扫");
                m_qrStatus = QStringLiteral("等待扫码");
                if (!saveQrDataUrl(qr, sourceId)) generateQrImage(qr, sourceId);
                // generateQrImage 只负责画图；轮询由这里显式启动（token 已在手）
                startQrPolling();
                emit qrChanged();
            });
        return;
    }

    // 已配置自建账号 API 服务（/auth 契约）→ 走服务侧链路
    m_qsBase = base;
    m_qrHint = QStringLiteral("正在获取汽水二维码…");
    m_qrStatus = QStringLiteral("准备中");
    emit qrChanged();

    HttpClient::instance()->request(
        QStringLiteral("GET"), QUrl(base + QStringLiteral("/auth/qrcode")), QVariantMap(),
        QByteArray(), [this](const HttpClient::Reply &r) {
            if (m_qrMode != QLatin1String("qishui")) return;
            if (!r.ok) {
                m_qrStatus = QStringLiteral("二维码获取失败：") + shortNetworkError(r.error);
                emit qrChanged();
                return;
            }
            // 服务可能把结果直接放顶层，也可能包在 data 里，两种都收
            QJsonObject root = Json::rootOf(r.json).toObject();
            const QJsonObject data = root.contains(QStringLiteral("data"))
                                         ? root.value(QStringLiteral("data")).toObject() : root;
            m_qsToken = Json::firstString(data, {QStringLiteral("token"),
                                                 QStringLiteral("qrcode_token")});
            const QString url = Json::firstString(data, {
                QStringLiteral("qrcode_index_url"), QStringLiteral("qrcode"),
                QStringLiteral("url"), QStringLiteral("login_url")});
            if (m_qsToken.isEmpty() || url.isEmpty()) {
                m_qrStatus = QStringLiteral("账号 API 服务未返回二维码（缺 token 或 qrcode 字段）");
                emit qrChanged();
                emit toast(m_qrStatus);
                return;
            }
            m_qrHint = QStringLiteral("请用汽水音乐 App 扫码");
            m_qrStatus = QStringLiteral("等待扫码");
            generateQrImage(url, QStringLiteral("qishui"));
            // generateQrImage 只负责画图；轮询由这里显式启动（token 已在手）
            startQrPolling();
            emit qrChanged();
        });
}

void AuthManager::pollQishuiQr()
{
    if (!m_polling || m_qsToken.isEmpty()) {
        m_poll.stop();
        m_polling = false;
        return;
    }
    // 未配置自建账号 API 服务 → 官方 passport/web/check_qrconnect；已配置 → 服务侧 /auth/qrcode/status
    if (m_qsBase.isEmpty()) {
        QUrl url(QStringLiteral("https://api.qishui.com/passport/web/check_qrconnect/"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("need_logo"), QStringLiteral("false"));
        query.addQueryItem(QStringLiteral("need_short_url"), QStringLiteral("false"));
        query.addQueryItem(QStringLiteral("is_frontier"), QStringLiteral("true"));
        query.addQueryItem(QStringLiteral("token"), m_qsToken);
        query.addQueryItem(QStringLiteral("is_new_login"), QStringLiteral("1"));
        query.addQueryItem(QStringLiteral("next"), QStringLiteral("https://api.qishui.com"));
        url.setQuery(query);
        QVariantMap headers = qishuiPassportHeaders(qishuiCookieHeader());
        headers.insert(QStringLiteral("Content-Type"), QStringLiteral("application/x-www-form-urlencoded"));
        QUrlQuery bodyQuery;
        bodyQuery.addQueryItem(QStringLiteral("passport_jssdk_version"), QStringLiteral("2.4.13"));
        bodyQuery.addQueryItem(QStringLiteral("passport_jssdk_type"), QStringLiteral("normal"));
        bodyQuery.addQueryItem(QStringLiteral("is_from_ttaccountsdk"), QStringLiteral("1"));
        bodyQuery.addQueryItem(QStringLiteral("aid"), QStringLiteral("386088"));
        bodyQuery.addQueryItem(QStringLiteral("iid"), QStringLiteral("27960026095955"));
        bodyQuery.addQueryItem(QStringLiteral("need_logo"), QStringLiteral("false"));
        bodyQuery.addQueryItem(QStringLiteral("need_short_url"), QStringLiteral("false"));
        bodyQuery.addQueryItem(QStringLiteral("is_frontier"), QStringLiteral("true"));
        bodyQuery.addQueryItem(QStringLiteral("token"), m_qsToken);
        bodyQuery.addQueryItem(QStringLiteral("is_new_login"), QStringLiteral("1"));
        bodyQuery.addQueryItem(QStringLiteral("next"), QStringLiteral("https://api.qishui.com"));
        const QByteArray formBody = bodyQuery.toString().toUtf8();
        HttpClient::instance()->request(
            QStringLiteral("POST"), url, headers, formBody,
            [this](const HttpClient::Reply &r) {
                if (m_qrMode != QLatin1String("qishui") || !m_polling) return;
                if (m_qsBase.isEmpty()) {
                    // 官方 passport 链路：扫码确认后 sessionid 走 Set-Cookie，这里统一收口
                    QMap<QString, QString> cookies = m_qsCookies;
                    for (const QString &c : r.setCookies) {
                        const QString kv = c.section(QLatin1Char(';'), 0, 0).trimmed();
                        const int eq = kv.indexOf(QLatin1Char('='));
                        if (eq <= 0) continue;
                        cookies.insert(kv.left(eq).trimmed().toLower(), kv.mid(eq + 1).trimmed());
                    }
                    m_qsCookies = cookies;
                    const QString session = cookies.value(QStringLiteral("sessionid"));
                    const QString st = Json::firstString(
                                          Json::rootOf(r.json).toObject(),
                                          {QStringLiteral("status"), QStringLiteral("state"),
                                           QStringLiteral("msg"), QStringLiteral("message")}).toLower();
                    handleQishuiPollReply(r, session, st);
                    return;
                }
                const QJsonObject body{{QStringLiteral("token"), m_qsToken}};
                QVariantMap jsonHeaders;
                jsonHeaders.insert(QStringLiteral("Content-Type"), QStringLiteral("application/json"));
                HttpClient::instance()->request(
                    QStringLiteral("POST"), QUrl(m_qsBase + QStringLiteral("/auth/qrcode/status")), jsonHeaders,
                    QJsonDocument(body).toJson(QJsonDocument::Compact),
                    [this](const HttpClient::Reply &r) {
                        if (m_qrMode != QLatin1String("qishui") || !m_polling) return;
                        if (!r.ok) {
                            // 单次网络抖动不该终止会话，继续下一轮
                            m_qrStatus = QStringLiteral("轮询失败，正在重试：") + shortNetworkError(r.error);
                            emit qrChanged();
                            return;
                        }
                        QJsonObject root = Json::rootOf(r.json).toObject();
                        const QJsonObject data = root.contains(QStringLiteral("data"))
                                                     ? root.value(QStringLiteral("data")).toObject() : root;
                        const QString session = Json::firstString(data, {
                            QStringLiteral("sessionid"), QStringLiteral("session_id"),
                            QStringLiteral("session")});
                        const QString st = Json::firstString(data, {
                            QStringLiteral("status"), QStringLiteral("state"),
                            QStringLiteral("msg"), QStringLiteral("message")}).toLower();
                        handleQishuiPollReply(r, session, st);
                    });
                return;
            });
        return;
    }
    const QJsonObject body{{QStringLiteral("token"), m_qsToken}};
    QVariantMap headers;
    headers.insert(QStringLiteral("Content-Type"), QStringLiteral("application/json"));
    HttpClient::instance()->request(
        QStringLiteral("POST"), QUrl(m_qsBase + QStringLiteral("/auth/qrcode/status")), headers,
        QJsonDocument(body).toJson(QJsonDocument::Compact), [this](const HttpClient::Reply &r) {
            if (m_qrMode != QLatin1String("qishui") || !m_polling) return;
            if (!r.ok) {
                // 单次网络抖动不该终止会话，继续下一轮
                m_qrStatus = QStringLiteral("轮询失败，正在重试：") + shortNetworkError(r.error);
                emit qrChanged();
                return;
            }
            QJsonObject root = Json::rootOf(r.json).toObject();

            const QJsonObject data = root.contains(QStringLiteral("data"))
                                         ? root.value(QStringLiteral("data")).toObject() : root;
            const QString session = Json::firstString(data, {
                QStringLiteral("sessionid"), QStringLiteral("session_id"),
                QStringLiteral("session")});
            const QString st = Json::firstString(data, {
                QStringLiteral("status"), QStringLiteral("state"),
                QStringLiteral("msg"), QStringLiteral("message")}).toLower();

            if (!session.isEmpty()) {
                m_poll.stop();
                m_polling = false;
                m_qrStatus = QStringLiteral("扫码成功，正在读取账号…");
                emit qrChanged();
                finishQishuiLogin(session);
                return;
            }
            if (st.contains(QLatin1String("expire")) || st.contains(QStringLiteral("过期"))
                || st.contains(QLatin1String("invalid"))) {
                m_poll.stop();
                m_polling = false;
                m_qrImagePath.clear();
                m_qrStatus = QStringLiteral("二维码已过期，请重新获取");
                emit qrChanged();
                return;
            }
            handleQishuiPollReply(r, session, st);
        });
    }

void AuthManager::handleQishuiPollReply(const HttpClient::Reply &r, const QString &sessionId,
                                       const QString &st)
{
    if (m_qrMode != QLatin1String("qishui") || !m_polling) return;
    if (!r.ok) {
        // 单次网络抖动不该终止会话，继续下一轮
        m_qrStatus = QStringLiteral("轮询失败，正在重试：") + shortNetworkError(r.error);
        emit qrChanged();
        return;
    }
    if (!sessionId.isEmpty()) {
        m_poll.stop();
        m_polling = false;
        m_qrStatus = QStringLiteral("扫码成功，正在读取账号…");
        emit qrChanged();
        finishQishuiLogin(sessionId);
        return;
    }
    if (st.contains(QLatin1String("expire")) || st.contains(QStringLiteral("过期"))
        || st.contains(QLatin1String("timeout")) || st.contains(QLatin1String("invalid"))
        || st.contains(QLatin1String("cancel"))) {
        m_poll.stop();
        m_polling = false;
        m_qrImagePath.clear();
        m_qrStatus = QStringLiteral("二维码已过期，请重新获取");
        emit qrChanged();
        return;
    }
    if (st.contains(QLatin1String("scanned")) || st.contains(QLatin1String("confirm"))
        || st.contains(QStringLiteral("已扫"))) {
        m_qrStatus = QStringLiteral("已扫码，请在手机上确认登录");
    } else {
        m_qrStatus = QStringLiteral("等待扫码");
    }
    emit qrChanged();
}

// 用 sessionid 换账号资料：资料只是锦上添花，拿不到也要把会话存下来
void AuthManager::finishQishuiLogin(const QString &sessionId)
{
    const QString sourceId = QStringLiteral("qishui");
    QString cookie = QStringLiteral("sessionid=") + sessionId;
    QString uid;
    QString nick;
    QString avatar;
    if (m_qsBase.isEmpty()) {
        // 官方直连链路：GET /luna/pc/me?aid=386088，Cookie 带上 sessionid
        HttpClient::instance()->request(
            QStringLiteral("GET"),
            QUrl(QStringLiteral("https://api.qishui.com/luna/pc/me") + QStringLiteral("?aid=386088")),
            qishuiPassportHeaders(cookie), QByteArray(),
            [this, sourceId, sessionId, uid, nick, avatar, cookie](const HttpClient::Reply &r) mutable {
            if (r.ok) {
                QJsonObject root = Json::rootOf(r.json).toObject();
                const QJsonObject data = root.contains(QStringLiteral("data"))
                                             ? root.value(QStringLiteral("data")).toObject() : root;
                const QJsonObject profile = data.contains(QStringLiteral("profile"))
                                                ? data.value(QStringLiteral("profile")).toObject()
                                                : data;
                uid = Json::firstString(profile, {QStringLiteral("uid"), QStringLiteral("user_id"),
                                                  QStringLiteral("id")});
                nick = Json::firstString(profile, {QStringLiteral("nickname"),
                                                   QStringLiteral("nick_name"),
                                                   QStringLiteral("name")});
                avatar = Json::firstString(profile, {QStringLiteral("avatar"),
                                                     QStringLiteral("avatar_url")});
                cookie = Json::firstString(data, {QStringLiteral("cookie"),
                                                  QStringLiteral("cookies")});
            }
            if (cookie.trimmed().isEmpty())
                cookie = QStringLiteral("sessionid=") + sessionId;
            // 收尾：把码面清掉、状态收敛成「登录成功」（QML 的 qrPhase 认「成功」这个词 → done），
            // 否则卡片上会一直挂着一张已经用过的二维码和「正在读取账号…」的文案。
            m_polling = false;
            m_poll.stop();
            m_qrImagePath.clear();
            m_qrStatus = QStringLiteral("登录成功");
            emit qrChanged();
            saveSession(sourceId, onlyCookiePairs(cookie), uid, nick, avatar,
                        nick.isEmpty() ? QStringLiteral("汽水音乐登录成功")
                                       : QStringLiteral("汽水音乐登录成功：") + nick);
            });
            return;
        }
    // 自建账号 API 服务（/auth 契约）链路：POST {base}/auth/me，body 带 sessionid
    const QJsonObject body{{QStringLiteral("sessionid"), sessionId}};
    QVariantMap headers;
    headers.insert(QStringLiteral("Content-Type"), QStringLiteral("application/json"));
    HttpClient::instance()->request(
        QStringLiteral("POST"), QUrl(m_qsBase + QStringLiteral("/auth/me")), headers,
        QJsonDocument(body).toJson(QJsonDocument::Compact),
        [this, sourceId, sessionId, uid, nick, avatar, cookie](const HttpClient::Reply &r) mutable {
            if (r.ok) {
                QJsonObject root = Json::rootOf(r.json).toObject();
                const QJsonObject data = root.contains(QStringLiteral("data"))
                                                 ? root.value(QStringLiteral("data")).toObject() : root;
                const QJsonObject profile = data.contains(QStringLiteral("profile"))
                                                 ? data.value(QStringLiteral("profile")).toObject()
                                                 : data;
                uid = Json::firstString(profile, {QStringLiteral("uid"), QStringLiteral("user_id"),
                                                  QStringLiteral("id")});
                nick = Json::firstString(profile, {QStringLiteral("nickname"),
                                                   QStringLiteral("nick_name"),
                                                   QStringLiteral("name")});
                avatar = Json::firstString(profile, {QStringLiteral("avatar"),
                                                     QStringLiteral("avatar_url")});
                cookie = Json::firstString(data, {QStringLiteral("cookie"),
                                                  QStringLiteral("cookies")});
            }
            if (cookie.trimmed().isEmpty())
                cookie = QStringLiteral("sessionid=") + sessionId;
            m_polling = false;
            m_poll.stop();
            m_qrImagePath.clear();
            m_qrStatus = QStringLiteral("登录成功");
            emit qrChanged();
            saveSession(sourceId, onlyCookiePairs(cookie), uid, nick, avatar,
                        nick.isEmpty() ? QStringLiteral("汽水音乐登录成功")
                                       : QStringLiteral("汽水音乐登录成功：") + nick);
        });
}

// ---- 酷我：微信扫码登录 ----
// 站点「微信」图标打开的是开放平台 snsapi_login：appid wx41c1275bb3e28427，
// redirect_uri 为 i.kuwo.cn/US/platform/WeixinCallback.jsp?f=web&callbackUrl=<站点>/callback。
// 纯 HTTP 就能完整走完，不需要内嵌浏览器：
//   1) GET connect/qrconnect 页面里取出 /connect/qrcode/<uuid>
//   2) 二维码图片就是 https://open.weixin.qq.com/connect/qrcode/<uuid>
//   3) 长轮询 long.open.weixin.qq.com/connect/l/qrconnect?uuid=…，返回 window.wx_errcode=…;wx_code='…'
//   4) 拿 code 打 i.kuwo.cn 回调，逐跳收 Set-Cookie，直到出现 userid 才算登录成功
// QQ / 微博同理，但 callback 是 HTML 弹窗回调，没有 WebEngine 收不到，故只内置微信。
void AuthManager::startKuwoWxQr()
{
    m_qrMode = QStringLiteral("kuwo_wx");
    m_qrSource = QStringLiteral("kuwo");
    m_kwUuid.clear();
    m_kwPollBusy = false;
    m_qrImagePath.clear();
    m_qrHint = QStringLiteral("正在获取微信二维码…");
    m_qrStatus = QStringLiteral("准备中");
    emit qrChanged();

    const QString redirect = QStringLiteral("http://i.kuwo.cn/US/platform/WeixinCallback.jsp?f=web"
                                            "&callbackUrl=")
        + QString::fromLatin1(QUrl::toPercentEncoding(QStringLiteral("http://www.kuwo.cn/callback")));
    const QString url = QStringLiteral("https://open.weixin.qq.com/connect/qrconnect"
                                       "?appid=wx41c1275bb3e28427&response_type=code"
                                       "&scope=snsapi_login&redirect_uri=")
        + QString::fromLatin1(QUrl::toPercentEncoding(redirect)) + QStringLiteral("&state=kuwo");

    QVariantMap h = kuwoApiHeaders(QStringLiteral("https://open.weixin.qq.com/"));
    h.remove(QStringLiteral("Content-Type"));
    HttpClient::instance()->request(QStringLiteral("GET"), QUrl(url), h, QByteArray(),
        [this](const HttpClient::Reply &r) {
            if (m_qrMode != QLatin1String("kuwo_wx")) return;
            const QString html = QString::fromUtf8(r.body);
            static const QRegularExpression re(QStringLiteral("/connect/qrcode/([A-Za-z0-9_\\-]+)"));
            const QString uuid = re.match(html).captured(1);
            if (!r.ok || uuid.isEmpty()) {
                m_qrStatus = QStringLiteral("微信二维码获取失败：") +
                             (r.ok ? QStringLiteral("开放平台未返回 uuid")
                                   : shortNetworkError(r.error));
                emit qrChanged();
                emit toast(m_qrStatus);
                return;
            }
            m_kwUuid = uuid;
            QVariantMap ih = kuwoApiHeaders(QStringLiteral("https://open.weixin.qq.com/"));
            ih.remove(QStringLiteral("Content-Type"));
            HttpClient::instance()->request(QStringLiteral("GET"),
                QUrl(QStringLiteral("https://open.weixin.qq.com/connect/qrcode/") + uuid), ih,
                QByteArray(),
                [this, uuid](const HttpClient::Reply &ir) {
                    if (m_qrMode != QLatin1String("kuwo_wx") || m_kwUuid != uuid) return;
                    if (!ir.ok || ir.body.isEmpty()) {
                        m_qrStatus = QStringLiteral("微信二维码下载失败：") +
                                     shortNetworkError(ir.error);
                        emit qrChanged();
                        return;
                    }
                    const QString path = Settings::filePath(QStringLiteral("qr_kuwo.png"));
                    QFile f(path);
                    if (!f.open(QIODevice::WriteOnly)) {
                        m_qrStatus = QStringLiteral("二维码保存失败");
                        emit qrChanged();
                        return;
                    }
                    f.write(ir.body);
                    f.close();
                    m_qrImagePath = path;
                    m_qrHint = QStringLiteral("请用微信「扫一扫」扫码，确认后自动登录酷我");
                    m_qrStatus = QStringLiteral("等待扫码");
                    emit qrChanged();
                    startQrPolling();
                });
        });
}

void AuthManager::pollKuwoWx()
{
    if (m_kwUuid.isEmpty()) { m_poll.stop(); m_polling = false; return; }
    // 长轮询最长会挂 30s，2s 定时器会叠满请求：上一发没回来就跳过这一拍
    if (m_kwPollBusy) return;
    m_kwPollBusy = true;
    const QString uuid = m_kwUuid;
    const QString url = QStringLiteral("https://long.open.weixin.qq.com/connect/l/qrconnect"
                                       "?uuid=%1&_=%2")
        .arg(uuid).arg(QDateTime::currentMSecsSinceEpoch());
    QVariantMap h = kuwoApiHeaders(QStringLiteral("https://open.weixin.qq.com/"));
    h.remove(QStringLiteral("Content-Type"));
    HttpClient::instance()->request(QStringLiteral("GET"), QUrl(url), h, QByteArray(),
        [this, uuid](const HttpClient::Reply &r) {
            m_kwPollBusy = false;
            if (!m_polling || m_qrMode != QLatin1String("kuwo_wx") || m_kwUuid != uuid) return;
            const QString body = QString::fromUtf8(r.body);
            // 长轮询请求超时/空回复都属于「继续等」，不要把它说成失败
            if (body.trimmed().isEmpty()) return;
            static const QRegularExpression errRe(QStringLiteral("wx_errcode=(\\d+)"));
            static const QRegularExpression codeRe(QStringLiteral("wx_code='([^']*)'"));
            const QString ec = errRe.match(body).captured(1);
            const QString code = codeRe.match(body).captured(1);
            if (!code.isEmpty()
                || (ec == QLatin1String("405") && body.contains(QStringLiteral("wx_code")))) {
                if (code.isEmpty()) return;      // 已确认但 code 还没下发，下一拍再取
                m_poll.stop();
                m_polling = false;
                m_qrStatus = QStringLiteral("微信已确认，正在换取酷我登录态…");
                emit qrChanged();
                kuwoWxExchange(code);
                return;
            }
            if (ec == QLatin1String("404")) {
                if (m_qrStatus != QStringLiteral("已扫码，请在手机上确认")) {
                    m_qrStatus = QStringLiteral("已扫码，请在手机上确认");
                    emit qrChanged();
                }
                return;
            }
            if (ec == QLatin1String("403")) {
                m_poll.stop();
                m_polling = false;
                m_qrStatus = QStringLiteral("已在手机上取消登录，请重新获取二维码");
                emit qrChanged();
                return;
            }
            if (ec == QLatin1String("402")) {
                m_poll.stop();
                m_polling = false;
                m_qrImagePath.clear();
                m_qrStatus = QStringLiteral("二维码已过期，请重新获取");
                emit qrChanged();
                return;
            }
            // 408 及其它：仍在等待扫码
        },
        32000);
}

void AuthManager::kuwoWxExchange(const QString &code)
{
    const QString callback = QStringLiteral("http://i.kuwo.cn/US/platform/WeixinCallback.jsp?f=web"
                                            "&callbackUrl=")
        + QString::fromLatin1(QUrl::toPercentEncoding(QStringLiteral("http://www.kuwo.cn/callback")));
    auto jar = std::make_shared<QMap<QString, QString>>();
    auto hop = std::make_shared<std::function<void(const QString &, int)>>();
    *hop = [this, jar, hop](const QString &url, int depth) {
        QVariantMap h = kuwoApiHeaders(QStringLiteral("https://www.kuwo.cn/"));
        h.remove(QStringLiteral("Content-Type"));
        if (!jar->isEmpty()) h.insert(QStringLiteral("Cookie"), kuwoCookieText(*jar));
        HttpClient::instance()->request(QStringLiteral("GET"), QUrl(url), h, QByteArray(),
            [this, jar, hop, depth](const HttpClient::Reply &r) {
                for (const QString &sc : r.setCookies) {
                    const QString kv = sc.section(QLatin1Char(';'), 0, 0).trimmed();
                    const int eq = kv.indexOf(QLatin1Char('='));
                    if (eq <= 0) continue;
                    const QString name = kv.left(eq);
                    QString value = kv.mid(eq + 1);
                    if (name.compare(QLatin1String("JSESSIONID"), Qt::CaseInsensitive) == 0
                        && value.contains(QLatin1Char('.'))) {
                        value = value.section(QLatin1Char('.'), 0, 0);   // tomcat 路由后缀不参与校验
                    }
                    jar->insert(name, value);
                }
                const QString loc = r.headers.value(QStringLiteral("location"));
                if (r.status >= 300 && r.status < 400 && !loc.isEmpty() && depth < 3) {
                    // 回调是 302 链：WeixinCallback.jsp → /US_NEW/kuwo/login_by_weixin_web
                    // → www.kuwo.cn/callback，登录 Cookie 可能落在任意一跳，必须逐跳收
                    (*hop)(loc, depth + 1);
                    return;
                }
                const QString cookie = kuwoCookieText(*jar);
                const QString uid = cookieValue(cookie, {QStringLiteral("userid")});
                if (uid.isEmpty()) {
                    m_qrStatus = QStringLiteral("微信已授权，但酷我没有下发登录态："
                                                "该微信大概率还没绑定酷我账号，请先在网页端绑定");
                    emit qrChanged();
                    emit loginStateChanged(QStringLiteral("kuwo"), false, m_qrStatus);
                    emit toast(m_qrStatus);
                    return;
                }
                QString nick = cookieValue(cookie, {QStringLiteral("uname3"),
                                                    QStringLiteral("username")});
                if (!nick.isEmpty()) nick = QUrl::fromPercentEncoding(nick.toUtf8());
                const QString avatar = cookieValue(cookie, {QStringLiteral("pic3")});
                m_qrStatus = QStringLiteral("登录成功");
                emit qrChanged();
                saveSession(QStringLiteral("kuwo"), cookie, uid, nick, avatar,
                            QStringLiteral("酷我微信登录成功"));
            },
            -1, false);
    };
    (*hop)(callback + QStringLiteral("&code=") + code + QStringLiteral("&state=kuwo"), 0);
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
        emit toast(QStringLiteral("请先登录该平台"));
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
