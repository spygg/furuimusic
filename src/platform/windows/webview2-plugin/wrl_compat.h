// FuruiMusic: MinGW 兼容层，补齐 mingw-w64 的 WRL 里缺失的 Microsoft::WRL::Callback<>。
//
// Qt 官方只在 MSVC 下启用 QtWebView 的 WebView2 后端（Qt 6.11.1 mingw_64 的
// qtwebview-config.h 里 webview_webview2_plugin = -1）。本工程保持 MinGW 工具链，
// 用 MinGW 自行编译同一个插件源码，因此需要自己把 lambda 包成 COM 回调对象。
//
// 说明：QueryInterface 对 IUnknown 与本回调接口自身返回自身指针；对 IAgileObject
// 显式返回 E_NOINTERFACE（避免被误判 free-threaded 后在非 UI 线程 Invoke），
// 其余 IID 同样返回自身指针。WebView2 只会对回调对象查询这些接口并调用 Invoke，
// 引用计数为原子操作，允许跨线程 AddRef/Release。

#ifndef FURUI_WRL_COMPAT_H
#define FURUI_WRL_COMPAT_H

#include <wrl.h>
#include <wrl/client.h>
#include <webview2.h>

#include <atomic>
#include <functional>
#include <utility>

#include <QtCore/QtGlobal>

// Q_ASSERT_SUCCEEDED 在 Release（QT_NO_DEBUG）下展开为空。它只适合「对象已验证
// 有效、调用不可能失败」的场景；WebView2 接口的可用性取决于用户机器的 Runtime
// 版本（如 ICoreWebView2_22 需要很新的 Runtime），QueryInterface 失败是常态而非
// 断言错误——这类调用点必须改用下面的宏，Release 下同样生效并降级。
#ifndef Q_ASSERT_SUCCEEDED
#define Q_ASSERT_SUCCEEDED(hr) Q_ASSERT_X(SUCCEEDED(hr), Q_FUNC_INFO, "HRESULT failed");
#endif
// 用于「失败必须处理」的调用点：直接 return 错误码（只能在返回 HRESULT 的
// 函数 / 回调里用）。
#define FURUI_CHECK_HR(hr) do { if (FAILED(hr)) return hr; } while (0)

