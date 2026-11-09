#include "ScriptEngine.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QRandomGenerator>
#include <QSet>
#include <QUrl>
#include <QtMath>
#include <cstdio>
#include <cstring>
#include <memory>

extern "C" {
#include "3rd/quickjs/quickjs.h"
}

namespace Furui {

// lx.request 的安全边界：默认阻断本机/回环/私有网段与非 HTTP(S) 目标，
// 避免用户导入的脚本借宿主权限探测本机服务或内网（SSRF）。
// 需访问自建内网音源时，设 FURUI_SCRIPT_ALLOW_PRIVATE=1 显式放开。
static QString unsafeRequestTarget(const QUrl &u)
{
    static const bool allowPrivate = qEnvironmentVariableIsSet("FURUI_SCRIPT_ALLOW_PRIVATE");
    if (allowPrivate) return QString();
    const QString scheme = u.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return QStringLiteral("只允许 http/https 协议");
    const QString host = u.host().toLower();
    if (host.isEmpty()) return QStringLiteral("URL 缺少主机名");
    if (host == QLatin1String("localhost") || host.endsWith(QLatin1String(".localhost")) ||
        host.endsWith(QLatin1String(".local")) || host == QLatin1String("localhost.localdomain"))
        return QStringLiteral("禁止访问本机/本地域名");
    const QHostAddress addr(host);
    if (!addr.isNull()) {
        if (addr == QHostAddress(QHostAddress::LocalHost) ||
            addr == QHostAddress(QHostAddress::LocalHostIPv6))
            return QStringLiteral("禁止访问回环地址");
        const quint32 v4 = addr.toIPv4Address();
        if (v4 != 0) {
            const quint8 a = quint8(v4 >> 24), b = quint8(v4 >> 16);
            if (a == 0 || a == 10 || a == 127 || a >= 224 ||
                (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
                (a == 169 && b == 254) || (a == 100 && b >= 64 && b <= 127) ||
                (a == 192 && b == 0))
                return QStringLiteral("禁止访问私有/保留网段");
        } else {
            const QString ip = addr.toString().toLower();
            if (ip.startsWith(QLatin1String("fc")) || ip.startsWith(QLatin1String("fd")) ||
                ip.startsWith(QLatin1String("fe80")) || ip.startsWith(QLatin1String("ff")) ||
                ip.startsWith(QLatin1String("::ffff:127.")) ||
                ip == QLatin1String("::") || ip == QLatin1String("::1"))
                return QStringLiteral("禁止访问私有/回环 IPv6 地址");
        }
    }
    return QString();
}

// 排查脚本行为用：设置 FURUI_SCRIPT_TRACE=1 后打印脚本的 lx.send / lx.request。
static bool scriptTraceEnabled()
{
    static const bool on = qEnvironmentVariableIsSet("FURUI_SCRIPT_TRACE");
    return on;
}

static void scriptTrace(const QString &line)
{
    if (!scriptTraceEnabled()) return;
    const QByteArray bytes = line.toUtf8();
    std::fprintf(stderr, "%s\n", bytes.constData());
    std::fflush(stderr);
}

namespace {

struct Holder {
    JSValue v;
};

JSValue holderValue(void *h) { return h ? static_cast<Holder *>(h)->v : JS_UNDEFINED; }

void *hold(JSValue v) { return new Holder{v}; }

void dropHolder(JSContext *ctx, void *h)
{
    if (!h) return;
    Holder *holder = static_cast<Holder *>(h);
    JS_FreeValue(ctx, holder->v);
    delete holder;
}

QString jsString(JSContext *ctx, JSValueConst v)
{
    const char *s = JS_ToCString(ctx, v);
    QString out = s ? QString::fromUtf8(s) : QString();
    JS_FreeCString(ctx, s);
    return out;
}

QString exceptionText(JSContext *ctx, JSValue exception)
{
    if (JS_IsObject(exception)) {
        JSValue msg = JS_GetPropertyStr(ctx, exception, "message");
        QString text;
        if (!JS_IsException(msg) && !JS_IsUndefined(msg)) {
            text = jsString(ctx, msg);
        }
        JS_FreeValue(ctx, msg);
        JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
        const QString stackText = (!JS_IsException(stack) && !JS_IsUndefined(stack))
                                      ? jsString(ctx, stack) : QString();
        JS_FreeValue(ctx, stack);
        if (!stackText.isEmpty()) {
            // 只保留首行栈帧，避免把整个调用栈塞进 UI
            const QStringList lines = stackText.split(QLatin1Char('\n'), QString::SkipEmptyParts);
            if (lines.size() > 1) text += QStringLiteral("（") + lines.at(1).trimmed() + QStringLiteral("）");
            else if (text.isEmpty()) text = lines.first().trimmed();
        }
        if (!text.isEmpty()) return text;
    }
    return jsString(ctx, exception);
}

QVariant jsonToVariant(const QJsonValue &v)
{
    switch (v.type()) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        return QVariant();
    case QJsonValue::Bool:
        return v.toBool();
    case QJsonValue::Double: {
        const double d = v.toDouble();
        if (qAbs(d) < 9.0e15 && d == qFloor(d)) return QVariant::fromValue<qlonglong>(qint64(d));
        return d;
    }
    case QJsonValue::String:
        return v.toString();
    case QJsonValue::Array: {
        QVariantList list;
        for (const QJsonValue &item : v.toArray()) list << jsonToVariant(item);
        return list;
    }
    case QJsonValue::Object: {
        QVariantMap map;
        const QJsonObject obj = v.toObject();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it)
            map.insert(it.key(), jsonToVariant(it.value()));
        return map;
    }
    }
    return QVariant();
}

JSValue variantToJs(JSContext *ctx, const QVariant &value, int depth = 0);
JSValue qjsonToJs(JSContext *ctx, const QJsonValue &value, int depth = 0);

JSValue objectMapToJs(JSContext *ctx, const QVariantMap &map, int depth)
{
    JSValue obj = JS_NewObject(ctx);
    for (auto it = map.constBegin(); it != map.constEnd(); ++it)
        JS_SetPropertyStr(ctx, obj, it.key().toUtf8().constData(),
                          variantToJs(ctx, it.value(), depth + 1));
    return obj;
}

JSValue variantToJs(JSContext *ctx, const QVariant &value, int depth)
{
    if (depth > 12) return JS_NULL;
    switch (value.type()) {
    case QVariant::Invalid:
        return JS_NULL;
    case QVariant::Bool:
        return JS_NewBool(ctx, value.toBool());
    case QVariant::Int:
    case QVariant::LongLong:
    case QVariant::UInt:
    case QVariant::ULongLong:
    case QVariant::Char:
    case QVariant::Double:
        return JS_NewFloat64(ctx, value.toDouble());
    case QVariant::String:
        return JS_NewString(ctx, value.toString().toUtf8().constData());
    case QVariant::StringList: {
        const QStringList list = value.toStringList();
        JSValue arr = JS_NewArray(ctx);
        for (int i = 0; i < list.size(); ++i)
            JS_SetPropertyUint32(ctx, arr, quint32(i),
                                 JS_NewString(ctx, list.at(i).toUtf8().constData()));
        return arr;
    }
    case QVariant::ByteArray: {
        const QByteArray bytes = value.toByteArray();
        JSValue arr = JS_NewArray(ctx);
        for (int i = 0; i < bytes.size(); ++i)
            JS_SetPropertyUint32(ctx, arr, quint32(i), JS_NewInt32(ctx, bytes.at(i)));
        return arr;
    }
    default:
        break;
    }
    if (value.type() == QVariant::List || value.type() == QVariant::StringList) {
        JSValue arr = JS_NewArray(ctx);
        const QVariantList list = value.toList();
        for (int i = 0; i < list.size(); ++i)
            JS_SetPropertyUint32(ctx, arr, quint32(i), variantToJs(ctx, list.at(i), depth + 1));
        return arr;
    }
    if (value.canConvert<QVariantMap>()) {
        const QVariantMap map = value.toMap();
        return objectMapToJs(ctx, map, depth);
    }
    return JS_NewString(ctx, value.toString().toUtf8().constData());
}

JSValue qjsonToJs(JSContext *ctx, const QJsonValue &value, int depth)
{
    return variantToJs(ctx, jsonToVariant(value), depth);
}

QByteArray bytesFromJs(JSContext *ctx, JSValueConst value)
{
    if (JS_IsString(value)) {
        const char *s = JS_ToCString(ctx, value);
        QByteArray out = s ? QByteArray(s, int(strlen(s))) : QByteArray();
        JS_FreeCString(ctx, s);
        return out;
    }
    if (JS_IsArray(ctx, value)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, value, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);
        QByteArray out;
        out.reserve(len);
        for (int i = 0; i < len; ++i) {
            JSValue item = JS_GetPropertyUint32(ctx, value, quint32(i));
            int32_t byte = 0;
            JS_ToInt32(ctx, &byte, item);
            JS_FreeValue(ctx, item);
            out.append(char(byte & 0xff));
        }
        return out;
    }
    size_t size = 0;
    uint8_t *data = JS_GetArrayBuffer(ctx, &size, value);
    if (data) return QByteArray(reinterpret_cast<char *>(data), int(size));
    // TypedArray 走这里
    JSValue buffer = JS_GetTypedArrayBuffer(ctx, value, nullptr, nullptr, nullptr);
    if (!JS_IsException(buffer)) {
        size_t n = 0;
        uint8_t *raw = JS_GetArrayBuffer(ctx, &n, buffer);
        QByteArray out = raw ? QByteArray(reinterpret_cast<char *>(raw), int(n)) : QByteArray();
        JS_FreeValue(ctx, buffer);
        return out;
    }
    JS_FreeValue(ctx, buffer);
    return QByteArray();
}

