// 福瑞音乐 - 内置浏览器控制
#include "BrowserController.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QMap>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <cstdio>
#include <string>

#if defined(Q_OS_ANDROID) && QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
// QJniObject 是 Qt 6 的类（Qt 5 叫 QtAndroidExtras 的 QAndroidJniObject，头文件不同）；
// 本工程的 Qt 5 Android qmake 链路也要编这个文件，必须加版本守卫，
// 否则 Qt5 Android 构建在 include 处直接 fatal error（与 UpdateChecker.cpp 同款写法）。
#include <QJniObject>
#endif

namespace Furui {
namespace {

// 用户可能直接粘 "music.163.com"，补上 https 并保证没有尾部空白。
QString normalizeUrl(const QString &raw)
{
    QString url = raw.trimmed();
    if (url.isEmpty())
        return url;
    if (!url.contains(QLatin1Char(':')))
        url.prepend(QStringLiteral("https://"));
    return QUrl(url).toString();
}

QString windowsCookies(const QString &url)
{
#if defined(Q_OS_WIN) && defined(FURUI_HAVE_WEBVIEW)
    const bool trace = !qEnvironmentVariableIsEmpty("FURUI_BROWSER_TRACE");
    // 自编译后端与 Qt 自带的同名插件二选一；Qt 加载插件时该 DLL 已在进程内，
    // QLibrary::resolve 拿到的是同一个 HMODULE，不会重复加载。
    const QString plugin = QCoreApplication::applicationDirPath()
            + QStringLiteral("/plugins/webview/qtwebview_webview2.dll");
    if (!QFileInfo::exists(plugin)) {
        if (trace)
            std::fprintf(stderr, "[browser] plugin missing: %s\n", qPrintable(plugin));
        return QString();
    }

    QLibrary lib(plugin);
    if (!lib.load()) {
        if (trace)
            std::fprintf(stderr, "[browser] plugin load failed: %s\n",
                         qPrintable(lib.errorString()));
        return QString();
    }

    using CookieFn = int (*)(const wchar_t *, wchar_t *, int);
    auto fn = reinterpret_cast<CookieFn>(lib.resolve("furui_webview2_cookies"));
    if (!fn) {
        if (trace)
            std::fprintf(stderr, "[browser] cookie bridge symbol not found\n");
        return QString();
    }

    QVector<wchar_t> buffer(32768);
    const QString target = normalizeUrl(url);
    const int written = fn(reinterpret_cast<const wchar_t *>(target.utf16()),
                           buffer.data(), buffer.size());
    if (trace)
        std::fprintf(stderr, "[browser] cookies(%s) -> %d chars\n",
                     qPrintable(target), written);
    if (written <= 0)
        return QString();
    return QString::fromWCharArray(buffer.constData(), written);
#else
    Q_UNUSED(url)
    return QString();
#endif
}

#if defined(Q_OS_ANDROID) && defined(FURUI_HAVE_WEBVIEW)
// QtWebView 的 Android 后端用的是系统 WebView，Cookie 存在全局
// android.webkit.CookieManager 里（含 HttpOnly），JNI 直接读即可。
QString androidCookies(const QString &url)
{
    const QJniObject manager = QJniObject::callStaticObjectMethod(
            "android/webkit/CookieManager", "getInstance",
            "()Landroid/webkit/CookieManager;");
    if (!manager.isValid())
        return QString();
    const QJniObject jUrl = QJniObject::fromString(normalizeUrl(url));
    const QJniObject cookies = manager.callObjectMethod(
            "getCookie", "(Ljava/lang/String;)Ljava/lang/String;",
            jUrl.object<jstring>());
    return cookies.isValid() ? cookies.toString() : QString();
}
#endif

} // namespace

namespace {

// 各平台官网的「真实登录页」。
// url：用户点开就能看到登录表单（或首页 + 自动点开的登录框）的地址；
// cookies：登录成功后才会出现的关键 Cookie，命中任意一个即认定已登录；
// css：注入样式，把站点头部/页脚/侧栏收掉，页面上剩下的就是登录区；
// js：没有独立登录页的平台用它在载入后把登录框点开（只点最内层且文字是「登录」的元素，
//     避免误点页面上的其它控件）。
struct WebLoginSpec {
    const char *url;
    const char *cookies;
    const char *css;
    const char *js;
};

// 没有独立登录页的平台（酷我/酷狗/QQ/汽水）靠脚本把首页的登录框点开。
//
// 三个要点，都是实机踩出来的：
// ① 别点「像登录」的任意链接：酷我页脚有 ICP 备案的 loginPortal.action，早期版本
//    先匹配 a[href*=login] 就点它，结果一直点不开登录框（还会跳出站）。所以候选
//    必须排除备案/协议类链接。
// ② 每个平台真正的入口各不一样（酷我是 .reg_text 里的「登录 / 注册」两个 span，
//    酷狗是 header 的 ._login，汽水是右上角按钮），所以先用「专用选择器」，再来
//    文字兜底；文字比较去掉所有空白，"登录 / 注册" 才能和 "登录/注册" 对得上。
// ③ 一次点不中就得换下一个：脚本自己记轮次（window.__furuiPick.i），每次只取一个
//    候选，几轮轮询下来自然把候选表走一遍，不会死磕同一个点不开的元素。
const char *kLoginCandidatesJs = R"JS(
  var doc=document;
  function box(e){return e.getBoundingClientRect();}
  function vis(e){
    var r=box(e);
    if(r.width<6||r.height<6)return false;
    var vw=window.innerWidth,vh=window.innerHeight;
    // 必须落在视口内：早期只看 right>0/bottom>0，右侧溢出到屏外的入口也算"可见"，
    // 派发的点击坐标落在视口外，等于没点
    if(r.right<0||r.bottom<0||r.left>vw||r.top>vh)return false;
    var st=doc.defaultView.getComputedStyle(e);
    if(st.visibility==='hidden'||st.display==='none')return false;
    if(parseFloat(st.opacity||'1')===0)return false;
    return true;
  }
  function small(e){var r=box(e);return r.width<=260&&r.height<=72;}
  function text(e){return (e.textContent||'').replace(/\s+/g,'');}
  // 登录入口的常见文案（比较前已去掉空白，"登录 / 注册" 也能对上 "登录/注册"）
  var wantWords=['\u767b\u5f55','\u7acb\u5373\u767b\u5f55','\u767b\u5f55/\u6ce8\u518c',
                 '\u767b\u5f55\u6ce8\u518c','\u5e10\u53f7\u767b\u5f55','\u8d26\u53f7\u767b\u5f55'];
  function bad(e){
    var h=(e.getAttribute&&e.getAttribute('href'))||'';
    return /beian|miit|loginportal|agreement|privacy|protocol|help|faq/i.test(h);
  }
  // Vue 这类框架常常把 click 绑在「登录」这两个字的 span 上而不是外层容器
  // （酷我就是 <div class="reg_text"><span>登录</span><span>/</span><span>注册</span></div>），
  // 点容器中心可能落在「/」上、监听器收不到，所以优先取最内层的文字节点。
  function leafOf(el){
    var subs=el.querySelectorAll('span,a,button,i,p,label,em'),i,c;
    for(i=0;i<subs.length;i++){
      c=subs[i];
      if(wantWords.indexOf(text(c))<0)continue;
      if(!vis(c)||!small(c))continue;
      return c;
    }
    for(i=0;i<subs.length;i++){
      c=subs[i];
      if(vis(c)&&small(c)&&text(c))return c;
    }
    return el;
  }
  var cands=[];
  function add(sels){
    for(var i=0;i<sels.length;i++){
      var list=doc.querySelectorAll(sels[i]);
      for(var j=0;j<list.length;j++){
        var e=list[j];
        if(!vis(e)||!small(e)||bad(e))continue;
        cands.push([sels[i],e]);
        break;
      }
    }
  }
  // 专用入口优先（酷我 .reg_text / 酷狗 ._login / 各家 header 里的登录按钮）
  add(['.reg_text','#loginBtn','[class*=loginBtn]','[class*=login-btn]','[class*=_login]',
       '[class*=header] [class*=login]','[class*=top] [class*=login]',
       '[class*=user] [class*=login]','[class*=nav] [class*=login]']);
  // 文字兜底
  if(cands.length===0){
    var nodes=doc.querySelectorAll('a,button,span,div,p,li');
    for(var k=0;k<nodes.length;k++){
      var n=nodes[k],t=text(n);
      if(wantWords.indexOf(t)<0)continue;
      if(!vis(n)||!small(n)||bad(n))continue;
      cands.push(['text:'+t,n]);
    }
  }
)JS";

// 取「下一个登录入口」的坐标：返回 "cx,cy,vw"（CSS 像素，相对视口），
// 没有候选时返回 "none"。QML 拿到坐标后交给 WebView2 发真实鼠标点击
// （酷我这类的站点只认真实输入事件，JS 合成的点不开登录框）。
const std::string &loginTargetScript()
{
    static const std::string js = std::string("(function(){") + kLoginCandidatesJs + R"JS(
  // 登录框已经开着（密码框可见 / 大尺寸登录层）就别再点入口：再点一次可能把
  // 弹框关掉，或者点到面板内部的页签上。
  var pw=doc.querySelectorAll('input[type=password]');
  for(var i=0;i<pw.length;i++)if(vis(pw[i]))return 'open';
  var lg=doc.querySelectorAll('[class*=reg_login],[class*=login-dialog],[class*=loginDialog],'
                            +'[class*=login-layer],[class*=loginLayer],[class*=login-pop],'
                            +'[class*=loginPop],[class*=login_box],[class*=loginBox]');
  for(i=0;i<lg.length;i++){
    if(!vis(lg[i]))continue;
    var lr=box(lg[i]);
    if(lr.width>=240&&lr.height>=200)return 'open';
  }
  if(cands.length===0)return 'none';
  var W=window.__furuiPick||(window.__furuiPick={i:0});
  var e=leafOf(cands[W.i%cands.length][1]);
  W.i++;
  try{e.scrollIntoView({block:'center',inline:'center'});}catch(x){}
  var r=box(e);
  if(r.width<4||r.height<4)return 'none';
  return (r.left+r.width/2)+','+(r.top+r.height/2)+','+window.innerWidth;
})())JS";
    return js;
}

