#pragma once
// 福瑞音乐 - 自定义音源（VIP 歌曲替代链）
// 三级：① 用户自建的合规 HTTP 接口；② 用户自己的本地音乐目录；
//       ③ 内置默认音源 GD音乐台（gdMusicSource 可关）→ 用户导入的 LX 在线脚本音源。
// 项目不提供绕过付费/DRM 的能力，请仅使用你拥有合法授权的内容。
#include <QObject>
#include <QVector>
#include <functional>

#include "Types.h"

namespace Furui {

class CustomSourceManager : public QObject {
    Q_OBJECT
public:
    struct Item {
        QString id;
        QString name;
        QString type;      // http / local
        QString value;     // baseUrl 或目录
        bool enabled = true;
    };
    using PlayableCallback = std::function<void(const Playable &)>;
    using SongsCallback = std::function<void(const QVector<Song> &)>;
    using StringCallback = std::function<void(const QString &)>;

    static CustomSourceManager *instance();
    void refresh();

    QVector<Item> items() const { return m_items; }
    QStringList ids() const;
    QStringList names() const;
    QString localDirectory() const;

    void addHttp(const QString &name, const QString &baseUrl);
    void setLocalDirectory(const QString &path);
    void remove(const QString &id);

    // 依次尝试所有自定义音源，返回第一个可用结果
    void resolve(const Song &song, const QString &quality, PlayableCallback cb);
    void search(const QString &keyword, int limit, SongsCallback cb);
    void lyric(const Song &song, StringCallback cb);

signals:
    void changed();

private:
    explicit CustomSourceManager(QObject *parent = nullptr);
    void resolveAt(int index, const Song &song, const QString &quality, PlayableCallback cb);
    void lyricAt(int index, const Song &song, StringCallback cb);
    void scanLocal();
    Playable matchLocal(const Song &song) const;
    QVector<Item> m_items;
    // norm：预先归一化过的文件名，匹配时免去逐个 normalize（否则要编译几千次正则）
    struct LocalFile { QString path; QString name; QString norm; };
    QVector<LocalFile> m_localFiles;
    QString m_scannedDir;
};

} // namespace Furui
