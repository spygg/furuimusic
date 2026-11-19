// 福瑞音乐 - 自定义 Activity：只在 QtActivity 之上加返回键埋点日志，
// 用于诊断 ColorOS Android16 上「MV 页返回键不进 QML、Activity 被 finish、
// hwuiTask 线程 SIGABRT」的问题。不改任何行为，只打日志。
package cn.furui.music;

import android.view.KeyEvent;
import android.util.Log;

public class FuruiActivity extends org.qtproject.qt.android.bindings.QtActivity
{
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        final int code = event.getKeyCode();
        // 车机实体媒体键：BYD 等机型把 NEXT/PREV 当普通 KeyEvent 发给前台
        // Activity，不走 MediaSession 回调。这里先接住并转成播放命令；UP 也吞掉，
        // 避免 Qt 再收到一次配对事件（QML 侧也有兜底处理，两边只会命中一边）。
        if (code == KeyEvent.KEYCODE_MEDIA_NEXT
                || code == KeyEvent.KEYCODE_MEDIA_PREVIOUS
                || code == KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE
                || code == KeyEvent.KEYCODE_MEDIA_PLAY
                || code == KeyEvent.KEYCODE_MEDIA_PAUSE
                || code == KeyEvent.KEYCODE_MEDIA_SKIP_FORWARD
                || code == KeyEvent.KEYCODE_MEDIA_SKIP_BACKWARD) {
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                Log.w("FuruiBack", "media key code=" + code);
                MediaSessionService.handleMediaKey(code);
            }
            return true;
        }
        if (event.getAction() == KeyEvent.ACTION_DOWN
            && (event.getKeyCode() == KeyEvent.KEYCODE_BACK
                || event.getKeyCode() == KeyEvent.KEYCODE_HOME))
            Log.w("FuruiBack", "dispatchKeyEvent code=" + event.getKeyCode());
        return super.dispatchKeyEvent(event);
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        Log.w("FuruiBack", "onBackPressed called");
        super.onBackPressed();
    }
}