// 合成点击（兜底）：没有真实点击能力的平台（Android / 非 WebView2 后端）用这条。
const std::string &syntheticClickScript()
{
    static const std::string js = std::string("(function(){") + kLoginCandidatesJs + R"JS(
  function fire(el){
    var r=box(el);
    var x=r.left+r.width/2,y=r.top+r.height/2;
    var down={bubbles:true,cancelable:true,view:window,clientX:x,clientY:y,
              screenX:x,screenY:y,button:0,buttons:1};
    var up={bubbles:true,cancelable:true,view:window,clientX:x,clientY:y,
            screenX:x,screenY:y,button:0,buttons:0};
    // Vue/React 18 之后不少入口监听 pointerdown，光派发 mouse 事件点不开
    try{el.dispatchEvent(new PointerEvent('pointerdown',down));}catch(e1){}
    try{el.dispatchEvent(new MouseEvent('mousedown',down));}catch(e2){}
    try{el.dispatchEvent(new PointerEvent('pointerup',up));}catch(e3){}
    try{el.dispatchEvent(new MouseEvent('mouseup',up));}catch(e4){}
    try{el.dispatchEvent(new MouseEvent('click',up));}catch(e5){}
    try{el.click();}catch(e6){}
  }
  if(cands.length===0)return 'no-candidate';
  var W=window.__furuiPick||(window.__furuiPick={i:0});
  var pick=cands[W.i%cands.length];
  W.i++;
  var target=leafOf(pick[1]);
  try{target.scrollIntoView({block:'center',inline:'center'});}catch(x0){}
  fire(target);
  // 登录框是否已经出来了：密码框 / 二维码 / 大尺寸登录层，命中任一即算
  var i,r,e,pw=doc.querySelectorAll('input[type=password]');
  for(i=0;i<pw.length;i++)if(vis(pw[i]))return pick[0]+'|open=pwd';
  var qr=doc.querySelectorAll('[class*=qrcode],[class*=qr-code],[class*=QRCode],[class*=qr_code],canvas');
  for(i=0;i<qr.length;i++)if(vis(qr[i])&&box(qr[i]).width>=80)return pick[0]+'|open=qr';
  var lg=doc.querySelectorAll('[class*=reg_login],[class*=login-dialog],[class*=loginDialog],'
                            +'[class*=login-layer],[class*=loginLayer],[class*=login-pop],'
                            +'[class*=loginPop],[class*=login_box],[class*=loginBox]');
  for(i=0;i<lg.length;i++){
    e=lg[i];if(!vis(e))continue;
    r=box(e);if(r.width>=240&&r.height>=200)return pick[0]+'|open=layer';
  }
  return pick[0]+'|open=-';
})())JS";
    return js;
}