JSValue bytesToJs(JSContext *ctx, const QByteArray &bytes)
{
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < bytes.size(); ++i)
        JS_SetPropertyUint32(ctx, arr, quint32(i), JS_NewInt32(ctx, bytes.at(i)));
    return arr;
}

QVariantMap readOptions(JSContext *ctx, JSValueConst options, QString *urlOut,
                        QString *methodOut, QVariantMap *headersOut,
                        QByteArray *bodyOut, int *timeoutOut, bool *followOut)
{
    QVariantMap out;
    if (!JS_IsObject(options)) return out;
    JSValue v = JS_GetPropertyStr(ctx, options, "url");
    if (urlOut && JS_IsString(v)) *urlOut = jsString(ctx, v);
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, options, "method");
    if (methodOut && JS_IsString(v)) *methodOut = jsString(ctx, v).toUpper();
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, options, "headers");
    if (headersOut && JS_IsObject(v)) {
        JSValue json = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
        if (!JS_IsException(json)) {
            const QString text = jsString(ctx, json);
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
            if (doc.isObject()) *headersOut = jsonToVariant(QJsonValue(doc.object())).toMap();
        }
        JS_FreeValue(ctx, json);
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, options, "body");
    if (bodyOut) {
        if (JS_IsString(v)) {
            *bodyOut = jsString(ctx, v).toUtf8();
        } else if (JS_IsObject(v)) {
            JSValue json = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
            if (!JS_IsException(json)) *bodyOut = jsString(ctx, json).toUtf8();
            JS_FreeValue(ctx, json);
        }
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, options, "timeout");
    if (timeoutOut) {
        int32_t t = 0;
        if (!JS_IsUndefined(v) && JS_ToInt32(ctx, &t, v) == 0 && t > 0) *timeoutOut = t;
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, options, "follow_max");
    if (followOut && !JS_IsUndefined(v)) {
        int32_t n = 1;
        JS_ToInt32(ctx, &n, v);
        *followOut = n > 0;
    }
    JS_FreeValue(ctx, v);
    return out;
}

} // namespace

struct ScriptEngineHost {
    static ScriptEngine *self(JSContext *ctx)
    {
        return static_cast<ScriptEngine *>(JS_GetContextOpaque(ctx));
    }

