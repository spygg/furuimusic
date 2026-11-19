#include "core/QtCompat.h"
#include "Types.h"

#include <QRegularExpression>
#include <QTextStream>
#include <algorithm>

namespace Furui {

QString qualityKey(Quality q)
{
    switch (q) {
    case Quality::Standard: return QStringLiteral("standard");
    case Quality::High:     return QStringLiteral("high");
    case Quality::Lossless: return QStringLiteral("lossless");
    case Quality::HiRes:    return QStringLiteral("hires");
    }
    return QStringLiteral("standard");
}

QString qualityName(Quality q)
{
    switch (q) {
    case Quality::Standard: return QStringLiteral("标准");
    case Quality::High:     return QStringLiteral("高品质");
    case Quality::Lossless: return QStringLiteral("无损");
    case Quality::HiRes:    return QStringLiteral("高清臻音");
    }
    return QStringLiteral("标准");
}

Quality qualityFromKey(const QString &key)
{
    if (key == QLatin1String("high")) return Quality::High;
    if (key == QLatin1String("lossless")) return Quality::Lossless;
    if (key == QLatin1String("hires")) return Quality::HiRes;
    return Quality::Standard;
}

QString normalize(const QString &s)
{
    // 正则只在首次调用时编译：本地匹配会拿几千个文件名反复调 normalize，
    // 每次现构造 QRegularExpression 会编译一遍模式，是明显的热点。
    static const QRegularExpression brackets(
        QStringLiteral("\\([^)]*\\)|［[^］]*］|\\[[^\\]]*\\]|（[^）]*）"));
    QString t = s.toLower();
    // 去掉常见括号补充信息（feat. / live / remix / 伴奏 等）与所有非字母数字/CJK
    t.remove(brackets);
    QString out;
    out.reserve(t.size());
    for (const QChar &c : t) {
        if (c.isLetterOrNumber()) out.append(c);
    }
    return out;
}

qreal similarity(const QString &a, const QString &b)
{
    return similarityNormalized(normalize(a), normalize(b));
}

qreal similarityNormalized(const QString &x, const QString &y)
{
    if (x.isEmpty() && y.isEmpty()) return 1.0;
    if (x.isEmpty() || y.isEmpty()) return 0.0;
    if (x == y) return 1.0;
    const int n = x.size(), m = y.size();
    // 编辑距离下界剪枝：长度差已经超过可接受阈值时，怎么算都到不了 0.55
    if (qAbs(n - m) * 100 > qMax(n, m) * 45) return 0.0;
    QVector<int> prev(m + 1), cur(m + 1);
    for (int j = 0; j <= m; ++j) prev[j] = j;
    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        for (int j = 1; j <= m; ++j) {
            const int cost = (x.at(i - 1) == y.at(j - 1)) ? 0 : 1;
            cur[j] = std::min(std::min(cur[j - 1] + 1, prev[j] + 1), prev[j - 1] + cost);
        }
        prev.swap(cur);
    }
    const int dist = prev[m];
    return qreal(qMax(n, m) - dist) / qreal(qMax(n, m));
}

QVector<LyricLine> parseLrc(const QString &lrc)
{
    QVector<LyricLine> lines;
    static const QRegularExpression tag(QStringLiteral("\\[(\\d{1,3}):(\\d{1,2}(?:[.:]\\d{1,3})?)\\]"));
    const QStringList rawLines = lrc.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (QString line : rawLines) {
        line.remove(QLatin1Char('\r'));
        QString text = line;
        QVector<qint64> times;
        auto it = tag.globalMatch(line);
        int lastEnd = 0;
        while (it.hasNext()) {
            const auto m = it.next();
            const qint64 min = m.captured(1).toLongLong();
            QString sec = m.captured(2);
            sec.replace(QLatin1Char('.'), QLatin1Char(':'));
            const QStringList parts = sec.split(QLatin1Char(':'));
            const qint64 s = parts.value(0).toLongLong();
            const qint64 ms = parts.value(1, QStringLiteral("0")).leftJustified(3, QLatin1Char('0')).left(3).toLongLong();
            times.append((min * 60 + s) * 1000 + ms);
            lastEnd = m.capturedEnd(0);
        }
        text = line.mid(lastEnd).trimmed();
        for (qint64 t : times) {
            if (!text.isEmpty()) lines.append({t, text});
        }
    }
    std::sort(lines.begin(), lines.end(), [](const LyricLine &a, const LyricLine &b) {
        return a.time < b.time;
    });
    return lines;
}

