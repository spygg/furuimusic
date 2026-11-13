// FuruiMusic: MinGW 下避免链接 MSVC 编译的 WebView2LoaderStatic.lib。
//
// 该静态库引用 __security_cookie / _Init_thread_* 等 MSVC CRT 私有符号，
// MinGW 链接不过。这里直接实现 WebView2.h 里声明的 C 入口，
// 运行时从随程序部署的 WebView2Loader.dll 解析真正的实现。

#include <webview2.h>
#include <windows.h>

#include <mutex>

namespace {

using CreateEnvironmentWithOptionsFn = HRESULT(STDAPICALLTYPE *)(
        PCWSTR browserExecutableFolder,
        PCWSTR userDataFolder,
        ICoreWebView2EnvironmentOptions *environmentOptions,
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *environmentCreatedHandler);

CreateEnvironmentWithOptionsFn resolveCreateEnvironment()
{
    static CreateEnvironmentWithOptionsFn fn = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        HMODULE module = LoadLibraryW(L"WebView2Loader.dll");
        if (!module) {
            // LoadLibrary 默认会搜 exe 所在目录，这里再兜一次绝对路径，
            // 防止工作目录/插件目录导致搜索路径变化。
            wchar_t exePath[MAX_PATH] = {};
            const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            if (len > 0 && len < MAX_PATH) {
                wchar_t *slash = wcsrchr(exePath, L'\\');
                if (slash)
                    wcscpy_s(slash + 1, MAX_PATH - static_cast<size_t>(slash + 1 - exePath),
                             L"WebView2Loader.dll");
                module = LoadLibraryW(exePath);
            }
        }
        if (module) {
            fn = reinterpret_cast<CreateEnvironmentWithOptionsFn>(
                    GetProcAddress(module, "CreateCoreWebView2EnvironmentWithOptions"));
        }
    });
    return fn;
}

} // namespace

extern "C" HRESULT STDAPICALLTYPE CreateCoreWebView2EnvironmentWithOptions(
        PCWSTR browserExecutableFolder,
        PCWSTR userDataFolder,
        ICoreWebView2EnvironmentOptions *environmentOptions,
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *environmentCreatedHandler)
{
    const auto fn = resolveCreateEnvironment();
    if (!fn)
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    return fn(browserExecutableFolder, userDataFolder, environmentOptions,
              environmentCreatedHandler);
}
