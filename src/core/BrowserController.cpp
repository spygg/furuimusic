// 福瑞音乐 - 内置浏览器控制
#include "BrowserController.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QUrl>
#include <QVector>
#include <cstdio>

#if defined(Q_OS_ANDROID)
#include <QJniObject>
#endif

namespace Furui {
namespace {

// 用户可能直接粘 "music.163.com"，补上 https 并保证没有尾部空白。
QString normalizeUrl(const QString &raw)
{
    QString url = raw.trimmed();
    if (url.isEmpty())
        return url;
    if (!url.contains(QLatin1Char(':')))
        url.prepend(QStringLiteral("https://"));
    return QUrl(url).toString();
}

QString windowsCookies(const QString &url)
{
#if defined(Q_OS_WIN) && defined(FURUI_HAVE_WEBVIEW)
    const bool trace = !qEnvironmentVariableIsEmpty("FURUI_BROWSER_TRACE");
    // 自编译后端与 Qt 自带的同名插件二选一；Qt 加载插件时该 DLL 已在进程内，
    // QLibrary::resolve 拿到的是同一个 HMODULE，不会重复加载。
    const QString plugin = QCoreApplication::applicationDirPath()
            + QStringLiteral("/plugins/webview/qtwebview_webview2.dll");
    if (!QFileInfo::exists(plugin)) {
        if (trace)
            std::fprintf(stderr, "[browser] plugin missing: %s\n", qPrintable(plugin));
        return QString();
    }

    QLibrary lib(plugin);
    if (!lib.load()) {
        if (trace)
            std::fprintf(stderr, "[browser] plugin load failed: %s\n",
                         qPrintable(lib.errorString()));
        return QString();
    }

    using CookieFn = int (*)(const wchar_t *, wchar_t *, int);
    auto fn = reinterpret_cast<CookieFn>(lib.resolve("furui_webview2_cookies"));
    if (!fn) {
        if (trace)
            std::fprintf(stderr, "[browser] cookie bridge symbol not found\n");
        return QString();
    }

    QVector<wchar_t> buffer(32768);
    const QString target = normalizeUrl(url);
    const int written = fn(reinterpret_cast<const wchar_t *>(target.utf16()),
                           buffer.data(), buffer.size());
    if (trace)
        std::fprintf(stderr, "[browser] cookies(%s) -> %d chars\n",
                     qPrintable(target), written);
    if (written <= 0)
        return QString();
    return QString::fromWCharArray(buffer.constData(), written);
#else
    Q_UNUSED(url)
    return QString();
#endif
}

#if defined(Q_OS_ANDROID) && defined(FURUI_HAVE_WEBVIEW)
// QtWebView 的 Android 后端用的是系统 WebView，Cookie 存在全局
// android.webkit.CookieManager 里（含 HttpOnly），JNI 直接读即可。
QString androidCookies(const QString &url)
{
    const QJniObject manager = QJniObject::callStaticObjectMethod(
            "android/webkit/CookieManager", "getInstance",
            "()Landroid/webkit/CookieManager;");
    if (!manager.isValid())
        return QString();
    const QJniObject jUrl = QJniObject::fromString(normalizeUrl(url));
    const QJniObject cookies = manager.callObjectMethod(
            "getCookie", "(Ljava/lang/String;)Ljava/lang/String;",
            jUrl.object<jstring>());
    return cookies.isValid() ? cookies.toString() : QString();
}
#endif

} // namespace

BrowserController *BrowserController::instance()
{
    static BrowserController *s_instance = new BrowserController();
    return s_instance;
}

BrowserController::BrowserController(QObject *parent)
    : QObject(parent)
{
}

bool BrowserController::available() const
{
    if (!m_availableProbed) {
        m_available = detectAvailable();
        m_availableProbed = true;
    }
    return m_available;
}

bool BrowserController::detectAvailable() const
{
#ifndef FURUI_HAVE_WEBVIEW
    return false; // Qt 5 兼容构建没有 QtWebView 模块
#else
#  if defined(Q_OS_ANDROID)
    return true;  // Qt 6 Android 自带 webview_android_plugin
#  elif defined(Q_OS_WIN)
    // 自编译插件随 exe 部署在 plugins/webview/；MSVC 官方套件则装在 Qt 目录。
    // 两者都存在时 Qt 会优先用 addLibraryPath 加进来的那个。
    const QString local = QCoreApplication::applicationDirPath()
            + QStringLiteral("/plugins/webview/qtwebview_webview2.dll");
    const QString qtOfficial = QDir(QCoreApplication::applicationDirPath())
            .absoluteFilePath(QStringLiteral("../plugins/webview/qtwebview_webview2.dll"));
    return QFileInfo::exists(local) || QFileInfo::exists(qtOfficial);
#  else
    // macOS/iOS 用 WKWebView，Linux 需要 WebEngine 后端；本工程暂未接入。
    return false;
#  endif
#endif
}

void BrowserController::open(const QString &url, const QString &mode)
{
    m_url = normalizeUrl(url);
    m_mode = mode.isEmpty() ? QStringLiteral("browse") : mode;
    m_opened = true;
    emit urlChanged();
    emit openedChanged();
}

void BrowserController::navigate(const QString &url)
{
    m_url = normalizeUrl(url);
    emit urlChanged();
}

void BrowserController::close()
{
    if (!m_opened)
        return;
    m_opened = false;
    emit openedChanged();
}

QString BrowserController::cookiesForUrl(const QString &url)
{
#if defined(Q_OS_ANDROID) && defined(FURUI_HAVE_WEBVIEW)
    return androidCookies(url);
#elif defined(Q_OS_WIN)
    return windowsCookies(url);
#else
    Q_UNUSED(url)
    return QString();
#endif
}

} // namespace Furui
