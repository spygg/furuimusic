#pragma once
// 福瑞音乐 - 轻量运行日志
//
// 默认只进内存环形缓冲（设置页 / 开放 API 网页可查看、拷贝）；
// 只有设置里打开「写入日志文件」后才落盘到 <数据目录>/logs/furui-YYYYMMDD.log。
// 之所以默认不写文件：手机 / 车机的存储写入代价高，绝大多数用户不需要。
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>

namespace Furui {

class Logger : public QObject {
public:
    static Logger *instance();

    // 安装 Qt 消息处理器：qDebug/qInfo/qWarning/qCritical 统一收进日志。
    // 启动时调用一次即可；重复调用无害（Qt 只保留最后一个处理器）。
    static void install();

    // 应用级事件（不是 qDebug 发出的）用这个补记
    static void write(const QString &level, const QString &message);

    void setFileEnabled(bool on);
    bool fileEnabled() const;
    // 当天日志文件的完整路径（未开启文件日志时仅用于展示）
    QString filePath() const;

    // 最近 maxLines 行，给开放 API / 设置页「复制日志」用
    QString recentText(int maxLines = 400);

private:
    explicit Logger(QObject *parent = nullptr);
    void append(const QString &level, const QString &message);

    mutable QMutex m_mutex;
    QStringList m_ring;
    bool m_fileEnabled = false;
    QString m_path;
};

} // namespace Furui
