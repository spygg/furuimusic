// 福瑞音乐 - 内置浏览器原生 WebView 调整
//
// QtWebView 的 Android 后端没有暴露 mixed content 开关，系统默认是
// MIXED_CONTENT_NEVER_ALLOW：https 页面里所有 http:// 资源（音乐站封面尤其多）
// 都会被当作混合内容拦掉，表现是「网页能打开，图片全是空白」。
//
// 应用已经在 AndroidManifest 里声明 usesCleartextTraffic=true，这里把 WebView 的
// mixedContentMode 也放开，让封面/封面背景图正常显示。Qt 每次打开浏览器都会新建
// 一个 WebView，所以每次 open 都要调一次，并且要轮询等视图树里出现 WebView。
package cn.furui.music;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.webkit.WebSettings;
import android.webkit.WebView;

import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;

public final class WebViewTweaks {
    // 100ms * 40 ≈ 4s：WebView 由 QML Loader 创建，可能比调用晚几十毫秒
    private static final long RETRY_MS = 100;
    private static final int MAX_TRIES = 40;

    private WebViewTweaks() {}

    public static void enableMixedContent(final Activity activity) {
        if (activity == null) return;
        final Handler handler = new Handler(Looper.getMainLooper());
        final Runnable[] task = new Runnable[1];
        task[0] = new Runnable() {
            private int tries = 0;

            @Override
            public void run() {
                if (apply(activity.getWindow().getDecorView())) return;
                if (++tries >= MAX_TRIES) return;
                handler.postDelayed(task[0], RETRY_MS);
            }
        };
        activity.runOnUiThread(task[0]);
    }

    // 历史栈查询：后退/前进按钮是否可用。
    //
    // Qt 的 JNI 调用跑在 qtMainLoopThread，不是 Android UI 线程，而 WebView 的方法
    // 必须在 UI 线程调用（直接在 JNI 里调会抛 "WebView method was called on thread
    // qtMainLoopThread"）。所以这里和 Qt 自己的后端一样 post 到 UI 线程，但把等待
    // 时间从 Qt 的 250ms 放宽到 1500ms——UI 线程正忙时 250ms 经常不够，超时后
    // 一律返回 false，表现就是「后退按钮一直灰着、返回键直接关浏览器」。
    public static boolean canGoBack(Activity activity) {
        return queryHistory(activity, true);
    }

    public static boolean canGoForward(Activity activity) {
        return queryHistory(activity, false);
    }

    private static boolean queryHistory(Activity activity, final boolean back) {
        if (activity == null) return false;
        final boolean[] result = {false};
        final Semaphore sem = new Semaphore(0);
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    WebView view = findWebView(activity.getWindow().getDecorView());
                    result[0] = view != null && (back ? view.canGoBack() : view.canGoForward());
                } catch (Throwable t) {
                    Log.w("FuruiWebView", "history query failed", t);
                } finally {
                    sem.release();
                }
            }
        });
        try {
            if (!sem.tryAcquire(1500, TimeUnit.MILLISECONDS))
                Log.w("FuruiWebView", "history query timeout");
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        return result[0];
    }

    private static WebView findWebView(View view) {
        if (view == null) return null;
        if (view instanceof WebView) return (WebView) view;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); ++i) {
                WebView found = findWebView(group.getChildAt(i));
                if (found != null) return found;
            }
        }
        return null;
    }

    // 递归遍历视图树；返回 true 表示至少命中一个 WebView（已设置过）
    private static boolean apply(View view) {
        if (view == null) return false;
        boolean found = false;
        if (view instanceof WebView) {
            WebSettings settings = ((WebView) view).getSettings();
            settings.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW);
            found = true;
        }
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); ++i) {
                if (apply(group.getChildAt(i))) found = true;
            }
        }
        return found;
    }
}