    static JSValue throwErr(JSContext *ctx, const QString &msg)
    {
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, msg.toUtf8().constData()));
        return JS_Throw(ctx, err);
    }

    static void consoleLog(JSContext *ctx, int level, int argc, JSValueConst *argv)
    {
        QStringList parts;
        for (int i = 0; i < argc; ++i) {
            JSValue json = JS_JSONStringify(ctx, argv[i], JS_UNDEFINED, JS_UNDEFINED);
            if (!JS_IsException(json)) {
                parts << jsString(ctx, json);
                JS_FreeValue(ctx, json);
            } else {
                JS_FreeValue(ctx, json);
                parts << jsString(ctx, argv[i]);
            }
        }
        const QString tag = self(ctx) ? self(ctx)->name() : QStringLiteral("script");
        if (scriptTraceEnabled()) {
            const QString levelName = level == 0 ? QStringLiteral("info") : QStringLiteral("log");
            scriptTrace(QStringLiteral("[console.%1] %2")
                            .arg(levelName).arg(parts.join(QLatin1Char(' '))));
        }
        if (level == 0)
            qInfo().noquote() << QStringLiteral("[音源:%1]").arg(tag) << parts.join(QLatin1Char(' '));
        else
            qDebug().noquote() << QStringLiteral("[音源:%1]").arg(tag) << parts.join(QLatin1Char(' '));
    }

    static JSValue jsConsole(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    { consoleLog(ctx, 1, argc, argv); return JS_UNDEFINED; }
    static JSValue jsInfo(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    { consoleLog(ctx, 0, argc, argv); return JS_UNDEFINED; }
    static JSValue jsWarn(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    { consoleLog(ctx, 1, argc, argv); return JS_UNDEFINED; }
    static JSValue jsError(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    { consoleLog(ctx, 1, argc, argv); return JS_UNDEFINED; }
    static JSValue jsNoop(JSContext *, JSValueConst, int, JSValueConst *)
    { return JS_UNDEFINED; }

    static JSValue jsOn(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        ScriptEngine *e = self(ctx);
        if (!e || argc < 2 || !JS_IsString(argv[0]) || !JS_IsFunction(ctx, argv[1]))
            return JS_UNDEFINED;
        const QString event = jsString(ctx, argv[0]);
        auto it = e->m_handlers.find(event);
        if (it != e->m_handlers.end()) dropHolder(ctx, it.value());
        e->m_handlers.insert(event, hold(JS_DupValue(ctx, argv[1])));
        return JS_UNDEFINED;
    }

    static JSValue jsSend(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        ScriptEngine *e = self(ctx);
        if (!e || argc < 2) return JS_UNDEFINED;
        QString event;
        if (JS_IsString(argv[0])) event = jsString(ctx, argv[0]);
        QVariant payload;
        JSValue json = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
        if (!JS_IsException(json)) {
            const QString text = jsString(ctx, json);
            payload = QJsonDocument::fromJson(text.toUtf8()).toVariant();
            JS_FreeValue(ctx, json);
        } else {
            JS_FreeValue(ctx, json);
        }
        if (event == QLatin1String("inited")) {
            if (scriptTraceEnabled())
                scriptTrace(QStringLiteral("[lx.send] inited ")
                    + QString::fromUtf8(QJsonDocument::fromVariant(payload)
                                            .toJson(QJsonDocument::Compact)));
            e->onInited(payload);
        } else if (event == QLatin1String("updateAlert")) {
            if (scriptTraceEnabled())
                scriptTrace(QStringLiteral("[lx.send] updateAlert ")
                    + QString::fromUtf8(QJsonDocument::fromVariant(payload)
                                            .toJson(QJsonDocument::Compact)));
            qInfo().noquote() << QStringLiteral("[音源:%1] updateAlert").arg(e->name());
        } else if (scriptTraceEnabled()) {
            scriptTrace(QStringLiteral("[lx.send] %1 %2").arg(event,
                QString::fromUtf8(QJsonDocument::fromVariant(payload)
                                      .toJson(QJsonDocument::Compact))));
        }
        return JS_UNDEFINED;
    }

    static JSValue jsRequest(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        ScriptEngine *e = self(ctx);
        if (!e || argc < 2) return throwErr(ctx, QStringLiteral("lx.request 参数不完整"));

        QString url;
        QString method = QStringLiteral("GET");
        QVariantMap headers;
        QByteArray body;
        int timeout = 8000;
        bool follow = true;
        JSValueConst cb = JS_UNDEFINED;

        if (JS_IsString(argv[0])) {
            url = jsString(ctx, argv[0]);
            readOptions(ctx, argv[1], nullptr, &method, &headers, &body, &timeout, &follow);
            if (argc > 2 && JS_IsFunction(ctx, argv[2])) cb = argv[2];
        } else if (JS_IsObject(argv[0])) {
            readOptions(ctx, argv[0], &url, &method, &headers, &body, &timeout, &follow);
            if (argc > 1 && JS_IsFunction(ctx, argv[1])) cb = argv[1];
        }
        if (url.isEmpty()) return throwErr(ctx, QStringLiteral("lx.request 缺少 url"));
        if (!JS_IsFunction(ctx, cb))
            return throwErr(ctx, QStringLiteral("lx.request 缺少回调"));

        const QUrl target(url);
        if (!target.isValid()) return throwErr(ctx, QStringLiteral("无效 URL：") + url);
        const QString blocked = unsafeRequestTarget(target);
        if (!blocked.isEmpty())
            return throwErr(ctx, QStringLiteral("lx.request 目标被安全策略拒绝：%1（%2）").arg(blocked, url));

        struct ReqState {
            QPointer<ScriptEngine> engine;
            JSValue cb = JS_UNDEFINED;
        };
        auto st = std::make_shared<ReqState>();
        st->engine = e;
        st->cb = JS_DupValue(ctx, cb);

        HttpClient::instance()->request(method, target, headers, body,
            [st, method, url](const HttpClient::Reply &r) {
                if (scriptTraceEnabled())
                    scriptTrace(QStringLiteral("[lx.request] %1 %2 => %3 %4")
                                    .arg(method, url, QString::number(r.status),
                                         r.error.left(160)));
                ScriptEngine *engine = st->engine.data();
                if (!engine || engine->m_stopped) {
                    // 引擎已销毁：JSValue 随上下文一起释放，这里不再碰它
                    return;
                }
                JSContext *c = static_cast<JSContext *>(engine->m_ctx);
                if (!c) return;
                JSValue args[2];
                if (!r.ok) {
                    args[0] = JS_NewString(c, r.error.toUtf8().constData());
                    args[1] = JS_UNDEFINED;
                } else {
                    args[0] = JS_NULL;
                    JSValue resp = JS_NewObject(c);
                    JS_SetPropertyStr(c, resp, "status", JS_NewInt32(c, r.status));
                    if (r.json.isArray() || r.json.isObject()) {
                        QJsonValue root = r.json.isArray()
                            ? QJsonValue(r.json.array())
                            : QJsonValue(r.json.object());
                        JS_SetPropertyStr(c, resp, "body", qjsonToJs(c, root));
                    } else {
                        JS_SetPropertyStr(c, resp, "body",
                            JS_NewString(c, r.body.constData()));
                    }
                    JSValue hdrs = JS_NewObject(c);
                    for (auto it = r.headers.constBegin(); it != r.headers.constEnd(); ++it)
                        JS_SetPropertyStr(c, hdrs, it.key().toUtf8().constData(),
                                          JS_NewString(c, it.value().toUtf8().constData()));
                    JS_SetPropertyStr(c, resp, "headers", hdrs);
                    JS_SetPropertyStr(c, resp, "bytes", JS_NewInt32(c, r.body.size()));
                    args[1] = resp;
                }
                JSValue ret;
                {
                    ScriptEngine::ExecDeadlineGuard deadline(engine, 12000);
                    ret = JS_Call(c, st->cb, JS_UNDEFINED, 2, args);
                }
                if (JS_IsException(ret)) engine->reportException(QStringLiteral("lx.request"));
                JS_FreeValue(c, ret);
                JS_FreeValue(c, args[0]);
                JS_FreeValue(c, args[1]);
                JS_FreeValue(c, st->cb);
                st->cb = JS_UNDEFINED;
                engine->drainJobs();
            });

        return JS_UNDEFINED;
    }

    static JSValue jsMd5(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        QByteArray data = argc > 0 ? bytesFromJs(ctx, argv[0]) : QByteArray();
        QCryptographicHash hash(QCryptographicHash::Md5);
        hash.addData(data);
        const QByteArray digest = hash.result();
        QString format = QStringLiteral("hex");
        if (argc > 1 && JS_IsString(argv[1])) format = jsString(ctx, argv[1]);
        if (format.compare(QLatin1String("base64"), Qt::CaseInsensitive) == 0)
            return JS_NewString(ctx, digest.toBase64().constData());
        return JS_NewString(ctx, digest.toHex().constData());
    }

    static JSValue hashValue(JSContext *ctx, int argc, JSValueConst *argv,
                             QCryptographicHash::Algorithm algo)
    {
        QByteArray data = argc > 0 ? bytesFromJs(ctx, argv[0]) : QByteArray();
        QCryptographicHash hash(algo);
        hash.addData(data);
        return JS_NewString(ctx, hash.result().toHex().constData());
    }

    static JSValue jsSha1(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    { return hashValue(ctx, argc, argv, QCryptographicHash::Sha1); }
    static JSValue jsSha256(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    { return hashValue(ctx, argc, argv, QCryptographicHash::Sha256); }

    static JSValue jsRandomBytes(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        int32_t n = 16;
        if (argc > 0) JS_ToInt32(ctx, &n, argv[0]);
        if (n <= 0 || n > 4096) n = 16;
        QByteArray bytes;
        bytes.resize(n);
        for (int i = 0; i < n; ++i) bytes[i] = char(QRandomGenerator::global()->bounded(256));
        return bytesToJs(ctx, bytes);
    }

    static JSValue jsNotSupported(JSContext *ctx, JSValueConst, int, JSValueConst *)
    {
        return throwErr(ctx, QStringLiteral("宿主未实现该能力（当前仅支持 md5/sha1/sha256/randomBytes 与 buffer 基础操作）"));
    }

    static JSValue jsTrace(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        if (argc < 1) return JS_UNDEFINED;
        scriptTrace(jsString(ctx, argv[0]));
        return JS_UNDEFINED;
    }

    static JSValue jsBufFrom(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        if (argc < 1) return JS_NewArray(ctx);
        if (JS_IsString(argv[0]) && argc > 1 && JS_IsString(argv[1])) {
            const QString text = jsString(ctx, argv[0]);
            const QString enc = jsString(ctx, argv[1]).toLower();
            if (enc == QLatin1String("hex"))
                return bytesToJs(ctx, QByteArray::fromHex(text.toUtf8()));
            if (enc == QLatin1String("base64"))
                return bytesToJs(ctx, QByteArray::fromBase64(text.toUtf8()));
            return bytesToJs(ctx, text.toUtf8());
        }
        return bytesToJs(ctx, bytesFromJs(ctx, argv[0]));
    }

    static JSValue jsBufToStr(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        if (argc < 1) return JS_NewString(ctx, "");
        const QByteArray bytes = bytesFromJs(ctx, argv[0]);
        QString encoding = QStringLiteral("utf8");
        if (argc > 1 && JS_IsString(argv[1])) encoding = jsString(ctx, argv[1]).toLower();
        if (encoding == QLatin1String("hex"))
            return JS_NewString(ctx, bytes.toHex().constData());
        if (encoding == QLatin1String("base64"))
            return JS_NewString(ctx, bytes.toBase64().constData());
        return JS_NewString(ctx, QString::fromUtf8(bytes).toUtf8().constData());
    }

    static JSValue jsBufConcat(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        QByteArray out;
        if (argc > 0 && JS_IsArray(ctx, argv[0])) {
            JSValue lenVal = JS_GetPropertyStr(ctx, argv[0], "length");
            int32_t len = 0;
            JS_ToInt32(ctx, &len, lenVal);
            JS_FreeValue(ctx, lenVal);
            for (int i = 0; i < len; ++i) {
                JSValue item = JS_GetPropertyUint32(ctx, argv[0], quint32(i));
                out += bytesFromJs(ctx, item);
                JS_FreeValue(ctx, item);
            }
        }
        return bytesToJs(ctx, out);
    }

    static JSValue jsBufAlloc(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        int32_t n = 0;
        if (argc > 0) JS_ToInt32(ctx, &n, argv[0]);
        if (n < 0) n = 0;
        if (n > 1024 * 1024) n = 1024 * 1024;
        QByteArray bytes;
        bytes.resize(n);
        return bytesToJs(ctx, bytes);
    }

    static JSValue jsSetTimeout(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        ScriptEngine *e = self(ctx);
        if (!e || argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, -1);
        int32_t ms = 0;
        if (argc > 1) JS_ToInt32(ctx, &ms, argv[1]);
        if (ms < 0) ms = 0;
        if (ms > 60000) ms = 60000;
        const int id = ++e->m_nextTimer;
        e->m_timers.insert(id, hold(JS_DupValue(ctx, argv[0])));
        QPointer<ScriptEngine> guard(e);
        QTimer::singleShot(ms, e, [guard, id] {
            ScriptEngine *engine = guard.data();
            if (!engine || engine->m_stopped) return;
            JSContext *c = static_cast<JSContext *>(engine->m_ctx);
            void *fnHolder = engine->m_timers.take(id);
            if (!c || !fnHolder) return;
            JSValue fn = holderValue(fnHolder);
            JSValue ret;
            {
                ScriptEngine::ExecDeadlineGuard deadline(engine, 12000);
                ret = JS_Call(c, fn, JS_UNDEFINED, 0, nullptr);
            }
            if (JS_IsException(ret)) engine->reportException(QStringLiteral("setTimeout"));
            JS_FreeValue(c, ret);
            dropHolder(c, fnHolder);
            engine->drainJobs();
        });
        return JS_NewInt32(ctx, id);
    }

    static JSValue jsClearTimeout(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        ScriptEngine *e = self(ctx);
        if (!e || argc < 1) return JS_UNDEFINED;
        int32_t id = -1;
        JS_ToInt32(ctx, &id, argv[0]);
        // 周期定时器（setInterval）复用同一 id 空间，一并停止
        if (QTimer *timer = e->m_intervals.take(id)) {
            timer->stop();
            timer->deleteLater();
        }
        // 只能取消尚未触发的计时器（QTimer::singleShot 本身无法取消，
        // 这里清掉回调，让到点的 lambda 直接返回）
        void *holder = e->m_timers.take(id);
        dropHolder(ctx, holder);
        return JS_UNDEFINED;
    }

    // setInterval：真正的周期定时器，回调执行同样受硬超时保护。
    static JSValue jsSetInterval(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        ScriptEngine *e = self(ctx);
        if (!e || argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, -1);
        int32_t ms = 0;
        if (argc > 1) JS_ToInt32(ctx, &ms, argv[1]);
        if (ms < 16) ms = 16;          // 0ms 周期会刷爆 UI 线程
        if (ms > 60000) ms = 60000;
        const int id = ++e->m_nextTimer;
        e->m_timers.insert(id, hold(JS_DupValue(ctx, argv[0])));
        QTimer *timer = new QTimer(e);
        e->m_intervals.insert(id, timer);
        QPointer<ScriptEngine> guard(e);
        QObject::connect(timer, &QTimer::timeout, e, [guard, id] {
            ScriptEngine *engine = guard.data();
            if (!engine || engine->m_stopped) return;
            JSContext *c = static_cast<JSContext *>(engine->m_ctx);
            void *fnHolder = engine->m_timers.value(id, nullptr);
            if (!c || !fnHolder) return;
            JSValue ret;
            {
                ScriptEngine::ExecDeadlineGuard deadline(engine, 12000);
                ret = JS_Call(c, holderValue(fnHolder), JS_UNDEFINED, 0, nullptr);
            }
            if (JS_IsException(ret)) engine->reportException(QStringLiteral("setInterval"));
            JS_FreeValue(c, ret);
            engine->drainJobs();
        });
        timer->start(ms);
        return JS_NewInt32(ctx, id);
    }

    static JSValue jsClearInterval(JSContext *ctx, JSValueConst thisVal, int argc, JSValueConst *argv)
    {
        return jsClearTimeout(ctx, thisVal, argc, argv);
    }

    // __furuiCb(tag, ok, value)：脚本 Promise 结算后回传结果
    static JSValue jsSettleCallback(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv)
    {
        ScriptEngine *e = self(ctx);
        if (!e || argc < 3) return JS_UNDEFINED;
        int32_t tag = 0;
        JS_ToInt32(ctx, &tag, argv[0]);
        auto it = e->m_pending.find(tag);
        if (it == e->m_pending.end()) return JS_UNDEFINED;
        ScriptEngine::Pending pending = it.value();
        e->m_pending.erase(it);
        if (pending.timer) {
            pending.timer->stop();
            pending.timer->deleteLater();
        }

        ScriptActionResult result;
        const int ok = JS_ToBool(ctx, argv[1]);
        if (ok > 0) {
            result.ok = true;
            if (JS_IsString(argv[2])) {
                result.data = jsString(ctx, argv[2]);
            } else if (JS_IsBool(argv[2])) {
                result.data = bool(JS_ToBool(ctx, argv[2]) > 0);
            } else if (JS_IsNumber(argv[2])) {
                double d = 0.0;
                JS_ToFloat64(ctx, &d, argv[2]);
                result.data = d;
            } else if (JS_IsObject(argv[2])) {
                JSValue json = JS_JSONStringify(ctx, argv[2], JS_UNDEFINED, JS_UNDEFINED);
                if (!JS_IsException(json)) {
                    const QByteArray text = jsString(ctx, json).toUtf8();
                    QJsonParseError err;
                    const QJsonDocument doc = QJsonDocument::fromJson(text, &err);
                    if (err.error == QJsonParseError::NoError && !doc.isNull())
                        result.data = doc.toVariant();
                    JS_FreeValue(ctx, json);
                } else {
                    JS_FreeValue(ctx, json);
                }
            }
        } else {
            result.ok = false;
            result.error = jsString(ctx, argv[2]);
            if (JS_IsObject(argv[2])) {
                JSValue msg = JS_GetPropertyStr(ctx, argv[2], "message");
                if (!JS_IsException(msg) && !JS_IsUndefined(msg)) result.error = jsString(ctx, msg);
                JS_FreeValue(ctx, msg);
            }
            if (result.error.isEmpty()) result.error = QStringLiteral("脚本未返回有效结果");
            result.errorType = result.error.contains(QStringLiteral("未实现")) ? QStringLiteral("unsupported")
                                : result.error.contains(QStringLiteral("timeout")) ? QStringLiteral("timeout")
                                : QStringLiteral("runtime");
        }
        if (pending.cb) pending.cb(result);
        return JS_UNDEFINED;
    }
};

ScriptEngine::ScriptEngine(QString name, QObject *parent)
    : QObject(parent), m_name(std::move(name))
{
}

ScriptEngine::~ScriptEngine() { stop(); }

void ScriptEngine::stop()
{
    if (m_stopped) return;
    m_stopped = true;
    // 仍在等待初始化的调用方也要收到失败，否则 start() 会永远悬着
    if (m_initCb) finishInit(false, QStringLiteral("runtime"), QStringLiteral("音源已停止"));
    if (m_initTimer) {
        m_initTimer->stop();
        m_initTimer->deleteLater();
        m_initTimer = nullptr;
    }
    for (QTimer *timer : m_intervals) {
        timer->stop();
        timer->deleteLater();
    }
    m_intervals.clear();
    // 先摘除再回调：避免 cb 重入时改动正在遍历的容器，并保证挂起的
    // lyric()/resolve() 一定返回（否则界面会永远停在「加载中」）。
    const QHash<int, Pending> orphaned = m_pending;
    m_pending.clear();
    for (auto it = orphaned.constBegin(); it != orphaned.constEnd(); ++it) {
        const Pending &p = it.value();
        if (p.timer) {
            p.timer->stop();
            p.timer->deleteLater();
        }
        if (p.cb) {
            ScriptActionResult r;
            r.errorType = QStringLiteral("runtime");
            r.error = QStringLiteral("音源已停止");
            p.cb(r);
        }
    }
    JSContext *ctx = static_cast<JSContext *>(m_ctx);
    if (ctx) {
        for (void *h : m_handlers) dropHolder(ctx, h);
        for (void *h : m_timers) dropHolder(ctx, h);
    }
    m_handlers.clear();
    m_timers.clear();
    if (ctx) JS_FreeContext(ctx);
    m_ctx = nullptr;
    if (m_rt) JS_FreeRuntime(static_cast<JSRuntime *>(m_rt));
    m_rt = nullptr;
}

void ScriptEngine::createContext()
{
    JSRuntime *rt = JS_NewRuntime();
    JS_SetMemoryLimit(rt, 256ull * 1024 * 1024);
    JS_SetMaxStackSize(rt, 2ull * 1024 * 1024);
    JS_SetRuntimeOpaque(rt, this);
    JS_SetHostPromiseRejectionTracker(rt, [](JSContext *ctx, JSValueConst, JSValueConst reason,
                                            JS_BOOL isHandled, void *opaque) {
        if (isHandled) return;
        ScriptEngine *engine = static_cast<ScriptEngine *>(opaque);
        if (!engine) return;
        const JSValue dup = JS_DupValue(ctx, reason);
        const QString text = exceptionText(ctx, dup);
        JS_FreeValue(ctx, dup);
        if (engine->m_lastRejection.isEmpty()) engine->m_lastRejection = text;
        if (scriptTraceEnabled())
            scriptTrace(QStringLiteral("[rejection] ") + text);
    }, this);
    JS_SetInterruptHandler(rt, [](JSRuntime *, void *opaque) -> int {
        ScriptEngine *engine = static_cast<ScriptEngine *>(opaque);
        if (!engine || engine->m_execDeadline <= 0) return 0;
        return QDateTime::currentMSecsSinceEpoch() > engine->m_execDeadline ? 1 : 0;
    }, this);
    JSContext *ctx = JS_NewContext(rt);
    JS_AddIntrinsicPromise(ctx);
    JS_SetContextOpaque(ctx, this);
    m_rt = rt;
    m_ctx = ctx;

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "self", JS_DupValue(ctx, global));

    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, ScriptEngineHost::jsConsole, "log", 1));
    JS_SetPropertyStr(ctx, console, "info", JS_NewCFunction(ctx, ScriptEngineHost::jsInfo, "info", 1));
    JS_SetPropertyStr(ctx, console, "warn", JS_NewCFunction(ctx, ScriptEngineHost::jsWarn, "warn", 1));
    JS_SetPropertyStr(ctx, console, "error", JS_NewCFunction(ctx, ScriptEngineHost::jsError, "error", 1));
    JS_SetPropertyStr(ctx, console, "debug", JS_NewCFunction(ctx, ScriptEngineHost::jsConsole, "debug", 1));
    JS_SetPropertyStr(ctx, console, "trace", JS_NewCFunction(ctx, ScriptEngineHost::jsConsole, "trace", 1));
    // huibq/ikun 等脚本在桌面环境会调用 console.group，缺了会直接 TypeError
    JS_SetPropertyStr(ctx, console, "group", JS_NewCFunction(ctx, ScriptEngineHost::jsNoop, "group", 1));
    JS_SetPropertyStr(ctx, console, "groupEnd", JS_NewCFunction(ctx, ScriptEngineHost::jsNoop, "groupEnd", 1));
    JS_SetPropertyStr(ctx, console, "table", JS_NewCFunction(ctx, ScriptEngineHost::jsNoop, "table", 1));
    JS_SetPropertyStr(ctx, console, "time", JS_NewCFunction(ctx, ScriptEngineHost::jsNoop, "time", 1));
    JS_SetPropertyStr(ctx, console, "timeEnd", JS_NewCFunction(ctx, ScriptEngineHost::jsNoop, "timeEnd", 1));
    JS_SetPropertyStr(ctx, global, "console", console);

    JS_SetPropertyStr(ctx, global, "setTimeout",
        JS_NewCFunction(ctx, ScriptEngineHost::jsSetTimeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global, "clearTimeout",
        JS_NewCFunction(ctx, ScriptEngineHost::jsClearTimeout, "clearTimeout", 1));
    JS_SetPropertyStr(ctx, global, "setInterval",
        JS_NewCFunction(ctx, ScriptEngineHost::jsSetInterval, "setInterval", 2));
    JS_SetPropertyStr(ctx, global, "clearInterval",
        JS_NewCFunction(ctx, ScriptEngineHost::jsClearInterval, "clearInterval", 1));

    // lx 宿主对象
    JSValue lx = JS_NewObject(ctx);

    JSValue events = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, events, "request", JS_NewString(ctx, "request"));
    JS_SetPropertyStr(ctx, events, "inited", JS_NewString(ctx, "inited"));
    JS_SetPropertyStr(ctx, events, "updateAlert", JS_NewString(ctx, "updateAlert"));
    JS_SetPropertyStr(ctx, lx, "EVENT_NAMES", events);

    JS_SetPropertyStr(ctx, lx, "request", JS_NewCFunction(ctx, ScriptEngineHost::jsRequest, "request", 3));
    JS_SetPropertyStr(ctx, lx, "on", JS_NewCFunction(ctx, ScriptEngineHost::jsOn, "on", 2));
    JS_SetPropertyStr(ctx, lx, "send", JS_NewCFunction(ctx, ScriptEngineHost::jsSend, "send", 2));

#if defined(Q_OS_ANDROID)
    JS_SetPropertyStr(ctx, lx, "env", JS_NewString(ctx, "mobile"));
#else
    JS_SetPropertyStr(ctx, lx, "env", JS_NewString(ctx, "desktop"));
#endif
    // 部分脚本会把 lx.version 当作宿主版本判断（低于自身要求时会静默不初始化），
    // 取当前 LX Music 桌面版的实际版本号，避免被误判为过旧宿主。
    JS_SetPropertyStr(ctx, lx, "version", JS_NewString(ctx, "2.9.0"));

    JSValue info = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, info, "name", JS_NewString(ctx, m_name.toUtf8().constData()));
    JS_SetPropertyStr(ctx, info, "version", JS_NewString(ctx, m_version.toUtf8().constData()));
    JS_SetPropertyStr(ctx, info, "author", JS_NewString(ctx, m_meta.value(QStringLiteral("author")).toString().toUtf8().constData()));
    JS_SetPropertyStr(ctx, info, "homepage", JS_NewString(ctx, m_meta.value(QStringLiteral("homepage")).toString().toUtf8().constData()));
    JS_SetPropertyStr(ctx, info, "description", JS_NewString(ctx, m_meta.value(QStringLiteral("description")).toString().toUtf8().constData()));
    JS_SetPropertyStr(ctx, info, "rawScript", JS_NewString(ctx, m_meta.value(QStringLiteral("rawScript")).toString().toUtf8().constData()));
    JS_SetPropertyStr(ctx, lx, "currentScriptInfo", info);

    JSValue crypto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, crypto, "md5", JS_NewCFunction(ctx, ScriptEngineHost::jsMd5, "md5", 2));
    JS_SetPropertyStr(ctx, crypto, "sha1", JS_NewCFunction(ctx, ScriptEngineHost::jsSha1, "sha1", 2));
    JS_SetPropertyStr(ctx, crypto, "sha256", JS_NewCFunction(ctx, ScriptEngineHost::jsSha256, "sha256", 1));
    JS_SetPropertyStr(ctx, crypto, "randomBytes", JS_NewCFunction(ctx, ScriptEngineHost::jsRandomBytes, "randomBytes", 1));
    JS_SetPropertyStr(ctx, crypto, "aesEncrypt", JS_NewCFunction(ctx, ScriptEngineHost::jsNotSupported, "aesEncrypt", 5));
    JS_SetPropertyStr(ctx, crypto, "rsaEncrypt", JS_NewCFunction(ctx, ScriptEngineHost::jsNotSupported, "rsaEncrypt", 3));

    JSValue buffer = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, buffer, "from", JS_NewCFunction(ctx, ScriptEngineHost::jsBufFrom, "from", 2));
    JS_SetPropertyStr(ctx, buffer, "bufToStr", JS_NewCFunction(ctx, ScriptEngineHost::jsBufToStr, "bufToStr", 2));
    // LX 文档里 buffer 转换函数名有两种写法，脚本里都出现过，同时提供别名。
    JS_SetPropertyStr(ctx, buffer, "bufToString", JS_NewCFunction(ctx, ScriptEngineHost::jsBufToStr, "bufToString", 2));
    JS_SetPropertyStr(ctx, buffer, "concat", JS_NewCFunction(ctx, ScriptEngineHost::jsBufConcat, "concat", 1));
    JS_SetPropertyStr(ctx, buffer, "alloc", JS_NewCFunction(ctx, ScriptEngineHost::jsBufAlloc, "alloc", 1));

    JSValue zlib = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, zlib, "inflate", JS_NewCFunction(ctx, ScriptEngineHost::jsNotSupported, "inflate", 1));
    JS_SetPropertyStr(ctx, zlib, "deflate", JS_NewCFunction(ctx, ScriptEngineHost::jsNotSupported, "deflate", 1));

    JSValue utils = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, utils, "crypto", crypto);
    JS_SetPropertyStr(ctx, utils, "buffer", buffer);
    JS_SetPropertyStr(ctx, utils, "zlib", zlib);
    JS_SetPropertyStr(ctx, lx, "utils", utils);

    JS_SetPropertyStr(ctx, global, "lx", lx);

    // Promise 结算辅助：统一处理脚本返回值与 Promise
    const char *settleCode =
        "globalThis.__furuiCb = globalThis.__furuiCb || undefined;"
        "globalThis.__furuiSettle = function (p, tag) {"
        "  Promise.resolve(p).then("
        "    function (v) { __furuiCb(tag, true, v); },"
        "    function (e) { __furuiCb(tag, false, e); }"
        "  );"
        "};";
    JSValue v;
    {
        ExecDeadlineGuard deadline(this, 4000);
        v = JS_Eval(ctx, settleCode, strlen(settleCode), "<host>", JS_EVAL_TYPE_GLOBAL);
    }
    if (JS_IsException(v)) reportException(QStringLiteral("host-prelude"));
    JS_FreeValue(ctx, v);
    JSValue cbFn = JS_NewCFunction(ctx, ScriptEngineHost::jsSettleCallback, "__furuiCb", 3);
    JS_SetPropertyStr(ctx, global, "__furuiCb", cbFn);

    if (scriptTraceEnabled()) {
        // 排查脚本缺失能力：把 lx 及各 utils 命名空间套上 Proxy，记录脚本读取到的未实现属性。
        JS_SetPropertyStr(ctx, global, "__furuiTrace",
            JS_NewCFunction(ctx, ScriptEngineHost::jsTrace, "__furuiTrace", 1));
        const char *traceCode =
            "(function(){"
            "  function wrap(o,label){ return new Proxy(o,{get:function(t,p,r){"
            "    if(typeof p==='string' && !(p in t)) __furuiTrace('missing '+label+'.'+p);"
            "    return Reflect.get(t,p,r);}}); }"
            "  globalThis.lx = wrap(globalThis.lx,'lx');"
            "  lx.utils = wrap(lx.utils,'utils');"
            "  lx.utils.crypto = wrap(lx.utils.crypto,'crypto');"
            "  lx.utils.buffer = wrap(lx.utils.buffer,'buffer');"
            "  lx.utils.zlib = wrap(lx.utils.zlib,'zlib');"
            "})();";
        JSValue tv;
        {
            ExecDeadlineGuard deadline(this, 4000);
            tv = JS_Eval(ctx, traceCode, strlen(traceCode), "<trace>", JS_EVAL_TYPE_GLOBAL);
        }
        if (JS_IsException(tv)) reportException(QStringLiteral("host-trace"));
        JS_FreeValue(ctx, tv);
    }

    JS_FreeValue(ctx, global);
}

