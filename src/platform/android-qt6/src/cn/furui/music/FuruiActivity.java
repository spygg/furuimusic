// 福瑞音乐 - 自定义 Activity：在 QtActivity 之上做两件事：
//  1) 返回键/媒体键埋点与车机媒体键转派（诊断 ColorOS Android16 的返回键问题）；
//  2) 参考 legado 设置透明状态栏 + LAYOUT_FULLSCREEN，让 QML 背景铺到挖孔区。
package cn.furui.music;

import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.view.KeyEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.util.Log;

public class FuruiActivity extends org.qtproject.qt.android.bindings.QtActivity
{
    // 由 C++ 侧（AndroidMediaSession::install）注册：请求 Qt 事件循环退出。
    // 后台连播要求事件循环不冻结（background_running=true），于是从多任务划掉
    // 任务时 Qt 不再自动 quit，QtActivityBase.onDestroy 会无限等 Qt 退出，
    // 主线程 20s 后 ANR，同进程再次启动也一起卡死（2026-11-27 真机取证）。
    private static native void nativePrepareQtExit();

    @Override
    protected void onDestroy() {
        // 配置变更（旋转/分屏等）会走同一条 onDestroy 但 Qt 会保留实例重建，
        // 那里绝不能退，交回 Qt 原生流程。
        if (isChangingConfigurations()) {
            super.onDestroy();
            return;
        }
        // 真正销毁（划掉任务/系统回收）：先请求 Qt 退出事件循环（exit(0)），
        // 然后立刻返回，由后台线程兜底结束进程。
        //
        // 为什么不调用 super.onDestroy()：QtActivityBase.onDestroy 会在主线程
        // sem_wait 等 Qt 主线程退出；而 Qt 退出时的 QtMultimedia 音频设备清理
        // （QtAudioDeviceManager.qAndroidAudioDevicesDestroyed）把清理任务 post
        // 到主线程 looper 并 latch.await()。主线程此时正阻塞在 sem_wait 上，
        // 两边互等死锁 → 20s 后 destroy timeout + ANR（真机 16:15 取证：
        // qtMainLoopThread 卡在 CountDownLatch.await）。
        // 进程照常退出（与 Qt 自己 onDestroy 最后的 System.exit(0) 等价），
        // 只是不再等 Qt 的握手；退出前 exit(0) 已触发 aboutToQuit 落盘设置。
        try {
            nativePrepareQtExit();
        } catch (Throwable t) {
            // native 尚未注册（启动瞬间就被销毁）：保持静默，直接走兜底退出
            Log.w("FuruiBack", "prepare qt exit failed", t);
        }
        Thread killer = new Thread(() -> {
            try {
                Thread.sleep(1200);
            } catch (InterruptedException ignored) {
            }
            android.os.Process.killProcess(android.os.Process.myPid());
        }, "furui-exit");
        killer.setDaemon(true);
        killer.start();
    }

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // 参考 legado 的 fullScreen()+透明状态栏：让 QML 窗口铺到状态栏/挖孔区下方，
        // 由 QML 自己画背景，状态栏透明，避免 ColorOS 强绘白底造成割裂。
        // 只用平台标准 API，不做 OEM 反射；Qt 侧 PlatformAdapter 会按 insets 内缩内容。
        setupTransparentStatusBar();
        // 车机兼容版挂钩：该变体（包名 com.netease.cloudmusic.iot，车机上须先卸载
        // 官方网易云）在 dex 里额外包含 BydVoiceHelper，用于向比亚迪小迪语音登记；
        // 主流版不含此类，Class.forName 落空 → no-op，两个版本共用这份源码。
        try {
            Class<?> helper = Class.forName("cn.furui.music.BydVoiceHelper");
            helper.getMethod("register", android.content.Context.class)
                  .invoke(null, this);
        } catch (ClassNotFoundException | NoSuchMethodException ignored) {
            // 主流版：无 BYD 对接类，静默跳过
        } catch (Throwable t) {
            Log.w("FuruiBack", "car compat hook failed", t);
        }
    }

    @SuppressWarnings("deprecation")
    private void setupTransparentStatusBar() {
        Window window = getWindow();
        if (window == null)
            return;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // Android 11+ 的官方开关：decor 不自动为系统栏留白，由 View 的 insets 处理。
            window.setDecorFitsSystemWindows(true);
        }
        View decor = window.getDecorView();
        // LAYOUT_FULLSCREEN + LAYOUT_STABLE：内容可以画到状态栏下，且旋转/切主题时
        // insets 高度稳定。这是 legado ActivityExtensions.fullScreen() 的核心两行。
        decor.setSystemUiVisibility(View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        window.clearFlags(WindowManager.LayoutParams.FLAG_TRANSLUCENT_STATUS
                | WindowManager.LayoutParams.FLAG_TRANSLUCENT_NAVIGATION);
        window.addFlags(WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP)
            window.setStatusBarColor(Color.TRANSPARENT);
    }

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
