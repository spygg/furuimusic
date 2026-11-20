// 福瑞音乐 - 车机兼容版专用：比亚迪小迪语音命令接收器
//
// 只打进车机兼容版 APK；manifest（AndroidManifest.carcompat.xml）里注册了
// 网易云车机版同款 34 个 action。命令映射对照 reference/netease_car 的
// BydControlReceiver：
//   byd.intent.action.MEDIA_BUTTON      → KeyEvent → 播放/暂停/上下曲
//   com.byd.action.AUTOVOICE_FORWARD    → 快进 N 秒（EXTRA_SET_TIME，默认 10）
//   com.byd.action.AUTOVOICE_REWIND     → 快退 N 秒
//   com.byd.action.AUTOVOICE_QUIT       → 停止播放
//   其余 action（收藏/歌单/音质等）→ 记日志待实现，不影响基础控制
package cn.furui.music;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.util.Log;
import android.view.KeyEvent;

public class BydControlReceiver extends BroadcastReceiver {
    private static final String TAG = "BydVoice";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null)
            return;
        final String action = intent.getAction();
        BydVoiceHelper.log("rx action=" + action);

        if ("byd.intent.action.MEDIA_BUTTON".equals(action)) {
            KeyEvent key;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
                key = intent.getParcelableExtra(Intent.EXTRA_KEY_EVENT, KeyEvent.class);
            else
                key = intent.getParcelableExtra(Intent.EXTRA_KEY_EVENT);
            if (key == null) {
                BydVoiceHelper.log("MEDIA_BUTTON without key event");
                return;
            }
            BydVoiceHelper.log("MEDIA_BUTTON key=" + key.getKeyCode()
                    + " action=" + key.getAction());
            // 网易云车机版只处理 DOWN；UP 也进来的话靠 MediaSessionService 的
            // 500ms 去抖保证一次按键只切一首
            if (key.getAction() == KeyEvent.ACTION_DOWN
                    && MediaSessionService.hasAudioFocus())
                MediaSessionService.handleMediaKey(key.getKeyCode());
            return;
        }

        if ("com.byd.action.AUTOVOICE_FORWARD".equals(action)
                || "com.byd.action.AUTOVOICE_REWIND".equals(action)) {
            int seconds = parseSeconds(intent);
            boolean forward = "com.byd.action.AUTOVOICE_FORWARD".equals(action);
            BydVoiceHelper.log((forward ? "seekForward " : "seekBack ") + seconds + "s");
            if (MediaSessionService.hasAudioFocus())
                MediaSessionService.seekBy((forward ? 1 : -1) * seconds * 1000L);
            return;
        }

        if ("com.byd.action.AUTOVOICE_QUIT".equals(action)) {
            BydVoiceHelper.log("quit");
            try {
                Intent stop = new Intent(context, MediaSessionService.class);
                stop.setAction("cn.furui.music.action.STOP");
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
                    context.startForegroundService(stop);
                else
                    context.startService(stop);
            } catch (Throwable t) {
                BydVoiceHelper.log("quit failed: " + t);
            }
            return;
        }

        if ("com.byd.action.AUTOVOICE_SEARCH_PLUS".equals(action)
                || "com.byd.action.AUTOVOICE_SEARCH".equals(action)) {
            // 语音点歌「播放周杰伦的晴天」。协议对照网易云车机版反编译：
            //   EXTRA_KEYWORDS_SEARCH 歌名/关键词（必有）
            //   EXTRA_ARTIST_SEARCH   歌手（可空）
            // 官方 BydControlReceiver.m8705g/m8706h 的做法是：候选字段按优先级取，
            // 字段里若带 '|' 只取第一段（小迪常把多个识别候选塞在同一个 extra 里）。
            // 旧实现把整个 extra（可能含 '|'）与歌手直接拼成 "周杰伦 晴天|..." 去搜，
            // 聚合搜索拿不到结果 —— 这正是「播放周杰伦的晴天无效」的根因。
            String keywords = firstCandidate(intent.getStringExtra("EXTRA_KEYWORDS_SEARCH"),
                    intent.getStringExtra("EXTRA_PROGRAM_TAGS"),
                    intent.getStringExtra("EXTRA_GENRE_SEARCH"),
                    intent.getStringExtra("EXTRA_LANG_SEARCH"),
                    intent.getStringExtra("EXTRA_SOURCE_SEARCH"),
                    intent.getStringExtra("EXTRA_AREA_SEARCH"));
            String artist = firstCandidate(intent.getStringExtra("EXTRA_ARTIST_SEARCH"));
            BydVoiceHelper.log("search keywords=" + keywords
                    + " artist=" + artist + " extras=" + describeExtras(intent));
            if (!keywords.isEmpty() || !artist.isEmpty())
                MediaSessionService.dispatchVoiceSearch(keywords, artist);
            else
                BydVoiceHelper.log("search ignored: empty query");
            return;
        }

        // ---- 歌单/模式类口令：无 extras，直接映射 CMD_*（协议来自
        //      MediaCenterTestActivity 反编译源码）----
        Integer cmd = null;
        switch (action == null ? "" : action) {
            case "com.byd.action.AUTOVOICE_PLAY_LIKE":    cmd = 9;  break; // 播放我喜欢的歌
            case "com.byd.action.AUTOVOICE_PLAY_LATELY":  cmd = 10; break; // 播放最近在听
            case "com.byd.action.AUTOVOICE_PLAY_RANDOM":  cmd = 11; break; // 随机播放
            case "com.byd.action.AUTOVOICE_COLLECT":      cmd = 16; break; // 收藏当前歌
            case "com.byd.action.AUTOVOICE_UNCOLLECT":    cmd = 17; break; // 取消收藏
            case "com.byd.action.AUTOVOICE_PLAY_MODE":
                // EXTRA_PLAY_MODE: LOOP=单曲循环 CYCLE=列表循环 ORDER=顺序 RANDOM=随机
                String mode = intent.getStringExtra("EXTRA_PLAY_MODE");
                BydVoiceHelper.log("play mode=" + mode);
                if ("LOOP".equals(mode))          cmd = 14;
                else if ("CYCLE".equals(mode))    cmd = 13;
                else if ("ORDER".equals(mode))    cmd = 12;
                else if ("RANDOM".equals(mode))   cmd = 15;
                break;
            default:
                break;
        }
        if (cmd != null) {
            MediaSessionService.dispatchVoiceCommand(cmd);
            return;
        }

        BydVoiceHelper.log("unsupported action=" + action
                + " extras=" + describeExtras(intent));
    }

    /**
     * 取第一个非空候选；与网易云 m8706h 一致：含 '|' 时只取 '|' 前第一段。
     * 小迪常把一个字段写成 "晴天|周杰伦的晴天|..." 供 app 挑选。
     */
    private static String firstCandidate(String... values) {
        for (String v : values) {
            if (v == null)
                continue;
            String s = v.trim();
            if (s.isEmpty())
                continue;
            int bar = s.indexOf('|');
            if (bar >= 0)
                s = s.substring(0, bar).trim();
            if (!s.isEmpty())
                return s;
        }
        return "";
    }

    private static int parseSeconds(Intent intent) {
        String v = intent.getStringExtra("EXTRA_SET_TIME");
        try {
            int n = v == null ? 0 : Integer.parseInt(v.trim());
            return n <= 0 ? 10 : n;
        } catch (NumberFormatException e) {
            return 10;
        }
    }

    private static String describeExtras(Intent intent) {
        StringBuilder sb = new StringBuilder();
        try {
            android.os.Bundle extras = intent.getExtras();
            if (extras == null)
                return "{}";
            sb.append('{');
            boolean first = true;
            for (String key : extras.keySet()) {
                if (!first)
                    sb.append(',');
                first = false;
                sb.append(key).append('=').append(extras.get(key));
            }
            sb.append('}');
        } catch (Throwable t) {
            sb.append("(?)");
        }
        return sb.toString();
    }
}
