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

    // 找到登录区所在的容器，沿祖先链逐层隐藏兄弟节点，只留登录区这一支。
    // 识别顺序：① class/id 里带 login 且足够大的可见容器（取面积最小的那个，最具体）；
    // ② 密码框 / 手机号框 / 二维码图。检测不到就返回原因、完全不改动页面。
    // 只留「登录区」：定位当前真正可见的登录框，沿祖先链隐藏兄弟分支。
    // 两个安全阀：① 用 elementFromPoint 打在屏幕中线上找「正在显示」的元素，不用纯选择器猜；
    // ② 隐藏后校验登录框仍可见，不可见就原样恢复——绝不能把页面裁成一片空白。
    // 网易云把内容装在同域名 iframe（#g_iframe）里，命中 iframe 就钻进去再裁，
    // 主文档只留这个 iframe 并让它铺满。
    readonly property string focusJs: "(function(){"
        + "function loginish(e){var s=((e.className||'')+' '+(e.id||'')).toLowerCase();"
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
        + "function probe(doc){var w=doc.defaultView;if(!w)return null;"
        + "var pts=[[0.5,0.5],[0.5,0.3],[0.5,0.7],[0.42,0.5],[0.58,0.5]];"
        + "for(var i=0;i<pts.length;i++){"
        + "var el=doc.elementFromPoint(w.innerWidth*pts[i][0],w.innerHeight*pts[i][1]);"
        + "if(!el)continue;var b=walkUp(el,doc);if(b)return b;}"
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
        + "for(i=0;i<p.children.length;i++){var c=p.children[i];if(c===n)continue;"
        + "hidden.push([c,c.style.display]);c.style.setProperty('display','none','important');}"
        + "n=p;}"
        + "if(n&&n.parentElement===doc.body){"
        + "for(i=0;i<doc.body.children.length;i++){var d=doc.body.children[i];if(d===n)continue;"
        + "hidden.push([d,d.style.display]);d.style.setProperty('display','none','important');}}"
        + "var r=box.getBoundingClientRect();"
        + "if(box.getClientRects().length===0||r.width<80||r.height<80){"
        + "for(i=0;i<hidden.length;i++)hidden[i][0].style.display=hidden[i][1]||'';"
        + "return false;}"
        + "doc.documentElement.style.setProperty('background','#fff','important');"
        + "doc.body.style.setProperty('margin','0','important');"
        + "try{box.scrollIntoView({block:'center'});}catch(e2){}"
        + "return true;}"
        + "var el=document.elementFromPoint(window.innerWidth/2,window.innerHeight/2);"
        + "if(el&&el.tagName==='IFRAME'){"
        + "var fd=null;try{fd=el.contentDocument;}catch(e3){fd=null;}"
        + "if(fd&&fd.body){var ib=probe(fd);"
        + "if(ib&&trim(document,el)){"
        + "el.style.setProperty('width','100%','important');"
        + "el.style.setProperty('height','100%','important');"
        + "el.style.setProperty('position','fixed','important');"
        + "el.style.setProperty('left','0','important');"
        + "el.style.setProperty('top','0','important');"
        + "el.style.setProperty('border','0','important');"
        + "trim(fd,ib);return 'ok:iframe';}}}"
        + "var b=walkUp(el,document);"
        + "if(!b)return 'no-box';"
        + "return trim(document,b)?'ok:top':'undo';"
        + "})()"

    // FURUI_BROWSER_TRACE 用的诊断输出：登录层/密码框/真实输入事件计数
    readonly property string probeJs: "(function(){"
        + "var out=['vw='+window.innerWidth+' vh='+window.innerHeight];"
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
        + "var doc=document;"
        + "function vis(e){var r=e.getBoundingClientRect();if(r.width<6||r.height<6)return false;"
        + "var st=doc.defaultView.getComputedStyle(e);"
        + "return st.visibility!=='hidden'&&st.display!=='none';}"
        + "var want=['\u8d26\u53f7\u767b\u5f55','\u5e10\u53f7\u767b\u5f55',"
        + "'\u8d26\u53f7\u5bc6\u7801\u767b\u5f55','\u8d26\u53f7\u5bc6\u7801',"
        + "'\u5bc6\u7801\u767b\u5f55','\u8d26\u6237\u767b\u5f55'];"
        + "var nodes=doc.querySelectorAll('a,button,span,div,p,li,label');"
        + "for(var i=0;i<nodes.length;i++){var n=nodes[i];"
        + "var t=(n.textContent||'').replace(/\\s+/g,'');"
        + "if(want.indexOf(t)<0)continue;if(!vis(n))continue;"
        + "var r=n.getBoundingClientRect();if(r.width>200||r.height>60)continue;"
        + "var o={bubbles:true,cancelable:true,view:window,"
        + "clientX:r.left+r.width/2,clientY:r.top+r.height/2};"
        + "try{n.dispatchEvent(new MouseEvent('mousedown',o));}catch(e1){}"
        + "try{n.dispatchEvent(new MouseEvent('mouseup',o));}catch(e2){}"
        + "try{n.dispatchEvent(new MouseEvent('click',o));}catch(e3){}"
        + "try{n.click();}catch(e4){}"
        + "return 'ok:'+t;}return 'none';})()"

    function injectOnce() {
        const view = webLoader.item
        if (!view || typeof view.runJavaScript !== "function") return
        const current = view.url ? view.url.toString() : ""
        if (current.length === 0) return
        if (root.trimmedFor === current) return
        root.trimmedFor = current
        root.focused = false
        root.focusTries = 0
        root.accountDone = false
        root.accountTries = 0
        // 样式用 JSON 转义后塞进脚本，避免 CSS 里的引号/换行把脚本截断
        const css = browser.loginCss(root.sourceId)
        if (css.length > 0) {
            view.runJavaScript("(function(){var s=document.createElement('style');"
                               + "s.id='furui-login-trim';s.textContent=" + JSON.stringify(css)
                               + ";document.head&&document.head.appendChild(s);})()")
        }
        // 收一份页面脚本错误，登录入口点不开时能直接看到是站点自己报错还是没点中
        if (browser.trace) {
            view.runJavaScript("(function(){if(window.__furuiErrHook)return;"
                               + "window.__furuiErrHook=1;window.__furuiErrs=[];"
                               + "window.__mdCount=0;window.__mdTrusted=null;"
                               + "window.addEventListener('error',function(e){"
                               + "window.__furuiErrs.push(String(e.message||e.type));},true);"
                               + "window.addEventListener('unhandledrejection',function(e){"
                               + "window.__furuiErrs.push('rej:'+e.reason);});"
                               + "document.addEventListener('mousedown',function(e){"
                               + "window.__mdCount++;window.__mdTrusted=e.isTrusted;},true);})()")
        }
    }

    // 每轮先试一下「点开登录框」，再裁剪到登录区。两件事都必须可重入：
    // 单页应用里登录框是渐次出现的。
    function focusLogin() {
        const view = webLoader.item
        if (!view || typeof view.runJavaScript !== "function") return
        root.focusTries++

        // 登录框一开就不再点入口：继续点会把它开合关掉。之后只做裁剪。
        if (!root.focused && root.focusTries <= 12) {
            if (root.focusTries <= 8) {
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
                    if (!browser.trace) return
                    console.log("[weblogin]", root.sourceId, "click=" + r,
                                "try=" + root.focusTries)
                    view.runJavaScript(root.probeJs, function (p) {
                        console.log("[weblogin] dom", p)
                    })
                })
            }
        }
        view.runJavaScript(root.focusJs, function (result) {
            const res = String(result || "")
            if (browser.trace)
                console.log("[weblogin]", root.sourceId, "focus=" + res, "try=" + root.focusTries)
            // 只认 ok:top / ok:iframe；no-box（还没弹框）和 undo（裁完不可见已回滚）都算没成
            if (res.indexOf("ok") !== 0) {
                if (root.focusTries === 12)
                    root.status = "请在下方页面里点一下「登录」入口，登录成功后会自动导入"
                return
            }
            root.focused = true
            root.status = "请在官网页面完成登录（滑块 / 短信验证都能过）"
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
                    url: root.startUrl
                    onLoadingChanged: function (request) {
                        if (request.status === WebView.LoadSucceededStatus) {
                            root.status = "正在整理登录页…"
                            root.injectOnce()
                        } else if (request.status === WebView.LoadFailedStatus) {
                            root.status = "官网页面打开失败：检查网络后点「刷新」重试"
                        } else if (request.status === WebView.LoadStartedStatus) {
                            root.status = "正在打开官网登录页…"
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
        running: root.visible && root.trimmedFor.length > 0 && root.focusTries < 60
                 && (!root.focused || (!root.accountDone && root.accountTries < 3))
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
