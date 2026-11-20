"""Regenerate src/core/PinyinTable.h (requires pypinyin on the dev machine).

The generated header is checked in, so the app has no runtime dependency.
Only GB2312 (level 1 + 2) Hanzi are included -- they cover virtually all
Chinese song titles / artist names. The first reading of a polyphonic
character is used.
"""
import os
import unicodedata

from pypinyin import pinyin_dict


def main():
    table = pinyin_dict.pinyin_dict
    rows = []
    for cp in range(0x4E00, 0x9FA6):
        ch = chr(cp)
        try:
            ch.encode('gb2312')
        except Exception:
            continue
        if cp not in table:
            continue
        raw = table[cp].split(',')[0]
        py = unicodedata.normalize('NFKD', raw).encode('ascii', 'ignore').decode().lower()
        py = py.replace('u:', 'v').replace('ü', 'v')
        if not py or not all('a' <= c <= 'z' for c in py):
            continue
        rows.append((cp, py))
    rows.sort()

    out = [
        '#pragma once',
        '// 自动生成：GB2312 常用汉字的全拼（小写、无声调，ü 记作 v）。',
        '// 数据源：pypinyin 的 pinyin_dict，取每个字的第一读音；多音字其它读音未收录。',
        '// 由 src/tools/gen_pinyin_table.py 生成，运行时零依赖。',
        '',
        '#include <QtGlobal>',
        '',
        'namespace Furui {',
        'namespace PinyinTable {',
        '',
        'struct Entry {',
        '    ushort code;',
        '    const char *pinyin;',
        '};',
        '',
        'static const Entry kEntries[] = {',
    ]
    for cp, py in rows:
        out.append('    {0x%04X, "%s"},' % (cp, py))
    out += [
        '};',
        '',
        'static const int kCount = sizeof(kEntries) / sizeof(kEntries[0]);',
        '',
        '} // namespace PinyinTable',
        '} // namespace Furui',
        '',
    ]

    path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        'core', 'PinyinTable.h')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out))
    print('wrote', path, len(rows), 'entries')


if __name__ == '__main__':
    main()
