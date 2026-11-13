// FuruiMusic: QtWebView 的公开 API 只能 setCookie/deleteCookie，读不到网站写入的
// Cookie；而网易云的登录凭证 MUSIC_U 是 HttpOnly，document.cookie 也拿不到。
// Qt 6.11 的 WebView2 后端内部本来就持有 ICoreWebView2CookieManager，这里把它
// 注册下来，并以纯 C 接口导出一个「取指定 URL 全部 Cookie」的函数，主程序用
// QLibrary::resolve 调用，避免跨 DLL 依赖 Qt 私有 C++ ABI。
//
// SPI 保持最小：int furui_webview2_cookies(const wchar_t *url,
//                                          wchar_t *out, int outChars)
// 返回写入的字符数（不含结尾 0），无 Cookie 返回 0，失败返回 -1。

#include "webview2_cookie_bridge.h"
#include "wrl_compat.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QString>
#include <QTimer>

#include <windows.h>

#include <cstring>
#include <mutex>

namespace {

std::mutex g_mutex;
Microsoft::WRL::ComPtr<ICoreWebView2CookieManager> g_manager;

QString cookiesToString(ICoreWebView2CookieList *list)
{
    QString result;
    if (!list)
        return result;

    UINT count = 0;
    if (FAILED(list->get_Count(&count)))
        return result;

    for (UINT i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<ICoreWebView2Cookie> cookie;
        if (FAILED(list->GetValueAtIndex(i, &cookie)) || !cookie)
            continue;

        wchar_t *namePtr = nullptr;
        wchar_t *valuePtr = nullptr;
        if (FAILED(cookie->get_Name(&namePtr)) || !namePtr)
            continue;
        if (FAILED(cookie->get_Value(&valuePtr)))
            valuePtr = nullptr;

        if (!result.isEmpty())
            result += QStringLiteral("; ");
        result += QString::fromWCharArray(namePtr);
        result += QLatin1Char('=');
        if (valuePtr)
            result += QString::fromWCharArray(valuePtr);

        CoTaskMemFree(namePtr);
        if (valuePtr)
            CoTaskMemFree(valuePtr);
    }
    return result;
}

} // namespace

void FuruiRegisterCookieManager(ICoreWebView2CookieManager *manager)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_manager = manager;
}

extern "C" __declspec(dllexport)
int furui_webview2_cookies(const wchar_t *url, wchar_t *out, int outChars)
{
    if (!url || !out || outChars <= 0)
        return -1;

    Microsoft::WRL::ComPtr<ICoreWebView2CookieManager> manager;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        manager = g_manager;
    }
    if (!manager)
        return -1;

    QString result;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(4000);

    const HRESULT hr = manager->GetCookies(
            url,
            Microsoft::WRL::Callback<ICoreWebView2GetCookiesCompletedHandler>(
                    [&result, &loop](HRESULT, ICoreWebView2CookieList *list) -> HRESULT {
                        result = cookiesToString(list);
                        // WebView2 回调可能不在 Qt 的线程上，排队退出更安全
                        QMetaObject::invokeMethod(&loop, &QEventLoop::quit,
                                                  Qt::QueuedConnection);
                        return S_OK;
                    }).Get());
    if (FAILED(hr))
        return -1;

    loop.exec();
    if (result.isEmpty())
        return 0;

    const int written = qMin(result.size(), outChars - 1);
    std::memcpy(out, result.utf16(), static_cast<size_t>(written) * sizeof(wchar_t));
    out[written] = L'\0';
    return written;
}
