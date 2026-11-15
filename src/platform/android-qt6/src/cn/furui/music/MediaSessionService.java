package cn.furui.music;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

/**
 * 播放器前台服务 + 系统 MediaSession。
 *
 * 职责只有两件事：
 *   1) 作为前台服务让进程在后台/锁屏时不被回收（音频继续播）；
 *   2) 通过 MediaSession 把「在播什么、能做什么」交给系统，从而获得
 *      通知栏/锁屏媒体卡片、蓝牙耳机与车机（AVRCP）按键、Android Auto 控制。
 *
 * 这里刻意只用平台自带的 android.media.session.MediaSession（API 21+），
 * 不引入 androidx.media：Qt 的 Gradle 模板没有该依赖，改依赖会让打包链路变脆。
 *
 * 播放状态由 C++ 侧（AndroidMediaSession）单向推送；用户从通知/耳机/车机发来的
 * 指令通过 native 方法回传给 C++，由 PlayerController 真正执行。
 */
public class MediaSessionService extends Service {
    private static final String TAG = "FuruiMedia";
    private static final String CHANNEL_ID = "furui_playback";
    private static final int NOTIFICATION_ID = 1196445257; // "FURU"

    // 与 C++ 侧 AndroidMediaSession::Command 保持一致
    private static final int CMD_PLAY = 1;
    private static final int CMD_PAUSE = 2;
    private static final int CMD_NEXT = 3;
    private static final int CMD_PREV = 4;
    private static final int CMD_SEEK = 5;
    private static final int CMD_STOP = 6;

    private static final String ACTION_PLAY = "cn.furui.music.action.PLAY";
    private static final String ACTION_PAUSE = "cn.furui.music.action.PAUSE";
    private static final String ACTION_NEXT = "cn.furui.music.action.NEXT";
    private static final String ACTION_PREV = "cn.furui.music.action.PREV";
    private static final String ACTION_STOP = "cn.furui.music.action.STOP";

    // C++ 注册的 native 回调：把通知/耳机/车机指令交回播放器
    private static native void onNativeCommand(int action, long position);

    private static MediaSessionService sInstance;

    // C++ 可能先 start() 再 update*()，而服务 onCreate 是异步的，
    // 所以状态同时存一份静态副本，onCreate 时用它构建首个通知。
    private static String sTitle = "";
    private static String sArtist = "";
    private static String sAlbum = "";
    private static String sCover = "";
    private static long sDuration = 0;
    private static boolean sPlaying = false;
    private static long sPosition = 0;

    private MediaSession mSession;
    private NotificationManager mNotificationManager;

    /** 启动（或复用）前台服务；重复调用无副作用。 */
    public static void start(Context ctx) {
        if (ctx == null)
            return;
        try {
            Intent intent = new Intent(ctx, MediaSessionService.class);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
                ctx.startForegroundService(intent);
            else
                ctx.startService(intent);
        } catch (Throwable t) {
            // 后台启动前台服务在部分 ROM 上会被拒；播放仍可在应用内进行，不致命
            Log.w(TAG, "start failed", t);
        }
    }

    public static void updateMetadata(String title, String artist, String album,
                                      String cover, long duration) {
        sTitle = title == null ? "" : title;
        sArtist = artist == null ? "" : artist;
        sAlbum = album == null ? "" : album;
        sCover = cover == null ? "" : cover;
        sDuration = duration;
        MediaSessionService service = sInstance;
        if (service != null) {
            service.pushSessionState();
            service.refreshNotification();
        }
    }

    public static void updatePlayback(boolean playing, long position) {
        sPlaying = playing;
        sPosition = Math.max(0, position);
        MediaSessionService service = sInstance;
        if (service != null) {
            service.pushSessionState();
            service.refreshNotification();
        }
    }

    /** 应用内已无播放内容（如清空队列）时收起通知并退出。 */
    public static void stop(Context ctx) {
        if (ctx != null) {
            try {
                ctx.stopService(new Intent(ctx, MediaSessionService.class));
            } catch (Throwable t) {
                Log.w(TAG, "stop failed", t);
            }
        }
        MediaSessionService service = sInstance;
        if (service != null)
            service.shutdown();
    }

