#pragma once
// 福瑞音乐 - 开放 API 首页（网页版本机界面）
//
// 单文件、零外部依赖：不引用 CDN、字体、图标库，全部内联，断网 / 内网也能用。
// 打开 http://<设备局域网 IP>:23330/ 即可看到当前播放状态、控制播放、浏览队列，
// 以及查看并一键拷贝运行日志（手机上没有控制台时这是最方便的排障入口）。
#include <QByteArray>

namespace Furui {

inline QByteArray openApiIndexHtml()
{
    static const char kHtml[] = R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>福瑞音乐 · 控制台</title>
<style>
  :root{
    --bg:#f5f6f8; --card:#fff; --line:#e6e8ec; --text:#1b2029; --sub:#5c6470;
    --accent:#22c55e; --accent-soft:rgba(34,197,94,.12); --shadow:0 1px 2px rgba(16,24,40,.06),0 8px 24px rgba(16,24,40,.06);
  }
  @media (prefers-color-scheme:dark){
    :root{ --bg:#0f1115; --card:#181a1f; --line:#2a2d35; --text:#eceef2; --sub:#9aa1ad;
           --accent-soft:rgba(34,197,94,.18); --shadow:0 1px 2px rgba(0,0,0,.4); }
  }
  *{box-sizing:border-box}
  html,body{margin:0;padding:0;background:var(--bg);color:var(--text)}
  body{font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;
       padding:16px;padding-bottom:32px}
  .wrap{max-width:760px;margin:0 auto}
  header{display:flex;align-items:center;gap:10px;margin-bottom:14px;flex-wrap:wrap}
  .logo{width:28px;height:28px;border-radius:9px;background:var(--accent);color:#fff;
        display:flex;align-items:center;justify-content:center;font-size:15px;font-weight:700}
  h1{font-size:15px;margin:0;font-weight:600}
  .ver{color:var(--sub);font-size:12px;font-weight:400}
  .pill{margin-left:auto;font-size:12px;color:var(--sub);background:var(--card);border:1px solid var(--line);
        border-radius:999px;padding:3px 10px}
  .pill.on{color:#15a34a;background:var(--accent-soft);border-color:transparent}
  .card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:14px;box-shadow:var(--shadow)}
  .now{display:flex;gap:14px;align-items:center}
  .cover{width:84px;height:84px;border-radius:12px;object-fit:cover;background:var(--accent-soft);
         flex:0 0 auto;display:block}
  .info{min-width:0;flex:1}
  .title{font-size:17px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .artist{color:var(--sub);font-size:13px;margin-top:2px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .badge{display:inline-block;margin-top:6px;font-size:11px;color:#15a34a;background:var(--accent-soft);
         border-radius:999px;padding:2px 8px}
  .lyric{margin-top:12px;color:var(--sub);font-size:13px;text-align:center;min-height:18px}
  .bar{height:8px;border-radius:999px;background:var(--line);margin-top:12px;position:relative;cursor:pointer}
  .bar>i{position:absolute;left:0;top:0;bottom:0;border-radius:999px;background:var(--accent);width:0;transition:width .18s linear}
  .times{display:flex;justify-content:space-between;color:var(--sub);font-size:11px;margin-top:4px;font-variant-numeric:tabular-nums}
  .controls{display:flex;align-items:center;gap:10px;margin-top:12px;flex-wrap:wrap}
  button{font:inherit;color:var(--text);background:var(--card);border:1px solid var(--line);border-radius:10px;
         height:36px;min-width:36px;padding:0 12px;cursor:pointer;transition:.15s}
  button:hover{border-color:var(--accent);color:#15a34a}
  button.primary{background:var(--accent);border-color:var(--accent);color:#fff;min-width:48px}
  button.primary:hover{filter:brightness(1.05);color:#fff}
  .vol{display:flex;align-items:center;gap:8px;margin-left:auto;color:var(--sub);font-size:12px}
  input[type=range]{width:96px;accent-color:var(--accent)}
  .tabs{display:flex;gap:8px;margin:16px 0 10px}
  .tabs button{height:32px;border-radius:999px;font-size:13px;background:transparent;border-color:transparent;color:var(--sub)}
  .tabs button.on{background:var(--accent-soft);color:#15a34a;font-weight:600;border-color:transparent}
  .row{display:flex;align-items:center;gap:10px;padding:8px 6px;border-radius:10px;cursor:pointer}
  .row:hover{background:var(--accent-soft)}
  .row .idx{width:22px;color:var(--sub);font-size:12px;text-align:right;flex:0 0 auto}
  .row .meta{min-width:0;flex:1}
  .row .n{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .row .a{color:var(--sub);font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
  .row .d{color:var(--sub);font-size:12px;font-variant-numeric:tabular-nums;flex:0 0 auto}
  .empty{color:var(--sub);font-size:13px;padding:18px;text-align:center}
  .logbar{display:flex;gap:8px;margin-bottom:8px;flex-wrap:wrap}
  pre#log{margin:0;background:#0b0d11;color:#c9d1d9;border-radius:12px;padding:12px;font-size:12px;
          line-height:1.55;max-height:52vh;overflow:auto;white-space:pre-wrap;word-break:break-all;
          font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
  footer{color:var(--sub);font-size:12px;text-align:center;margin-top:18px;line-height:1.9}
  code{background:var(--accent-soft);color:#15a34a;border-radius:6px;padding:1px 6px;font-size:11px}
  #flash{position:fixed;left:50%;bottom:24px;transform:translate(-50%,12px);opacity:0;pointer-events:none;
         background:#1b2029;color:#fff;border-radius:999px;padding:8px 16px;font-size:13px;transition:.2s}
  #flash.show{opacity:.94;transform:translate(-50%,0)}
  .hide{display:none!important}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <div class="logo">福</div>
    <h1>福瑞音乐 <span class="ver" id="ver"></span></h1>
    <span class="pill" id="pill">连接中</span>
  </header>

  <section class="card">
    <div class="now">
      <img class="cover" id="cover" alt="" src="data:image/svg+xml;utf8,%3Csvg xmlns='http://www.w3.org/2000/svg'/%3E">
      <div class="info">
        <div class="title" id="title">未在播放</div>
        <div class="artist" id="artist">在手机或桌面端开始播放后，这里会同步显示</div>
        <span class="badge hide" id="badge"></span>
      </div>
    </div>
    <div class="lyric" id="lyric"></div>
    <div class="bar" id="bar"><i id="fill"></i></div>
    <div class="times"><span id="cur">0:00</span><span id="dur">0:00</span></div>
    <div class="controls">
      <button id="btnPrev" title="上一首">◀◀</button>
      <button class="primary" id="btnPlay" title="播放/暂停">▶</button>
      <button id="btnNext" title="下一首">▶▶</button>
      <div class="vol"><span>音量</span><input type="range" id="vol" min="0" max="100" value="100"></div>
    </div>
  </section>

  <div class="tabs">
    <button id="tabQueue" class="on">播放队列</button>
    <button id="tabLog">运行日志</button>
  </div>

  <section class="card" id="paneQueue"><div id="queue"><div class="empty">加载中…</div></div></section>

  <section class="card hide" id="paneLog">
    <div class="logbar">
      <button id="btnReload">刷新</button>
      <button id="btnCopy">复制日志</button>
      <a href="/logs?lines=2000&download=1"><button>下载</button></a>
      <button id="btnClearView">清空显示</button>
    </div>
    <pre id="log">(暂无日志)</pre>
  </section>

  <footer>
    本页由福瑞音乐本机服务提供 · 控制端点：<code>/play</code> <code>/pause</code>
    <code>/skip-next</code> <code>/seek?time=ms</code> <code>/volume?value=0-100</code><br>
    接口说明见 <a href="/help">/help</a>
  </footer>
</div>
<div id="flash"></div>

<script>
(function(){
  var $ = function(s){ return document.querySelector(s); };
  var state = { dur:0, pos:0, playing:false };
  var lastCover = '';

  function fmt(ms){
    ms = Math.max(0, Math.round(ms || 0));
    var t = Math.floor(ms/1000);
    return Math.floor(t/60) + ':' + String(t%60).padStart(2,'0');
  }
  function api(path){
    return fetch(path, {cache:'no-store'}).then(function(r){
      if(!r.ok) throw new Error(r.status);
      return r;
    });
  }
  function flash(msg){
    var el = $('#flash');
    el.textContent = msg;
    el.className = 'show';
    clearTimeout(flash._t);
    flash._t = setTimeout(function(){ el.className = ''; }, 1800);
  }
  function paintProgress(){
    var pct = state.dur > 0 ? Math.min(100, state.pos / state.dur * 100) : 0;
    $('#fill').style.width = pct.toFixed(2) + '%';
    $('#cur').textContent = fmt(state.pos);
    $('#dur').textContent = fmt(state.dur);
  }

  function paint(st){
    if(!st) return;
    var has = !!(st.name && st.name.length);
    state.dur = st.duration || 0;
    state.pos = st.progress || 0;
    state.playing = st.status === 'playing';

    $('#title').textContent = has ? st.name : '未在播放';
    $('#artist').textContent = has ? (st.singer || '未知艺术家')
                                   : '在手机或桌面端开始播放后，这里会同步显示';
    var badge = [st.source, st.substitute].filter(Boolean).join(' · ');
    $('#badge').textContent = badge;
    $('#badge').className = 'badge' + (badge ? '' : ' hide');
    $('#lyric').textContent = st.lyricLineText || '';

    $('#btnPlay').textContent = state.playing ? '❚❚' : '▶';
    var pill = $('#pill');
    pill.textContent = state.playing ? '播放中' : (has ? '已暂停' : '空闲');
    pill.className = 'pill' + (state.playing ? ' on' : '');
    if (st.version) $('#ver').textContent = 'v' + st.version;
    if (typeof st.volume === 'number' && document.activeElement !== $('#vol'))
      $('#vol').value = st.volume;
    if (st.cover && st.cover !== lastCover) { lastCover = st.cover; $('#cover').src = st.cover; }
    paintProgress();
  }

  // ---- 控制 ----
  $('#btnPlay').onclick = function(){ api('/toggle').then(refresh).catch(function(){ flash('控制失败'); }); };
  $('#btnPrev').onclick = function(){ api('/skip-prev').then(refresh).catch(function(){ flash('控制失败'); }); };
  $('#btnNext').onclick = function(){ api('/skip-next').then(refresh).catch(function(){ flash('控制失败'); }); };
  $('#bar').addEventListener('pointerdown', function(e){
    var r = $('#bar').getBoundingClientRect();
    var ratio = Math.min(1, Math.max(0, (e.clientX - r.left) / (r.width || 1)));
    if (state.dur > 0) api('/seek?time=' + Math.round(ratio * state.dur)).then(refresh);
  });
  var volTimer = null;
  $('#vol').oninput = function(){
    var v = this.value;
    clearTimeout(volTimer);
    volTimer = setTimeout(function(){ api('/volume?value=' + v); }, 160);
  };

  function refresh(){
    return api('/status').then(function(r){ return r.json(); }).then(paint).catch(function(){});
  }

  // ---- 队列 ----
  function loadQueue(){
    return api('/queue').then(function(r){ return r.json(); }).then(function(arr){
      var box = $('#queue');
      box.innerHTML = '';
      if(!arr || !arr.length){
        box.innerHTML = '<div class="empty">播放队列是空的</div>';
        return;
      }
      arr.forEach(function(s, i){
        var row = document.createElement('div');
        row.className = 'row';
        var idx = document.createElement('span'); idx.className = 'idx'; idx.textContent = (i+1);
        var meta = document.createElement('div'); meta.className = 'meta';
        var n = document.createElement('div'); n.className = 'n'; n.textContent = s.name || '(未知曲目)';
        var a = document.createElement('div'); a.className = 'a';
        a.textContent = [s.singer, s.source].filter(Boolean).join(' · ');
        var d = document.createElement('span'); d.className = 'd'; d.textContent = fmt(s.duration);
        meta.appendChild(n); meta.appendChild(a);
        row.appendChild(idx); row.appendChild(meta); row.appendChild(d);
        row.onclick = function(){ api('/play-index?index=' + i).then(refresh); };
        box.appendChild(row);
      });
    }).catch(function(){
      $('#queue').innerHTML = '<div class="empty">读取队列失败</div>';
    });
  }

  // ---- 日志 ----
  function loadLogs(){
    return api('/logs?lines=400').then(function(r){ return r.text(); }).then(function(t){
      var pre = $('#log');
      pre.textContent = t && t.length ? t : '(暂无日志)';
      pre.scrollTop = pre.scrollHeight;
    }).catch(function(){ $('#log').textContent = '(读取日志失败)'; });
  }
  function copyLogs(){
    var text = $('#log').textContent || '';
    var done = function(){ flash('已复制到剪贴板'); };
    var fail = function(){ flash('复制失败，请手动选择文本'); };
    // http://<内网 IP> 不是安全上下文，navigator.clipboard 不可用，必须回退 execCommand
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(done).catch(function(){ legacyCopy(text) ? done() : fail(); });
    } else {
      legacyCopy(text) ? done() : fail();
    }
  }
  function legacyCopy(text){
    try {
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
    } catch(e){ return false; }
  }
  $('#btnReload').onclick = function(){ loadLogs(); flash('已刷新'); };
  $('#btnCopy').onclick = copyLogs;
  $('#btnClearView').onclick = function(){ $('#log').textContent = ''; };

  // ---- Tab ----
  var logLoaded = false;
  $('#tabQueue').onclick = function(){
    this.classList.add('on'); $('#tabLog').classList.remove('on');
    $('#paneQueue').classList.remove('hide'); $('#paneLog').classList.add('hide');
    loadQueue();
  };
  $('#tabLog').onclick = function(){
    this.classList.add('on'); $('#tabQueue').classList.remove('on');
    $('#paneLog').classList.remove('hide'); $('#paneQueue').classList.add('hide');
    loadLogs(); logLoaded = true;
  };

  // ---- 状态推送：优先 SSE，失败退回轮询 ----
  var es = null;
  function connect(){
    if (!window.EventSource) { setInterval(refresh, 2000); return; }
    try { es = new EventSource('/subscribe-player-status'); } catch(e){ setInterval(refresh, 2000); return; }
    es.addEventListener('status', function(e){
      try { paint(JSON.parse(e.data)); } catch(err){}
    });
    es.onerror = function(){
      // EventSource 自带重连，但被代理断开时它不一定恢复；显式重建更稳
      try { es.close(); } catch(e){}
      es = null;
      setTimeout(connect, 2500);
    };
  }

  refresh();
  loadQueue();
  connect();
  // /?tab=log 直接落到日志页：手机拍下二维码 / 发链接时少一步点击
  if (location.search.indexOf('tab=log') >= 0) $('#tabLog').click();
  // 队列在外部切歌后会变，低频兜底刷新
  setInterval(function(){ if ($('#paneQueue').classList.contains('hide') === false) loadQueue(); }, 15000);
})();
</script>
</body>
</html>
)HTML";
    return QByteArray(kHtml);
}

} // namespace Furui
