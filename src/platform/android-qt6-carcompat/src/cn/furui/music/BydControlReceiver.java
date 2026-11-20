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

        BydVoiceHelper.log("unsupported action=" + action
                + " extras=" + describeExtras(intent));
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
