// 福瑞音乐 - 内置浏览器控制（QML WebView 统一外壳的平台侧支撑）
#pragma once

#include <QObject>
#include <QString>

namespace Furui {

// QML 侧是同一个 BrowserWindow.qml + `import QtWebView`，本类只负责：
//   1) 记录打开/关闭状态、初始 URL、模式（普通浏览 / 登录抓 Cookie）；
//   2) 提供 available，让没有 QtWebView 后端的 Qt5 构建自动隐藏入口；
//   3) 把平台原生的「读全部 Cookie（含 HttpOnly）」能力暴露给 QML。
//
// HttpOnly 是这里的关键：网易云 MUSIC_U 用 document.cookie 读不到，
// QtWebView 公开 API 也只有 setCookie/deleteCookie。Windows 走自编译
// WebView2 后端导出的 ICoreWebView2CookieManager，Android 走
// android.webkit.CookieManager，两边都能拿到完整 Cookie。
class BrowserController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool trace READ trace CONSTANT)
    Q_PROPERTY(bool opened READ opened NOTIFY openedChanged)
    Q_PROPERTY(QString url READ url NOTIFY urlChanged)
    Q_PROPERTY(QString mode READ mode NOTIFY openedChanged)

public:
    static BrowserController *instance();

    bool available() const;
    // FURUI_BROWSER_TRACE=1：官网登录页的「点入口 / 裁剪」诊断输出，
    // QML 侧用它决定要不要把每轮结果打到日志（正常使用完全静默）。
    bool trace() const;
    bool opened() const { return m_opened; }
    QString url() const { return m_url; }
    QString mode() const { return m_mode; }

    // mode: "browse" 普通浏览；"login:<sourceId>" 登录并自动抓取 Cookie
    Q_INVOKABLE void open(const QString &url, const QString &mode = QStringLiteral("browse"));
    Q_INVOKABLE void close();
    Q_INVOKABLE void navigate(const QString &url);

    // 同步返回该 URL 可用的整串 Cookie（"name=value; name2=value2"）；
    // 平台不支持或没有 Cookie 时返回空串。调用频率由 QML 定时器控制。
    Q_INVOKABLE QString cookiesForUrl(const QString &url);

    // 在网页里发一次「真实」左键点击（CSS 像素坐标，相对视口）。
    //
    // 合成事件（dispatchEvent / element.click()）对有些站点的登录入口完全无效：
    // 酷我首页的「登录 / 注册」能收到 doc 级 click，却不开弹框，也没有 JS 错误。
    // Windows 下往 WebView2 的渲染子窗口投鼠标消息，事件的 isTrusted=true，
    // 对站点来说和用户手点没有区别。viewportWidth 传页面的 window.innerWidth，
    // 插件用它把 CSS 坐标换算成客户区像素（WebView2 的渲染缩放不等于 DPI）。
    Q_INVOKABLE bool realClick(qreal x, qreal y, qreal viewportWidth);

    // ---- 各平台「真实网页登录」 ----
    // 账号密码在本地表单里提交，服务端看到的是一台陌生设备，网易云直接回 8821 行为
    // 验证码、酷狗/咪咕也要人机校验——这些校验只有真正的官网页面才过得去。所以登录页
    // 不再自己画表单，而是把平台官网的登录页嵌进来，登录态从平台原生 CookieManager 读回。
    //
    // 登录页地址：优先用该平台独立的登录页；没有独立页的（酷狗/汽水是首页弹登录框）
    // 用入口页 + loginJs 主动把登录框点开。
    Q_INVOKABLE QString loginUrl(const QString &sourceId) const;
    // 登录成功后才会出现的关键 Cookie（分号分隔，命中任意一个即认定已登录）
    Q_INVOKABLE QString loginCookieNames(const QString &sourceId) const;
    // 注入到官网页面里的样式：把站点头部/页脚/侧栏收掉，页面上只剩登录表单
    Q_INVOKABLE QString loginCss(const QString &sourceId) const;
    // 载入后执行的脚本：没有独立登录页的平台靠它点开登录框
    Q_INVOKABLE QString loginJs(const QString &sourceId) const;
    // 取「下一个登录入口」的中心坐标，返回 "cx,cy,vw"（CSS 像素，相对视口），
    // 没有候选返回 "none"。配合 realClick 使用；合成点击（loginJs）作兜底。
    Q_INVOKABLE QString loginTargetJs() const;
    // Cookie 里是否已出现该平台的登录态标记（HttpOnly 也只能从原生 CookieManager 读到，
    // 所以 document.cookie 那条路在这里不成立）
    Q_INVOKABLE bool looksLoggedIn(const QString &sourceId, const QString &cookie) const;

signals:
    void openedChanged();
    void urlChanged();

private:
    explicit BrowserController(QObject *parent = nullptr);
    bool detectAvailable() const;

    bool m_opened = false;
    QString m_url;
    QString m_mode = QStringLiteral("browse");
    mutable bool m_availableProbed = false;
    mutable bool m_available = false;
};

} // namespace Furui
