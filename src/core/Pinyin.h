#pragma once
// 轻量拼音检索：汉字 → 全拼 / 首字母，供本地歌单的「歌名 / 歌手」过滤使用。
// 数据表是生成产物（见 PinyinTable.h），运行时不依赖任何第三方库。
#include <QChar>
#include <QString>

namespace Furui {
namespace Pinyin {

// 单字全拼（小写、无声调，ü→v）；非汉字或表外字返回空串
QString of(QChar c);

// 把文本转成连续全拼：汉字→全拼，字母/数字保留，空白与标点丢弃。
// 例："周杰伦 晴天" → "zhoujielunqintian"
QString text(const QString &s);

// 把文本转成拼音首字母串：每个汉字取全拼首字母，字母/数字保留。
// 例："周杰伦 晴天" → "zjlqt"
QString initials(const QString &s);

// 检索词归一化：小写 + 只保留汉字/字母/数字
QString normalizedQuery(const QString &s);

// 综合匹配：原文（大小写不敏感）/ 全拼 / 首字母任意一种命中即可
bool matches(const QString &haystack, const QString &query);

} // namespace Pinyin
} // namespace Furui
