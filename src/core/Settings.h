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

    // 默认下载目录：
    //   Windows 便携式，放在 exe 同级的「下载」目录，跟着程序走，文件管理器里一眼能找到；
    //   macOS 的 exe 位于 .app/Contents/MacOS，bundle 通常只读；Linux AppImage 的
    //   applicationDirPath 指向只读挂载点（/tmp/.mount_xxx/usr/bin）。这两个平台写进
    //   程序目录会直接失败，统一改用系统「下载」目录下的 FuruiMusic 子目录。
    //   移动端（下载入口已隐藏）回退应用数据目录。
    static QString defaultDownloadsDir()
    {
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
        return filePath(QStringLiteral("downloads"));
#elif defined(Q_OS_WIN)
        return QCoreApplication::applicationDirPath() + QStringLiteral("/下载");
#else
        QString base = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        if (base.isEmpty())
            base = QDir::homePath();
        return base + QStringLiteral("/FuruiMusic");
#endif
    }

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

    // 取链顺序：设置页已移除「平台优先」，旧配置里的 platform 一律按 VIP 优先处理，
    // 避免升级后仍在偷偷走已下线的策略。只保留 vip（默认）/ custom 两个有效值。
    static QString sourceFirstOrder()
    {
        const QString v = string(QStringLiteral("sourceFirst"), QStringLiteral("vip"));
        return v == QLatin1String("custom") ? v : QStringLiteral("vip");
    }

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
