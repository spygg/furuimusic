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
#include <QDebug>
#include <QEventLoop>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QtGlobal>
#include <QTimer>

#include <windows.h>

#include <cstring>
#include <memory>
#include <mutex>

namespace {

std::mutex g_mutex;
Microsoft::WRL::ComPtr<ICoreWebView2CookieManager> g_manager;

// 页面内「真实鼠标点击」用：部分站点（酷我）对 JS 合成事件完全不响应，
// 只有浏览器输入管线产生的事件（isTrusted=true）才会走它自己的弹框逻辑。
std::mutex g_controllerMutex;
Microsoft::WRL::ComPtr<ICoreWebView2Controller> g_controller;
HWND g_parentHwnd = nullptr;

// 「登录页白纱」的一次性开关（计数器）：见 webview2_cookie_bridge.h 的说明。
// 用计数而不是布尔：换平台时旧 WebView 的控制器可能迟到创建（那时 WebView
// 已经销毁、回调会提前返回，但保险起见按名额计数更稳），布尔会被它吃掉名额，
// 导致新 WebView 拿不到白纱。
std::mutex g_veilMutex;
int g_veilModeCount = 0;

// 白纱脚本：文档创建时（页面任何脚本之前）执行，把整页盖成白底 + 全透明，
// 只剩「登录区」裁剪完成后才掀开。id 与 QML 侧注入的一致，掀纱时都能被移除。
QString buildVeilScript()
{
    return QStringLiteral(
            "(function(){try{"
            // 自检标记：脚本有没有跑、写进 window 供 QML 侧读
            "window.__furuiVeilRan=1;"
            "var css='body{background:#fff!important}"
            "body *{opacity:0!important;transition:none!important;}';"
            // inject 内部自己兜住异常：文档创建那一刻 document.head /
            // document.documentElement 都可能还不存在，若让异常冒出去，
            // 后面注册 DOMContentLoaded 重试的那两行就永远不会执行。
            "function inject(){try{"
            "if(document.getElementById('furui-login-veil'))return;"
            "var host=document.head||document.documentElement;if(!host)return;"
            "var s=document.createElement('style');s.id='furui-login-veil';"
            "s.textContent=css;host.appendChild(s);"
            "}catch(e){window.__furuiVeilErr=String(e);}}"
            "inject();"
            "document.addEventListener('DOMContentLoaded',inject);"
            "window.addEventListener('load',inject);"
            "}catch(e){window.__furuiVeilErr=String(e);}})();");
}

// 给指定 WebView 注册白纱脚本（可重复调用，脚本幂等）
void registerVeilScript(ICoreWebView2 *webview)
{
    if (!webview)
        return;
    auto handler = Microsoft::WRL::Callback<
            ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
            [](HRESULT errorCode, LPCWSTR /*result*/) -> HRESULT {
                if (FAILED(errorCode))
                    qWarning() << "AddScriptToExecuteOnDocumentCreated:"
                               << QString::number(errorCode, 16);
                return S_OK;
            });
    const QString script = buildVeilScript();
    const HRESULT hr = webview->AddScriptToExecuteOnDocumentCreated(
            (wchar_t *)script.utf16(), handler.Get());
    if (FAILED(hr))
        qWarning() << "AddScriptToExecuteOnDocumentCreated failed:"
                   << QString::number(hr, 16);
    else
        qWarning() << "[webview2] veil script registered";
}

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

void FuruiRegisterWebViewController(ICoreWebView2Controller *controller, HWND parent)
{
    std::lock_guard<std::mutex> lock(g_controllerMutex);
    g_controller = controller;
    g_parentHwnd = parent;
}

void FuruiSetLoginVeilMode(bool on)
{
    std::lock_guard<std::mutex> lock(g_veilMutex);
    if (on)
        ++g_veilModeCount;
    else
        g_veilModeCount = 0;
}

bool FuruiTakeLoginVeilMode()
{
    std::lock_guard<std::mutex> lock(g_veilMutex);
    if (g_veilModeCount <= 0)
        return false;
    --g_veilModeCount;   // 一次性：只给下一个创建的控制器
    return true;
}

void FuruiRegisterLoginVeilScript(ICoreWebView2 *webview)
{
    if (!FuruiTakeLoginVeilMode())
        return;
    registerVeilScript(webview);
}

namespace {

struct FindChildCtx {
    HWND render = nullptr;   // Chrome_RenderWidgetHostHWND：真正接收输入的那个
    HWND shell = nullptr;    // Chrome_WidgetWin_0：兜底
};

BOOL CALLBACK findRenderedChild(HWND hwnd, LPARAM lp)
{
    auto *ctx = reinterpret_cast<FindChildCtx *>(lp);
    wchar_t cls[128] = {};
    if (GetClassNameW(hwnd, cls, 128) > 0) {
        if (wcsstr(cls, L"Chrome_RenderWidgetHost")) {
            ctx->render = hwnd;
            return FALSE;
        }
        if (!ctx->shell && wcsstr(cls, L"Chrome_WidgetWin"))
            ctx->shell = hwnd;
    }
    return TRUE;
}

} // namespace