const char *kClickLoginJs = syntheticClickScript().c_str();

const WebLoginSpec *webLoginSpec(const QString &sourceId)
{
    static const WebLoginSpec netease = {
        "https://music.163.com/#/login",
        "MUSIC_U",
        R"CSS(html,body{background:#fff!important}
.m-top,.m-footer,.g-topbar,.g-footer,#g-topbar,#g-footer,.m-subnav,.n-navbar,.m-sch{display:none!important}
.u-main{padding-top:0!important})CSS",
        // 「#/login」这个路由本身就把登录框弹出来了，不需要再点一次登录入口
        ""
    };
    // QQ 音乐的 /n/ryqq/login 会直接跳回首页，登录框是从首页右上角「登录」弹出的，
    // 所以这里落首页 + 点开登录框；header 里就是登录入口，别藏。
    static const WebLoginSpec qq = {
        "https://y.qq.com/",
        "qm_keyst;qqmusic_key;music_key",
        R"CSS(html,body{background:#fff!important}
.mod_footer,.yqq_footer,.player_bar{display:none!important})CSS",
        kClickLoginJs
    };
    // 酷狗没有独立登录页：首页右上角的「登录」点开就是官网登录框（账号/短信/扫码三合一）。
    // 注意别把 .cmhead1 藏掉——登录入口就在那条 header 里。
    static const WebLoginSpec kugou = {
        "https://www.kugou.com/",
        "KuGoo;token",
        R"CSS(html,body{background:#fff!important}
.cmfoot1,.cmfoot,.footer{display:none!important})CSS",
        kClickLoginJs
    };
    // 酷我 /login 会跳回首页，登录框同样由首页 header 的「登录/注册」弹出
    static const WebLoginSpec kuwo = {
        "https://www.kuwo.cn/",
        "kw_token;userid;websid",
        R"CSS(html,body{background:#fff!important}
.kuwo-footer,.footer{display:none!important})CSS",
        kClickLoginJs
    };
    static const WebLoginSpec migu = {
        "https://passport.migu.cn/login?sourceid=208003&apptype=0",
        "SESSION;PUID;USERID;token",
        R"CSS(html,body{background:#fff!important}
.header,.footer,.navbar,.m-top{display:none!important})CSS",
        ""
    };
    // 汽水（抖音音乐）登录框是从首页右上角弹出来的；header 里有登录入口，别整体藏掉
    static const WebLoginSpec qishui = {
        "https://music.douyin.com/",
        "sessionid;sessionid_ss;sid_tt",
        R"CSS(html,body{background:#fff!important}
.semi-navigation-footer,footer{display:none!important})CSS",
        kClickLoginJs
    };

    if (sourceId == QLatin1String("netease")) return &netease;
    if (sourceId == QLatin1String("qq"))      return &qq;
    if (sourceId == QLatin1String("kugou"))   return &kugou;
    if (sourceId == QLatin1String("kuwo"))    return &kuwo;
    if (sourceId == QLatin1String("migu"))    return &migu;
    if (sourceId == QLatin1String("qishui"))  return &qishui;
    return nullptr;
}

} // namespace

