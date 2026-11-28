#pragma once

#include <QString>

namespace Furui {

enum class AndroidUpdateChannel {
    Release,
    Legacy,
    Unknown
};

inline QString normalizedCertificateSha256(const QString &value)
{
    const QString text = value.trimmed();
    if (text.size() != 64)
        return QString();
    for (const QChar ch : text) {
        const bool digit = ch >= QLatin1Char('0') && ch <= QLatin1Char('9');
        const bool lower = ch >= QLatin1Char('a') && ch <= QLatin1Char('f');
        const bool upper = ch >= QLatin1Char('A') && ch <= QLatin1Char('F');
        if (!digit && !lower && !upper)
            return QString();
    }
    return text.toLower();
}

inline AndroidUpdateChannel selectAndroidUpdateChannel(const QString &installedCertificate,
                                                       const QString &releaseCertificate)
{
    const QString installed = normalizedCertificateSha256(installedCertificate);
    if (installed == QStringLiteral("fdfbc10b047a6d94bac5c73dba220e075d4a7ef8807473c3d315d76c73850c4a"))
        return AndroidUpdateChannel::Legacy;
    const QString release = normalizedCertificateSha256(releaseCertificate);
    if (!release.isEmpty() && installed == release)
        return AndroidUpdateChannel::Release;
    return AndroidUpdateChannel::Unknown;
}

inline QString androidUpdateAsset(bool carCompat, AndroidUpdateChannel channel)
{
    if (channel == AndroidUpdateChannel::Legacy) {
        return carCompat ? QStringLiteral("FuruiMusic-carcompat-netease-arm64-v8a.apk")
                         : QStringLiteral("FuruiMusic-android-arm64-v8a.apk");
    }
    return carCompat ? QStringLiteral("FuruiMusic-carcompat-netease-arm64-v8a-signed.apk")
                     : QStringLiteral("FuruiMusic-android-arm64-v8a-signed.apk");
}

inline QString androidUpdateDownloadKey(bool carCompat, AndroidUpdateChannel channel)
{
    if (channel == AndroidUpdateChannel::Legacy)
        return carCompat ? QStringLiteral("android-carcompat")
                         : QStringLiteral("android");
    return carCompat ? QStringLiteral("android-carcompat-release")
                     : QStringLiteral("android-release");
}

} // namespace Furui