    @Override
    public void onCreate() {
        super.onCreate();
        sInstance = this;
        mNotificationManager = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        createChannel();

        mSession = new MediaSession(this, "FuruiMusic");
        mSession.setFlags(MediaSession.FLAG_HANDLES_MEDIA_BUTTONS
                | MediaSession.FLAG_HANDLES_TRANSPORT_CONTROLS);
        mSession.setCallback(new MediaSession.Callback() {
            @Override
            public void onPlay() { sendCommand(CMD_PLAY, 0); }

            @Override
            public void onPause() { sendCommand(CMD_PAUSE, 0); }

            @Override
            public void onSkipToNext() { sendCommand(CMD_NEXT, 0); }

            @Override
            public void onSkipToPrevious() { sendCommand(CMD_PREV, 0); }

            @Override
            public void onSeekTo(long pos) { sendCommand(CMD_SEEK, pos); }

            @Override
            public void onStop() {
                sendCommand(CMD_STOP, 0);
                shutdown();
            }
        });
        mSession.setSessionActivity(launchAppIntent());
        mSession.setActive(true);

        pushSessionState();
        // Android 14 起 startForeground 必须在 onCreate 后 5 秒内完成
        startForeground(NOTIFICATION_ID, buildNotification());
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent == null ? null : intent.getAction();
        if (ACTION_PLAY.equals(action))
            sendCommand(CMD_PLAY, 0);
        else if (ACTION_PAUSE.equals(action))
            sendCommand(CMD_PAUSE, 0);
        else if (ACTION_NEXT.equals(action))
            sendCommand(CMD_NEXT, 0);
        else if (ACTION_PREV.equals(action))
            sendCommand(CMD_PREV, 0);
        else if (ACTION_STOP.equals(action)) {
            sendCommand(CMD_STOP, 0);
            shutdown();
            return START_NOT_STICKY;
        }
        // 通知刷新放在指令之后：C++ 回推状态会再次刷新，这里先保证按钮图标跟手
        refreshNotification();
        // 进程被系统回收后不必复活一个「没有播放内容」的空服务
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        shutdown();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void sendCommand(int action, long position) {
        // 通知按下先在本地翻转状态，避免等 C++ 回推造成图标迟滞
        if (action == CMD_PLAY)
            sPlaying = true;
        else if (action == CMD_PAUSE)
            sPlaying = false;
        try {
            onNativeCommand(action, position);
        } catch (Throwable t) {
            // C++ 尚未注册（或进程重启后回调丢失）不能让服务崩掉
            Log.w(TAG, "native command failed: " + action, t);
        }
    }

    private void pushSessionState() {
        if (mSession == null)
            return;
        MediaMetadata.Builder metadata = new MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_TITLE, sTitle)
                .putString(MediaMetadata.METADATA_KEY_ARTIST, sArtist)
                .putString(MediaMetadata.METADATA_KEY_ALBUM, sAlbum)
                .putLong(MediaMetadata.METADATA_KEY_DURATION, sDuration);
        Bitmap art = decodeCover(sCover);
        if (art != null)
            metadata.putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, art);
        mSession.setMetadata(metadata.build());

