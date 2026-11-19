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