void ScriptEngine::evalScript(const QString &script)
{
    JSContext *ctx = static_cast<JSContext *>(m_ctx);
    if (!ctx) { finishInit(false, QStringLiteral("runtime"), QStringLiteral("JS 引擎未初始化")); return; }
    JSValue v;
    {
        ExecDeadlineGuard deadline(this, 12000);
        v = JS_Eval(ctx, script.toUtf8().constData(), size_t(script.toUtf8().size()),
                    "user-script", JS_EVAL_TYPE_GLOBAL);
    }
    if (JS_IsException(v)) {
        JSValue exception = JS_GetException(ctx);
        const QString msg = exceptionText(ctx, exception);
        JS_FreeValue(ctx, exception);
        JS_FreeValue(ctx, v);
        finishInit(false, msg.contains(QLatin1String("SyntaxError")) || msg.contains(QLatin1String("unexpected token"))
                        ? QStringLiteral("syntax") : QStringLiteral("runtime"), msg);
        return;
    }
    JS_FreeValue(ctx, v);
    drainJobs();
}

void ScriptEngine::drainJobs()
{
    if (m_draining) return;
    JSRuntime *rt = static_cast<JSRuntime *>(m_rt);
    JSContext *ctx = static_cast<JSContext *>(m_ctx);
    if (!rt || !ctx) return;
    m_draining = true;
    {
        ExecDeadlineGuard deadline(this, 12000);
        while (JS_IsJobPending(rt)) {
            JSContext *jobCtx = ctx;
            const int r = JS_ExecutePendingJob(rt, &jobCtx);
            if (r < 0) {
                reportException(QStringLiteral("promise-job"));
                break;
            }
        }
    }
    m_draining = false;
}