BrowserController *BrowserController::instance()
{
    static BrowserController *s_instance = new BrowserController();
    return s_instance;
}

BrowserController::BrowserController(QObject *parent)
    : QObject(parent)
{
}

bool BrowserController::available() const
{
    if (!m_availableProbed) {
        m_available = detectAvailable();
        m_availableProbed = true;
    }
    return m_available;
}

bool BrowserController::trace() const
{
    return !qEnvironmentVariableIsEmpty("FURUI_BROWSER_TRACE");
}

bool BrowserController::detectAvailable() const
{
#ifndef FURUI_HAVE_WEBVIEW
    return false; // Qt 5 兼容构建没有 QtWebView 模块
#else
#  if defined(Q_OS_ANDROID)
    return true;  // Qt 6 Android 自带 webview_android_plugin
#  elif defined(Q_OS_WIN)
    // 自编译插件随 exe 部署在 plugins/webview/；MSVC 官方套件则装在 Qt 目录。
    // 两者都存在时 Qt 会优先用 addLibraryPath 加进来的那个。
    const QString local = QCoreApplication::applicationDirPath()
            + QStringLiteral("/plugins/webview/qtwebview_webview2.dll");
    const QString qtOfficial = QDir(QCoreApplication::applicationDirPath())
            .absoluteFilePath(QStringLiteral("../plugins/webview/qtwebview_webview2.dll"));
    return QFileInfo::exists(local) || QFileInfo::exists(qtOfficial);
#  else
    // macOS/iOS 用 WKWebView，Linux 需要 WebEngine 后端；本工程暂未接入。
    return false;
#  endif
#endif
}

