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
