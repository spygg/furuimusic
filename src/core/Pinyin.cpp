#include "Pinyin.h"

#include "PinyinTable.h"

#include <QtGlobal>

namespace Furui {
namespace Pinyin {

QString of(QChar c)
{
    const ushort code = c.unicode();
    // 表按 code 升序排列，二分查找
    int lo = 0;
    int hi = PinyinTable::kCount - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const ushort v = PinyinTable::kEntries[mid].code;
        if (v == code)
            return QString::fromLatin1(PinyinTable::kEntries[mid].pinyin);
        if (v < code) lo = mid + 1;
        else hi = mid - 1;
    }
    return QString();
}

QString text(const QString &s)
{
    QString out;
    out.reserve(s.size() * 4);
    for (const QChar &c : s) {
        const QString py = of(c);
        if (!py.isEmpty()) { out.append(py); continue; }
        if (c.isLetterOrNumber()) out.append(c.toLower());
    }
    return out;
}

QString initials(const QString &s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar &c : s) {
        const QString py = of(c);
        if (!py.isEmpty()) { out.append(py.at(0)); continue; }
        if (c.isLetterOrNumber()) out.append(c.toLower());
    }
    return out;
}

QString normalizedQuery(const QString &s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar &c : s) {
        if (c.isLetterOrNumber()) out.append(c.toLower());
    }
    return out;
}

bool matches(const QString &haystack, const QString &query)
{
    if (query.trimmed().isEmpty()) return true;
    // 直接匹配原文（支持中文输入，或带空格的英文片段）
    if (haystack.contains(query, Qt::CaseInsensitive)) return true;
    const QString q = normalizedQuery(query);
    if (q.isEmpty()) return true;
    if (text(haystack).contains(q)) return true;
    if (initials(haystack).contains(q)) return true;
    return false;
}

} // namespace Pinyin
} // namespace Furui