// 在 WebView 内发一次「真实」左键点击（CSS 像素坐标，相对视口）。
//
// 为什么需要它：酷我首页的「登录 / 注册」对 dispatchEvent 合成的
// mousedown/mouseup/click 完全不响应（document 能收到事件，站点逻辑不动作，
// 也没有 JS 报错），只有浏览器自身输入管线产生的事件才会打开登录弹框。
// 普通模式的 ICoreWebView2Controller 没有 SendMouseInput（那是组合模式才有的），
// 所以改成往 WebView2 的子窗口投 WM_LBUTTONDOWN/UP——同样走浏览器输入管线。
extern "C" __declspec(dllexport)
int furui_webview2_click(double cssX, double cssY, double viewportWidth)
{
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
    HWND parent = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_controllerMutex);
        controller = g_controller;
        parent = g_parentHwnd;
    }
    if (!controller || !parent)
        return -1;

    FindChildCtx ctx;
    EnumChildWindows(parent, findRenderedChild, reinterpret_cast<LPARAM>(&ctx));
    // 优先投给渲染窗口；它才是 Chrome 的输入入口，投给外层 WidgetWin 不会
    // 产生页面的 mousedown（实测 md 计数不变）
    HWND target = ctx.render ? ctx.render : (ctx.shell ? ctx.shell : parent);

    // CSS 像素 → 客户区像素的缩放不能按 DPI 猜：WebView2 的渲染缩放由
    // 它自己的 rasterization scale 决定（本机实测 CSS 1055 宽落在 ~700 客户区像素上，
    // 而 ICoreWebView2Controller 没有暴露 get_RasterizationScale）。
    // 直接用「客户区宽 / 页面 innerWidth」反推，任何缩放比例都准。
    double scale = 1.0;
    RECT rc = {};
    if (GetClientRect(target, &rc) && viewportWidth > 1.0 && rc.right > 0)
        scale = static_cast<double>(rc.right) / viewportWidth;

    const LONG px = static_cast<LONG>(cssX * scale + 0.5);
    const LONG py = static_cast<LONG>(cssY * scale + 0.5);

    controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    SetFocus(target);

    // Chrome 在处理鼠标消息时会参考系统光标位置，先把光标挪到自己身上再投消息
    POINT screenPt = {px, py};
    if (ClientToScreen(target, &screenPt))
        SetCursorPos(screenPt.x, screenPt.y);

    const LPARAM pos = MAKELPARAM(px, py);
    SendMessageW(target, WM_MOUSEMOVE, 0, pos);
    SendMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, pos);
    SendMessageW(target, WM_LBUTTONUP, 0, pos);

    if (qEnvironmentVariableIsEmpty("FURUI_BROWSER_TRACE") == false) {
        wchar_t cls[128] = {};
        GetClassNameW(target, cls, 128);
        std::fprintf(stderr,
                     "[webview2] click parent=%p target=%p class=%ls client=%ldx%ld "
                     "vw=%.0f scale=%.3f at=%ld,%ld same=%d\n",
                     static_cast<void *>(parent), static_cast<void *>(target), cls,
                     static_cast<long>(rc.right), static_cast<long>(rc.bottom),
                     viewportWidth, scale, static_cast<long>(px), static_cast<long>(py),
                     target == parent ? 1 : 0);
    }
    return 0;
}

// 打开官网登录页之前调用：让插件在下一个 WebView 的文档创建时就注入白纱，
// 挡住首帧的站点外壳闪现。传 0 关闭（覆盖层关闭时调用）。
extern "C" __declspec(dllexport)
int furui_webview2_set_veil(int on)
{
    FuruiSetLoginVeilMode(on != 0);
    return 0;
}

extern "C" __declspec(dllexport)
int furui_webview2_cookies(const wchar_t *url, wchar_t *out, int outChars){
    if (!url || !out || outChars <= 0)
        return -1;

    Microsoft::WRL::ComPtr<ICoreWebView2CookieManager> manager;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        manager = g_manager;
    }
    if (!manager)
        return -1;

    // GetCookies 的完成回调被 WebView2 持有（AddRef），超时放弃后仍可能在
    // 任意线程迟到触发。回调若按引用捕获本函数栈变量，就是向已释放的栈帧
    // 写入 → use-after-free。因此把可变状态放到堆上，由回调与本函数共同
    // 持有（shared_ptr），用 expired + mutex 串行化，迟到结果直接丢弃。
    struct GetCookiesState {
        std::mutex mutex;
        QString result;
        QPointer<QEventLoop> loop;   // loop 在栈上，可能先于回调销毁
        bool expired = false;
    };
    const auto state = std::make_shared<GetCookiesState>();

    QEventLoop loop;
    state->loop = &loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&state] {
        { std::lock_guard<std::mutex> lock(state->mutex); state->expired = true; }
        if (state->loop)
            QMetaObject::invokeMethod(state->loop.data(), &QEventLoop::quit,
                                      Qt::QueuedConnection);
    });
    timeout.start(4000);

    const HRESULT hr = manager->GetCookies(
            url,
            Microsoft::WRL::Callback<ICoreWebView2GetCookiesCompletedHandler>(
                    [state](HRESULT, ICoreWebView2CookieList *list) -> HRESULT {
                        {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            if (state->expired)
                                return S_OK;   // 超时已放弃：丢弃结果，不碰栈帧
                            state->result = cookiesToString(list);
                        }
                        // WebView2 回调可能不在 Qt 的线程上，排队退出更安全；
                        // loop 已销毁时 QPointer 为空，跳过即可
                        if (state->loop)
                            QMetaObject::invokeMethod(state->loop.data(), &QEventLoop::quit,
                                                      Qt::QueuedConnection);
                        return S_OK;
                    }).Get());
    if (FAILED(hr))
        return -1;

    loop.exec();
    QString result;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        result = state->result;
    }
    if (result.isEmpty())
        return 0;

    const int written = qMin(result.size(), outChars - 1);
    std::memcpy(out, result.utf16(), static_cast<size_t>(written) * sizeof(wchar_t));
    out[written] = L'\0';
    return written;
}
