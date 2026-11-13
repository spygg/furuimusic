#pragma once
// 福瑞音乐 - 开放 API 首页（网页版本机界面）
//
// 单文件、零外部依赖：不引用 CDN、字体、图标库，全部内联，断网 / 内网也能用。
// 布局照搬桌面版外壳：左侧「曲库 / 播放 / 账号 / 设置」四项导航 + 常驻底部播放栏，
// 曲库页保留搜索行、Tab 胶囊、封面卡片墙 / 列表两种排布，
// 手机等窄屏下自动折成底部导航（与 MainMobile 一致）。
#include <QByteArray>

namespace Furui {

inline QByteArray openApiIndexHtml()
{
    static const char kHtml[] = R"HTML(<!doctype html>
<html lang="zh-CN" data-theme="light">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>福瑞音乐 · 网页控制台</title>
<style>
  :root{
    --bg:#f5f6f8; --rail:#ffffff; --card:#ffffff; --card2:#fbfbfc; --line:#e6e8ec;
    --text:#1b2029; --sub:#5c6470; --accent:#22c55e; --accent-d:#16a34a;
    --soft:rgba(34,197,94,.12); --hover:rgba(0,0,0,.04);
    --shadow:0 1px 2px rgba(16,24,40,.05),0 10px 28px rgba(16,24,40,.06);
    --radius:16px;
    /* 表单控件（滑条、滚动条）跟随页面主题；交给 UA 自动判断会得到深色轨道 */
    color-scheme:light;
  }
  html[data-theme=dark]{
    --bg:#0f1115; --rail:#14161b; --card:#181a1f; --card2:#1d2026; --line:#2a2d35;
    --text:#eceef2; --sub:#9aa1ad; --accent:#22c55e; --accent-d:#34d399;
    --soft:rgba(34,197,94,.18); --hover:rgba(255,255,255,.06);
    --shadow:0 1px 2px rgba(0,0,0,.45);
    color-scheme:dark;
  }
  *{box-sizing:border-box}
  html,body{height:100%}
  body{margin:0;background:var(--bg);color:var(--text);
       font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;
       -webkit-font-smoothing:antialiased}
  button{font:inherit;color:inherit;border:0;background:none;cursor:pointer}
  input{font:inherit}
  a{color:var(--accent-d);text-decoration:none}
  ::-webkit-scrollbar{width:10px;height:10px}
  ::-webkit-scrollbar-thumb{background:var(--line);border-radius:99px;border:3px solid transparent;background-clip:padding-box}

  /* ---------- 外壳：左导航 + 内容 + 底栏 ---------- */
  .shell{display:grid;grid-template-columns:210px 1fr;grid-template-rows:1fr 78px;height:100vh}
  .rail{grid-row:1/3;display:flex;flex-direction:column;gap:6px;padding:16px 12px;
        background:var(--rail);border-right:1px solid var(--line)}
  .brand{display:flex;align-items:center;gap:10px;padding:4px 6px 16px}
  .mark{width:34px;height:34px;border-radius:11px;background:var(--accent);color:#fff;
        display:grid;place-items:center;font-weight:700;font-size:16px;flex:0 0 auto}
  .bname{font-weight:600;font-size:15px;display:flex;flex-direction:column;line-height:1.25}
  .bname em{font-style:normal;color:var(--sub);font-size:11px;font-weight:400}
  .nav{display:flex;align-items:center;gap:10px;padding:11px 12px;border-radius:12px;
       color:var(--sub);font-size:14px;text-align:left;transition:.15s}
  .nav:hover{background:var(--hover);color:var(--text)}
  .nav.on{background:var(--soft);color:var(--accent-d);font-weight:600}
  .nav .ico{width:20px;display:grid;place-items:center;font-size:15px}
  .nav .ico svg{width:17px;height:17px;display:block}
  .round svg,.iconbtn svg,.pctrl svg,.pbbtn svg{display:block}
  .railfoot{margin-top:auto;padding:10px 12px;border-top:1px solid var(--line);color:var(--sub);font-size:11px;
            display:flex;align-items:center;gap:6px;word-break:break-all}
  .dot{width:7px;height:7px;border-radius:99px;background:var(--sub);flex:0 0 auto}
  .dot.on{background:var(--accent);box-shadow:0 0 0 3px var(--soft)}

  .main{overflow-y:auto;overflow-x:hidden;padding:24px 28px 8px}
  .page{display:none;animation:fade .18s ease}
  .page.on{display:block}
  @keyframes fade{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}
  .pagetitle{font-size:20px;font-weight:700;margin:0 0 4px}
  .pagesub{color:var(--sub);font-size:12px;margin-bottom:16px}

  /* ---------- 曲库：搜索行 / Tab / 卡片 ---------- */
  .searchrow{display:flex;align-items:center;gap:10px;margin-bottom:14px;flex-wrap:wrap}
  .search{display:flex;align-items:center;gap:8px;flex:1 1 260px;max-width:520px;height:44px;
          background:var(--card);border:1px solid var(--line);border-radius:999px;padding:0 14px}
  .search:focus-within{border-color:var(--accent);box-shadow:0 0 0 3px var(--soft)}
  .search span{color:var(--sub);font-size:15px;cursor:pointer}
  .search input{flex:1;min-width:0;border:0;background:none;outline:none;color:var(--text);font-size:13px}
  .search input::placeholder{color:var(--sub)}
  .clr{color:var(--sub);font-size:12px;padding:4px 6px;border-radius:8px}
  .clr:hover{background:var(--hover)}
  .round{width:44px;height:44px;border-radius:999px;background:var(--accent);color:#fff;
         display:grid;place-items:center;flex:0 0 auto;font-size:16px}
  .round:hover{filter:brightness(1.06)}
  .spacer{flex:1}
  .chipbtn{height:44px;padding:0 18px;border-radius:999px;background:var(--accent);color:#fff;
           font-weight:600;font-size:13px;box-shadow:0 2px 8px rgba(34,197,94,.28)}
  .chipbtn:hover{filter:brightness(1.06)}
  .iconbtn{width:44px;height:44px;border-radius:999px;background:var(--card);border:1px solid var(--line);
           color:var(--text);display:grid;place-items:center;font-size:15px}
  .iconbtn:hover{background:var(--hover);color:var(--text)}
  .tabs{display:flex;gap:8px;overflow-x:auto;padding-bottom:6px;margin-bottom:14px;scrollbar-width:none}
  .tabs::-webkit-scrollbar{display:none}
  .tab{height:34px;padding:0 16px;border-radius:999px;background:var(--card);border:1px solid var(--line);
       color:var(--sub);font-size:13px;white-space:nowrap;flex:0 0 auto}
  .tab:hover{color:var(--text)}
  .tab.on{background:var(--accent);border-color:transparent;color:#fff;font-weight:600}
  .libbody{min-height:200px}

  .grid{display:grid;gap:16px;grid-template-columns:repeat(auto-fill,minmax(158px,1fr))}
  .card{background:var(--card);border:1px solid var(--line);border-radius:var(--radius);padding:10px;
        cursor:pointer;transition:.16s;box-shadow:var(--shadow);overflow:hidden}
  .card:hover{transform:translateY(-2px);border-color:rgba(34,197,94,.45)}
  .thumb{position:relative;width:100%;aspect-ratio:1/1;border-radius:12px;overflow:hidden;background:var(--soft)}
  .thumb img{position:relative;z-index:1;width:100%;height:100%;object-fit:cover;display:block}
  .thumb .ph{position:absolute;inset:0;z-index:0;display:grid;place-items:center;color:var(--accent-d);font-size:26px}
  .card .cname{margin-top:9px;font-size:13px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .card .csub{margin-top:3px;color:var(--sub);font-size:11px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .tagpill{position:absolute;left:8px;top:8px;background:rgba(0,0,0,.55);color:#fff;border-radius:999px;
           padding:2px 8px;font-size:10px;backdrop-filter:blur(4px)}

  .list{display:flex;flex-direction:column;gap:6px}
  .row{display:flex;align-items:center;gap:12px;padding:8px 10px;border-radius:12px;cursor:pointer;
       border:1px solid transparent;background:var(--card)}
  .row:hover{background:var(--card2);border-color:var(--line)}
  .row .idx{width:26px;text-align:right;color:var(--sub);font-size:12px;font-variant-numeric:tabular-nums;flex:0 0 auto}
  .row .art{width:42px;height:42px;border-radius:10px;overflow:hidden;background:var(--soft);flex:0 0 auto;
            display:grid;place-items:center;color:var(--accent-d)}
  .row .art img{width:100%;height:100%;object-fit:cover;display:block}
  .row .meta{min-width:0;flex:1}
  .row .n{display:block;font-size:13px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .row .a{display:block;color:var(--sub);font-size:11.5px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-top:2px}
  .row .d{color:var(--sub);font-size:11.5px;font-variant-numeric:tabular-nums;flex:0 0 auto}
  .row.playing .n{color:var(--accent-d)}
  .row .pill{font-size:10px;color:var(--accent-d);background:var(--soft);border-radius:999px;padding:1px 7px;flex:0 0 auto}
  .empty{color:var(--sub);font-size:13px;padding:46px 18px;text-align:center;line-height:2}
  .empty b{display:block;color:var(--text);font-size:14px;margin-bottom:2px}
  .backbar{display:flex;align-items:center;gap:12px;margin-bottom:12px}
  .back{height:34px;padding:0 14px;border-radius:999px;border:1px solid var(--line);color:var(--sub);font-size:12.5px}
  .back:hover{color:var(--text);border-color:var(--accent)}
  .detailtitle{font-weight:700;font-size:15px;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
  .detailcount{color:var(--sub);font-size:12px;flex:0 0 auto}

  /* ---------- 播放页 ---------- */
  .player{display:grid;gap:18px;grid-template-columns:minmax(280px,380px) minmax(0,1fr)}
  /* 网格子项默认 min-width:auto，会被内部 nowrap 文本撑到比容器还宽（窄屏就横向溢出）*/
  .player>*{min-width:0}
  .pcard{background:var(--card);border:1px solid var(--line);border-radius:20px;padding:18px;box-shadow:var(--shadow)}
  .pcover{width:100%;max-width:100%;aspect-ratio:1/1;border-radius:16px;object-fit:cover;background:var(--soft);display:block}
  .ptitle{font-size:19px;font-weight:700;margin-top:14px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .partist{color:var(--sub);font-size:13px;margin-top:3px}
  .pbadge{display:inline-block;margin-top:8px;font-size:11px;color:var(--accent-d);background:var(--soft);
          border-radius:999px;padding:2px 9px}
  .pbar{height:8px;border-radius:999px;background:var(--line);margin-top:16px;position:relative;cursor:pointer}
  .pbar>i{position:absolute;left:0;top:0;bottom:0;border-radius:999px;background:var(--accent);width:0;
          transition:width .2s linear}
  .ptime{display:flex;justify-content:space-between;color:var(--sub);font-size:11px;margin-top:5px;
         font-variant-numeric:tabular-nums}
  .pctrl{display:flex;align-items:center;gap:10px;margin-top:14px}
  .pctrl button{width:42px;height:42px;border-radius:999px;border:1px solid var(--line);background:var(--card2);
                display:grid;place-items:center;font-size:14px;color:var(--text)}
  .pctrl button:hover{border-color:var(--accent);color:var(--accent-d)}
  .pctrl button.primary{width:52px;height:52px;background:var(--accent);border-color:transparent;color:#fff;font-size:17px}
  .pmode{margin-left:auto;font-size:11.5px;color:var(--sub)}
  .vol{display:flex;align-items:center;gap:8px;color:var(--sub);font-size:11.5px;margin-top:12px}
  /* 滑条自己画：交给 UA 渲染时，轨道在部分 Chromium 版本会被画成深灰，和浅色主题冲突。
     --v 由 JS 按音量写进去，用它做「已填充」渐变 */
  input[type=range]{-webkit-appearance:none;appearance:none;height:18px;flex:1;min-width:80px;
                    background:transparent;outline:none}
  input[type=range]::-webkit-slider-runnable-track{height:6px;border-radius:99px;
      background:linear-gradient(to right,var(--accent) 0 var(--v,0%),var(--line) var(--v,0%) 100%)}
  input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:14px;height:14px;border-radius:99px;
      background:var(--accent);border:2px solid var(--card);box-shadow:0 0 0 2px var(--soft);
      margin-top:-4px;cursor:pointer}
  input[type=range]::-moz-range-track{height:6px;border-radius:99px;background:var(--line)}
  input[type=range]::-moz-range-progress{height:6px;border-radius:99px;background:var(--accent)}
  input[type=range]::-moz-range-thumb{width:12px;height:12px;border:0;border-radius:99px;background:var(--accent)}
  .vol input{flex:1}
  .lyricbox,.queuebox{background:var(--card);border:1px solid var(--line);border-radius:20px;padding:14px 16px;
                      box-shadow:var(--shadow);min-height:0}
  .boxhead{display:flex;align-items:center;gap:8px;font-size:12.5px;font-weight:600;color:var(--sub);margin-bottom:8px}
  .boxhead .grow{flex:1}
  .lyrics{max-height:calc(100vh - 300px);overflow:auto;padding:8px 4px;mask-image:linear-gradient(to bottom,
          transparent,#000 12%,#000 88%,transparent)}
  .ln{color:var(--text);opacity:.6;font-size:13.5px;padding:7px 2px;text-align:center;transition:.2s;line-height:1.55}
  .ln.act{opacity:1;color:var(--accent-d);font-size:15.5px;font-weight:600}
  .rightcol{display:flex;flex-direction:column;gap:14px;min-width:0}
  .qlist{max-height:300px;overflow:auto}

  /* ---------- 账号 / 设置 ---------- */
  .cards{display:grid;gap:14px;grid-template-columns:repeat(auto-fill,minmax(250px,1fr))}
  .acct{background:var(--card);border:1px solid var(--line);border-radius:var(--radius);padding:14px;
        display:flex;gap:12px;align-items:center;box-shadow:var(--shadow)}
  .ava{width:44px;height:44px;border-radius:14px;background:var(--soft);color:var(--accent-d);
       display:grid;place-items:center;font-weight:700;font-size:15px;flex:0 0 auto;overflow:hidden}
  .ava img{width:100%;height:100%;object-fit:cover}
  .acct .an{font-weight:600;font-size:13.5px}
  .acct .as{color:var(--sub);font-size:11.5px;margin-top:2px}
  .state{font-size:10.5px;border-radius:999px;padding:2px 8px;background:var(--hover);color:var(--sub)}
  .state.on{background:var(--soft);color:var(--accent-d)}
  .panel{background:var(--card);border:1px solid var(--line);border-radius:var(--radius);padding:16px;
         box-shadow:var(--shadow);margin-bottom:14px}
  .panel h3{margin:0 0 12px;font-size:13px;font-weight:700;color:var(--text)}
  .kv{display:grid;grid-template-columns:120px 1fr;gap:8px 14px;font-size:12.5px}
  .kv dt{color:var(--sub)}
  .kv dd{margin:0;word-break:break-all}
  .tag{display:inline-block;font-size:11px;background:var(--soft);color:var(--accent-d);border-radius:7px;
       padding:2px 8px;margin:0 6px 6px 0;font-family:ui-monospace,Menlo,Consolas,monospace}
  .btnrow{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:10px}
  .btnrow a,.btnrow button{height:32px;padding:0 12px;border-radius:9px;border:1px solid var(--line);
       background:var(--card2);color:var(--text);font-size:12.5px;display:inline-flex;align-items:center}
  .btnrow button:hover,.btnrow a:hover{border-color:var(--accent);color:var(--accent-d)}
  pre#log{margin:0;background:#0b0d11;color:#c9d1d9;border-radius:12px;padding:12px;font-size:12px;
          line-height:1.55;max-height:46vh;overflow:auto;white-space:pre-wrap;word-break:break-all;
          font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}

  /* ---------- 底栏 ---------- */
  .playbar{grid-column:2;display:flex;align-items:center;gap:14px;padding:0 20px;background:var(--rail);
           border-top:1px solid var(--line)}
  .pbcover{width:48px;height:48px;border-radius:12px;object-fit:cover;background:var(--soft);flex:0 0 auto}
  .pbmeta{min-width:0;width:190px}
  .pbmeta .t{font-size:13px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .pbmeta .a{color:var(--sub);font-size:11.5px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .pbbtn{width:36px;height:36px;border-radius:999px;display:grid;place-items:center;color:var(--text);font-size:13px}
  .pbbtn:hover{background:var(--hover)}
  .pbbtn.primary{width:42px;height:42px;background:var(--accent);color:#fff;font-size:15px}
  .pbprog{flex:1;display:flex;align-items:center;gap:10px;min-width:90px}
  .pbprog .bar{position:relative;flex:1;height:6px;border-radius:99px;background:var(--line);cursor:pointer}
  .pbprog .bar>i{position:absolute;left:0;top:0;bottom:0;border-radius:99px;background:var(--accent);width:0}
  .pbprog span{color:var(--sub);font-size:11px;font-variant-numeric:tabular-nums;flex:0 0 auto}
  .pbvol{display:flex;align-items:center;gap:8px;color:var(--sub);font-size:11.5px}
  .pbvol input{width:88px;flex:0 0 auto}

  #flash{position:fixed;left:50%;bottom:96px;transform:translate(-50%,10px);opacity:0;pointer-events:none;
         background:#1b2029;color:#fff;border-radius:999px;padding:8px 16px;font-size:13px;transition:.2s;z-index:9}
  #flash.show{opacity:.95;transform:translate(-50%,0)}
  .hide{display:none!important}

  /* ---------- 窄屏：折成底部导航（对齐 MainMobile） ---------- */
  @media (max-width:820px){
    .shell{grid-template-columns:1fr;grid-template-rows:1fr 70px 58px}
    .rail{grid-row:3;flex-direction:row;align-items:center;justify-content:space-around;padding:0 6px;
          border-right:0;border-top:1px solid var(--line)}
    .brand,.railfoot{display:none}
    .nav{flex-direction:column;gap:2px;padding:6px 2px;font-size:10.5px;border-radius:10px;flex:1;text-align:center;
         align-items:center}
    .nav .ico{font-size:16px}
    .main{padding:16px 14px 6px}
    .playbar{grid-column:1;grid-row:2;padding:0 12px;gap:10px}
    .pbmeta{width:auto;flex:1}
    .pbprog,.pbvol{display:none}
    .searchrow>.round{display:none}
    .player{grid-template-columns:minmax(0,1fr)}
    .lyrics{max-height:46vh}
  }
</style>
</head>
<body>
<div class="shell">
  <aside class="rail">
    <div class="brand">
      <div class="mark">福</div>
      <div class="bname">福瑞音乐<em id="ver">v</em></div>
    </div>
    <button class="nav on" data-page="library"><span class="ico" data-icon="music"></span><span>曲库</span></button>
    <button class="nav" data-page="player"><span class="ico" data-icon="playCircle"></span><span>播放</span></button>
    <button class="nav" data-page="account"><span class="ico" data-icon="user"></span><span>账号</span></button>
    <button class="nav" data-page="settings"><span class="ico" data-icon="sliders"></span><span>设置</span></button>
    <div class="railfoot"><span class="dot" id="dot"></span><span id="addr">连接中…</span></div>
  </aside>

  <div class="main">
    <!-- 曲库 -->
    <section class="page on" id="page-library">
      <div class="searchrow">
        <div class="search">
          <span id="searchIco">⌕</span>
          <input id="kw" placeholder="搜索歌曲、歌手或歌单" autocomplete="off">
          <button class="clr hide" id="kwclr" title="清空">✕</button>
        </div>
        <button class="round" id="btnSearch" title="搜索"></button>
        <div class="spacer"></div>
        <button class="chipbtn" id="btnRefresh">刷新歌单</button>
        <button class="iconbtn" id="btnView" title="切换列表 / 封面视图"></button>
      </div>
      <div class="tabs" id="tabs">
        <button class="tab on" data-tab="playlists">我的歌单</button>
        <button class="tab" data-tab="discovery">发现榜单</button>
        <button class="tab" data-tab="queue">播放队列</button>
        <button class="tab" data-tab="favorites">我的收藏</button>
        <button class="tab" data-tab="history">播放历史</button>
        <button class="tab hide" data-tab="results" id="tabResults">搜索结果</button>
      </div>
      <div class="libbody" id="libBody"><div class="empty">加载中…</div></div>
    </section>

    <!-- 播放 -->
    <section class="page" id="page-player">
      <h2 class="pagetitle">正在播放</h2>
      <div class="pagesub" id="playSub">与设备端实时同步</div>
      <div class="player">
        <div>
          <div class="pcard">
            <img class="pcover" id="pcover" alt="">
            <div class="ptitle" id="ptitle">未在播放</div>
            <div class="partist" id="partist">在手机或桌面端开始播放后，这里会同步显示</div>
            <span class="pbadge hide" id="pbadge"></span>
            <div class="pbar" id="pbar"><i id="pfill"></i></div>
            <div class="ptime"><span id="pcur">0:00</span><span id="pdur">0:00</span></div>
            <div class="pctrl">
              <button id="pprev" title="上一首"></button>
              <button class="primary" id="pplay" title="播放 / 暂停"></button>
              <button id="pnext" title="下一首"></button>
              <span class="pmode" id="pmode"></span>
            </div>
            <div class="vol"><span>音量</span><input type="range" id="pvol" min="0" max="100" value="100"></div>
          </div>
          <div class="queuebox" style="margin-top:14px">
            <div class="boxhead"><span>播放队列</span><span class="grow"></span><span id="qcount"></span></div>
            <div class="qlist" id="pqueue"></div>
          </div>
        </div>
        <div class="rightcol">
          <div class="lyricbox" style="flex:1">
            <div class="boxhead"><span>歌词</span></div>
            <div class="lyrics" id="plrc"><div class="empty">暂无歌词</div></div>
          </div>
        </div>
      </div>
    </section>

    <!-- 账号 -->
    <section class="page" id="page-account">
      <h2 class="pagetitle">账号</h2>
      <div class="pagesub">登录、扫码与退出需在设备端操作，这里只读展示各平台会话状态</div>
      <div class="cards" id="acctBody"><div class="empty">加载中…</div></div>
    </section>

    <!-- 设置 -->
    <section class="page" id="page-settings">
      <h2 class="pagetitle">设置</h2>
      <div class="pagesub">只读展示，改动请到设备端的设置页</div>
      <div class="panel"><h3>播放</h3><dl class="kv" id="setPlay"></dl></div>
      <div class="panel"><h3>开放 API</h3><dl class="kv" id="setApi"></dl>
        <div style="margin-top:12px" id="endpoints"></div>
      </div>
      <div class="panel"><h3>运行日志</h3><dl class="kv" id="setLog"></dl>
        <div class="btnrow" style="margin-top:12px">
          <button id="btnLogLoad">查看日志</button>
          <button id="btnLogCopy">复制</button>
          <a href="/logs?lines=2000&download=1">下载</a>
        </div>
        <pre id="log" class="hide">(暂无日志)</pre>
      </div>
      <div class="panel"><h3>关于</h3><dl class="kv" id="setAbout"></dl></div>
    </section>
  </div>

  <footer class="playbar">
    <img class="pbcover" id="pbcover" alt="">
    <div class="pbmeta">
      <div class="t" id="pbtitle">未在播放</div>
      <div class="a" id="pbsub">—</div>
    </div>
    <button class="pbbtn" id="pbprev" title="上一首"></button>
    <button class="pbbtn primary" id="pbplay" title="播放 / 暂停"></button>
    <button class="pbbtn" id="pbnext" title="下一首"></button>
    <div class="pbprog">
      <span id="pbcur">0:00</span>
      <div class="bar" id="pbbar"><i id="pbfill"></i></div>
      <span id="pbdur">0:00</span>
    </div>
    <div class="pbvol"><span>音量</span><input type="range" id="pbvol" min="0" max="100" value="100"></div>
  </footer>
</div>
<div id="flash"></div>

<script>
(function(){
  'use strict';
  var $ = function(s){ return document.querySelector(s); };
  var $$ = function(s){ return Array.prototype.slice.call(document.querySelectorAll(s)); };

  // 图标一律用内联 SVG：系统字体常把 ▦ / ⚙ / ⌕ 之类画成彩色表情，跨平台观感很差
  var ICON_PATHS = {
    search:'<circle cx="11" cy="11" r="6.4"/><path d="M16.1 16.1 20.4 20.4"/>',
    music:'<circle cx="7" cy="17.4" r="2.5"/><circle cx="17.6" cy="15.4" r="2.5"/><path d="M9.5 17.4V6.6l10.6-2v10.8"/>',
    playCircle:'<circle cx="12" cy="12" r="8.6"/><path d="M10.2 8.6 16 12l-5.8 3.4z" fill="currentColor" stroke="none"/>',
    user:'<circle cx="12" cy="8.4" r="3.5"/><path d="M5 19.8c.8-3.5 3.6-5.3 7-5.3s6.2 1.8 7 5.3"/>',
    sliders:'<path d="M4 8.2h8.4M17.4 8.2H20M4 15.8h3.4M12.4 15.8H20"/><circle cx="15" cy="8.2" r="2.4"/><circle cx="10" cy="15.8" r="2.4"/>',
    prev:'<path d="M7.2 6.2h2.3v11.6H7.2z" fill="currentColor" stroke="none"/><path d="M19 6.6v10.8L9.8 12z" fill="currentColor" stroke="none"/>',
    next:'<path d="M14.5 6.2h2.3v11.6h-2.3z" fill="currentColor" stroke="none"/><path d="M5 6.6v10.8L14.2 12z" fill="currentColor" stroke="none"/>',
    play:'<path d="M8.6 5.8v12.4L18.6 12z" fill="currentColor" stroke="none"/>',
    pause:'<rect x="7" y="5.4" width="3.5" height="13.2" rx="1.2" fill="currentColor" stroke="none"/><rect x="13.5" y="5.4" width="3.5" height="13.2" rx="1.2" fill="currentColor" stroke="none"/>',
    grid:'<rect x="3.4" y="3.4" width="7.2" height="7.2" rx="2" fill="currentColor" stroke="none"/><rect x="13.4" y="3.4" width="7.2" height="7.2" rx="2" fill="currentColor" stroke="none"/><rect x="3.4" y="13.4" width="7.2" height="7.2" rx="2" fill="currentColor" stroke="none"/><rect x="13.4" y="13.4" width="7.2" height="7.2" rx="2" fill="currentColor" stroke="none"/>',
    list:'<rect x="3.6" y="5" width="16.8" height="2.6" rx="1.3" fill="currentColor" stroke="none"/><rect x="3.6" y="10.7" width="16.8" height="2.6" rx="1.3" fill="currentColor" stroke="none"/><rect x="3.6" y="16.4" width="16.8" height="2.6" rx="1.3" fill="currentColor" stroke="none"/>'
  };
  function icon(name, size){
    var s = size || 18;
    return '<svg viewBox="0 0 24 24" width="' + s + '" height="' + s
      + '" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round">'
      + (ICON_PATHS[name] || '') + '</svg>';
  }

  var S = {
    page:'library', tab:'playlists', view:'grid',
    st:{}, settings:null, detailTitle:'', detailFrom:'playlists',
    playlists:[], discovery:[], queue:[], results:[], favs:[], hist:[],
    lyrics:[], lyricIdx:-1, logShown:false
  };

  function fmt(ms){
    ms = Math.max(0, Math.round(ms || 0));
    var t = Math.floor(ms / 1000);
    return Math.floor(t / 60) + ':' + String(t % 60).padStart(2, '0');
  }
  function esc(v){
    return String(v == null ? '' : v).replace(/[&<>"']/g, function(c){
      return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];
    });
  }
  function api(path){
    return fetch(path, {cache:'no-store'}).then(function(r){
      if(!r.ok) throw new Error(r.status);
      return r;
    });
  }
  function json(path){ return api(path).then(function(r){ return r.json(); }); }
  function flash(msg){
    var el = $('#flash');
    el.textContent = msg;
    el.className = 'show';
    clearTimeout(flash._t);
    flash._t = setTimeout(function(){ el.className = ''; }, 1800);
  }
  function ctl(path, okMsg){
    return api(path).then(function(){ if(okMsg) flash(okMsg); return refreshStatus(); })
                    .catch(function(){ flash('控制失败'); });
  }
  // 封面占位：取不到图时用音符色块，不留破图图标
  function coverHtml(url, cls){
    if(url) return '<img class="' + cls + '" src="' + esc(url) + '" alt="" loading="lazy" onerror="this.style.display=\'none\'">';
    return '';
  }

  /* ================= 页面切换 ================= */
  function go(page){
    S.page = page;
    $$('.nav').forEach(function(b){ b.classList.toggle('on', b.dataset.page === page); });
    $$('.page').forEach(function(p){ p.classList.toggle('on', p.id === 'page-' + page); });
    if(page === 'account') loadAccounts();
    if(page === 'settings') loadSettings();
    if(page === 'player') loadLyrics();
  }
  $$('.nav').forEach(function(b){ b.onclick = function(){ go(b.dataset.page); }; });

  /* ================= 曲库 ================= */
  function setTab(tab, opt){
    opt = opt || {};
    S.tab = tab;
    $$('.tab').forEach(function(b){ b.classList.toggle('on', b.dataset.tab === tab); });
    if(tab === 'results') $('#tabResults').classList.toggle('hide', !S.detailTitle);
    if(!opt.keep) S.detailTitle = opt.title || '';
    // 刷新按钮文案跟着 Tab 走，与桌面版一致
    $('#btnRefresh').textContent = tab === 'discovery' ? '刷新榜单' : '刷新歌单';
    renderLibrary();
    if(tab === 'discovery' && !S.discovery.length) loadPlaylists('discovery');
    if(tab === 'playlists' && !S.playlists.length) loadPlaylists('playlists');
    if(tab === 'queue') loadQueue();
    if(tab === 'favorites') loadSongs('favorites');
    if(tab === 'history') loadSongs('history');
  }
  $$('.tab').forEach(function(b){ b.onclick = function(){ if(b.dataset.tab === 'results' && !S.detailTitle) return; setTab(b.dataset.tab, {keep:true}); }; });

  $('#btnView').onclick = function(){
    S.view = S.view === 'grid' ? 'list' : 'grid';
    this.innerHTML = icon(S.view === 'grid' ? 'list' : 'grid', 17);
    this.title = S.view === 'grid' ? '切换为列表视图' : '切换为封面视图';
    renderLibrary();
  };
  $('#btnRefresh').onclick = function(){
    var p = (S.tab === 'discovery') ? '/refresh-discovery' : '/refresh';
    ctl(p, '已开始刷新').then(function(){
      setTimeout(function(){ loadPlaylists(S.tab === 'discovery' ? 'discovery' : 'playlists'); }, 1200);
    });
  };
  function doSearch(){
    var kw = $('#kw').value.trim();
    if(!kw){ flash('请输入关键词'); return; }
    api('/search?keyword=' + encodeURIComponent(kw)).then(function(){
      S.detailTitle = '搜索：' + kw;
      S.detailFrom = S.tab;
      $('#tabResults').classList.remove('hide');
      setTab('results', {keep:true});
      pollResults(8);
    }).catch(function(){ flash('搜索失败'); });
  }
  $('#btnSearch').onclick = doSearch;
  $('#searchIco').onclick = doSearch;
  $('#kw').addEventListener('keydown', function(e){ if(e.key === 'Enter') doSearch(); });
  $('#kw').addEventListener('input', function(){ $('#kwclr').classList.toggle('hide', !this.value); });
  $('#kwclr').onclick = function(){ $('#kw').value = ''; this.classList.add('hide'); $('#kw').focus(); };

  function loadPlaylists(which){
    var url = which === 'discovery' ? '/discovery' : '/playlists';
    return json(url).then(function(arr){
      if(which === 'discovery') S.discovery = arr || []; else S.playlists = arr || [];
      if(S.tab === which) renderLibrary();
    }).catch(function(){ if(S.tab === which) $('#libBody').innerHTML = '<div class="empty">读取歌单失败</div>'; });
  }
  function loadQueue(){
    return json('/queue').then(function(arr){
      S.queue = arr || [];
      if(S.tab === 'queue') renderLibrary();
      renderQueueBox();
    }).catch(function(){});
  }
  function loadSongs(kind){
    var url = kind === 'history' ? '/history' : '/favorites';
    return json(url).then(function(arr){
      if(kind === 'history') S.hist = arr || []; else S.favs = arr || [];
      if(S.tab === kind) renderLibrary();
    }).catch(function(){});
  }
  function pollResults(times){
    var n = 0;
    (function step(){
      json('/results').then(function(o){
        S.results = (o && o.songs) || [];
        renderLibrary();
        if(S.results.length === 0 && ++n < (times || 6)) setTimeout(step, 420);
      }).catch(function(){});
    })();
  }
  function openPlaylist(uid, title, from){
    S.detailTitle = title || '歌单';
    S.detailFrom = from;
    S.results = [];
    $('#tabResults').classList.remove('hide');
    setTab('results', {keep:true});
    var url = (from === 'discovery' ? '/open-discovery?id=' : '/open-playlist?id=') + encodeURIComponent(uid);
    api(url).then(function(){ pollResults(10); }).catch(function(){ flash('打开歌单失败'); });
  }

  function songRow(s, i, playPath, playing){
    var sub = [s.singer, s.sourceName || s.source].filter(Boolean).join(' · ');
    var art = s.cover ? '<img src="' + esc(s.cover) + '" alt="" loading="lazy" onerror="this.remove()">' : '♪';
    return '<div class="row' + (playing ? ' playing' : '') + '" data-play="' + i + '" data-path="' + playPath + '">'
      + '<span class="idx">' + (playing ? '▶' : (i + 1)) + '</span>'
      + '<span class="art">' + art + '</span>'
      + '<span class="meta"><span class="n">' + esc(s.name || '(未知曲目)') + '</span>'
      + '<span class="a">' + esc(sub) + '</span></span>'
      + (s.vip ? '<span class="pill">VIP</span>' : '')
      + '<span class="d">' + fmt(s.duration) + '</span></div>';
  }
  function songList(arr, playPath, currentIndex){
    if(!arr || !arr.length) return '';
    var h = '<div class="list">';
    arr.forEach(function(s, i){
      h += songRow(s, i, playPath, typeof currentIndex === 'number' && i === currentIndex);
    });
    return h + '</div>';
  }
  function cardGrid(arr, kind){
    if(!arr || !arr.length) return '';
    var h = '<div class="grid">';
    arr.forEach(function(p){
      h += '<div class="card" data-kind="' + kind + '" data-id="' + esc(p.id) + '" data-title="' + esc(p.name) + '">'
        + '<div class="thumb">' + (p.cover ? '<img src="' + esc(p.cover) + '" alt="" loading="lazy" onerror="this.remove()">' : '')
        + '<span class="ph">♪</span>'
        + (p.sourceName ? '<span class="tagpill">' + esc(p.sourceName) + '</span>' : '') + '</div>'
        + '<div class="cname">' + esc(p.name || '未命名歌单') + '</div>'
        + '<div class="csub">' + esc([p.count > 0 ? p.count + ' 首' : '', p.sourceName, p.owner].filter(Boolean).join(' · ') || '歌单')
        + '</div></div>';
    });
    return h + '</div>';
  }
  function cardList(arr, kind){
    if(!arr || !arr.length) return '';
    var h = '<div class="list">';
    arr.forEach(function(p){
      h += '<div class="row" data-kind="' + kind + '" data-id="' + esc(p.id) + '" data-title="' + esc(p.name) + '">'
        + '<span class="art">' + (p.cover ? '<img src="' + esc(p.cover) + '" alt="" loading="lazy" onerror="this.remove()">' : '♪') + '</span>'
        + '<span class="meta"><span class="n">' + esc(p.name || '未命名歌单') + '</span>'
        + '<span class="a">' + esc([p.count > 0 ? p.count + ' 首' : '', p.owner].filter(Boolean).join(' · ') || '歌单') + '</span></span>'
        + '<span class="d">' + esc(p.sourceName || '') + '</span></div>';
    });
    return h + '</div>';
  }
  function emptyBox(title, hint){
    return '<div class="empty"><b>' + esc(title) + '</b>' + esc(hint || '') + '</div>';
  }

  function renderLibrary(){
    var box = $('#libBody');
    var body = '';
    var detailOpen = (S.tab === 'results' && S.detailTitle);
    if(detailOpen){
      body += '<div class="backbar"><button class="back" id="btnBack">← 返回</button>'
            + '<span class="detailtitle">' + esc(S.detailTitle) + '</span>'
            + '<span class="detailcount">' + S.results.length + ' 首</span></div>';
      body += S.results.length ? songList(S.results, '/play-result')
                               : emptyBox('正在加载曲目…', '歌单曲目由设备端拉取，稍等片刻');
    } else if(S.tab === 'playlists' || S.tab === 'discovery'){
      var arr = S.tab === 'discovery' ? S.discovery : S.playlists;
      var kind = S.tab;
      if(!arr.length){
        body = emptyBox(S.tab === 'discovery' ? '还没有公开榜单' : '还没有歌单',
                        S.tab === 'discovery' ? '点右上角「刷新歌单」重新拉取' : '在设备端登录账号后点「刷新歌单」');
      } else {
        body = S.view === 'grid' ? cardGrid(arr, kind) : cardList(arr, kind);
      }
    } else if(S.tab === 'queue'){
      body = S.queue.length ? songList(S.queue, '/play-index', S.st.currentIndex)
                            : emptyBox('播放队列是空的', '在曲库里点歌加入队列');
    } else if(S.tab === 'favorites'){
      body = S.favs.length ? songList(S.favs, '', -1) : emptyBox('还没有收藏', '在设备端点歌曲行尾的心形按钮');
    } else if(S.tab === 'history'){
      body = S.hist.length ? songList(S.hist, '', -1) : emptyBox('还没有播放记录', '播放过的歌曲会出现在这里');
    }
    box.innerHTML = body;
    if(detailOpen){
      var back = $('#btnBack');
      if(back) back.onclick = function(){ setTab(S.detailFrom || 'playlists', {keep:false}); };
    }
    $$('#libBody [data-play]').forEach(function(el){
      el.onclick = function(){
        var path = el.dataset.path, i = el.dataset.play;
        if(!path) return;
        ctl(path + '?index=' + i, null);
      };
    });
    $$('#libBody [data-kind]').forEach(function(el){
      el.onclick = function(){ openPlaylist(el.dataset.id, el.dataset.title, el.dataset.kind); };
    });
  }

  /* ================= 状态 / 底栏 / 播放页 ================= */
  function paintProgress(st){
    var dur = st.duration || 0, pos = st.progress || 0;
    var pct = dur > 0 ? Math.min(100, pos / dur * 100) : 0;
    $('#pfill').style.width = pct.toFixed(2) + '%';
    $('#pbfill').style.width = pct.toFixed(2) + '%';
    $('#pcur').textContent = fmt(pos); $('#pdur').textContent = fmt(dur);
    $('#pbcur').textContent = fmt(pos); $('#pbdur').textContent = fmt(dur);
  }
  function paint(st){
    if(!st) return;
    S.st = st;
    var has = !!(st.name && st.name.length);
    var playing = st.status === 'playing';
    var badge = [st.source, st.substitute].filter(Boolean).join(' · ');
    var artist = has ? (st.singer || '未知艺术家') : '在手机或桌面端开始播放后，这里会同步显示';

    $('#ptitle').textContent = has ? st.name : '未在播放';
    $('#partist').textContent = artist;
    $('#pbadge').textContent = badge;
    $('#pbadge').classList.toggle('hide', !badge);
    $('#pbtitle').textContent = has ? st.name : '未在播放';
    $('#pbsub').textContent = has ? [st.singer, st.source].filter(Boolean).join(' · ') : '—';
    $('#pplay').innerHTML = icon(playing ? 'pause' : 'play', 19);
    $('#pbplay').innerHTML = icon(playing ? 'pause' : 'play', 16);
    $('#pmode').textContent = st.playMode || '';
    $('#playSub').textContent = playing ? ('播放中 · ' + (st.playMode || '')) : (has ? '已暂停' : '与设备端实时同步');
    var dot = $('#dot');
    dot.className = 'dot' + (playing ? ' on' : '');
    if(st.version) $('#ver').textContent = 'v' + st.version;

    var cover = st.cover || '';
    if(cover){ $('#pcover').src = cover; $('#pbcover').src = cover;
               $('#pcover').style.background = $('#pbcover').style.background = 'transparent'; }
    if(typeof st.volume === 'number') syncVolume(st.volume);
    paintProgress(st);
    highlightLyric(st.lyricLineText);
    // 换歌就重拉一次歌词：只在进播放页时拉会让「点开时刚好没歌」的页面一直空着
    if(st.name !== S.lastName){ S.lastName = st.name; loadLyrics(); }
    // 队列高亮跟着当前曲目走
    if(S.tab === 'queue' && typeof st.currentIndex === 'number') renderLibrary();
  }
  function refreshStatus(){
    return json('/status').then(paint).catch(function(){});
  }

  function renderQueueBox(){
    var box = $('#pqueue');
    $('#qcount').textContent = S.queue.length ? S.queue.length + ' 首' : '';
    box.innerHTML = S.queue.length ? songList(S.queue, '/play-index', S.st.currentIndex)
                                   : '<div class="empty" style="padding:18px">队列是空的</div>';
    $$('#pqueue [data-play]').forEach(function(el){
      el.onclick = function(){ ctl(el.dataset.path + '?index=' + el.dataset.play, null); };
    });
  }

  // 进度条：播放页与底栏共用一套点击拖动逻辑
  function bindSeek(barId){
    var bar = $(barId);
    function seekAt(clientX){
      var r = bar.getBoundingClientRect();
      var ratio = Math.min(1, Math.max(0, (clientX - r.left) / (r.width || 1)));
      if((S.st.duration || 0) > 0) api('/seek?time=' + Math.round(ratio * S.st.duration)).then(refreshStatus);
    }
    bar.addEventListener('pointerdown', function(e){ seekAt(e.clientX); });
  }
  bindSeek('#pbar'); bindSeek('#pbbar');

  $('#pplay').onclick = $('#pbplay').onclick = function(){ ctl('/toggle'); };
  $('#pprev').onclick = $('#pbprev').onclick = function(){ ctl('/skip-prev'); };
  $('#pnext').onclick = $('#pbnext').onclick = function(){ ctl('/skip-next'); };
  // 两个音量条（播放页 / 底栏）共用一份值；--v 用来画填充比例
  function syncVolume(v){
    [$('#pvol'), $('#pbvol')].forEach(function(el){
      if(!el) return;
      if(document.activeElement !== el) el.value = v;
      el.style.setProperty('--v', (el.value || 0) + '%');
    });
  }
  function bindVolume(id){
    var el = $(id), t = null;
    el.oninput = function(){
      clearTimeout(t);
      var v = this.value;
      syncVolume(v);
      t = setTimeout(function(){ api('/volume?value=' + v); }, 150);
    };
  }
  bindVolume('#pvol'); bindVolume('#pbvol');

  /* ================= 歌词 ================= */
  function loadLyrics(){
    return json('/lyrics').then(function(o){
      S.lyrics = (o && o.lines) || [];
      S.lyricIdx = o ? (o.index >= 0 ? o.index : -1) : -1;
      var box = $('#plrc');
      if(!S.lyrics.length){
        box.innerHTML = '<div class="empty">暂无歌词</div>';
        return;
      }
      box.innerHTML = S.lyrics.map(function(l, i){
        return '<div class="ln" data-i="' + i + '">' + esc(l.text || '·') + '</div>';
      }).join('');
      markLyric(S.lyricIdx);
    }).catch(function(){});
  }
  function markLyric(idx){
    if(idx === S.lyricIdx && $('.ln.act')) return;
    S.lyricIdx = idx;
    $$('#plrc .ln').forEach(function(el){
      el.classList.toggle('act', parseInt(el.dataset.i, 10) === idx);
    });
    var box = $('#plrc'), act = $('#plrc .ln.act');
    // 只滚歌词容器：scrollIntoView 会连带滚动外层 .main（窄屏上会把整页横向/纵向拖走）
    if(act && box){
      var top = act.offsetTop - box.clientHeight / 2 + act.offsetHeight / 2;
      box.scrollTo({top: Math.max(0, top), behavior:'smooth'});
    }
  }
  function highlightLyric(text){
    // 兜底：没有时间轴歌词时只能按行文本匹配
    if(S.lyrics.length && S.lyrics[0].t !== undefined) return;
    if(!text) return;
    $$('#plrc .ln').forEach(function(el){ el.classList.toggle('act', el.textContent === text); });
  }

  /* ================= 账号 ================= */
  function loadAccounts(){
    return json('/accounts').then(function(arr){
      var box = $('#acctBody');
      if(!arr || !arr.length){ box.innerHTML = emptyBox('还没有账号', '在设备端扫码或登录后这里会显示'); return; }
      box.innerHTML = arr.map(function(a){
        var ava = a.avatar ? '<img src="' + esc(a.avatar) + '" alt="" onerror="this.remove()">'
                           : esc((a.sourceName || a.source || '?').slice(0, 1));
        var label = a.nickname || a.sourceName || a.source;
        var sub = a.nickname ? a.sourceName : '未设置昵称';
        return '<div class="acct"><div class="ava">' + ava + '</div><div style="min-width:0;flex:1">'
          + '<div class="an">' + esc(label) + '</div>'
          + '<div class="as">' + esc([sub, a.logged ? '' : '在设备端可扫码登录'].filter(Boolean).join(' · ')) + '</div></div>'
          + '<span class="state' + (a.logged ? ' on' : '') + '">' + esc(a.status || (a.logged ? '已登录' : '未登录')) + '</span></div>';
      }).join('');
    }).catch(function(){ $('#acctBody').innerHTML = emptyBox('读取账号失败', ''); });
  }

  /* ================= 设置 ================= */
  function kv(obj){
    return Object.keys(obj).map(function(k){
      return '<dt>' + esc(k) + '</dt><dd>' + esc(obj[k]) + '</dd>';
    }).join('');
  }
  function loadSettings(){
    return json('/settings').then(function(o){
      S.settings = o;
      if(o.theme) document.documentElement.dataset.theme = o.theme === 'dark' ? 'dark' : 'light';
      $('#setPlay').innerHTML = kv({
        '音质': o.quality, '播放模式': o.playMode,
        'VIP 替代音源': o.vipSubstitute ? '已开启' : '已关闭',
        '自动换源': o.autoToggle ? '已开启' : '已关闭'
      });
      $('#setApi').innerHTML = kv({
        '服务状态': o.openApi ? '运行中' : '未开启',
        '访问地址': o.address || '—',
        '仅本机': o.localOnly ? '是' : '否'
      });
      $('#addr').textContent = o.address || (location.host || '');
      $('#setLog').innerHTML = kv({
        '写入日志文件': o.logToFile ? '已开启' : '仅内存缓冲',
        '日志路径': o.logPath || '—'
      });
      $('#setAbout').innerHTML = kv({ '应用': o.appName || '福瑞音乐', '版本': o.version || '', '音源': o.sources || '' });
      var eps = ['/status','/lyrics','/queue','/playlists','/discovery','/results','/accounts','/settings',
                 '/toggle','/skip-next','/skip-prev','/seek?time=ms','/volume?value=0-100',
                 '/play-index?index=n','/search?keyword=','/refresh','/logs','/help'];
      $('#endpoints').innerHTML = eps.map(function(e){ return '<span class="tag">' + esc(e) + '</span>'; }).join('');
    }).catch(function(){});
  }
  $('#btnLogLoad').onclick = function(){
    var pre = $('#log');
    pre.classList.remove('hide');
    api('/logs?lines=400').then(function(r){ return r.text(); }).then(function(t){
      pre.textContent = t && t.length ? t : '(暂无日志)';
      pre.scrollTop = pre.scrollHeight;
    }).catch(function(){ pre.textContent = '(读取日志失败)'; });
  };
  $('#btnLogCopy').onclick = function(){
    var pre = $('#log');
    if(pre.classList.contains('hide')){ flash('先点「查看日志」'); return; }
    var text = pre.textContent || '';
    var done = function(){ flash('已复制到剪贴板'); };
    var fail = function(){ flash('复制失败，请手动选择文本'); };
    // http://<内网 IP> 不是安全上下文，navigator.clipboard 不可用，必须回退 execCommand
    if(navigator.clipboard && window.isSecureContext){
      navigator.clipboard.writeText(text).then(done).catch(function(){ legacyCopy(text) ? done() : fail(); });
    } else {
      legacyCopy(text) ? done() : fail();
    }
  };
  function legacyCopy(text){
    try{
      var ta = document.createElement('textarea');
      ta.value = text;
      ta.setAttribute('readonly', '');
      ta.style.position = 'fixed';
      ta.style.top = '-1000px';
      document.body.appendChild(ta);
      ta.select();
      ta.setSelectionRange(0, text.length);
      var ok = document.execCommand('copy');
      document.body.removeChild(ta);
      return ok;
    }catch(e){ return false; }
  }

  /* ================= 状态推送：优先 SSE，失败退回轮询 ================= */
  function connect(){
    if(!window.EventSource){ setInterval(refreshStatus, 2000); return; }
    var es;
    try { es = new EventSource('/subscribe-player-status'); } catch(e){ setInterval(refreshStatus, 2000); return; }
    es.addEventListener('status', function(e){
      try { paint(JSON.parse(e.data)); } catch(err){}
    });
    es.onerror = function(){
      // EventSource 自带重连，但被代理断开时不一定恢复；显式重建更稳
      try { es.close(); } catch(e){}
      setTimeout(connect, 2500);
    };
  }

  /* ================= 启动 ================= */
  $('#addr').textContent = location.host;
  // 静态图标先铺一遍，播放/暂停等随状态变化的在 paint() 里更新
  $$('.ico[data-icon]').forEach(function(el){ el.innerHTML = icon(el.dataset.icon, 17); });
  $('#btnSearch').innerHTML = icon('search', 18);
  $('#btnView').innerHTML = icon('list', 17);
  $('#pprev').innerHTML = icon('prev', 15); $('#pnext').innerHTML = icon('next', 15);
  $('#pbprev').innerHTML = icon('prev', 13); $('#pbnext').innerHTML = icon('next', 13);
  $('#pplay').innerHTML = icon('play', 19); $('#pbplay').innerHTML = icon('play', 16);
  refreshStatus();
  loadPlaylists('playlists');
  loadQueue();
  loadSettings();
  connect();
  // 低频兜底：SSE 断掉时队列/进度也不会一直停在旧值
  setInterval(function(){
    if(S.page === 'library' && S.tab === 'queue') loadQueue();
  }, 15000);
  // 直接从某个页面打开：/?page=player / ?page=settings&tab=log
  var q = location.search || '';
  var mPage = /[?&]page=(\w+)/.exec(q);
  if(mPage && document.getElementById('page-' + mPage[1])) go(mPage[1]);
  if(q.indexOf('tab=log') >= 0){
    go('settings');
    setTimeout(function(){ $('#btnLogLoad').click(); }, 300);
  }
})();
</script>
</body>
</html>
)HTML";
    return QByteArray(kHtml);
}

} // namespace Furui