void ScriptEngine::onInited(const QVariant &data)
{
    if (m_inited) return;
    m_inited = true;
    const QVariantMap map = data.toMap();
    const bool status = !map.contains(QStringLiteral("status"))
                        || map.value(QStringLiteral("status")).toBool();
    if (!status) {
        QString msg = map.value(QStringLiteral("msg")).toString();
        if (msg.isEmpty()) msg = map.value(QStringLiteral("message")).toString();
        if (msg.isEmpty()) msg = map.value(QStringLiteral("log")).toString();
        if (msg.isEmpty()) msg = QStringLiteral("脚本初始化返回 status=false");
        finishInit(false, QStringLiteral("init"), msg);
        return;
    }
    const QVariantMap sources = map.value(QStringLiteral("sources")).toMap();
    m_sources.clear();
    for (auto it = sources.constBegin(); it != sources.constEnd(); ++it) {
        const QVariantMap src = it.value().toMap();
        ScriptSourceInfo info;
        info.key = it.key();
        info.name = src.value(QStringLiteral("name")).toString();
        info.type = src.value(QStringLiteral("type"), QStringLiteral("music")).toString();
        for (const QVariant &a : src.value(QStringLiteral("actions")).toList())
            info.actions << a.toString();
        const QVariantList qualityList = src.value(QStringLiteral("qualitys")).toList().isEmpty()
            ? src.value(QStringLiteral("qualities")).toList()
            : src.value(QStringLiteral("qualitys")).toList();
        for (const QVariant &q : qualityList) info.qualities << q.toString();
        // LX 规范里 sources 的 name 是可选的（flower/grass 等源只给 key），
        // 缺省用 key 作为显示名，不能因缺 name 就丢弃该源。
        if (info.name.isEmpty()) info.name = info.key;
        m_sources << info;
    }
    if (m_sources.isEmpty()) {
        finishInit(false, QStringLiteral("empty"),
                   QStringLiteral("脚本完成初始化但未声明任何音源（可能是远端配置拉取失败，可稍后重试检查）"));
        return;
    }
    finishInit(true, QString(), QString());
}

