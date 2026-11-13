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
    Q_PROPERTY(bool opened READ opened NOTIFY openedChanged)
    Q_PROPERTY(QString url READ url NOTIFY urlChanged)
    Q_PROPERTY(QString mode READ mode NOTIFY openedChanged)

public:
    static BrowserController *instance();

    bool available() const;
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
