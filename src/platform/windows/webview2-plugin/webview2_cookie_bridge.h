// FuruiMusic: 让主程序从自编译的 QtWebView 后端里读出完整 Cookie（含 HttpOnly）。
#pragma once

#include <webview2.h>

// 由 qwebview2webview.cpp 在 CookieManager 就绪后调用（内部 AddRef 持有）。
void FuruiRegisterCookieManager(ICoreWebView2CookieManager *manager);
