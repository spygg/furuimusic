#pragma once
// 福瑞音乐 - 老 Qt 5 兼容层（Qt 5.7 / 5.8 / 5.9 等）
//
// 源码按 Qt 5.15 / Qt 6 的 API 编写，老 Qt 5 缺三样东西：
//   1. QRandomGenerator（5.10+）：这里用 qrand() 补一个同接口的最小实现，
//      只用于 device id / nonce / 随机播放序号；真正的密钥生成走 NeteaseCrypto。
//   2. Qt::SkipEmptyParts / Qt::KeepEmptyParts（5.14+）：老版本同名枚举在
//      QString 下，给 Qt 命名空间补两个常量，调用点一行都不用改。
//   3. <memory>：老 Qt 头不再间接包含它，std::make_shared 需要显式引入。
//   4. qEnvironmentVariable()（5.10+）/ QDateTime::currentSecsSinceEpoch()（5.8+）
//      / QSysInfo::machineUniqueId()（5.11+）/ QByteArray 忽略大小写比较（5.12+）。
//      前三个用同名自由函数或 furui* 辅助函数兜底，调用点极少。
// Qt >= 5.15 本头等价于 #include <QRandomGenerator> + <memory>，行为不变。

#include <QtGlobal>
#include <QByteArray>
#include <QDateTime>
#include <QSysInfo>
#include <QString>
#include <memory>

#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
#  include <QRandomGenerator>
#else
#  include <QDateTime>

// 只覆盖工程内实际用到的接口：global()/system()/generate()/generate64()/
// bounded(int)/bounded(int,int)。Windows 的 qrand() 只有 15 bit，拼两次凑 30 bit。
class QRandomGenerator
{
public:
    static QRandomGenerator *global()
    {
        static QRandomGenerator instance;
        static bool seeded = false;
        if (!seeded) {
            qsrand(uint(QDateTime::currentMSecsSinceEpoch() & 0xffffffffu));
            seeded = true;
        }
        return &instance;
    }
    static QRandomGenerator *system() { return global(); }

    quint32 generate()
    {
        return ((quint32(qrand()) & 0x7fffu) << 15) | (quint32(qrand()) & 0x7fffu);
    }
    quint64 generate64()
    {
        return (quint64(generate()) << 32) | quint64(generate());
    }
    int bounded(int highest)
    {
        return highest > 0 ? int(generate() % quint32(highest)) : 0;
    }
    int bounded(int lowest, int highest)
    {
        return highest > lowest ? lowest + bounded(highest - lowest) : lowest;
    }
};
#endif

// 老 Qt 5 缺失的零散 API 兜底。用自由函数而不是宏，避免污染 Qt 自己的类型。
// 高版本 Qt 直接走原生实现（furui* 辅助函数只是薄封装，行为一致）。
#if QT_VERSION < QT_VERSION_CHECK(5, 10, 0)
inline QString qEnvironmentVariable(const char *varName,
                                    const QString &defaultValue = QString())
{
    const QByteArray value = qgetenv(varName);
    return value.isNull() ? defaultValue : QString::fromLocal8Bit(value);
}
#endif

inline qint64 furuiCurrentSecsSinceEpoch()
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 8, 0)
    return QDateTime::currentSecsSinceEpoch();
#else
    return QDateTime::currentMSecsSinceEpoch() / 1000;
#endif
}

inline QByteArray furuiMachineUniqueId()
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
    return QSysInfo::machineUniqueId();
#else
    // Qt 5.7 没有机器 ID；调用方（Library::storageKey）已有环境指纹兜底分支。
    return QByteArray();
#endif
}

// Qt 5.15 起 QWindow 才有 windowStates()/startSystemMove()/startSystemResize()，
// 老 Qt 5 只有单个 windowState()。UI 层用下面这组薄封装，调用点无需到处 #if。
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
template <typename WindowT>
inline Qt::WindowStates furuiWindowStates(const WindowT *window)
{
    return window->windowStates();
}
template <typename WindowT>
inline bool furuiStartSystemMove(WindowT *window) { return window->startSystemMove(); }
template <typename WindowT>
inline bool furuiStartSystemResize(WindowT *window, Qt::Edges edges)
{
    return window->startSystemResize(edges);
}
#else
template <typename WindowT>
inline Qt::WindowStates furuiWindowStates(const WindowT *window)
{
    return Qt::WindowStates(window->windowState());
}
template <typename WindowT>
inline bool furuiStartSystemMove(WindowT *) { return false; }
template <typename WindowT>
inline bool furuiStartSystemResize(WindowT *, Qt::Edges) { return false; }
#endif

// Qt 5.7 的 QByteArray 连 compare() 本身都没有（5.12 才加），调用点只需判等。
inline bool furuiByteArrayEqualsIC(const QByteArray &a, const QByteArray &b)
{
    return a.toLower() == b.toLower();
}

#if QT_VERSION < QT_VERSION_CHECK(5, 14, 0)
namespace Qt {
// Qt 5.14 把 QString::SplitBehavior 搬到 Qt 命名空间并改名 SplitBehaviorFlags。
// 老版本补同名常量，让 `Qt::SkipEmptyParts` 继续可编译。
static const QString::SplitBehavior SkipEmptyParts = QString::SkipEmptyParts;
static const QString::SplitBehavior KeepEmptyParts = QString::KeepEmptyParts;
} // namespace Qt
#endif