void BrowserController::open(const QString &url, const QString &mode)
{
    m_url = normalizeUrl(url);
    m_mode = mode.isEmpty() ? QStringLiteral("browse") : mode;
    m_opened = true;
    emit urlChanged();
    emit openedChanged();
}

void BrowserController::navigate(const QString &url)
{
    m_url = normalizeUrl(url);
    emit urlChanged();
}

void BrowserController::close()
{
    if (!m_opened)
        return;
    m_opened = false;
    emit openedChanged();
}

QString BrowserController::cookiesForUrl(const QString &url)
{
#if defined(Q_OS_ANDROID) && defined(FURUI_HAVE_WEBVIEW)
    return androidCookies(url);
#elif defined(Q_OS_WIN)
    return windowsCookies(url);
#else
    Q_UNUSED(url)
    return QString();
#endif
}

bool BrowserController::realClick(qreal x, qreal y, qreal viewportWidth)
{
#if defined(Q_OS_WIN) && defined(FURUI_HAVE_WEBVIEW)
    // 与读 Cookie 同一条路：插件已随 WebView 加载，QLibrary 解析到的是同一个
    // HMODULE，不会重复初始化。
    const QString plugin = QCoreApplication::applicationDirPath()
            + QStringLiteral("/plugins/webview/qtwebview_webview2.dll");
    if (!QFileInfo::exists(plugin))
        return false;
    QLibrary lib(plugin);
    if (!lib.load())
        return false;
    using ClickFn = int (*)(double, double, double);
    auto fn = reinterpret_cast<ClickFn>(lib.resolve("furui_webview2_click"));
    if (!fn)
        return false;
    const int rc = fn(x, y, viewportWidth);
    if (trace())
        std::fprintf(stderr, "[browser] realClick(%.0f,%.0f,vw=%.0f) -> %d\n",
                     x, y, viewportWidth, rc);
    return rc == 0;
#else
    Q_UNUSED(x)
    Q_UNUSED(y)
    Q_UNUSED(viewportWidth)
    return false;
#endif
}

QString BrowserController::loginUrl(const QString &sourceId) const
{
    const WebLoginSpec *spec = webLoginSpec(sourceId);
    return spec ? QString::fromLatin1(spec->url) : QString();
}

QString BrowserController::loginCookieNames(const QString &sourceId) const
{
    const WebLoginSpec *spec = webLoginSpec(sourceId);
    return spec ? QString::fromLatin1(spec->cookies) : QString();
}

QString BrowserController::loginCss(const QString &sourceId) const
{
    const WebLoginSpec *spec = webLoginSpec(sourceId);
    return spec ? QString::fromUtf8(spec->css) : QString();
}

QString BrowserController::loginTargetJs() const
{
    return QString::fromLatin1(loginTargetScript().c_str());
}

QString BrowserController::loginJs(const QString &sourceId) const
{
    const WebLoginSpec *spec = webLoginSpec(sourceId);
    return spec ? QString::fromLatin1(spec->js) : QString();
}

bool BrowserController::looksLoggedIn(const QString &sourceId, const QString &cookie) const
{
    const QString names = loginCookieNames(sourceId);
    if (names.isEmpty() || cookie.isEmpty())
        return false;

    // Cookie 串拆成 name=value。MUSIC_U / qm_keyst 这类都是 HttpOnly，
    // document.cookie 读不到，只有原生 CookieManager 能给出完整串。
    QMap<QString, QString> jar;
    const QStringList pairs = cookie.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &pair : pairs) {
        const int eq = pair.indexOf(QLatin1Char('='));
        if (eq <= 0) continue;
        const QString name = pair.left(eq).trimmed();
        const QString value = pair.mid(eq + 1).trimmed();
        if (!name.isEmpty() && !value.isEmpty())
            jar.insert(name, value);
    }

    const QStringList wanted = names.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &name : wanted) {
        if (jar.contains(name.trimmed()))
            return true;
    }
    return false;
}

} // namespace Furui
