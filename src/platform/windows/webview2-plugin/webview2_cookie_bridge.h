// FuruiMusic: 让主程序从自编译的 QtWebView 后端里读出完整 Cookie（含 HttpOnly）。
#pragma once

#include <webview2.h>

#include <QString>

// 由 qwebview2webview.cpp 在 CookieManager 就绪后调用（内部 AddRef 持有）。
void FuruiRegisterCookieManager(ICoreWebView2CookieManager *manager);

// 同上，注册 controller 与承载它的父窗口，供「页面内真实鼠标点击」使用。
// 普通模式（CreateCoreWebView2Controller）没有 SendMouseInput，只能往 WebView2
// 自己的子窗口投窗口消息来产生真实输入事件。
void FuruiRegisterWebViewController(ICoreWebView2Controller *controller, HWND parent);

// 「登录页白纱」的一次性开关。
//
// 为什么需要它：登录页在页面第一次绘制之后、load 事件之前会把整站外壳
// （导航/轮播/播放器）闪一下再被裁掉。QML 侧只能在 load 之后注入样式，拦不住
// 首帧。WebView2 有 AddScriptToExecuteOnDocumentCreated：在新文档创建时、
// 页面任何脚本之前执行，正好能在首帧之前把白纱盖上。
//
// 用「一次性」而不是「按 URL 匹配」：换平台时登录 WebView 实例会复用，导航
// 发生在新平台前缀生效之前，按前缀匹配的脚本会落空；一次性开关则由
// 「下一个被创建的控制器」消费掉——而下一个控制器必定是登录页那个。
//
// 插件在创建 controller 时取走这个开关并注册脚本；内置浏览器等其它 WebView
// 不会拿到白纱。
void FuruiSetLoginVeilMode(bool on);
bool FuruiTakeLoginVeilMode();
// 给指定 WebView 注册白纱脚本（controller 就绪时调用）
void FuruiRegisterLoginVeilScript(ICoreWebView2 *webview);
