FuruiMusic 内置浏览器所需的 Microsoft WebView2 SDK（仅头与加载器）
===============================================================

来源：NuGet 包 Microsoft.Web.WebView2 1.0.4314.35
      https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.4314.35
许可证：见同目录 LICENSE.txt（Microsoft 软件许可条款）

内容
----
include/WebView2.h                       MIDL 生成的 COM 接口声明
include/WebView2EnvironmentOptions.h     ICoreWebView2EnvironmentOptions 辅助声明
x64/WebView2Loader.dll                   运行时加载器（191 KB，随 exe 部署）

用途
----
Qt 6.11.1 的官方 MinGW 二进制把 QtWebView 的五个后端插件全部关掉了
（include/QtWebView/qtwebview-config.h 里 webview_webview2_plugin = -1），
而 QtWebEngine 官方从不支持 MinGW。为了在保持 MinGW 工具链的同时让 QML
统一 `import QtWebView`，工程用 MinGW 自行编译 QtWebView 的 WebView2 后端插件，
源码在 `src/platform/windows/webview2-plugin/`（源自 Qt 6.11.1 的
Src/qtwebview/src/plugins/windows，保留原始版权头）。

为什么不链 WebView2LoaderStatic.lib
----------------------------------
该静态库由 MSVC 编译，引用 __security_cookie / _Init_thread_* 等 MSVC CRT
私有符号，MinGW 链接不过。插件改为运行时 LoadLibrary("WebView2Loader.dll")
并 GetProcAddress("CreateCoreWebView2EnvironmentWithOptions")，
见 webview2_loader_compat.cpp。因此这里只需要 DLL，不需要 .lib。

部署
----
`src/cmake/FuruiWebView2.cmake` 在构建期把
  - qtwebview_webview2.dll -> <exe>/plugins/webview/
  - WebView2Loader.dll      -> <exe>/
拷到目标目录；`main.cpp` 用 QCoreApplication::addLibraryPath(<exe>/plugins)
让 Qt 找到插件。终端还需要安装 Edge WebView2 Runtime（Win11 自带，
Win10 可由微软「常青版运行库」安装包补齐），本工程不随包分发 Runtime 本体。

Android 走 Qt 自带的 android WebView 后端，不需要本目录任何文件。
