#pragma once
// 福瑞音乐 - 公共数据类型
// 纯数据层，不依赖任何 UI 框架（Widgets / Qt Quick 共用）
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <QVector>
#include <QMetaType>

namespace Furui {

enum class Quality { Standard = 0, High = 1, Lossless = 2, HiRes = 3 };

QString qualityKey(Quality q);
QString qualityName(Quality q);
Quality qualityFromKey(const QString &key);

// 一首歌在所有来源间的统一表示
struct Song {
    QString uid;        // "source:sourceId"，全局唯一
    QString source;     // 平台 id，如 netease / qq / kugou / kuwo / migu / custom
    QString sourceId;   // 平台内部歌曲 id
    QString title;
    QString artist;
    QString album;
    QString cover;      // 封面 URL 或本地路径
    QString albumId;
    qint64 duration = 0;   // 毫秒
    bool vip = false;      // 是否需要会员/付费
    bool playable = true;
    QString quality;       // 可播放音质标记
    QVariantMap extra;     // 平台原始字段，供自定义音源透传

    QString normalizedKey() const;  // 歌名+歌手归一化，用于跨源去重/替代音源匹配
};

// 歌单 / 榜单 / 我的收藏
struct Playlist {
    QString uid;
    QString source;
    QString sourceId;
    QString title;
    QString cover;
    QString owner;
    int count = 0;
    QString type;       // rank / user / favorite / local / custom
    QVariantMap extra;
};

// 某平台的一个登录会话
struct Account {
    QString source;
    QString uid;
    QString nickname;
    QString avatar;
    QString cookie;     // 本地持久化（混淆存储）
    QString token;
    bool logged = false;
    qint64 updated = 0;
    QString status;     // 状态描述，给 UI 直接显示
};

// 音源解析结果（原平台或替代音源）
struct Playable {
    QUrl url;
    bool ok = false;
    bool substituted = false;   // 是否使用了替代音源
    bool needSubstitute = false; // 原平台判定为 VIP / 不可播放，需要替代音源
    QString substituteSource;   // 替代音源名称
    QString message;            // 给 UI 的提示（如"VIP 歌曲，已由本地音源替代"）
    QString reason;             // 内部原因标记：vip / unavailable / empty
    QVariantMap headers;        // 播放时附带的请求头
};

struct LyricLine {
    qint64 time = 0;    // 毫秒
    QString text;
};

// 归一化字符串：去空白/标点/大小写/常见括号后缀
QString normalize(const QString &s);
// 编辑距离相似度 0..1
qreal similarity(const QString &a, const QString &b);
// 同上，但参数必须已经 normalize（批量匹配时预计算归一化结果，避免重复归一化）
qreal similarityNormalized(const QString &normalizedA, const QString &normalizedB);
// 解析 LRC
QVector<LyricLine> parseLrc(const QString &lrc);
// 从 "a=1; b=2" 形式的 Cookie 串里按候选名（忽略大小写）取第一个非空值
QString cookieValue(const QString &cookie, const QStringList &names);

} // namespace Furui

Q_DECLARE_METATYPE(Furui::Song)
Q_DECLARE_METATYPE(Furui::Playlist)
Q_DECLARE_METATYPE(Furui::Account)
Q_DECLARE_METATYPE(QVector<Furui::Song>)
