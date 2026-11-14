#pragma once
// 福瑞音乐 - 描述式 JSON 映射
// 配置里的 "path" 支持：a.b.c / a.b[0].c / artists[].name（通配数组）
// 模板字符串支持 {placeholder} 替换，由调用方传入变量表。
#include "core/QtCompat.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaType>
#include <QRegularExpression>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtGlobal>

#include "Types.h"

namespace Furui {
namespace Json {

inline QJsonValue valueAt(const QJsonValue &root, const QString &path)
{
    if (path.isEmpty() || path == QLatin1String(".") || path == QLatin1String("$"))
        return root;
    QJsonValue cur = root;
    const QStringList segments = path.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    for (QString seg : segments) {
        int idx = -1;
        const int lb = seg.indexOf(QLatin1Char('['));
        if (lb >= 0) {
            QString inside = seg.mid(lb + 1);
            inside.remove(QLatin1Char(']'));
            bool ok = false;
            idx = inside.toInt(&ok);
            if (!ok) idx = -1;
            seg = seg.left(lb);
        }
        if (!seg.isEmpty()) {
            if (!cur.isObject()) return QJsonValue();
            cur = cur.toObject().value(seg);
        }
        if (idx >= 0) {
            if (!cur.isArray()) return QJsonValue();
            cur = cur.toArray().at(idx);
        }
    }
    return cur;
}

inline QString str(const QJsonValue &v)
{
    switch (v.type()) {
    case QJsonValue::String: return v.toString();
    case QJsonValue::Double: {
        const double d = v.toDouble();
        if (qFuzzyCompare(d + 1.0, double(qint64(d)) + 1.0)) return QString::number(qint64(d));
        return QString::number(d);
    }
    case QJsonValue::Bool:   return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QJsonValue::Array: {
        QStringList out;
        const QJsonArray arr = v.toArray();
        for (int i = 0; i < arr.size() && i < 4; ++i) {
            const QString s = str(arr.at(i));
            if (!s.isEmpty()) out << s;
        }
        return out.join(QStringLiteral("/"));
    }
    case QJsonValue::Object: {
        const QJsonObject o = v.toObject();
        for (const QString &k : {QStringLiteral("name"), QStringLiteral("title"), QStringLiteral("url")})
            if (o.contains(k)) return str(o.value(k));
        return QString();
    }
    default: return QString();
    }
}

inline QString stringAt(const QJsonValue &root, const QString &path)
{
    if (path.isEmpty()) return str(root);
    if (path.contains(QStringLiteral("[]"))) {
        const int p = path.indexOf(QStringLiteral("[]"));
        const QString head = path.left(p);
        QString tail = path.mid(p + 2);
        if (tail.startsWith(QLatin1Char('.'))) tail.remove(0, 1);
        QJsonValue v = valueAt(root, head);
        if (!v.isArray()) return QString();
        QStringList out;
        for (const QJsonValue &e : v.toArray()) {
            const QString s = tail.isEmpty() ? str(e) : str(valueAt(e, tail));
            if (!s.isEmpty()) out << s;
        }
        return out.join(QStringLiteral("/"));
    }
    return str(valueAt(root, path));
}

inline QString firstString(const QJsonValue &root, const QStringList &paths)
{
    for (const QString &p : paths) {
        const QString s = stringAt(root, p).trimmed();
        if (!s.isEmpty()) return s;
    }
    return QString();
}

inline QString render(const QString &tpl, const QVariantMap &vars)
{
    QString out = tpl;
    for (auto it = vars.constBegin(); it != vars.constEnd(); ++it) {
        const QString key = QStringLiteral("{") + it.key() + QStringLiteral("}");
        if (out.contains(key)) out.replace(key, it.value().toString());
    }
    // 没提供的变量替换成空串，免得把 {xxx} 原样发给服务器。
    // 只匹配「标识符形状」的占位符：歌名/关键词/Cookie 里合法的 {…} 内容
    // （比如 JSON 片段或带空括号的标题）不能被删掉，原来那种宽匹配会篡改请求体。
    static const QRegularExpression leftover(
        QStringLiteral("\\{[A-Za-z_][A-Za-z0-9_]*\\}"));
    out.remove(leftover);
    return out;
}

// 整串形如 {var:int} 的模板渲染成真正的 JSON 数字：
// QQ 的 musicu 接口对 topid/num 类型敏感，收到字符串会返回 toplist.code=10006（0 首）。
inline QVariant renderNumeric(const QString &tpl, const QVariantMap &vars, bool *matched)
{
    static const QRegularExpression numTpl(
        QStringLiteral("^\\{([A-Za-z_][A-Za-z0-9_]*):int\\}$"));
    const QRegularExpressionMatch m = numTpl.match(tpl);
    if (!m.hasMatch()) { if (matched) *matched = false; return QVariant(); }
    if (matched) *matched = true;
    bool ok = false;
    const int n = vars.value(m.captured(1)).toString().toInt(&ok);
    return ok ? QVariant(n) : QVariant(0);
}

inline QVariant renderVariant(const QVariant &in, const QVariantMap &vars)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const int typeId = in.typeId();
    if (typeId == QMetaType::QString) {
        bool numeric = false;
        const QVariant n = renderNumeric(in.toString(), vars, &numeric);
        return numeric ? n : QVariant(render(in.toString(), vars));
    }
    if (typeId == QMetaType::QVariantMap) {
#else
    if (in.type() == QVariant::String) {
        bool numeric = false;
        const QVariant n = renderNumeric(in.toString(), vars, &numeric);
        return numeric ? n : QVariant(render(in.toString(), vars));
    }
    if (in.type() == QVariant::Map) {
#endif
        QVariantMap out;
        const QVariantMap m = in.toMap();
        for (auto it = m.constBegin(); it != m.constEnd(); ++it)
            out.insert(it.key(), renderVariant(it.value(), vars));
        return out;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (typeId == QMetaType::QVariantList) {
#else
    if (in.type() == QVariant::List) {
#endif
        QVariantList out;
        for (const QVariant &v : in.toList()) out << renderVariant(v, vars);
        return out;
    }
    return in;
}

inline QVariantMap renderMap(const QVariantMap &in, const QVariantMap &vars)
{ return renderVariant(in, vars).toMap(); }

inline QJsonArray arrayAt(const QJsonValue &root, const QString &path)
{
    const QJsonValue v = path.isEmpty() ? root : valueAt(root, path);
    if (v.isArray()) return v.toArray();
    if (v.isObject()) {
        // 常见包装：{ list: [...] } / { songs: [...] }，交给配置显式指定 root
        return QJsonArray();
    }
    return QJsonArray();
}

inline QJsonDocument document(const QByteArray &body)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError) return QJsonDocument();
    return doc;
}

inline QJsonValue rootOf(const QJsonDocument &doc)
{
    if (doc.isObject()) return QJsonValue(doc.object());
    if (doc.isArray()) return QJsonValue(doc.array());
    return QJsonValue();
}

// 酷狗的封面/头像地址是服务端模板（http://imge.kugou.com/stdmusic/{size}/xxx.jpg），
// 原样请求会 404，必须客户端把尺寸填进去才能拿到图。
inline QString fillImageSize(const QString &url)
{
    static const QRegularExpression sizeTpl(QStringLiteral("\\{size\\}"));
    if (!url.contains(sizeTpl)) return url;
    QString out = url;
    out.replace(sizeTpl, QStringLiteral("240"));
    return out;
}

// 封面字段可能是普通字符串，也可能是汽水 Luna 那种 {urls:[前缀], uri} 对象。
// 通用映射只认字符串时，用户歌单/曲目的封面会整片空白，这里统一摊平。
inline QString coverUrlOf(const QJsonValue &v, const QString &path)
{
    if (path.isEmpty()) return QString();
    const QJsonValue cv = valueAt(v, path);
    if (cv.isString()) return fillImageSize(cv.toString());
    if (cv.isObject()) {
        const QJsonObject o = cv.toObject();
        const QJsonArray urls = o.value(QStringLiteral("urls")).toArray();
        const QString prefix = urls.isEmpty() ? QString() : urls.at(0).toString();
        const QString joined = prefix + o.value(QStringLiteral("uri")).toString();
        if (!joined.isEmpty()) return fillImageSize(joined);
        const QJsonArray list = o.value(QStringLiteral("url_list")).toArray();
        if (!list.isEmpty()) return fillImageSize(list.at(0).toString());
        return fillImageSize(str(o.value(QStringLiteral("url"))));
    }
    return fillImageSize(str(cv));
}

inline Song mapSong(const QJsonValue &v, const QVariantMap &fields, const QString &source)
{
    Song s;
    s.source = source;
    s.sourceId = stringAt(v, fields.value(QStringLiteral("id")).toString());
    s.title    = stringAt(v, fields.value(QStringLiteral("title")).toString());
    s.artist   = stringAt(v, fields.value(QStringLiteral("artist")).toString());
    s.album    = stringAt(v, fields.value(QStringLiteral("album")).toString());
    // 没配 cover 字段时不能拿空路径去取值：stringAt 对空路径会 str(整个对象)，
    // 返回的其实是 name/title（歌名），封面就变成了一串中文，UI 只能当坏图丢掉。
    const QString coverPath = fields.value(QStringLiteral("cover")).toString();
    s.cover    = coverUrlOf(v, coverPath);
    s.albumId  = stringAt(v, fields.value(QStringLiteral("albumId")).toString());
    // 有些平台（QQ 搜索/榜单）响应里根本不给封面字段，只给专辑 mid，
    // 不给 coverTemplate 时网格里全是占位图。模板里的 {albumId} 用官方 CDN 实测可取到图。
    const QString coverTpl = fields.value(QStringLiteral("coverTemplate")).toString();
    if (s.cover.isEmpty() && !coverTpl.isEmpty() && !s.albumId.isEmpty()) {
        QString url = coverTpl;
        url.replace(QLatin1String("{albumId}"), s.albumId);
        url.replace(QLatin1String("{songId}"), s.sourceId);
        s.cover = fillImageSize(url);
    }
    const QString durPath = fields.value(QStringLiteral("duration")).toString();
    const double scale = fields.value(QStringLiteral("durationScale"), 1).toDouble();
    const double dur = stringAt(v, durPath).toDouble();
    s.duration = qint64(dur * (scale > 0 ? scale : 1.0));
    const QString vipPath = fields.value(QStringLiteral("vip")).toString();
    if (!vipPath.isEmpty()) {
        const QString flag = stringAt(v, vipPath);
        const QVariantList values = fields.value(QStringLiteral("vipValues")).toList();
        if (values.isEmpty()) {
            s.vip = (flag == QLatin1String("1") || flag == QLatin1String("true"));
        } else {
            for (const QVariant &x : values)
                if (x.toString() == flag) { s.vip = true; break; }
        }
    }
    if (s.sourceId.isEmpty())
        s.sourceId = QString::number(qHash(s.normalizedKey()), 16);
    s.uid = source + QLatin1Char(':') + s.sourceId;
    if (v.isObject()) s.extra = v.toObject().toVariantMap();
    return s;
}

inline Playlist mapPlaylist(const QJsonValue &v, const QVariantMap &fields, const QString &source,
                            const QString &type)
{
    Playlist p;
    p.source = source;
    p.type = type;
    p.sourceId = stringAt(v, fields.value(QStringLiteral("id")).toString());
    p.title    = stringAt(v, fields.value(QStringLiteral("title")).toString());
    const QString coverPath = fields.value(QStringLiteral("cover")).toString();
    p.cover    = coverUrlOf(v, coverPath);
    p.owner    = stringAt(v, fields.value(QStringLiteral("owner")).toString());
    p.count    = stringAt(v, fields.value(QStringLiteral("count")).toString()).toInt();
    if (p.sourceId.isEmpty()) p.sourceId = QString::number(qHash(p.title), 16);
    p.uid = source + QLatin1Char(':') + p.sourceId;
    if (v.isObject()) p.extra = v.toObject().toVariantMap();
    return p;
}

} // namespace Json
} // namespace Furui