QVariantMap Comment::toVariant() const
{
    QVariantMap m;
    m.insert(QStringLiteral("id"), id);
    m.insert(QStringLiteral("text"), text);
    m.insert(QStringLiteral("userName"), userName);
    m.insert(QStringLiteral("avatar"), avatar);
    m.insert(QStringLiteral("timeText"), timeText);
    m.insert(QStringLiteral("likedCount"), likedCount);
    m.insert(QStringLiteral("replyCount"), replyCount);
    m.insert(QStringLiteral("replyText"), replyText);
    m.insert(QStringLiteral("source"), source);
    return m;
}

bool hasLrcTimeTag(const QString &text)
{
    // 洛雪 existTimeExp = /\[\d{1,2}:.*\d{1,4}\]/ 的等价实现：
    // 只要出现一个形如 [mm:ss.xx] 的时间标签就认为有有效时间轴。
    static const QRegularExpression timeTag(
        QStringLiteral("\\[\\d{1,2}:[^\\]]*\\d{1,4}\\]"));
    return timeTag.match(text).hasMatch();
}

bool isLyricsUsable(const QString &text)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) return false;
    // 无歌词占位（各平台/自定义源的常见写法）
    if (t == QLatin1String("暂无歌词") || t == QLatin1String("无歌词")
        || t.compare(QLatin1String("no lyric"), Qt::CaseInsensitive) == 0
        || t.compare(QLatin1String("null"), Qt::CaseInsensitive) == 0) return false;
    const QString head = t.left(300);
    // HTML 反爬页/错误页：真实歌词正文绝不会以标签开头，也不会整段是标签
    if (head.contains(QLatin1String("<!doctype"), Qt::CaseInsensitive)
        || head.contains(QLatin1String("<html"), Qt::CaseInsensitive)
        || head.contains(QLatin1String("<head"), Qt::CaseInsensitive)
        || head.contains(QLatin1String("<body"), Qt::CaseInsensitive)
        || head.contains(QLatin1String("<script"), Qt::CaseInsensitive)) return false;
    // JSON 错误体（歌词接口挂了返回 {"code":...}，不是歌词）
    if ((head.startsWith(QLatin1Char('{')) || head.startsWith(QLatin1Char('[')))
        && !hasLrcTimeTag(t)) return false;
    // 有标准时间轴的一定可用
    if (hasLrcTimeTag(t)) return true;
    // 无时间轴的纯文本歌词：要求至少 2 行且不是明显的报错/网址，避免把
    // 「请求过于频繁」「Access Denied」这类单行提示当歌词显示
    const QStringList lines = t.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (lines.size() < 2) return false;
    if (head.contains(QLatin1String("access denied"), Qt::CaseInsensitive)
        || head.contains(QLatin1String("forbidden"), Qt::CaseInsensitive)
        || head.contains(QLatin1String("请求过于频繁"))
        || head.contains(QLatin1String("访问频繁"))) return false;
    return true;
}

QString Song::normalizedKey() const
{
    return normalize(title) + QLatin1Char('|') + normalize(artist);
}

QString cookieValue(const QString &cookie, const QStringList &names)
{
    for (const QString &part : cookie.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QString kv = part.trimmed();
        const int eq = kv.indexOf(QLatin1Char('='));
        if (eq <= 0) continue;
        const QString name = kv.left(eq).trimmed();
        const QString value = kv.mid(eq + 1).trimmed();
        if (value.isEmpty()) continue;
        for (const QString &key : names)
            if (name.compare(key, Qt::CaseInsensitive) == 0) return value;
    }
    return QString();
}

} // namespace Furui
