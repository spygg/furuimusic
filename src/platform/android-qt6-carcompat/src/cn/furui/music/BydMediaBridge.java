// 福瑞音乐 - 车机兼容版专用：比亚迪小迪「新语音协议」注册与命令分发桥。
//
// 与旧的 BydControlReceiver（网易云车机版老协议：AUTOVOICE_SEARCH_PLUS 等广播）
// 并存。新协议来自汽水车机版反编译：
//   注册：收到 com.byd.action.AUTOVOICE_MEDIA_REGISTER 广播 → bindService
//         com.byd.autovoice（action 同名）→ onServiceConnected 拿 IBydMediaAIDL
//         → register(运行时包名, {1001=MUSIC}, listener)。
//   命令：control(play/pause/next/previous/forward/rewind/setPlayPattern/jumpToPlay)
//         operation(searchMusic/playRecommend/...)
//         query(getPlayStatus/getPlayContent/getPlayTime/getSupportMediaTypes/...)
//   回执：buildResponse(action, code, msg) → IBydMediaCallback.onResult。
// 该协议在部分车机版本不生效（汽水在这些车机上同样不被语音支持），仅作兜底；
// 主力是 BydVoiceHelper 的老协议 FUNCTION_UPDATE。var 用运行时包名（车机特供版为
// com.netease.cloudmusic.iot，须先卸载官方网易云），与老协议登记的身份保持一致。
package cn.furui.music;

import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.ServiceConnection;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;

import cn.furui.music.BydAidl.IBydMediaAIDL;
import cn.furui.music.BydAidl.IBydMediaCallback;
import cn.furui.music.BydAidl.IBydMediaListener;

public final class BydMediaBridge {
    private static final String TAG = "BydVoice";
    private static final String VOICE_PKG = "com.byd.autovoice";
    private static final String REGISTER_ACTION = "com.byd.action.AUTOVOICE_MEDIA_REGISTER";
    private static final int MEDIA_TYPE_MUSIC = 1001;
    private static final int MEDIA_SOURCE_ONLINE_MUSIC = 12;
    private static final int CODE_SUCCESS = 0;
    private static final int CODE_NOT_SUPPORT = 3;
    private static final int CODE_NO_DATA = 7;
    private static final int STATE_PLAY = 1;
    private static final int STATE_PAUSE = 2;

    private static Context sApp;
    private static IBydMediaAIDL sService;
    private static volatile IBydMediaCallback sCallback;
    private static volatile boolean sRegistered;

    private static final IBydMediaListener LISTENER = new IBydMediaListener.Stub() {
        @Override public void setCallback(IBydMediaCallback cb) {
            sCallback = cb;
            BydVoiceHelper.log("newproto setCallback=" + (cb != null));
        }
        @Override public void control(String method, String json) {
            onControl(method, json);
        }
        @Override public void operation(String method, String json) {
            onOperation(method, json);
        }
        @Override public void query(String method, String json) {
            onQuery(method, json);
        }
    };