namespace Microsoft {
namespace WRL {

template <typename I>
class CallbackPtr
{
public:
    CallbackPtr() noexcept : ptr_(nullptr) {}
    explicit CallbackPtr(I *ptr) noexcept : ptr_(ptr) {} // 接管 new 出来的初始引用
    CallbackPtr(const CallbackPtr &other) noexcept : ptr_(other.ptr_)
    {
        if (ptr_)
            ptr_->AddRef();
    }
    CallbackPtr &operator=(const CallbackPtr &other) noexcept
    {
        if (this != &other) {
            if (other.ptr_)
                other.ptr_->AddRef();
            if (ptr_)
                ptr_->Release();
            ptr_ = other.ptr_;
        }
        return *this;
    }
    ~CallbackPtr()
    {
        if (ptr_)
            ptr_->Release();
    }
    I *Get() const noexcept { return ptr_; }
    I *operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    I *ptr_;
};

template <typename I>
struct CallbackTraits;

template <typename I, typename F>
CallbackPtr<I> Callback(F &&fn)
{
    return CallbackTraits<I>::Make(std::forward<F>(fn));
}

#define FURUI_WRL_CALLBACK(IFACE, RET, PARAMS, ARGNAMES)                                     \
    template <> struct CallbackTraits<IFACE> {                                                \
        template <typename F> static CallbackPtr<IFACE> Make(F &&fn)                          \
        {                                                                                      \
            struct Impl final : public IFACE {                                                 \
                using Signature = RET PARAMS;                                                  \
                std::function<Signature> callback;                                             \
                /* WebView2 的事件回调可能从非 UI 线程 AddRef/Release，引用计数必须原子；*/      \
                /* 非原子计数会被并发递减打穿，提前 delete this 或 double-free。 */             \
                std::atomic<ULONG> refs{1};                                                    \
                explicit Impl(std::function<Signature> cb) : callback(std::move(cb)) {}       \
                HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override    \
                {                                                                              \
                    if (!ppv)                                                                  \
                        return E_POINTER;                                                      \
                    /* 拒绝 IAgileObject：返回 S_OK 会让 WebView2 认为本回调可跨线程聚合 */       \
                    /* （free-threaded），从而在非 UI 线程 Invoke STA 语义的对象。 */             \
                    /* IAgileObject 的 IID 固定（COM 全局唯一），这里硬编码比对。 */             \
                    static const IID kAgileObjectIid = {                                       \
                        0x94ea2b94, 0xe9cc, 0x49e0,                                            \
                        {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};                     \
                    if (riid == kAgileObjectIid)                                               \
                        return E_NOINTERFACE;                                                  \
                    if (riid == IID_IUnknown)                                                  \
                        *ppv = static_cast<IUnknown *>(static_cast<IFACE *>(this));            \
                    else                                                                       \
                        *ppv = static_cast<IFACE *>(this);                                     \
                    AddRef();                                                                  \
                    return S_OK;                                                               \
                }                                                                              \
                ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }                   \
                ULONG STDMETHODCALLTYPE Release() override                                     \
                {                                                                              \
                    const ULONG left = --refs;                                                 \
                    if (!left)                                                                 \
                        delete this;                                                           \
                    return left;                                                               \
                }                                                                              \
                RET STDMETHODCALLTYPE Invoke PARAMS override { return callback ARGNAMES; }     \
            };                                                                                 \
            return CallbackPtr<IFACE>(                                                        \
                    new Impl(std::function<RET PARAMS>(std::forward<F>(fn))));                 \
        }                                                                                      \
    };

FURUI_WRL_CALLBACK(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                   HRESULT,
                   (HRESULT result, ICoreWebView2Controller *controller),
                   (result, controller))
FURUI_WRL_CALLBACK(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                   HRESULT,
                   (HRESULT result, ICoreWebView2Environment *environment),
                   (result, environment))
FURUI_WRL_CALLBACK(ICoreWebView2NavigationStartingEventHandler,
                   HRESULT,
                   (ICoreWebView2 *sender, ICoreWebView2NavigationStartingEventArgs *args),
                   (sender, args))
FURUI_WRL_CALLBACK(ICoreWebView2NavigationCompletedEventHandler,
                   HRESULT,
                   (ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args),
                   (sender, args))
FURUI_WRL_CALLBACK(ICoreWebView2WebResourceRequestedEventHandler,
                   HRESULT,
                   (ICoreWebView2 *sender, ICoreWebView2WebResourceRequestedEventArgs *args),
                   (sender, args))
FURUI_WRL_CALLBACK(ICoreWebView2ContentLoadingEventHandler,
                   HRESULT,
                   (ICoreWebView2 *sender, ICoreWebView2ContentLoadingEventArgs *args),
                   (sender, args))
FURUI_WRL_CALLBACK(ICoreWebView2NewWindowRequestedEventHandler,
                   HRESULT,
                   (ICoreWebView2 *sender, ICoreWebView2NewWindowRequestedEventArgs *args),
                   (sender, args))
FURUI_WRL_CALLBACK(ICoreWebView2GetCookiesCompletedHandler,
                   HRESULT,
                   (HRESULT result, ICoreWebView2CookieList *cookies),
                   (result, cookies))
FURUI_WRL_CALLBACK(ICoreWebView2ExecuteScriptCompletedHandler,
                   HRESULT,
                   (HRESULT errorCode, LPCWSTR resultObjectAsJson),
                   (errorCode, resultObjectAsJson))

#undef FURUI_WRL_CALLBACK

} // namespace WRL
} // namespace Microsoft

// MinGW 的 __uuidof 是模板模拟实现，MIDL_INTERFACE 的 __declspec(uuid) 不会
// 自动生成 GUID 特化，所以 IID_PPV_ARGS(&webview2) 那几处会链接报
// "__mingw_uuidof<T>() undefined"。下面三个 IID 取自 WebView2.h 的
// MIDL_INTERFACE("...") 声明（COM 接口 IID 固定，不随 SDK 版本变化）。
__CRT_UUID_DECL(ICoreWebView2_2,
                0x9e8f0cf8, 0xe670, 0x4b5e,
                0xb2, 0xbc, 0x73, 0xe0, 0x61, 0xe3, 0x18, 0x4c)
__CRT_UUID_DECL(ICoreWebView2Settings2,
                0xee9a0f68, 0xf46c, 0x4e32,
                0xac, 0x23, 0xef, 0x8c, 0xac, 0x22, 0x4d, 0x2a)
__CRT_UUID_DECL(ICoreWebView2_22,
                0xdb75dfc7, 0xa857, 0x4632,
                0xa3, 0x98, 0x69, 0x69, 0xdd, 0xe2, 0x6c, 0x0a)

#endif // FURUI_WRL_COMPAT_H
