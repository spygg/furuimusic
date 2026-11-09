#pragma once
// 福瑞音乐 - 轻量设置存取（QSettings + 统一数据目录）
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
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
    { return store()->value(QStringLiteral("furui/") + key, fallback); }

    static void set(const QString &key, const QVariant &value)
    {
        store()->setValue(QStringLiteral("furui/") + key, value);
        store()->sync();
    }

    static QString string(const QString &key, const QString &fallback = QString())
    { return get(key, fallback).toString(); }

    static bool boolean(const QString &key, bool fallback = false)
    { return get(key, fallback).toBool(); }

    static int integer(const QString &key, int fallback = 0)
    { return get(key, fallback).toInt(); }

private:
    static QSettings *store()
    {
        static QSettings s(filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        return &s;
    }
};

} // namespace Furui