    private static final ServiceConnection CONN = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder service) {
            IBydMediaAIDL api = IBydMediaAIDL.Stub.asInterface(service);
            sService = api;
            try {
                String pkg = sApp != null ? sApp.getPackageName() : "cn.furui.music";
                api.register(pkg, new int[]{MEDIA_TYPE_MUSIC}, LISTENER);
                sRegistered = true;
                BydVoiceHelper.log("newproto registered pkg=" + pkg);
            } catch (Throwable t) {
                BydVoiceHelper.log("newproto register failed: " + t);
            }
        }
        @Override public void onServiceDisconnected(ComponentName name) {
            sService = null;
            sRegistered = false;
            BydVoiceHelper.log("newproto service disconnected");
        }
    };

    /** FuruiActivity.onCreate / BydVoiceHelper.register 反射调用：主动注册一次。 */
    public static void register(Context ctx) {
        try {
            sApp = ctx != null ? ctx.getApplicationContext() : null;
            if (sApp == null)
                return;
            doBind();
        } catch (Throwable t) {
            BydVoiceHelper.log("newproto register(ctx) failed: " + t);
        }
    }

    private static void doBind() {
        if (sApp == null)
            return;
        try {
            Intent intent = new Intent();
            intent.setAction(REGISTER_ACTION);
            intent.setPackage(VOICE_PKG);
            boolean ok = sApp.bindService(intent, CONN, Context.BIND_AUTO_CREATE);
            BydVoiceHelper.log("newproto bindService=" + ok + " pkg=" + VOICE_PKG);
        } catch (Throwable t) {
            BydVoiceHelper.log("newproto bind failed: " + t);
        }
    }

    // -------------------- 广播接收器（车机也会用广播下发命令） --------------------
    public static final class Receiver extends BroadcastReceiver {
        @Override public void onReceive(Context context, Intent intent) {
            if (intent == null) return;
            String action = intent.getAction();
            BydVoiceHelper.log("newproto rx action=" + action);
            if (action == null) return;
            if (action.equals(REGISTER_ACTION)) {
                sApp = context.getApplicationContext();
                doBind();
                return;
            }
            if (action.equals("com.byd.action.AUTOVOICE_COMMON_CONTROL")) {
                onControl(intent.getStringExtra("EXTRA_COMMON_CONTROL_METHOD"),
                          intent.getStringExtra("EXTRA_COMMON_CONTROL_PARAM"));
                return;
            }
            if (action.equals("com.byd.action.AUTOVOICE_COMMON_OPERATION")) {
                onOperation(intent.getStringExtra("EXTRA_COMMON_OPERATION_METHOD"),
                            intent.getStringExtra("EXTRA_COMMON_OPERATION_PARAM"));
                return;
            }
            if (action.equals("com.byd.action.AUTOVOICE_COMMON_QUERY")) {
                onQuery(intent.getStringExtra("EXTRA_COMMON_QUERY_METHOD"),
                        intent.getStringExtra("EXTRA_COMMON_QUERY_PARAM"));
                return;
            }
        }
    }

    public static IntentFilter filter() {
        IntentFilter f = new IntentFilter();
        f.addAction(REGISTER_ACTION);
        f.addAction("com.byd.action.AUTOVOICE_COMMON_CONTROL");
        f.addAction("com.byd.action.AUTOVOICE_COMMON_OPERATION");
        f.addAction("com.byd.action.AUTOVOICE_COMMON_QUERY");
        return f;
    }

    // -------------------- control：播放控制 --------------------
    private static void onControl(String method, String json) {
        BydVoiceHelper.log("newproto control method=" + method + " json=" + json);
        if (method == null) return;
        int code = CODE_SUCCESS;
        try {
            if (method.equals("play")) {
                MediaSessionService.handleMediaKey(android.view.KeyEvent.KEYCODE_MEDIA_PLAY);
            } else if (method.equals("pause")) {
                MediaSessionService.handleMediaKey(android.view.KeyEvent.KEYCODE_MEDIA_PAUSE);
            } else if (method.equals("previous")) {
                MediaSessionService.handleMediaKey(android.view.KeyEvent.KEYCODE_MEDIA_PREVIOUS);
            } else if (method.equals("next")) {
                MediaSessionService.handleMediaKey(android.view.KeyEvent.KEYCODE_MEDIA_NEXT);
            } else if (method.equals("forward") || method.equals("rewind")) {
                int sec = jsonInt(json, "time", 30);
                if (sec <= 0) sec = 30;
                long delta = (method.equals("forward") ? 1 : -1) * sec * 1000L;
                MediaSessionService.seekBy(delta);
            } else if (method.equals("jumpToPlay")) {
                int sec = jsonInt(json, "time", 0);
                // jumpToPlay 是绝对定位；seekBy 是相对，这里用差值换算
                long cur = MediaSessionService.currentPosition();
                MediaSessionService.seekBy(sec * 1000L - cur);
            } else if (method.equals("setPlayPattern")) {
                String pattern = jsonString(json, "pattern", "CYCLE");
                int cmd;
                if ("LOOP".equals(pattern) || "SINGLE".equals(pattern)) cmd = 14;
                else if ("RANDOM".equals(pattern)) cmd = 15;
                else cmd = 13; // CYCLE / LIST
                MediaSessionService.dispatchVoiceCommand(cmd);
            } else if (method.equals("collect")) {
                MediaSessionService.dispatchVoiceCommand(16);
            } else if (method.equals("unCollect")) {
                MediaSessionService.dispatchVoiceCommand(17);
            } else {
                code = CODE_NOT_SUPPORT;
            }
        } catch (Throwable t) {
            BydVoiceHelper.log("newproto control failed: " + t);
            code = CODE_NOT_SUPPORT;
        }
        reply(method, code, null);
    }

    // -------------------- operation：点歌 / 推荐 --------------------
    private static void onOperation(String method, String json) {
        BydVoiceHelper.log("newproto operation method=" + method + " json=" + json);
        if (method == null) return;
        int code = CODE_SUCCESS;
        try {
            if (method.equals("searchMusic")) {
                // 汽水的 extractAllJsonValuesKeyword：遍历 JSON 所有值拼成关键词，
                // 兼容车机不同字段命名（keyword/song/name/artist/...）。
                String keyword = allValuesKeyword(json);
                String artist = jsonString(json, "artist", "");
                BydVoiceHelper.log("newproto searchMusic keyword=" + keyword + " artist=" + artist);
                if (!keyword.isEmpty()) {
                    MediaSessionService.dispatchVoiceSearch(keyword, artist);
                } else {
                    code = CODE_NO_DATA;
                }
            } else if (method.equals("playRecommend")) {
                MediaSessionService.dispatchVoiceCommand(11); // 随机/推荐
            } else if (method.equals("playLately")) {
                MediaSessionService.dispatchVoiceCommand(10);
            } else if (method.equals("playFavorite")) {
                MediaSessionService.dispatchVoiceCommand(9);
            } else if (method.equals("playMySongList")) {
                MediaSessionService.dispatchVoiceCommand(9);
            } else {
                code = CODE_NOT_SUPPORT;
            }
        } catch (Throwable t) {
            BydVoiceHelper.log("newproto operation failed: " + t);
            code = CODE_NOT_SUPPORT;
        }
        reply(method, code, null);
    }

    // -------------------- query：状态查询 --------------------
    private static void onQuery(String method, String json) {
        BydVoiceHelper.log("newproto query method=" + method);
        if (method == null) return;
        try {
            Object data = null;
            int code = CODE_SUCCESS;
            if (method.equals("getPlayStatus")) {
                JSONObject o = new JSONObject();
                o.put("playStatus", MediaSessionService.isPlaying() ? STATE_PLAY : STATE_PAUSE);
                data = o;
            } else if (method.equals("getPlayState")) {
                JSONObject o = new JSONObject();
                o.put("playStatus", MediaSessionService.isPlaying() ? STATE_PLAY : STATE_PAUSE);
                o.put("mediaType", MEDIA_TYPE_MUSIC);
                o.put("mediaSource", MEDIA_SOURCE_ONLINE_MUSIC);
                data = o;
            } else if (method.equals("getPlayContent")) {
                JSONObject o = new JSONObject();
                o.put("name", MediaSessionService.currentTitle());
                o.put("artist", MediaSessionService.currentArtist());
                o.put("album", "");
                data = o;
            } else if (method.equals("getPlayTime")) {
                JSONObject o = new JSONObject();
                o.put("time", (int) (MediaSessionService.currentPosition() / 1000));
                data = o;
            } else if (method.equals("getSupportMediaTypes")) {
                data = new JSONArray().put(MEDIA_TYPE_MUSIC);
            } else if (method.equals("getSupportMediaSource")) {
                data = new JSONArray().put(MEDIA_SOURCE_ONLINE_MUSIC);
            } else if (method.equals("getSupportPatterns")) {
                data = new JSONArray().put("CYCLE").put("LOOP").put("RANDOM");
            } else if (method.equals("getCurPattern")) {
                data = "CYCLE";
            } else {
                code = CODE_NOT_SUPPORT;
            }
            reply(method, code, data);
        } catch (Throwable t) {
            BydVoiceHelper.log("newproto query failed: " + t);
            reply(method, CODE_NOT_SUPPORT, null);
        }
    }

    // -------------------- 回执 --------------------
    private static void reply(String action, int code, Object msg) {
        try {
            JSONObject o = new JSONObject();
            o.put("action", action == null ? "" : action);
            o.put("code", code);
            if (msg != null) o.put("msg", msg);
            String json = o.toString();
            IBydMediaCallback cb = sCallback;
            if (cb != null) {
                String pkg = sApp != null ? sApp.getPackageName() : "cn.furui.music";
                cb.onResult(pkg, json);
                BydVoiceHelper.log("newproto reply via callback: " + json);
            } else {
                // callback 未就绪时按汽水做法用广播兜底
                Intent i = new Intent("com.byd.action.AUTOVOICE_COMMON_RESULT");
                i.setPackage(VOICE_PKG);
                i.putExtra("mediaApp", sApp != null ? sApp.getPackageName() : "cn.furui.music");
                i.putExtra("result", json);
                if (sApp != null) sApp.sendBroadcast(i);
                BydVoiceHelper.log("newproto reply via broadcast: " + json);
            }
        } catch (Throwable t) {
            BydVoiceHelper.log("newproto reply failed: " + t);
        }
    }

    // -------------------- JSON 工具 --------------------
    private static JSONObject parse(String json) {
        if (json == null || json.trim().isEmpty()) return null;
        try { return new JSONObject(json); }
        catch (Throwable t) { return null; }
    }

    private static String jsonString(String json, String key, String def) {
        JSONObject o = parse(json);
        if (o == null) return def;
        try {
            String v = o.optString(key, "");
            return v.isEmpty() ? def : v;
        } catch (Throwable t) { return def; }
    }

    private static int jsonInt(String json, String key, int def) {
        JSONObject o = parse(json);
        if (o == null) return def;
        try { return o.optInt(key, def); }
        catch (Throwable t) { return def; }
    }

    /** 对齐汽水 collectJsonValueParts：把所有层级的所有值收集起来用空格连接。 */
    private static String allValuesKeyword(String json) {
        JSONObject o = parse(json);
        if (o == null) {
            // 非 JSON：直接当纯关键词
            return json == null ? "" : json.trim();
        }
        List<String> parts = new ArrayList<>();
        collect(o, parts);
        StringBuilder sb = new StringBuilder();
        for (String p : parts) {
            if (p.isEmpty()) continue;
            if (sb.length() > 0) sb.append(' ');
            sb.append(p);
        }
        return sb.toString().trim();
    }

    private static void collect(Object v, List<String> out) {
        if (v == null) return;
        if (v instanceof JSONObject) {
            JSONObject o = (JSONObject) v;
            Iterator<String> it = o.keys();
            while (it.hasNext()) collect(o.opt(it.next()), out);
        } else if (v instanceof JSONArray) {
            JSONArray a = (JSONArray) v;
            for (int i = 0; i < a.length(); i++) collect(a.opt(i), out);
        } else {
            String s = String.valueOf(v).trim();
            if (!s.isEmpty() && !"null".equals(s)) out.add(s);
        }
    }
}