void ScriptEngine::finishInit(bool ok, const QString &type, const QString &msg)
{
    m_ready = ok;
    if (m_initTimer) {
        m_initTimer->stop();
        m_initTimer->deleteLater();
        m_initTimer = nullptr;
    }
    InitCallback cb = m_initCb;
    m_initCb = nullptr;
    ScriptInitResult result;
    result.ok = ok;
    result.errorType = type;
    result.errorMsg = msg;
    result.sources = m_sources;
    if (cb) cb(result);
}

bool ScriptEngine::supports(const QString &sourceKey, const QString &action) const
{
    for (const ScriptSourceInfo &s : m_sources)
        if (s.key == sourceKey && s.actions.contains(action)) return true;
    return false;
}

void ScriptEngine::start(const QString &script, const QVariantMap &meta,
                         InitCallback cb, int timeoutMs)
{
    m_meta = meta;
    m_version = meta.value(QStringLiteral("version")).toString();
    m_initCb = cb;
    createContext();
    if (m_stopped) return;

    m_initTimer = new QTimer(this);
    m_initTimer->setSingleShot(true);
    connect(m_initTimer, &QTimer::timeout, this, [this] {
        if (m_initCb) {
            const QString detail = m_lastRejection.isEmpty()
                ? QStringLiteral("脚本初始化超时（未调用 send(inited)）")
                : QStringLiteral("脚本初始化超时（未调用 send(inited)，内部错误：%1）")
                      .arg(m_lastRejection);
            finishInit(false, QStringLiteral("timeout"), detail);
        }
    });
    m_initTimer->start(qMax(3000, timeoutMs));

    evalScript(script);

    // 同步初始化或同步失败时 finishInit 已经清理了回调，别再留着超时定时器
    if (!m_initCb && m_initTimer) m_initTimer->stop();
}

