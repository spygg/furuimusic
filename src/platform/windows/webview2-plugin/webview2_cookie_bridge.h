// FuruiMusic: 让主程序从自编译的 QtWebView 后端里读出完整 Cookie（含 HttpOnly）。
#pragma once

#include <webview2.h>

// 由 qwebview2webview.cpp 在 CookieManager 就绪后调用（内部 AddRef 持有）。
void FuruiRegisterCookieManager(ICoreWebView2CookieManager *manager);

// 同上，注册 controller 与承载它的父窗口，供「页面内真实鼠标点击」使用。
// 普通模式（CreateCoreWebView2Controller）没有 SendMouseInput，只能往 WebView2
// 自己的子窗口投窗口消息来产生真实输入事件。
void FuruiRegisterWebViewController(ICoreWebView2Controller *controller, HWND parent);
