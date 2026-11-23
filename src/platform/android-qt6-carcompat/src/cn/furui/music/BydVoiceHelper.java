// 福瑞音乐 - 车机兼容版专用：比亚迪小迪语音登记 / 状态上报 / 日志桥
//
// 本文件只打进车机兼容版 APK（applicationId 用白名单内的网易云车机版包名
// com.netease.cloudmusic.iot；应用名保持「福瑞音乐」，车机设置里按包标签显示
// 福瑞音乐、可选为默认），车机上须先卸载官方网易云（同包名无法共存）。
// 主流版不包含此类；FuruiActivity / MediaSessionService 通过 Class.forName
// 反射挂钩，类缺失时静默跳过（no-op），两个版本互不影响。
//
// 协议来自 reference/byd_car_extract 反编译（网易云车机版 + 小迪语音助手）：
//   登记：向系统属性 persist.sys.autovoice.pkgName 指定的语音助手包发
//         com.byd.action.FUNCTION_UPDATE，extras 为 EXTRA_PACKAGE_NAME /
//         EXTRA_APPLICATION_TYPE=MUSIC / ISCONTROL=TRUE / EXTRA_SERVICE_NAME。
//         语音助手 MusicAppReceiver 会先过 MediaUtil.o(pkg)，只接受白名单五项
//         （cn.kuwo.kwmusiccar / com.kugou.android.auto /
//         com.ximalaya.ting.android.car / cmgyunting.vehicleplayer.cnr /
//         com.netease.cloudmusic.iot）；com.netease.cloudmusic 不在表内会被丢弃。
//         选 .iot 还因为语音词表 MediaSearch.m11684c() 把「网易云音乐」硬编码
//         映射到这个包名，且网易云车机版正是靠此广播进 MUSIC 组（央广云听空位
//         被硬编码进 NEWS/RADIO，进不了默认音乐列表）。
//   上报：com.byd.action.NOTIFY_AUTOVOICE_CURRENT_STATE，extras 为
//         EXTRA_PLAY_STATE(STATE_PLAY/STATE_PAUSE) / EXTRA_PROGRAM_NAME /
//         EXTRA_ARTIST_NAME。
package cn.furui.music;

import android.content.Context;
import android.content.Intent;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

public final class BydVoiceHelper {
    private static final String TAG = "BydVoice";
    private static final long MAX_LOG_BYTES = 512 * 1024;

    private static Context sApp;
    private static String sVoicePkg = "";

    private BydVoiceHelper() {
    }

    /** FuruiActivity.onCreate 反射调用：向车机语音助手登记本应用为受控音乐应用。 */
    public static void register(Context ctx) {
        try {
            Context app = ctx.getApplicationContext();
            sApp = app;
            String pkg = app.getPackageName();
            sVoicePkg = readSystemProperty("persist.sys.autovoice.pkgName");
            log("register pkg=" + pkg + " voicePkg=" + sVoicePkg);

            // 老协议 FUNCTION_UPDATE 是网易云车机版实证有效的注册方式，也是
            // 「加入默认应用列表」的入口（其源码日志原文：apply to join the
            // default application list）。车机按发送方真实包名登记，必须发。
            // 注意网易云在系统属性缺失时兜底用 com.byd.autovoice（C9548a.m25578b
            // 的默认参数），这里保持一致，绝不能因属性为空而跳过注册。
            if (sVoicePkg == null || sVoicePkg.isEmpty()) {
                sVoicePkg = "com.byd.autovoice";
                log("voicePkg empty, fallback to com.byd.autovoice");
            }
            Intent intent = new Intent("com.byd.action.FUNCTION_UPDATE");
            intent.setPackage(sVoicePkg);
            intent.putExtra("EXTRA_PACKAGE_NAME", pkg);
            intent.putExtra("EXTRA_APPLICATION_TYPE", "MUSIC");
            intent.putExtra("ISCONTROL", "TRUE");
            // 网易云填的是其 BydBroadcastService；我们没有对应的广播服务，
            // 填 MediaSessionService（真实存在的前台服务）让车机记录的组件有效
            intent.putExtra("EXTRA_SERVICE_NAME", "cn.furui.music.MediaSessionService");
            app.sendBroadcast(intent);
            log("FUNCTION_UPDATE sent to " + sVoicePkg);

            // 新协议 AIDL 注册保留作兜底：汽水车机版走这套，但该协议在部分车机
            // 版本上不生效（汽水在这些车机上同样不被语音支持）。它失败不影响
            // 上面的老协议注册，故放在其后、异步 bindService。
            BydMediaBridge.register(app);
        } catch (Throwable t) {
            Log.w(TAG, "register failed", t);
        }
    }

    /** MediaSessionService 反射调用：播放状态/曲目变化时向小迪上报。 */
    public static void notifyState(Context ctx, boolean playing, String title, String artist) {
        try {
            Context app = ctx != null ? ctx.getApplicationContext() : sApp;
            if (app == null)
                return;
            if (sVoicePkg == null || sVoicePkg.isEmpty())
                sVoicePkg = readSystemProperty("persist.sys.autovoice.pkgName");
            if (sVoicePkg == null || sVoicePkg.isEmpty())
                return;
            Intent intent = new Intent("com.byd.action.NOTIFY_AUTOVOICE_CURRENT_STATE");
            intent.setPackage(sVoicePkg);
            intent.putExtra("EXTRA_PLAY_STATE", playing ? "STATE_PLAY" : "STATE_PAUSE");
            intent.putExtra("EXTRA_PROGRAM_NAME", title == null ? "" : title);
            intent.putExtra("EXTRA_ARTIST_NAME", artist == null ? "" : artist);
            app.sendBroadcast(intent);
            log("notifyState " + (playing ? "PLAY" : "PAUSE") + " " + artist + " - " + title);
        } catch (Throwable t) {
            Log.w(TAG, "notifyState failed", t);
        }
    }

    // 隐藏 API SystemProperties.get，用反射取（minSdk 23 起可用）
    private static String readSystemProperty(String key) {
        try {
            Class<?> sp = Class.forName("android.os.SystemProperties");
            return (String) sp.getMethod("get", String.class).invoke(null, key);
        } catch (Throwable t) {
            Log.w(TAG, "SystemProperties.get failed", t);
            return "";
        }
    }

    /**
     * 三处日志之一：logcat + 落盘。车机上连不了 adb 时，用户可从
     * Android/data/com.netease.cloudmusic.iot/files/logs/byd-voice.log 拿到证据。
     */
    static void log(String msg) {
        Log.w(TAG, msg);
        try {
            Context app = sApp;
            if (app == null)
                return;
            File dir = new File(app.getExternalFilesDir(null), "logs");
            if (!dir.exists())
                dir.mkdirs();
            File file = new File(dir, "byd-voice.log");
            if (file.length() > MAX_LOG_BYTES) {
                File old = new File(dir, "byd-voice.log.old");
                if (old.exists())
                    old.delete();
                file.renameTo(old);
            }
            String line = android.text.format.DateFormat.format("MM-dd HH:mm:ss ",
                    System.currentTimeMillis()) + msg + "\n";
            FileOutputStream out = new FileOutputStream(file, true);
            out.write(line.getBytes(StandardCharsets.UTF_8));
            out.close();
        } catch (IOException | RuntimeException t) {
            Log.w(TAG, "file log failed", t);
        }
    }
}