void ScriptEngine::callAction(const QString &sourceKey, const QString &action,
                              const QVariantMap &info, ActionCallback cb, int timeoutMs)
{
    if (!m_ready) {
        ScriptActionResult r;
        r.errorType = QStringLiteral("init");
        r.error = QStringLiteral("脚本尚未就绪");
        if (cb) cb(r);
        return;
    }
    JSContext *ctx = static_cast<JSContext *>(m_ctx);
    void *handler = m_handlers.value(QStringLiteral("request"));
    if (!ctx || !handler) {
        ScriptActionResult r;
        r.errorType = QStringLiteral("runtime");
        r.error = QStringLiteral("脚本未注册 request 处理器");
        if (cb) cb(r);
        return;
    }
    const int tag = ++m_nextTag;

    JSValue arg = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, arg, "action", JS_NewString(ctx, action.toUtf8().constData()));
    JS_SetPropertyStr(ctx, arg, "source", JS_NewString(ctx, sourceKey.toUtf8().constData()));
    JS_SetPropertyStr(ctx, arg, "info", objectMapToJs(ctx, info, 0));

    JSValue ret;
    {
        ExecDeadlineGuard deadline(this, 15000);
        ret = JS_Call(ctx, holderValue(handler), JS_UNDEFINED, 1, &arg);
    }
    JS_FreeValue(ctx, arg);
    if (JS_IsException(ret)) {
        reportException(QStringLiteral("callAction:") + action);
        JS_FreeValue(ctx, ret);
        ScriptActionResult r;
        r.errorType = QStringLiteral("runtime");
        r.error = QStringLiteral("脚本执行异常：") + action;
        if (cb) cb(r);
        return;
    }

    Pending pending;
    pending.cb = cb;
    pending.timer = new QTimer(this);
    pending.timer->setSingleShot(true);
    connect(pending.timer, &QTimer::timeout, this, [this, tag] {
        auto it = m_pending.find(tag);
        if (it == m_pending.end()) return;
        auto p = it.value();
        m_pending.erase(it);
        if (p.timer) p.timer->deleteLater();
        ScriptActionResult r;
        r.errorType = QStringLiteral("timeout");
        r.error = QStringLiteral("脚本处理超时");
        if (p.cb) p.cb(r);
    });
    pending.timer->start(qMax(3000, timeoutMs));
    m_pending.insert(tag, pending);

    // 用 JS 侧辅助统一处理 Promise / 普通返回值
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue settle = JS_GetPropertyStr(ctx, global, "__furuiSettle");
    JS_FreeValue(ctx, global);
    JSValue tagVal = JS_NewInt32(ctx, tag);
    JSValueConst args[2] = {ret, tagVal};
    JSValue callRet;
    {
        ExecDeadlineGuard deadline(this, 12000);
        callRet = JS_Call(ctx, settle, JS_UNDEFINED, 2, args);
    }
    if (JS_IsException(callRet)) reportException(QStringLiteral("settle"));
    JS_FreeValue(ctx, callRet);
    JS_FreeValue(ctx, settle);
    JS_FreeValue(ctx, tagVal);
    JS_FreeValue(ctx, ret);
    drainJobs();
}

void ScriptEngine::reportException(const QString &where)
{
    JSContext *ctx = static_cast<JSContext *>(m_ctx);
    if (!ctx) return;
    JSValue exception = JS_GetException(ctx);
    const QString msg = exceptionText(ctx, exception);
    JS_FreeValue(ctx, exception);
    if (scriptTraceEnabled())
        scriptTrace(QStringLiteral("[exception] %1: %2").arg(where, msg));
    qWarning().noquote() << QStringLiteral("[音源:%1] %2: %3").arg(m_name, where, msg);
}

} // namespace Furui
