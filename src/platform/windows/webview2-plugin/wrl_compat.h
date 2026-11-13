// FuruiMusic: MinGW 兼容层，补齐 mingw-w64 的 WRL 里缺失的 Microsoft::WRL::Callback<>。
//
// Qt 官方只在 MSVC 下启用 QtWebView 的 WebView2 后端（Qt 6.11.1 mingw_64 的
// qtwebview-config.h 里 webview_webview2_plugin = -1）。本工程保持 MinGW 工具链，
// 用 MinGW 自行编译同一个插件源码，因此需要自己把 lambda 包成 COM 回调对象。
//
// 说明：QueryInterface 对任何 IID 都返回自身。WebView2 只会对回调对象查询
// IUnknown / 该回调接口本身并调用 Invoke，返回自身指针是安全的，也避免了
// MinGW 下 MIDL_INTERFACE 的 __uuidof 差异。

#ifndef FURUI_WRL_COMPAT_H
#define FURUI_WRL_COMPAT_H

#include <wrl.h>
#include <wrl/client.h>
#include <webview2.h>

#include <functional>
#include <utility>

#include <QtCore/QtGlobal>

#ifndef Q_ASSERT_SUCCEEDED
#define Q_ASSERT_SUCCEEDED(hr) Q_ASSERT_X(SUCCEEDED(hr), Q_FUNC_INFO, "HRESULT failed");
#endif

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
                ULONG refs = 1;                                                                \
                explicit Impl(std::function<Signature> cb) : callback(std::move(cb)) {}       \
                HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **ppv) override         \
                {                                                                              \
                    if (!ppv)                                                                  \
                        return E_POINTER;                                                      \
                    *ppv = static_cast<IFACE *>(this);                                         \
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
