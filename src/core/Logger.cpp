#include "Logger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QtGlobal>
#include <cstdio>

#ifdef Q_OS_ANDROID
#include <android/log.h>
#endif

#include "Settings.h"

namespace Furui {

namespace {
constexpr int kMaxRingLines = 400;
constexpr qint64 kMaxFileBytes = 8 * 1024 * 1024;

const char *levelOf(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return "D";
    case QtInfoMsg:     return "I";
    case QtWarningMsg:  return "W";
    case QtCriticalMsg: return "E";
    case QtFatalMsg:    return "F";
    }
    return "I";
}

QString todayPath()
{
    const QString dir = Settings::dataDir() + QStringLiteral("/logs");
    return dir + QStringLiteral("/furui-%1.log")
                     .arg(QDate::currentDate().toString(QStringLiteral("yyyyMMdd")));
}
} // namespace

Logger *Logger::instance()
{
    static Logger *s = new Logger();
    return s;
}

Logger::Logger(QObject *parent)
    : QObject(parent)
{
}

void Logger::install()
{
    // 无捕获 lambda 可以隐式转成函数指针，避免为处理器再单独定义一个静态函数
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &msg) {
        Logger::instance()->append(QString::fromLatin1(levelOf(type)), msg);
        // 装了自己的处理器就顶掉了 Qt 默认的：Windows 的 GUI 子系统没有控制台，
        // 写 stderr 只是给开发者留个口子；Android 上默认处理器是转 logcat 的，
        // 这里必须显式补一条，否则 adb logcat 里再也看不到应用日志。
#ifdef Q_OS_ANDROID
        int prio = ANDROID_LOG_INFO;
        if (type == QtDebugMsg) prio = ANDROID_LOG_DEBUG;
        else if (type == QtWarningMsg) prio = ANDROID_LOG_WARN;
        else if (type == QtCriticalMsg) prio = ANDROID_LOG_ERROR;
        else if (type == QtFatalMsg) prio = ANDROID_LOG_FATAL;
        __android_log_write(prio, "FuruiMusic", msg.toUtf8().constData());
#else
        fprintf(stderr, "%s\n", msg.toUtf8().constData());
#endif
    });
}

void Logger::write(const QString &level, const QString &message)
{
    instance()->append(level, message);
}

void Logger::setFileEnabled(bool on)
{
    bool changed = false;
    {
        QMutexLocker lock(&m_mutex);
        changed = (on != m_fileEnabled);
        m_fileEnabled = on;
        m_path = todayPath();
    }
    // append() 也要拿同一把锁，这里必须先放开再补记开日志这条记录
    if (!on) return;
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    if (changed) {
        append(QStringLiteral("I"),
               QStringLiteral("日志写入已开启：%1").arg(QDir::toNativeSeparators(m_path)));
    }
}

bool Logger::fileEnabled() const
{
    QMutexLocker lock(&m_mutex);
    return m_fileEnabled;
}

QString Logger::filePath() const
{
    QMutexLocker lock(&m_mutex);
    return m_path.isEmpty() ? todayPath() : m_path;
}

QString Logger::recentText(int maxLines)
{
    QMutexLocker lock(&m_mutex);
    if (m_ring.isEmpty()) return QString();
    const int n = qBound(1, maxLines, int(m_ring.size()));
    return m_ring.mid(m_ring.size() - n).join(QLatin1Char('\n'));
}

void Logger::append(const QString &level, const QString &message)
{
    QString body = message;
    if (body.size() > 6000) body = body.left(6000) + QStringLiteral("…");
    const QString stamp = QDateTime::currentDateTime()
                              .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    const QString line = QStringLiteral("%1 [%2] %3").arg(stamp, level, body);

    QMutexLocker lock(&m_mutex);
    m_ring.append(line);
    while (m_ring.size() > kMaxRingLines) m_ring.removeFirst();
    if (!m_fileEnabled) return;

    // 跨天时文件名要跟着换：长时间运行的桌面端可能连着挂好几天
    const QString want = todayPath();
    if (want != m_path) m_path = want;
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QFile f(m_path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    // 超过 8 MB 就留一份上一代，避免长期开启把手机存储写满
    if (f.size() > kMaxFileBytes) {
        f.close();
        const QString old = m_path + QStringLiteral(".1");
        QFile::remove(old);
        QFile::rename(m_path, old);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    }
    f.write(line.toUtf8());
    f.write("\n");
    f.close();
}

} // namespace Furui
