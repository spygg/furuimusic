import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtWebView

// 平台官网的「真实登录页」（内嵌在账号页里，不是自建表单）
//
// 为什么要这么做：自己在本地表单里拼账号密码，服务端看到的是陌生设备，网易云直接回
// 8821 行为验证码，酷狗/咪咕也要人机校验——这些校验只有官网页面本身过得去。所以
// 这里把各平台官网的登录页原样嵌进来，用户在真实页面里登录，登录态由平台原生
// CookieManager 读回（MUSIC_U / qm_keyst 都是 HttpOnly，document.cookie 读不到）。
//
// 只显示「登录」那一块：官网整页有导航/横幅/推荐位，这里用 loginCss 收掉常见的
// 站点外壳，再由 focusJs 找到登录表单所在的 DOM 分支、把其余分支隐藏，页面上
// 剩下的就是登录区。找不到表单时什么都不动（宁可不裁剪，也不能把页面弄坏）。
//
// 原生窗口约束：WebView 是原生子窗口，QML 的 clip 管不住它，所以这里必须被
// Loader 整块创建/销毁（隐藏时要 active=false），不能只置 visible=false。
Item {
    id: root

    property string sourceId: ""
    property string sourceName: ""
    signal closed()
    signal imported()

    readonly property string startUrl: browser.loginUrl(root.sourceId)

    // 桌面 Chrome UA：Android 系统 WebView 默认是移动 UA，QQ/酷我等站点在移动 UA
    // 下返回的是「下载 APP」版首页，根本没有桌面登录入口（.top_login__link 等），
    // 自动点击/裁剪全部失效。给登录页强制桌面 UA，让所有平台都走桌面登录布局，
    // 与 Windows 端保持一致。Windows 端 WebView2 本身就是桌面 UA，无需覆盖。
    readonly property string desktopUa: "Mozilla/5.0 (Windows NT 10.0; Win64; x64) " +
                                        "AppleWebKit/537.36 (KHTML, like Gecko) " +
                                        "Chrome/120.0.0.0 Safari/537.36"

    // 是否 Android：系统 WebView 是移动 UA，需要上面那套「桌面 UA + 裁剪后放大」
    // 的补偿；Windows/桌面端 WebView2 本身就是桌面渲染，无需处理。
    readonly property bool isAndroid: Qt.platform.os === "android"

    // 裁剪样式按 URL 只注入一次（单页应用的 URL 会一路变，重复注入没必要）
    property string trimmedFor: ""
    // captured：已经从 CookieManager 抓到登录态并交给 AuthManager，轮询暂停等结果；
    // 导入失败会把它复位，用户在页面里重新登录后还能再抓一次。
    property bool captured: false
    // 是否已经把页面上除登录区以外的部分收掉
    property bool focused: false
    property int focusTries: 0
    // 登录框默认可能停在「扫码登录」页签。账号页的这栏叫「账号密码登录」，
    // 所以裁剪成功后尝试切一次「账号登录」页签；切到过一次就不再动，
    // 免得用户自己切回扫码又被抢回去。
    property bool accountDone: false
    property int accountTries: 0
    property string status: "正在打开官网登录页…"
    // 「过渡层」：官网整页加载 → 裁剪脚本把导航/播放器等收掉，中间这几秒
    // 原本只能看到一片纯白。WebView 是原生子窗口，QML 遮罩盖不住它，隐藏原生
    // 窗口又有渲染恢复的风险，所以在页面里注入：① body 白底 + 所有子元素
    // opacity:0（布局、懒加载、命中测试全保留，真实点击照常把登录框点开）；
    // ② 一层白色过渡卡片，直接列出该平台可用的登录方式，裁剪成功后淡出，
    // 登录区从白纱下淡入。veilLifted 防止重复注入/重复掀。
    property bool veilLifted: false

    // 横竖屏切换：WebView 会改 page scale / visualViewport，页面里的 resize 监听
    // 会立即重算，但 Android 端裁剪成功后轮询 Timer 已经停了，兜底重算轮不到。
    // 这里主动补跑一轮 focusLogin（不重新加载页面，二维码不会被刷新）。
    property real lastRootW: 0
    property real lastRootH: 0
    onWidthChanged: root.onOrientationChanged()
    onHeightChanged: root.onOrientationChanged()
    function onOrientationChanged() {
        if (!root.isAndroid || root.width <= 0 || root.height <= 0) return
        if (Math.abs(root.width - root.lastRootW) < 2
            && Math.abs(root.height - root.lastRootH) < 2) return
        root.lastRootW = root.width
        root.lastRootH = root.height
        root.focusTries = 0
        Qt.callLater(function () { root.focusLogin() })
    }

    // 找到登录区所在的容器，沿祖先链逐层隐藏兄弟节点，只留登录区这一支。
    // 识别顺序：① class/id 里带 login 且足够大的可见容器（取面积最小的那个，最具体）；
    // ② 密码框 / 手机号框 / 二维码图。检测不到就返回原因、完全不改动页面。
    // 只留「登录区」：定位当前真正可见的登录框，沿祖先链隐藏兄弟分支。
    // 两个安全阀：① 用 elementFromPoint 打在屏幕中线上找「正在显示」的元素，不用纯选择器猜；
    // ② 隐藏后校验登录框仍可见，不可见就原样恢复——绝不能把页面裁成一片空白。
    // 网易云把内容装在同域名 iframe（#g_iframe）里，命中 iframe 就钻进去再裁，
    // 主文档只留这个 iframe 并让它铺满。
    // 裁剪共用函数库：box2/loginish/dialogLike/walkUp/probe/whiten/trim。
    // Windows 的 focusJs 与 Android 的 focusJsAndroid 共用这一段函数定义，
    // 只在「执行尾部」分叉（见下）。注意：字符串以 "(function(){" 开头、
    // 不闭合——两个 focusJs 各自补尾部和 "})()"。
    readonly property string trimFnsJs: "(function(){"
        + "function box2(e){return e.getBoundingClientRect();}"
        // 「可视区」尺寸：Android wide viewport 下 innerWidth 是 982 的布局视口，
        // 真正看得见的只有 visualViewport 那块（竖屏实测 336x584）。登录区必须
        // 按这块居中，否则会整体偏右/被裁掉（网易云竖屏二维码整块在屏幕外）。
        // 返回 [宽, 高, 左偏移, 上偏移]，旋转/软键盘/缩放变了都要重算。
        + "function port(doc){var w=doc.defaultView,vv=w.visualViewport;"
        + "return [vv?vv.width:w.innerWidth,vv?vv.height:w.innerHeight,"
        + "vv?vv.offsetLeft:0,vv?vv.offsetTop:0];}"
        // 把登录区摆到可视区正中并等比缩放。可重复调用：先撤掉上一次 transform
        // 量自然尺寸，再重新算，所以横竖屏切换时每轮重跑就能自适应。
        // translate 在 scale 左侧 => p'=s*p+t，t 直接就是最终屏幕偏移。
        + "function present(doc,box,W,H,ox,oy){"
        + "if(!box||!box.isConnected||!W||!H)return false;"
        + "box.style.removeProperty('transform');"
        + "var r=box.getBoundingClientRect();"
        + "var nw=r.width||300,nh=r.height||300;"
        + "if(nw<40||nh<40)return false;"
        + "var s=Math.min((W*0.94)/nw,(H*0.88)/nh,1.6);"
        + "if(!(s>0.05))s=0.05;"
        + "box.style.setProperty('position','fixed','important');"
        + "box.style.setProperty('left','0','important');"
        + "box.style.setProperty('top','0','important');"
        + "box.style.setProperty('margin','0','important');"
        + "box.style.setProperty('width',nw+'px','important');"
        + "box.style.setProperty('height','auto','important');"
        + "box.style.setProperty('transform-origin','0 0','important');"
        + "box.style.setProperty('transform','translate('+((W-nw*s)/2+ox)+'px,'"
        + "+((H-nh*s)/2+oy)+'px) scale('+s+')','important');"
        + "box.style.setProperty('overflow','visible','important');"
        + "box.style.setProperty('z-index','2147483647','important');"
        + "box.style.setProperty('pointer-events','auto','important');"
        + "var w=doc.defaultView;w.__furuiBox=box;w.__furuiPort=[W,H,ox,oy];"
        // 旋转时不等 1.5s 轮询，立即按最新 port 重摆一次。
        + "if(!w.__furuiPresentSync){w.__furuiPresentSync=1;"
        + "var re=function(){var b=w.__furuiBox;if(!b||!b.isConnected)return;"
        + "var q=null;"
        // iframe 内的文档：可见区要取「父文档」的 visualViewport，绝不能用
        // frameElement.getBoundingClientRect() —— 旋转后 WebView 的 page scale
        // 会变（GSI 实测 1 -> 1.958），iframe 元素 rect 变成 658x242，而 iframe
        // 文档里真正看得见的只有 336x124 CSS px，按 rect 算会把登录框顶到右边。
        + "try{if(w.parent&&w.parent!==w){var pv=w.parent.visualViewport;"
        + "if(pv)q=[pv.width,pv.height,pv.offsetLeft,pv.offsetTop];}}catch(xq){}"
        + "if(!q)q=port(doc);"
        + "present(doc,b,q[0],q[1],q[2],q[3]);};"
        + "w.addEventListener('resize',re);"
        + "if(w.visualViewport)w.visualViewport.addEventListener('resize',re);}"
        + "return true;}"
        // 同源 iframe 里的登录页（网易云 #g_iframe -> /login）：站点按 982 宽
        // 桌面布局，iframe 框却只有 336 宽，header 黑红条、左栏空白全露出来，
        // 二维码反而整个在屏幕外。这里不走「隐藏兄弟节点」（React 页面重绘会
        // 把隐藏状态冲掉），而是盖一层白底 shield + 把登录面板提到最上层，
        // 再用 present() 居中缩放。shield 不吃事件，面板内点击照常可用。
        + "function framePanel(doc){var a=null,cv=doc.querySelector('canvas');"
        + "if(cv&&vis(cv)&&box2(cv).width>=60)a=cv;"
        + "if(!a){var pw=doc.querySelector('input[type=password]');"
        + "if(pw&&vis(pw))a=pw;}"
        + "if(!a){var lgs=doc.querySelectorAll('[id*=login],[class*=login]');"
        + "for(var z=0;z<lgs.length;z++){if(vis(lgs[z])&&box2(lgs[z]).width>=200){"
        + "a=lgs[z];break;}}}"
        + "if(!a)return null;"
        + "var n=a,i=0;"
        + "while(n&&n!==doc.body&&i++<14){var r=box2(n);"
        + "if(r.width>=220&&r.width<=780&&r.height>=120&&r.height<=720)return n;"
        + "n=n.parentElement;}"
        + "return null;}"
        // 只有「确实是登录页」的 iframe 才接管呈现，避免把广告/播放器 iframe
        // 里的 canvas 误当二维码。文字判据覆盖网易云新版 React 登录页。
        + "function loginishDoc(doc){"
        + "if(doc.querySelector('input[type=password]'))return true;"
        + "var t='';try{t=(doc.body.textContent||'').replace(/\\s+/g,'');}catch(xl){}"
        + "if(/\u626b\u7801\u767b\u5f55|\u8d26\u53f7\u767b\u5f55|"
        + "\u624b\u673a\u53f7\u767b\u5f55|\u5bc6\u7801\u767b\u5f55|"
        + "\u767b\u5f55\u83b7\u53d6|\u9a8c\u8bc1\u7801/.test(t))return true;"
        + "var ls=doc.querySelectorAll('[id*=login],[class*=login]');"
        + "for(var z2=0;z2<ls.length;z2++){var lr=box2(ls[z2]);"
        + "if(lr.width>=200&&lr.height>=120)return true;}"
        + "return false;}"
        + "function presentFrame(doc,W,H,ox,oy,css){"
        + "if(!doc||!doc.body||!loginishDoc(doc))return false;"
        + "if(css&&doc.head&&!doc.getElementById('furui-frame-css')){"
        + "var st=doc.createElement('style');st.id='furui-frame-css';"
        + "st.textContent=css;doc.head.appendChild(st);}"
        + "var p=doc.defaultView.__furuiFramePanel;"
        + "if(!p||!p.isConnected)p=framePanel(doc);"
        + "if(!p)return false;"
        + "var sh=doc.getElementById('furui-frame-shield');"
        + "if(!sh){sh=doc.createElement('div');sh.id='furui-frame-shield';"
        + "sh.style.cssText='position:fixed;left:-3000px;top:-3000px;width:8000px;"
        + "height:8000px;background:#fff;z-index:2147483000;pointer-events:none';"
        + "doc.body.appendChild(sh);}"
        + "if(!present(doc,p,W,H,ox,oy))return false;"
        + "doc.defaultView.__furuiFramePanel=p;"
        + "return true;}"
        + "function loginish(e){var s=((e.className||'')+' '+(e.id||'')).toLowerCase();"
        // 注入到页面里的辅助 DOM（白纱 / 过渡卡片）绝不能被当成登录框，
        // 否则 trim() 会把真实页面全隐藏，只剩一张白卡片。
        + "if((e.id||'').indexOf('furui-')===0)return false;"
        + "return /login|layer|modal|dialog|popup|passport|qrcode|signin|sign-in/.test(s);}"
        + "function dialogLike(e){try{var st=e.ownerDocument.defaultView.getComputedStyle(e);"
        + "if(st.position!=='fixed'&&st.position!=='absolute')return false;"
        + "var r=e.getBoundingClientRect();var a=r.width*r.height;"
        + "return r.width>=240&&r.width<=860&&r.height>=200&&r.height<=920&&a>=60000;}catch(x){return false;}}"
        + "function walkUp(e,doc){"
        // 不能碰到第一个 login 类名就返回：酷我登录面板底部还有一排「第三方登录」
        // 图标（class 同样带 login），从那里向上走会先命中小区块，把主登录区
        // （扫码/手机号）当兄弟分支藏掉。所以收集祖先里所有 login/dialog 容器，
        // 取面积最大的那个——那才是整个登录面板。
        + "var best=null,bestA=0,n=e,i=0;"
        + "while(n&&n!==doc.body&&i++<14){"
        + "var r=n.getBoundingClientRect();var a=r.width*r.height;"
        + "if(loginish(n)&&a>bestA){best=n;bestA=a;}"
        + "n=n.parentElement;}"
        + "if(best)return best;"
        + "n=e;i=0;while(n&&n!==doc.body&&i++<14){"
        + "if(dialogLike(n)){var r2=n.getBoundingClientRect();"
        + "if(r2.width*r2.height>bestA){best=n;bestA=r2.width*r2.height;}}"
        + "n=n.parentElement;}"
        + "return best;}"
        + "function vis(e){try{var r=e.getBoundingClientRect();"
        + "if(r.width<8||r.height<8)return false;"
        + "var st=e.ownerDocument.defaultView.getComputedStyle(e);"
        // 不看 opacity：白纱就是靠 opacity:0 藏整页的，看了就永远「不可见」
        + "return st.visibility!=='hidden'&&st.display!=='none';}catch(x){return false;}}"
        + "function probe(doc){var w=doc.defaultView;if(!w)return null;"
        // ① 密码框：登录表单最可靠的锚点。酷我的登录层盖不满视口，屏幕中心
        //    命中的还是首页轮播，靠中心采样永远裁不到登录区。
        + "var pws=doc.querySelectorAll('input[type=password],input[name*=password],"
        + "input[id*=password],input[placeholder*=\\u5bc6\\u7801]');"
        + "for(var i=0;i<pws.length;i++){if(!vis(pws[i]))continue;"
        + "var b0=walkUp(pws[i],doc);if(b0)return b0;}"
        // ② 二维码 / 图形验证码：扫码登录页的锚点
        + "var qrs=doc.querySelectorAll('[class*=qrcode],[class*=qr-code],[class*=QRCode],"
        + "[class*=qr_code],[class*=code],[class*=verify],[class*=captcha],"
        + "[class*=yanzheng],canvas');"
        + "for(i=0;i<qrs.length;i++){if(!vis(qrs[i]))continue;"
        + "var b1=walkUp(qrs[i],doc);if(b1)return b1;}"
        // ③ 登录层本身（class/id 带 login 关键词、面积够大）
        + "var lgs=doc.querySelectorAll('[class*=reg_login],[class*=login],[class*=logon],"
        + "[class*=loginbox],[class*=login_box],[id*=login]:not([id^=furui-]),"
        + "[class*=user_login]');"
        + "for(i=0;i<lgs.length;i++){if(!vis(lgs[i]))continue;"
        + "if((lgs[i].id||'').indexOf('furui-')===0)continue;"
        + "var r2=box2(lgs[i]);if(r2.width>=240&&r2.height>=160)return lgs[i];}"
        // ④ 兜底：屏幕中心采样
        + "var pts=[[0.5,0.5],[0.5,0.3],[0.5,0.7],[0.42,0.5],[0.58,0.5]];"
        + "for(i=0;i<pts.length;i++){"
        + "var el=doc.elementFromPoint(w.innerWidth*pts[i][0],w.innerHeight*pts[i][1]);"
        + "if(!el)continue;"
        + "if(el.closest&&el.closest('[id^=furui-]'))continue;"
        + "var b=walkUp(el,doc);if(b)return b;}"
        + "return null;}"
        + "function whiten(doc,from){"
        + "var vw=doc.defaultView.innerWidth,vh=doc.defaultView.innerHeight;"
        + "var n=from;"
        + "while(n&&n!==doc.body){var r=n.getBoundingClientRect();"
        // 全屏遮罩型登录层（酷我 .reg_login 是 rgba(0,0,0,.5) 铺满整屏，登录框浮在中间）：
        // 遮罩必须留着（否则中间的登录框会被一起隐藏），但把它改成白底，
        // 看上去就只剩中间的登录面板，没有黑色/灰色蒙层。
        + "if(r.width>=vw*0.9&&r.height>=vh*0.9){"
        + "n.style.setProperty('background','#fff','important');"
        + "n.style.setProperty('background-image','none','important');"
        + "n.style.setProperty('backdrop-filter','none','important');}"
        + "n=n.parentElement;}}"
        + "function trim(doc,box){var hidden=[],n=box,i;"
        + "whiten(doc,box);"
        + "while(n&&n.parentElement&&n.parentElement!==doc.body){"
        + "var p=n.parentElement;"
        // 过渡卡片是 body 的直接子节点，不能被当「兄弟分支」display:none 掉：
        // 掀纱时还要靠它淡出，和登录区做交叉过渡。
        + "for(i=0;i<p.children.length;i++){var c=p.children[i];"
        + "if(c===n||c.id==='furui-veil-card')continue;"
        + "hidden.push([c,c.style.display]);c.style.setProperty('display','none','important');}"
        + "n=p;}"
        + "if(n&&n.parentElement===doc.body){"
        + "for(i=0;i<doc.body.children.length;i++){var d=doc.body.children[i];"
        + "if(d===n||d.id==='furui-veil-card')continue;"
        + "hidden.push([d,d.style.display]);d.style.setProperty('display','none','important');}}"
        + "var r=box.getBoundingClientRect();"
        + "if(box.getClientRects().length===0||r.width<80||r.height<80){"
        + "for(i=0;i<hidden.length;i++)hidden[i][0].style.display=hidden[i][1]||'';"
        + "return false;}"
        + "doc.documentElement.style.setProperty('background','#fff','important');"
        + "doc.body.style.setProperty('margin','0','important');"
        + "try{box.scrollIntoView({block:'center'});}catch(e2){}"
        // Android 端：早期按布局视口宽度硬放大（scale=innerWidth/box.width +
        // left:0/top:0），登录区会顶在左上角或因为两栏布局整体偏右；竖屏时
        // 更是直接被裁到屏幕外。统一交给 present() 按 visualViewport 居中 +
        // 等比缩放，每轮/每次 resize 重算，横竖屏都能自适应。
        // IFRAME 例外：Windows 的 iframe 铺满路径里 box 就是 iframe 本身，
        // 外面紧接着设 100% 铺满，这里不能给它叠 transform。
        + "if(window.__furuiLoginMobile&&box.tagName!=='IFRAME'){"
        + "try{var pt=port(doc);present(doc,box,pt[0],pt[1],pt[2],pt[3]);}"
        + "catch(e3){}}"
        + "return true;}";

    // Windows 端执行尾：WebView2 下 iframe 铺满/fullscreen resize 是安全的，
    // 保留原「同源 iframe 裁剪 + 铺满」路径。
    readonly property string focusJs: root.trimFnsJs
        + "var ifs=document.querySelectorAll('iframe');"
        // 网易云把登录内容装在同源 iframe（#g_iframe）里，但它的默认尺寸只有
        // 336x512——窄视口（手机宽屏）下 elementFromPoint(中心) 打不中它，
        // 中心命中的是主文档的 HTML，iframe 分支永远不触发，最后只能
        // 「no-box」兜底。所以不依赖中心命中：遍历所有同源、有实际尺寸的
        // iframe（0x0 的是统计/预加载框，跳过），逐一 probe 里面有没有登录框。
        + "for(var fi=0;fi<ifs.length;fi++){"
        + "var el=ifs[fi];var fd=null;try{fd=el.contentDocument;}catch(e3){fd=null;}"
        + "if(!fd||!fd.body)continue;"
        + "var fr=el.getBoundingClientRect();if(fr.width<80||fr.height<80)continue;"
        + "var ib=probe(fd);"
        // Windows（WebView2）：iframe 铺满安全，保留原路径。Android 端不走这条
        // （实测「隐藏兄弟 + position:fixed + 100% 铺满」触发 WebView surface
        // 全屏重排 → 主线程 SIGSEGV addr 0x8，模拟器必现），用 focusJsAndroid。
        + "if(ib&&trim(document,el)){"
        + "el.style.setProperty('width','100%','important');"
        + "el.style.setProperty('height','100%','important');"
        + "el.style.setProperty('position','fixed','important');"
        + "el.style.setProperty('left','0','important');"
        + "el.style.setProperty('top','0','important');"
        + "el.style.setProperty('border','0','important');"
        // iframe 铺满主文档布局视口后，iframe 文档自己的 device-width 也
        // 随之变成 982，桌面布局自然成立。
        + "trim(fd,ib);return 'ok:iframe';}}"
        // 主文档也走 probe()：酷我的登录层是 pointer-events:none，屏幕中心
        // 命中的还是首页轮播，只靠中心采样永远裁不到登录区。
        + "var b=probe(document);"
        + "if(!b)return 'no-box';"
        + "return trim(document,b)?'ok:top':'undo';"
        + "})()"

    // FURUI_BROWSER_TRACE 用的诊断输出：登录层/密码框/真实输入事件计数
    readonly property string probeJs: "(function(){"
        + "var out=['vw='+window.innerWidth+' vh='+window.innerHeight"
        + "+' rs='+document.readyState];"
        + "var kids=document.body?document.body.children:[];"
        + "var tags=[];"
        + "for(var k=0;k<kids.length&&k<8;k++){"
        + "var c=kids[k];var r=c.getBoundingClientRect();"
        + "tags.push(c.tagName.toLowerCase()+(c.id?'#'+c.id:'')"
        + "+(typeof c.className==='string'&&c.className?'.'+c.className.split(' ')[0]:'')"
        + "+'@'+Math.round(r.width)+'x'+Math.round(r.height));}"
        + "out.push('kids='+tags.join(','));"
        + "out.push('title='+(document.title||'-'));"
        // SPA 挂载情况：#app 里有没有子节点；图片有多少张是加载失败的
        + "var appEl=document.getElementById('app');"
        + "if(appEl){var ac=appEl.children.length;"
        + "var at=[];for(var a=0;a<appEl.children.length&&a<4;a++){"
        + "var an=appEl.children[a];var ar=an.getBoundingClientRect();"
        + "at.push(an.tagName.toLowerCase()"
        + "+(typeof an.className==='string'&&an.className?'.'+an.className.split(' ')[0]:'')"
        + "+'@'+Math.round(ar.width)+'x'+Math.round(ar.height));}"
        + "out.push('app='+ac+':'+at.join(','));}else out.push('app=nil');"
        + "var imgs=document.querySelectorAll('img'),bad=0;"
        + "for(var q=0;q<imgs.length;q++){if(imgs[q].complete&&imgs[q].naturalWidth===0)bad++;}"
        + "out.push('img='+imgs.length+'/bad'+bad);"
        // 裁剪用的 5 个采样点分别命中了什么：命中不到登录区时靠它定位遮挡层
        + "var pts=[[0.5,0.5],[0.5,0.3],[0.5,0.7],[0.42,0.5],[0.58,0.5]],hits=[];"
        + "for(var pi=0;pi<pts.length;pi++){"
        + "var he=document.elementFromPoint(window.innerWidth*pts[pi][0],"
        + "window.innerHeight*pts[pi][1]);"
        + "if(!he){hits.push(pi+':nil');continue;}"
        + "var hr2=he.getBoundingClientRect();"
        + "hits.push(pi+':'+he.tagName.toLowerCase()"
        + "+(typeof he.className==='string'&&he.className?'.'+he.className.split(' ')[0]:'')"
        + "+'@'+Math.round(hr2.width)+'x'+Math.round(hr2.height));}"
        + "out.push('hits='+hits.join(','));"
        // 登录相关元素的可见性清单：class/id 带 login/qr/code/phone/tab 的元素，
        // 打印矩形与 display/visibility/opacity/z-index，定位「到底哪个是可见的」
        + "var le=document.querySelectorAll('[class*=reg_login],[class*=login],"
        + "[class*=qr],[class*=QR],[class*=code],[class*=tab],[class*=phone],"
        + "[class*=yanzheng],[class*=captcha],[class*=account]');"
        + "var lst=[];"
        + "for(var li=0;li<le.length&&li<14;li++){"
        + "var le2=le[li];var lr3=le2.getBoundingClientRect();"
        + "var ls=getComputedStyle(le2);"
        + "lst.push(le2.tagName.toLowerCase()"
        + "+(typeof le2.className==='string'&&le2.className?'.'+le2.className.split(' ')[0]:'')"
        + "+'@'+Math.round(lr3.width)+'x'+Math.round(lr3.height)"
        + "+'/'+ls.display+'/'+ls.visibility+'/'+ls.opacity+'/'+ls.zIndex);}"
        + "out.push('loginels='+lst.join(','));"
        // 登录入口选择器命中情况：QQ 音乐是 .top_login__link，酷我是 .reg_text，
        // 酷狗是 ._login——把命中数与首个元素的矩形/可见性打出来，便于定位
        + "var sels=['.top_login__link','[class*=top_login]','[class*=_login]',"
        + "'[class*=reg_text]','[class*=login]','a[href*=login]'];"
        + "for(var si=0;si<sels.length;si++){"
        + "var l=document.querySelectorAll(sels[si]);"
        + "var d=l.length>0?l[0]:null;"
        + "if(d){var dr=d.getBoundingClientRect();"
        + "var ds=getComputedStyle(d);"
        + "out.push(sels[si]+'='+l.length+'@'+Math.round(dr.left)+','"
        + "+Math.round(dr.top)+','+Math.round(dr.width)+'x'+Math.round(dr.height)"
        + "+'/'+ds.display+'/'+ds.visibility+'/'+(d.textContent||'').trim().slice(0,6));}"
        + "else out.push(sels[si]+'=0');}"
        + "var pw=document.querySelectorAll('input[type=password]');out.push('pwd='+pw.length);"
        + "var lg=document.querySelectorAll('[class*=reg_login],[class*=login-layer],[class*=loginLayer]');"
        + "for(var i=0;i<lg.length;i++){var r=lg[i].getBoundingClientRect();"
        + "if(r.width>=240&&r.height>=200){"
        + "out.push('layer='+Math.round(r.left)+','+Math.round(r.top)+','"
        + "+Math.round(r.width)+'x'+Math.round(r.height));break;}}"
        + "out.push('bodykids='+document.body.children.length);"
        + "out.push('md='+(window.__mdCount||0)+'/'+window.__mdTrusted);"
        + "out.push('errs='+((window.__furuiErrs||[]).slice(0,3).join('~')||'-'));"
        + "return out.join(' | ');})()"

    // 登录框默认可能停在「扫码登录」页签，而这一栏叫「账号密码登录」。
    // 裁剪成功后点一次「账号登录」/「账号密码登录」这类页签；只认这些文字，
    // 绝不去点「扫码登录」，避免把用户想要的二维码页切走。
    readonly property string accountJs: "(function(){"
        // 网易云的登录页在同源 iframe 里：页签要点 iframe 文档内的元素，
        // 事件也必须用该文档的 MouseEvent 构造器，跨文档派发主文档的事件会失败。
        + "var doc=(window.__furuiAccountFrame&&window.__furuiAccountFrame.contentDocument)"
        + "||document;"
        + "var ME=doc.defaultView.MouseEvent||window.MouseEvent;"
        + "function vis(e){var r=e.getBoundingClientRect();if(r.width<6||r.height<6)return false;"
        + "var st=doc.defaultView.getComputedStyle(e);"
        + "return st.visibility!=='hidden'&&st.display!=='none';}"
        + "var c='';try{c=sessionStorage.getItem('__furuiLoginChoice')"
        + "||window.__furuiLoginChoice||'';}catch(e0){c=window.__furuiLoginChoice||'';}"
        // 过渡层里用户预选过登录方式时，按选择切页签；没选就维持原来的默认
        // （账号/密码优先，QQ 只有「QQ登录 / 微信登录」两个页签）。
        + "var want=c==='phone'?['\u624b\u673a\u53f7\u767b\u5f55','\u77ed\u4fe1\u767b\u5f55',"
        + "'\u5feb\u6377\u767b\u5f55','\u624b\u673a\u767b\u5f55']"
        + ":c==='password'?['\u8d26\u53f7\u767b\u5f55','\u5e10\u53f7\u767b\u5f55',"
        + "'\u8d26\u53f7\u5bc6\u7801\u767b\u5f55','\u5bc6\u7801\u767b\u5f55','\u0051\u0051\u767b\u5f55']"
        + ":c==='qrcode'?['\u626b\u7801\u767b\u5f55','\u4e8c\u7ef4\u7801\u767b\u5f55',"
        + "'\u5fae\u4fe1\u767b\u5f55','\u626b\u7801']"
        + ":['\u8d26\u53f7\u767b\u5f55','\u5e10\u53f7\u767b\u5f55',"
        // 默认列表：账号密码类优先，QQ 音乐登录页只有「QQ登录 / 微信登录」两个页签，
        // 不点「QQ登录」就会默认停在微信扫码页签；手机号登录放在最后兜底。
        + "'\u8d26\u53f7\u5bc6\u7801\u767b\u5f55','\u8d26\u53f7\u5bc6\u7801',"
        + "'\u5bc6\u7801\u767b\u5f55','\u8d26\u6237\u767b\u5f55',"
        + "'\u0051\u0051\u767b\u5f55','\u624b\u673a\u53f7\u767b\u5f55'];"
        + "var nodes=doc.querySelectorAll('a,button,span,div,p,li,label');"
        + "for(var i=0;i<nodes.length;i++){var n=nodes[i];"
        // 过渡卡片此刻可能还在淡出、DOM 尚未移除，它上面的「扫码登录」等按钮
        // 不能当站点页签点，否则切页签会假成功。
        + "if(n.closest&&n.closest('[id^=furui-]'))continue;"
        + "var t=(n.textContent||'').replace(/\\s+/g,'');"
        + "if(want.indexOf(t)<0)continue;if(!vis(n))continue;"
        + "var r=n.getBoundingClientRect();if(r.width>200||r.height>60)continue;"
        + "var o={bubbles:true,cancelable:true,view:doc.defaultView,"
        + "clientX:r.left+r.width/2,clientY:r.top+r.height/2};"
        + "try{n.dispatchEvent(new ME('mousedown',o));}catch(e1){}"
        + "try{n.dispatchEvent(new ME('mouseup',o));}catch(e2){}"
        + "try{n.dispatchEvent(new ME('click',o));}catch(e3){}"
        + "try{n.click();}catch(e4){}"
        + "return 'ok:'+t;}return 'none';})()"

    // —— 登录选项过渡层 ——
    // 白纱原本只是一片纯白，用户盯着空白等裁剪。现在改成「首页先隐藏、直接显示
    // 登录选项」的过渡卡片：站点外壳仍由 opacity:0 收掉（布局/命中测试保留，
    // 真实点击照样能点开登录框），卡片上列出该平台支持的登录方式，用户可预选，
    // 裁剪完成后卡片淡出、登录区从白纱下淡入。WebView 是原生子窗口，QML 遮罩
    // 盖不住它，所以这一层必须注入到页面里，和原来的白纱共用同一个 id。
    readonly property string veilChoicesHtml: {
        var h = ""
        var ms = []
        try { ms = app.loginMethods(root.sourceId) } catch (e) { }
        for (var i = 0; i < ms.length; i++) {
            var m = ms[i]
            var t = m === "qrcode" ? "扫码登录"
                  : (m === "phone" ? "手机号登录"
                  : (m === "password" ? "账号登录" : ""))
            if (t.length === 0) continue
            h += "<button data-c='" + m + "' style='pointer-events:auto;cursor:pointer;"
               + "border:1px solid #d8d8d8;background:#fff;color:#333;border-radius:18px;"
               + "padding:8px 16px;font-size:12px;margin:4px'>" + t + "</button>"
        }
        return h
    }

    // 卡片内容整段在 QML 里拼好、再 JSON.stringify 成 JS 字符串字面量，避免
    // 平台名/按钮 HTML 里的引号把注入脚本截断（之前手工拼 \" 很容易踩）。
    // max-width 用百分比而不是 vw：vw 是 982px 的布局视口，会超出可视区。
    readonly property string veilInnerHtml:
        "<div style='text-align:center;max-width:88%;animation:furui-rise .32s ease'>"
        + "<div style='width:32px;height:32px;border:3px solid #e8e8e8;"
        + "border-top-color:#12b7a6;border-radius:50%;margin:0 auto 20px;"
        + "animation:furui-spin .9s linear infinite'></div>"
        + "<div id='furui-veil-title' style='font-size:15px;font-weight:600;color:#222'></div>"
        + "<div style='font-size:12px;color:#888;margin-top:8px'>正在准备登录选项…</div>"
        + "<div style='margin-top:20px'>" + root.veilChoicesHtml + "</div></div>"

    readonly property string veilCss:
        "body{background:#fff!important}"
        + "body *{opacity:0!important;transition:none!important;}"
        // 过渡层自身与内部元素必须例外，否则会被上面那条一起藏掉。
        // 淡出动画也单独给 !important：body * 的 transition:none!important
        // 会盖掉内联 transition，否则掀纱时卡片是「瞬间消失」而不是淡出。
        + "#furui-veil-card,#furui-veil-card *{opacity:1!important}"
        + "#furui-veil-card{transition:opacity .36s ease!important}"
        + "@keyframes furui-spin{to{transform:rotate(360deg)}}"
        + "@keyframes furui-rise{from{transform:translateY(12px)}to{transform:translateY(0)}}"

    // 幂等注入：Windows 的 WebView2 插件会在文档创建时先塞一个同 id 的纯白样式，
    // 这里把它的内容换成含过渡层规则的版本，并补上卡片 DOM。
    //
    // 卡片 id 刻意不叫 *login*：probe() 的 [id*=login] / loginish() 会把它当成
    // 登录框，trim() 就会把真实页面全部 display:none、只留这张白卡片，掀纱后
    // 用户看到的全白正是这么来的。所有注入 DOM 统一用 furui- 前缀并显式排除。
    // 尺寸必须按 **visualViewport** 而不是 100vw/100vh：Android WebView 开了
    // wide viewport 后，布局视口 innerWidth=982、可视视口只有 336×512（scale=1），
    // 用 100vw 铺出来的卡片会把内容居中在 (491,748)——完全落在屏幕外，用户只看
    // 到一片纯白（实测 GSI 与模拟器都这样）。这里显式给 left/top/width/height，
    // 并在每轮 focusJsAndroid 里重新校正，另外挂 resize/scroll 监听扛旋转/键盘。
    readonly property string veilInjectJs: "(function(){"
        + "if(!document.head)return 'no-head';"
        + "if(window.__furuiVeilOff)return 'off';"
        + "var vs=document.getElementById('furui-login-veil');"
        + "if(vs&&vs.tagName!=='STYLE')vs.remove();"
        + "if(!vs||vs.tagName!=='STYLE'){"
        + "vs=document.createElement('style');vs.id='furui-login-veil';"
        + "document.head.appendChild(vs);}"
        + "if(vs.textContent.indexOf('furui-veil-card')<0)"
        + "vs.textContent=" + JSON.stringify(root.veilCss) + ";"
        + "if(!document.body)return 'no-body';"
        + "var vv=window.visualViewport;"
        + "var vw=vv?vv.width:window.innerWidth,vh=vv?vv.height:window.innerHeight;"
        + "var vl=vv?vv.offsetLeft:0,vt=vv?vv.offsetTop:0;"
        + "var o=document.getElementById('furui-veil-card');"
        + "if(o){o.style.left=vl+'px';o.style.top=vt+'px';"
        + "o.style.width=vw+'px';o.style.height=vh+'px';return 'ok';}"
        + "o=document.createElement('div');o.id='furui-veil-card';"
        + "o.setAttribute('style','position:fixed;left:'+vl+'px;top:'+vt+'px;"
        + "width:'+vw+'px;height:'+vh+'px;"
        + "z-index:2147483647;background:#fff;display:flex;align-items:center;"
        + "justify-content:center;pointer-events:none;"
        + "font-family:system-ui,-apple-system,sans-serif;"
        + "transition:opacity .36s ease');"
        + "o.innerHTML=" + JSON.stringify(root.veilInnerHtml) + ";"
        + "document.body.appendChild(o);"
        + "o.querySelector('#furui-veil-title').textContent="
        + JSON.stringify(root.sourceName + " · 官网登录") + ";"
        // 事件委托（不用内联 onclick，避开站点 CSP）：预选写进 sessionStorage，
        // 网易云那种 iframe → 顶层导航换文档后依然读得到。
        + "o.onclick=function(e){var b=e.target;"
        + "while(b&&b!==o&&!b.getAttribute('data-c'))b=b.parentElement;"
        + "if(!b||b===o)return;var c=b.getAttribute('data-c');"
        + "try{sessionStorage.setItem('__furuiLoginChoice',c);}catch(x){}"
        + "window.__furuiLoginChoice=c;"
        + "var bs=o.querySelectorAll('[data-c]');"
        + "for(var i=0;i<bs.length;i++){var on=bs[i].getAttribute('data-c')===c;"
        + "bs[i].style.background=on?'#12b7a6':'#fff';"
        + "bs[i].style.color=on?'#fff':'#333';"
        + "bs[i].style.borderColor=on?'#12b7a6':'#d8d8d8';}};"
        // 旋转 / 软键盘 / WebView 自己改缩放时，可视视口会变；把卡片跟着挪回去。
        // 只挂一次（veilInjectJs 每轮都会跑），并且卡片被掀掉后自动停手。
        + "if(vv&&!window.__furuiVeilVvSync){window.__furuiVeilVvSync=1;"
        + "var sync=function(){var q=document.getElementById('furui-veil-card');"
        + "if(!q)return;q.style.left=vv.offsetLeft+'px';q.style.top=vv.offsetTop+'px';"
        + "q.style.width=vv.width+'px';q.style.height=vv.height+'px';};"
        + "vv.addEventListener('resize',sync);vv.addEventListener('scroll',sync);}"
        + "return 'ok';})()"

    // 掀纱：先移除隐藏规则（登录区此时已经可见，但还被不透明卡片盖着），
    // 再让卡片淡出，形成「首页 → 登录选项 → 登录框」的过渡。
    readonly property string veilLiftJs: "(function(){"
        + "window.__furuiVeilOff=1;"
        + "var s=document.getElementById('furui-login-veil');if(s)s.remove();"
        + "var o=document.getElementById('furui-veil-card');"
        // 只做透明度淡出：给整屏 fixed 层加 transform:scale() 会触发 WebView
        // surface 全屏重排，模拟器 ARM 转译层上偶发主线程 SIGSEGV。
        + "if(o){setTimeout(function(){o.style.opacity='0';},20);"
        + "setTimeout(function(){if(o.parentNode)o.parentNode.removeChild(o);},480);}"
        + "return 'ok';})()"

    // Android 自治版执行尾：全程配合 fire-and-forget 的 runJavaScript（无回调）。
    // 背景：QtWebView 的 QML runJavaScript 把回调闭包存进 C++ 全局 QHash
    //（qquickwebview.cpp CallbackStorage），迟到投递时 callback.call() 不校验
    // 闭包上下文——实测高频回调 + 页面跳转组合下 QV4 use-after-free
    //（SIGSEGV addr 0x8/0x0，模拟器必现、GSI 真机 ~50%）。因此 Android 端：
    // ① 一律不用带回调的 runJavaScript；② 状态机全部搬进页面：没登录框→合成
    // 点击入口；登录框在同源 iframe→页面自己顶层导航过去（sessionStorage 防循环）；
    // 裁剪→掀纱→切页签一次完成；③ 结果写进 document.title（'[furui]' 前缀），
    // QML 读 view.title 属性即可拿到，无需回调。
    readonly property string focusJsAndroid: root.trimFnsJs
        // —— 页面内自治注入（Android 全程不使用带回调的 runJavaScript）——
        // 站点 SPA 会把自己动态改回 width=device-width，甚至再插入第二个
        // viewport meta（酷狗实测 innerWidth 1344→336）：只改第一个会被后来者
        // 盖掉。这里遍历所有 meta 统一校正，每轮一次。
        + "var mvs=document.querySelectorAll('meta[name=\"viewport\"]');"
        + "if(!mvs.length&&document.head){var m2=document.createElement('meta');"
        + "m2.name='viewport';m2.content='width=980,initial-scale=1.0';"
        + "document.head.appendChild(m2);}"
        + "else{for(var mi=0;mi<mvs.length;mi++){"
        + "if((mvs[mi].getAttribute('content')||'').indexOf('width=980')<0)"
        + "mvs[mi].setAttribute('content','width=980,initial-scale=1.0');}}"
        // 登录选项过渡层：裁剪完成前一直盖着（注入幂等，每轮都会校正
        // 站点 SPA 动态改回来的 viewport meta）。
        + root.veilInjectJs + ";"
        // 站点外壳裁剪样式：按 id 幂等注入，SPA 换文档也能补上
        + "if(!document.getElementById('furui-login-trim')&&document.head){"
        + "var ts=document.createElement('style');ts.id='furui-login-trim';"
        + "ts.textContent=" + JSON.stringify(browser.loginCss(root.sourceId)) + ";"
        + "document.head.appendChild(ts);}"
        + "var ifs=document.querySelectorAll('iframe');"
        + "var fp=port(document);"
        + "var frameCss=" + JSON.stringify(browser.loginCss(root.sourceId)) + ";"
        // iframe 呈现层只给「登录页本身在 iframe 里」的平台（网易云 #g_iframe）。
        // 其它平台是首页弹登录框，首页里往往还有广告/播放器 canvas iframe：
        // 若在这里抢先 return，主文档的登录入口点击就永远轮不到执行，登录框
        // 点不开、掀纱后露出整个首页（QQ 音乐实测踩过）。
        + "var allowFrame=" + (root.sourceId === "netease" ? "true" : "false") + ";"
        + "var frameEl=null;"
        + "for(var fi=0;fi<ifs.length;fi++){"
        + "var el=ifs[fi];"
        + "var fr=el.getBoundingClientRect();if(fr.width<80||fr.height<80)continue;"
        + "var fd=null;try{fd=el.contentDocument;}catch(e5){fd=null;}"
        // 用主文档 visualViewport（而不是 iframe 元素 rect）当舞台尺寸：旋转后
        // WebView 的 page scale 会变，两者不再等价，用 rect 会把登录框推出屏幕。
        + "if(allowFrame&&fd&&fd.body"
        + "&&presentFrame(fd,fp[0],fp[1],fp[2],fp[3],frameCss)){frameEl=el;break;}}"
        // 不再做「顶层导航到 iframe URL」兜底：旋转瞬间子帧会短暂不可读，
        // 旧代码这时 location.replace('/login')，站点随即重定向回首页 #，
        // 用户看到的就是黑红 header + 首页横幅，登录页彻底丢了。子帧恢复后
        // 下一轮（1.5s）自然重新接管，等一等远比重导航安全。
        + "if(frameEl){"
        + "window.__furuiWait=0;"
        + "window.__furuiAccountFrame=frameEl;"
        + "if(!window.__furuiFrameLifted){window.__furuiFrameLifted=1;"
        + root.veilLiftJs + ";"
        // 切「账号登录」类页签（过渡层里预选过登录方式时按选择切）
        + root.accountJs + ";"
        + "window.__furuiAccountDone=1;}"
        + "document.title='[furui]ok:frame';"
        + "return 'ok:frame';}"
        + "if(allowFrame){"
        + "window.__furuiWait=(window.__furuiWait||0)+1;"
        // 站点 SPA 仍可能自己把 hash 路由重置回首页，只拉回一次，避免死循环。
        + "if(location.hash.indexOf('login')<0&&!window.__furuiRouteFix){"
        + "window.__furuiRouteFix=1;location.hash='#/login';return 'route-fix';}"
        // 子帧还没渲染好：绝不裁剪主文档（在首页上 trim 会把首页当登录框，
        // 之后 iframe 恢复了也回不去），安静等下一轮。等满 10 轮说明站点
        // 改版成主文档登录了，放行到下面的通用流程，别把页面卡死。
        + "if(window.__furuiWait<10)return 'wait-frame';}"
        // 主文档没有登录框：合成点击登录入口（等价 loginJs 兜底路径），
        // 下一轮再 probe。登录框开着时合成点击入口反而会把它关掉，所以
        // 密码框/二维码/大登录层可见时跳过点击。
        + "var b=probe(document);"
        + "if(!b){"
        + "var opened=false;"
        + "var pws=document.querySelectorAll('input[type=password]');"
        + "for(var pi=0;pi<pws.length;pi++){if(vis(pws[pi])){opened=true;break;}}"
        + "var qrs=document.querySelectorAll('[class*=qrcode],[class*=qr-code],"
        + "[class*=QRCode],[class*=qr_code],canvas');"
        + "for(pi=0;pi<qrs.length;pi++){if(vis(qrs[pi])&&"
        + "qrs[pi].getBoundingClientRect().width>=80){opened=true;break;}}"
        + "if(!opened){" + browser.loginJs(root.sourceId) + ";}"
        + "return 'clicked';}"
        // 主文档裁剪只做一次（隐藏兄弟节点不可重复），但「呈现」每轮都重算：
        // 旋转 / 软键盘 / WebView 改缩放时 visualViewport 会变，登录区必须跟着
        // 重新居中缩放，否则横竖屏切换后又会偏到屏幕外。
        + "if(!window.__furuiTrimDone){"
        + "if(!trim(document,b))return 'undo';"
        + "window.__furuiTrimDone=1;"
        // 裁剪成功：自己掀掉过渡层（QML 不再参与这一步），登录区淡入
        + root.veilLiftJs + ";"
        // 切「账号登录」类页签（等价 accountJs），只做一次
        + "if(!window.__furuiAccountDone){"
        + root.accountJs + ";"
        + "window.__furuiAccountDone=1;}"
        + "document.title='[furui]ok:top';}"
        + "var pt=port(document);"
        + "present(document,window.__furuiBox||b,pt[0],pt[1],pt[2],pt[3]);"
        + "return 'ok:top';"
        + "})()"

    function injectOnce() {
        const view = webLoader.item
        if (!view || typeof view.runJavaScript !== "function") return
        const current = view.url ? view.url.toString() : ""
        if (current.length === 0) return
        if (root.trimmedFor === current) return
        root.focused = false
        root.focusTries = 0
        root.accountDone = false
        root.accountTries = 0
        // 新文档先蒙上白纱：用户看到的是纯白，不是站点外壳
        root.veilLifted = false
        // Android：白纱 / viewport / 裁剪样式全部由 focusJsAndroid 在页面内自治
        // 注入（每轮校正，扛得住站点 SPA 把 meta 改回 device-width）。这里不排
        // 任何带回调的 runJavaScript —— 回调闭包经 C++ 全局 QHash 往返、迟到投递
        // 不校验上下文，正是之前 QV4 use-after-free 崩溃的根因。
        if (root.isAndroid) {
            root.trimmedFor = current
            return
        }
        // WebView2 后端会在文档创建时（首帧之前）注入同一 id 的样式；这里补一层
        // 兜底。文档还没就绪（document.head 不存在）时返回 'no-head'，清掉去重键
        // 下一轮重试（酷我官网 load 事件迟迟不来，只能靠这个兜底）。
        view.runJavaScript(root.veilInjectJs,
                           function (r) {
                               const s = String(r || "")
                               // 文档还没建好（head/body 缺失）时下一轮重试；
                               // 'off' 表示已经掀过纱，不必再注。
                               if (s === "no-head" || s === "no-body") {
                                   root.trimmedFor = ""
                                   return
                               }
                               root.trimmedFor = current
                               injectTrimCss()
                               injectTraceHook()
                           })
    }

    // 站点外壳（导航/页脚/播放器）的裁剪样式
    function injectTrimCss() {
        const view = webLoader.item
        if (!view || typeof view.runJavaScript !== "function") return
        const css = browser.loginCss(root.sourceId)
        if (css.length === 0) return
        // 样式用 JSON 转义后塞进脚本，避免 CSS 里的引号/换行把脚本截断
        view.runJavaScript("(function(){var s=document.createElement('style');"
                           + "s.id='furui-login-trim';s.textContent=" + JSON.stringify(css)
                           + ";document.head&&document.head.appendChild(s);})()")
    }

    // 收一份页面脚本错误，登录入口点不开时能直接看到是站点自己报错还是没点中
    function injectTraceHook() {
        const view = webLoader.item
        if (!view || typeof view.runJavaScript !== "function") return
        if (!browser.trace) return
        view.runJavaScript("(function(){if(window.__furuiErrHook)return;"
                           + "window.__furuiErrHook=1;window.__furuiErrs=[];"
                           + "window.__mdCount=0;window.__mdTrusted=null;"
                           + "window.addEventListener('error',function(e){"
                           // 资源加载失败时 e.message 是空的，只有 e.target 里有 URL；
                           // 把 URL 一起记下来才知道是哪个脚本/图片挂了
                           + "var t=e.target;"
                           + "var u=(t&&(t.src||t.href))||'';"
                           + "window.__furuiErrs.push(String(e.message||e.type)"
                           + "+(u?(':'+u):''));},true);"
                           + "window.addEventListener('unhandledrejection',function(e){"
                           + "window.__furuiErrs.push('rej:'+e.reason);});"
                           + "document.addEventListener('mousedown',function(e){"
                           + "window.__mdCount++;window.__mdTrusted=e.isTrusted;},true);})()")
    }

    // 掀掉白纱：裁剪成功后立刻恢复页面显示（opacity 全部还原）。
    // 幂等——多轮 focusLogin 里反复调用没问题。
    function liftVeil() {
        if (root.veilLifted) return
        root.veilLifted = true
        const view = webLoader.item
        if (!view || typeof view.runJavaScript !== "function") return
        view.runJavaScript(root.veilLiftJs)
    }

    // 每轮先试一下「点开登录框」，再裁剪到登录区。两件事都必须可重入：
    // 单页应用里登录框是渐次出现的。
    function focusLogin() {
        const view = webLoader.item
        if (!view || typeof view.runJavaScript !== "function") return
        // 防御闸门：页面正在跳转/加载时绝不排队新的 runJavaScript。网易云登录页
        // 内部会连续跳转，跨导航的迟到 JS 回调是 Qt WebView 后端 use-after-free
        //（实测 SIGSEGV in qtMainLoopThrea / 主线程，addr 0x8，与 GSI 真机
        // tombstone 同签名）的高发区——宁可控住节奏，也不往火坑里排任务。
        if (view.loading) return

        if (root.isAndroid) {
            // —— Android 无回调自治管线 ——
            // QtWebView 的 runJavaScript 回调闭包经 C++ 全局 QHash 往返，迟到
            // 投递时 callback.call() 不校验闭包上下文，高频回调 + 页面跳转下
            // 实测 QV4 use-after-free SIGSEGV（详见 focusJsAndroid 注释）。
            // 所以这里全部 fire-and-forget：点击/goto/裁剪/掀纱/切页签都由
            // 页面脚本自治，结果写 document.title（'[furui]' 前缀）传回。
            view.runJavaScript("window.__furuiLoginMobile=true;")
            // 白纱 / viewport / 裁剪样式由 focusJsAndroid 每轮在页面内自治校正；
            // injectOnce 在 Android 只做状态复位，不排任何 JS 任务。
            root.injectOnce()
            root.focusTries++
            // 兜底：等了 21 秒还没裁出来（站点改版/检测不到登录框），先把白纱掀掉
            if (!root.veilLifted && root.focusTries > 14)
                root.liftVeil()
            // 合成事件点不开的站点（酷我首页「登录 / 注册」只认 isTrusted 的真实输入）：
            // 别让用户对着「正在整理登录页…」发呆，明确提示手动点一下。用户点开后
            // 下一轮 probe 就能找到登录框，裁剪 / 掀纱 / 切页签照常自动完成。
            if (!root.focused && root.focusTries === 15)
                root.status = "若还没看到登录框，请点一下页面里的「登录 / 注册」入口"
            view.runJavaScript(root.focusJsAndroid)
            // view.title 是 QML 属性，读取不需要回调
            const t = String(view.title || "")
            if (t.indexOf("[furui]") === 0 && !root.focused) {
                root.focused = true
                root.status = "请在官网页面完成登录（滑块 / 短信验证都能过）"
            }
            return
        }

        // —— Windows（WebView2）：回调路径无此崩溃，保留原流程 ——
        // 每轮先确保白纱/裁剪样式已注入：load 事件迟到或干脆不来时（酷我官网），
        // 这里就是唯一的注入时机。
        root.injectOnce()
        root.focusTries++
        // 兜底：等了 21 秒还没裁出来（站点改版/检测不到登录框），先把白纱掀掉，
        // 宁可多显示一点站点外壳，也不能让用户对着白屏发呆
        if (!root.veilLifted && root.focusTries > 14)
            root.liftVeil()

        // 登录框一开就不再点入口：继续点会把它开合关掉。之后只做裁剪。
        // 尝试窗口放到 20 轮（约 30 秒）：QQ 音乐首页是重 SPA，头部登录按钮
        // 要等它把 196 张图都拉完才出现，前几轮页面里根本没有这个入口。
        if (!root.focused && root.focusTries <= 20) {
            if (root.focusTries <= 14) {
                // 优先「真实鼠标点击」：酷我首页的「登录 / 注册」对 JS 合成事件完全
                // 不响应（document 收得到 click，站点逻辑不动作、也没有报错），
                // 只有 WebView2 输入管线产生的事件（isTrusted=true）才会开登录框。
                view.runJavaScript(browser.loginTargetJs(), function (r) {
                    const spec = String(r || "")
                    if (spec === "open")
                        return // 登录框已经开着，什么都不点
                    if (spec === "none") {
                        // 没找到入口坐标：退回合成点击（也是文字兜底）
                        view.runJavaScript(browser.loginJs(root.sourceId))
                        return
                    }
                    const p = spec.split(",")
                    if (p.length < 3) return
                    const ok = browser.realClick(parseFloat(p[0]), parseFloat(p[1]),
                                                 parseFloat(p[2]))
                    if (browser.trace)
                        console.log("[weblogin]", root.sourceId, "realClick", spec, ok)
                    // 没有真实点击能力（Android / 非 WebView2 后端）就用合成事件
                    if (!ok)
                        view.runJavaScript(browser.loginJs(root.sourceId))
                })
            } else {
                view.runJavaScript(browser.loginJs(root.sourceId), function (r) {
                    if (browser.trace)
                        console.log("[weblogin]", root.sourceId, "click=" + r,
                                    "try=" + root.focusTries)
                })
            }
        }
        // DOM 诊断探针：不管走哪条点击路径都打一次，方便定位「弹框开了但裁不到」
        if (browser.trace && root.focusTries <= 14) {
            view.runJavaScript(root.probeJs, function (p) {
                console.log("[weblogin] dom", p)
            })
        }
        view.runJavaScript(root.focusJs, function (result) {
            const res = String(result || "")
            if (browser.trace)
                console.log("[weblogin]", root.sourceId, "focus=" + res, "try=" + root.focusTries)
            // 只认 ok:top / ok:iframe；no-box（还没弹框）和 undo（裁完不可见已回滚）都算没成
            if (res.indexOf("ok") !== 0) {
                if (root.focusTries === 20)
                    root.status = "请在下方页面里点一下「登录」入口，登录成功后会自动导入"
                return
            }
            root.focused = true
            root.status = "请在官网页面完成登录（滑块 / 短信验证都能过）"
            root.liftVeil()
            if (browser.trace && root.focusTries <= 3)
                view.runJavaScript(root.probeJs, function (p) {
                    console.log("[weblogin] probe", p)
                })
            if (!root.accountDone && root.accountTries < 3) {
                root.accountTries++
                view.runJavaScript(root.accountJs, function (r) {
                    const acc = String(r || "")
                    if (acc.indexOf("ok") === 0) root.accountDone = true
                    if (browser.trace)
                        console.log("[weblogin]", root.sourceId, "account=" + acc)
                })
            }
        })
    }

    Rectangle {
        anchors.fill: parent
        color: theme.bg
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        // 顶部：返回 + 平台名 + 刷新。没有地址栏——这里不是浏览器，是「该平台的登录页」
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Rectangle {
                Layout.preferredWidth: 64
                Layout.preferredHeight: 32
                radius: 8
                color: Qt.rgba(theme.text.r, theme.text.g, theme.text.b, 0.07)
                Text {
                    anchors.centerIn: parent
                    text: "返回"
                    color: theme.text
                    font.pixelSize: 12
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.closed()
                }
            }

            Text {
                Layout.fillWidth: true
                text: (root.sourceName.length > 0 ? root.sourceName : root.sourceId)
                      + " · 官网登录"
                color: theme.text
                font.pixelSize: 13
                font.bold: true
                elide: Text.ElideRight
            }

            Rectangle {
                Layout.preferredWidth: 52
                Layout.preferredHeight: 32
                radius: 8
                color: Qt.rgba(theme.text.r, theme.text.g, theme.text.b, 0.07)
                Text {
                    anchors.centerIn: parent
                    text: "刷新"
                    color: theme.text
                    font.pixelSize: 12
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.trimmedFor = ""
                        const view = webLoader.item
                        if (view) view.reload()
                    }
                }
            }
        }

        // 网页区：原生窗口只占这一块，不能和上面的按钮重叠
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Rectangle {
                anchors.fill: parent
                color: "#FFFFFF"
                radius: 10
            }
            Loader {
                id: webLoader
                anchors.fill: parent
                active: root.visible
                sourceComponent: WebView {
                    id: wv
                    url: root.startUrl
                    Component.onCompleted: {
                        // 仅 Android：把移动 UA 换成桌面 UA，否则官网登录页会退回
                        // 「下载 APP」版首页，拿不到桌面登录入口。
                        if (Qt.platform.os === "android")
                            wv.httpUserAgent = root.desktopUa
                    }
                    onLoadingChanged: function (request) {
                        if (request.status === WebView.LoadStartedStatus) {
                            // Android 的 wide viewport 不靠 JNI 改 WebViewSettings
                            // （在 onLoadingChanged 的回调上下文里调原生方法会触发
                            // QML 引擎/WebView 的重入崩溃，见实测 SIGSEGV in
                            // libQt6Qml）。Android 14 的 WebView 默认 useWideViewport
                            // 为 true，只要把页面自带的 width=device-width 覆盖成
                            // width=980（见 injectOnce 的 meta 注入）即可得到 980
                            // 宽视口，无需动原生设置。
                            // 每次导航都是新文档：复位去重键，白纱会在 LoadSucceeded
                            // 的 injectOnce() 里重新注入
                            root.trimmedFor = ""
                            root.status = "正在打开官网登录页…"
                        } else if (request.status === WebView.LoadSucceededStatus) {
                            // 注意：不要在加载回调里同步调 runJavaScript——这会重入
                            // WebView 内部状态，实测触发 libQt6Qml 的 use-after-free
                            // SIGSEGV（backtrace: CallPropertyLookup::call ->
                            // QQmlContextWrapper::lookupIdObject，正好对应这里同步
                            // 调用 root.injectOnce()）。白纱/裁剪的注入改由
                            // focusLogin 的 Timer 每轮兜底（其首行即调 injectOnce），
                            // 完全等价且不在重入上下文里。
                            root.status = "正在整理登录页…"
                        } else if (request.status === WebView.LoadFailedStatus) {
                            // 打开失败页面是空的，先掀纱露出错误信息。同样延迟到
                            // 下一轮事件循环再掀，避免同步重入 WebView 回调。
                            root.status = "官网页面打开失败：检查网络后点「刷新」重试"
                            Qt.callLater(function () { root.liftVeil() })
                        }
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            text: root.status
            color: theme.subText
            font.pixelSize: 11
            lineHeight: 1.35
            wrapMode: Text.Wrap
        }
    }

    // 裁剪要等页面真正渲染出来：单页应用的登录框往往比 load 事件晚一拍，
    // 所以这里按节奏重试几次。裁到登录区之后再多跑几轮，把「账号登录」页签点上；
    // 页签切成功（或试满 3 次）就彻底停下，不再动用户的页面。
    Timer {
        interval: 1500
        repeat: true
        // Android 端裁出来后仍继续轮询：每轮会重算 visualViewport 下的居中/缩放，
        // 横竖屏切换、分屏、软键盘弹出都能自适应。Windows 端维持原来的
        // 「裁完 + 切完页签就停」。旋转时 onOrientationChanged 会复位 focusTries。
        running: root.visible && root.focusTries < (root.isAndroid ? 120 : 60)
                 && (root.isAndroid || !root.focused
                     || (!root.accountDone && root.accountTries < 3))
        onTriggered: root.focusLogin()
    }

    // 轮询平台原生 Cookie 存储。检测到该平台的登录态标记就交给 AuthManager 落库 + 同步歌单。
    Timer {
        id: pollTimer
        interval: 1200
        repeat: true
        running: root.visible && !root.captured
        onTriggered: {
            const view = webLoader.item
            if (!view || !view.url) return
            // 页面跳转中不碰 Cookie：与 focusLogin 同一道防御闸门
            if (view.loading) return
            const current = view.url.toString()
            if (current.length === 0) return
            const cookie = browser.cookiesForUrl(current)
            if (cookie.length === 0) return
            if (!browser.looksLoggedIn(root.sourceId, cookie)) return

            root.captured = true
            root.status = "已检测到登录态，正在导入并同步歌单…"
            app.loginByCookie(root.sourceId, cookie)
        }
    }

    Connections {
        target: app
        function onLoginStateChanged(sourceId, ok, message) {
            if (sourceId !== root.sourceId) return
            if (ok) {
                root.status = "登录成功，正在后台同步歌单"
                root.imported()
            } else {
                // 抓到的 Cookie 不够用（比如页面还没真正登录成功）：放开轮询再来一次
                root.captured = false
                root.status = "导入失败：" + message + "（在页面里重新登录后会自动重试）"
            }
        }
    }
}
