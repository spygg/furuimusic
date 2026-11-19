// 福瑞音乐 - 媒体按钮广播接收器
//
// 车机语音助手（比亚迪「小迪」等）有两种下发「下一首/上一首」的方式：
//   1) 直接给前台 Activity 发 KEYCODE_MEDIA_* KeyEvent —— 由 FuruiActivity 拦截；
//   2) 发 android.intent.action.MEDIA_BUTTON 广播 —— 由本接收器处理。
// 两条路都转成 MediaSessionService 的统一命令入口，避免各写一套切歌逻辑。
package cn.furui.music;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.util.Log;
import android.view.KeyEvent;

public class MediaButtonReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || !Intent.ACTION_MEDIA_BUTTON.equals(intent.getAction()))
            return;

        KeyEvent key;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            key = intent.getParcelableExtra(Intent.EXTRA_KEY_EVENT, KeyEvent.class);
        else
            key = intent.getParcelableExtra(Intent.EXTRA_KEY_EVENT);
        // 网易云车机版实测：车机发出的 MEDIA_BUTTON 广播可能只有 ACTION_UP
        // （其 MediaButtonEventReceiver 就只处理 ACTION_UP）。DOWN 和 UP 都接，
        // 由 MediaSessionService 的 500ms 去抖保证一次按键只切一首。
        if (key == null
                || (key.getAction() != KeyEvent.ACTION_DOWN
                    && key.getAction() != KeyEvent.ACTION_UP))
            return;

        // MEDIA_BUTTON 是全系统广播：别的应用正在放歌时不要跟着切歌
        if (!MediaSessionService.hasAudioFocus())
            return;

        MediaSessionService.handleMediaKey(key.getKeyCode());
        Log.w("FuruiMedia", "MEDIA_BUTTON broadcast key=" + key.getKeyCode());
    }
}