        long actions = PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE
                | PlaybackState.ACTION_PLAY_PAUSE | PlaybackState.ACTION_STOP
                | PlaybackState.ACTION_SKIP_TO_NEXT | PlaybackState.ACTION_SKIP_TO_PREVIOUS
                | PlaybackState.ACTION_SEEK_TO;
        PlaybackState.Builder state = new PlaybackState.Builder()
                .setActions(actions)
                .setState(sPlaying ? PlaybackState.STATE_PLAYING : PlaybackState.STATE_PAUSED,
                        sPosition, sPlaying ? 1.0f : 0.0f);
        mSession.setPlaybackState(state.build());
    }

    private void refreshNotification() {
        if (mNotificationManager == null)
            return;
        try {
            mNotificationManager.notify(NOTIFICATION_ID, buildNotification());
        } catch (Throwable t) {
            Log.w(TAG, "notify failed", t);
        }
    }

    private Notification buildNotification() {
        Notification.Builder builder = Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                ? new Notification.Builder(this, CHANNEL_ID)
                : new Notification.Builder(this);
        builder.setSmallIcon(android.R.drawable.ic_media_play)
                .setContentTitle(sTitle.isEmpty() ? "福瑞音乐" : sTitle)
                .setContentText(sArtist.isEmpty() ? sAlbum : sArtist)
                .setContentIntent(launchAppIntent())
                .setOnlyAlertOnce(true)
                .setShowWhen(false)
                .setVisibility(Notification.VISIBILITY_PUBLIC)
                .setOngoing(sPlaying);
        Bitmap art = decodeCover(sCover);
        if (art != null)
            builder.setLargeIcon(art);

        builder.addAction(new Notification.Action.Builder(
                android.R.drawable.ic_media_previous, "上一首", serviceIntent(ACTION_PREV)).build());
        builder.addAction(new Notification.Action.Builder(
                sPlaying ? android.R.drawable.ic_media_pause : android.R.drawable.ic_media_play,
                sPlaying ? "暂停" : "播放",
                serviceIntent(sPlaying ? ACTION_PAUSE : ACTION_PLAY)).build());
        builder.addAction(new Notification.Action.Builder(
                android.R.drawable.ic_media_next, "下一首", serviceIntent(ACTION_NEXT)).build());

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            Notification.MediaStyle style = new Notification.MediaStyle()
                    .setMediaSession(mSession == null ? null : mSession.getSessionToken())
                    .setShowActionsInCompactView(0, 1, 2);
            builder.setStyle(style);
        }
        return builder.build();
    }

    private void createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O || mNotificationManager == null)
            return;
        NotificationChannel channel = new NotificationChannel(CHANNEL_ID, "播放控制",
                NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("后台播放与媒体控制");
        channel.setShowBadge(false);
        mNotificationManager.createNotificationChannel(channel);
    }

    /** 点通知/锁屏卡片回到应用（复用已有任务，不重建 Activity）。 */
    private PendingIntent launchAppIntent() {
        Intent intent = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (intent == null)
            return null;
        intent.addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        return PendingIntent.getActivity(this, 0, intent, pendingFlags());
    }

    private PendingIntent serviceIntent(String action) {
        Intent intent = new Intent(this, MediaSessionService.class).setAction(action);
        return PendingIntent.getService(this, action.hashCode(), intent, pendingFlags());
    }

    private static int pendingFlags() {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.M
                ? PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE
                : PendingIntent.FLAG_UPDATE_CURRENT;
    }

    private static Bitmap sCachedCover;
    private static String sCachedCoverPath = "";

    /**
     * 封面目前只处理本地文件（C++ 侧会把网络封面下载到 cache 再传路径过来）。
     * 解码结果按路径缓存：播放/暂停会频繁重建通知，重复解码大图会拖慢主线程。
     */
    private static Bitmap decodeCover(String path) {
        if (path == null || path.isEmpty() || path.startsWith("http"))
            return null;
        if (path.equals(sCachedCoverPath))
            return sCachedCover;
        String local = path.startsWith("file://") ? path.substring(7) : path;
        Bitmap bitmap = null;
        try {
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inSampleSize = 2;
            bitmap = BitmapFactory.decodeFile(local, options);
        } catch (Throwable t) {
            bitmap = null;
        }
        sCachedCoverPath = path;
        sCachedCover = bitmap;
        return bitmap;
    }

    private void shutdown() {
        if (mSession != null) {
            mSession.setActive(false);
            mSession.release();
            mSession = null;
        }
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N)
                stopForeground(STOP_FOREGROUND_REMOVE);
            else
                stopForeground(true);
        } catch (Throwable ignored) {
        }
        if (mNotificationManager != null)
            mNotificationManager.cancel(NOTIFICATION_ID);
        if (sInstance == this)
            sInstance = null;
        stopSelf();
    }
}
