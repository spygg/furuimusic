#pragma once
// 福瑞音乐 - 轻量设置存取（QSettings + 统一数据目录）
#include <QCoreApplication>
#include <QDir>
#include <QObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVariant>

namespace Furui {

class Settings {
public:
    static QString dataDir()
    {
        static QString dir;
        if (dir.isEmpty()) {
            dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            if (dir.isEmpty()) dir = QDir::homePath() + QStringLiteral("/.FuruiMusic");
            QDir().mkpath(dir);
        }
        return dir;
    }

    static QString filePath(const QString &name) { return dataDir() + QLatin1Char('/') + name; }

    static QVariant get(const QString &key, const QVariant &fallback = QVariant())
    { return store()->settings.value(QStringLiteral("furui/") + key, fallback); }

    // 拖动音量条 / 切歌时 set() 会被高频调用。每次 sync() 都要落盘 + fsync，
    // 大量调用会让 UI 线程出现可见卡顿，所以这里只写内存，攒 600ms 再落盘；
    // 退出前 aboutToQuit 会强制 flush，不会丢设置。
    static void set(const QString &key, const QVariant &value)
    {
        store()->settings.setValue(QStringLiteral("furui/") + key, value);
        store()->scheduleSync();
    }

    static void flush() { store()->flush(); }

    static QString string(const QString &key, const QString &fallback = QString())
    { return get(key, fallback).toString(); }

    static bool boolean(const QString &key, bool fallback = false)
    { return get(key, fallback).toBool(); }

    static int integer(const QString &key, int fallback = 0)
    { return get(key, fallback).toInt(); }

private:
    struct Store : public QObject {
        Store()
            : settings(Settings::filePath(QStringLiteral("settings.ini")), QSettings::IniFormat)
        {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
            // Qt6 的 IniFormat 写入器把非 ASCII 写成裸 UTF-8，而 Qt5 默认按
            // Latin-1 读回：两代构建共用同一份 settings.ini 时，Qt5 会话会把
            // Qt6 存的队列标题读花，存回后逐会话叠加成更深的乱码。显式对齐
            // UTF-8 后 Qt5 读写与 Qt6 完全一致（旧 \xNNNN 转义仍能解码）。
            settings.setIniCodec("UTF-8");
#endif
            timer.setSingleShot(true);
            timer.setInterval(600);
            QObject::connect(&timer, &QTimer::timeout, this, [this] { settings.sync(); });
            if (QCoreApplication *app = QCoreApplication::instance())
                QObject::connect(app, &QCoreApplication::aboutToQuit, this, [this] { flush(); });
        }

        void scheduleSync()
        {
            if (!timer.isActive())
                timer.start();
        }

        void flush()
        {
            if (timer.isActive())
                timer.stop();
            settings.sync();
        }

        QSettings settings;
        QTimer timer;
    };

    static Store *store()
    {
        static Store *s = new Store();
        return s;
    }
};

} // namespace Furui
