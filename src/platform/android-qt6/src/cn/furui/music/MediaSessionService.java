package cn.furui.music;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.browse.MediaBrowser;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.os.SystemClock;
import android.provider.MediaStore;
import android.service.media.MediaBrowserService;
import android.util.Log;
import android.view.KeyEvent;

import java.util.ArrayList;
import java.util.List;

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
 * 同理继承的是 framework 的 android.service.media.MediaBrowserService（API 21+，
 * 与 androidx 的 MediaBrowserServiceCompat 接口一致）：BYD DiLink 媒体中心的
 * 「选择默认使用的音乐」列表按 白名单包名 ∩ 可浏览 MediaBrowserService 过滤，
 * 只继承普通 Service 不进列表，语音「播放XX」也就没有落点（2026-11-20 实测结论）。
 *
 * 播放状态由 C++ 侧（AndroidMediaSession）单向推送；用户从通知/耳机/车机发来的
 * 指令通过 native 方法回传给 C++，由 PlayerController 真正执行。
 */
public class MediaSessionService extends MediaBrowserService {
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
    // 音频焦点：导航播报等 CAN_DUCK 场景压低/恢复音量
    private static final int CMD_DUCK = 7;
    private static final int CMD_UNDUCK = 8;
    // 车机歌单/模式类口令（BYD AUTOVOICE_*）——与 AndroidMediaSession.cpp 的
    // Cmd 枚举一一对应；9~17 只有车机兼容版的 BydControlReceiver 会发。
    private static final int CMD_PLAY_LIKE = 9;     // 播放我喜欢的歌
    private static final int CMD_PLAY_LATELY = 10;  // 播放最近在听
    private static final int CMD_RANDOM = 11;       // 随机播放
    private static final int CMD_MODE_ORDER = 12;   // 顺序播放
    private static final int CMD_MODE_CYCLE = 13;   // 列表循环
    private static final int CMD_MODE_LOOP = 14;    // 单曲循环
    private static final int CMD_MODE_RANDOM = 15;  // 随机播放（同 11，语义别名）
    private static final int CMD_COLLECT = 16;      // 收藏当前歌
    private static final int CMD_UNCOLLECT = 17;    // 取消收藏当前歌

    private static final String ACTION_PLAY = "cn.furui.music.action.PLAY";
    private static final String ACTION_PAUSE = "cn.furui.music.action.PAUSE";
    private static final String ACTION_NEXT = "cn.furui.music.action.NEXT";
    private static final String ACTION_PREV = "cn.furui.music.action.PREV";
    private static final String ACTION_STOP = "cn.furui.music.action.STOP";

    // C++ 注册的 native 回调：把通知/耳机/车机指令交回播放器
    private static native void onNativeCommand(int action, long position);

    private static MediaSessionService sInstance;
    // 三条命令通路（MediaSession 回调 / MEDIA_BUTTON 广播 / Activity 媒体键）可能在
    // 同一次语音指令里同时到达；对照网易云车机版 MediaButtonEventReceiver 的 500ms
    // 去抖，这里也用 500ms（且广播 DOWN/UP 都会进来），只作用于切歌，暂停/播放不受影响。
    // 这些字段由 C++ 的 Qt 线程写、MediaSession 回调/广播在 Android 主线程读，
    // volatile 保证跨线程可见性（避免进度跳变/去抖读到陈旧值）。
    private static volatile long sLastSkipAt = 0;

    // C++ 可能先 start() 再 update*()，而服务 onCreate 是异步的，
    // 所以状态同时存一份静态副本，onCreate 时用它构建首个通知。
    private static volatile String sTitle = "";
    private static volatile String sArtist = "";
    private static volatile String sAlbum = "";
    private static volatile String sCover = "";
    private static volatile long sDuration = 0;
    private static volatile boolean sPlaying = false;
    private static volatile long sPosition = 0;

    private MediaSession mSession;
    private NotificationManager mNotificationManager;
    // 断开耳机/蓝牙（ACTION_AUDIO_BECOMING_NOISY）时立即暂停，避免外放突然大声
    private BroadcastReceiver mNoisyReceiver;

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
        notifyCarVoiceCompat();
    }

    public static void updatePlayback(boolean playing, long position) {
        sPlaying = playing;
        sPosition = Math.max(0, position);
        MediaSessionService service = sInstance;
        if (service != null) {
            service.pushSessionState();
            service.refreshNotification();
        }
        notifyCarVoiceCompat();
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

    // ---------- 音频焦点 ----------
    //
    // 主流播放器行为：本应用播放时申请独占焦点；
    //   其它应用开始播放 → LOSS：暂停（不自动恢复）；LOSS_TRANSIENT（来电、语音
    //   助手）→ 暂停且焦点回来后自动续播；LOSS_TRANSIENT_CAN_DUCK（导航播报、
    //   提醒音）→ 不暂停，只把音量压低（C++ 侧乘 0.3），焦点回来后恢复。
    // Qt6 FFmpeg 后端的 QMediaPlayer 不申请焦点，所以必须自己向系统要。

    private static AudioManager sAudioManager;
    private static AudioFocusRequest sFocusRequest; // API 26+
    private static boolean sFocused = false;
    private static boolean sResumeOnGain = false;   // transient 丢失后 GAIN 自动续播

    private static final AudioManager.OnAudioFocusChangeListener FOCUS_LISTENER =
            new AudioManager.OnAudioFocusChangeListener() {
        @Override
        public void onAudioFocusChange(int change) {
            switch (change) {
                case AudioManager.AUDIOFOCUS_LOSS:
                    // 永久丢失（如用户在别的播放器点了播放）：暂停且不自动恢复，释放焦点
                    sResumeOnGain = false;
                    sendCommand(CMD_PAUSE, 0);
                    abandonFocusInternal();
                    break;
                case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT:
                    // 来电/语音助手等短暂占用：暂停，焦点回来后续播
                    sResumeOnGain = sPlaying;
                    sendCommand(CMD_PAUSE, 0);
                    break;
                case AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK:
                    // 地图导航播报等：只压低音量，播报完恢复
                    sendCommand(CMD_DUCK, 0);
                    break;
                case AudioManager.AUDIOFOCUS_GAIN:
                    sendCommand(CMD_UNDUCK, 0);
                    if (sResumeOnGain) {
                        sResumeOnGain = false;
                        sendCommand(CMD_PLAY, 0);
                    }
                    break;
                default:
                    break;
            }
        }
    };

    /** 开始播放时申请焦点；焦点被别的媒体占着（返回 DELAYED/FAILED）就先不出声。 */
    public static void acquireFocus(Context ctx) {
        if (ctx == null || sFocused)
            return;
        AudioManager am = (AudioManager) ctx.getApplicationContext()
                .getSystemService(Context.AUDIO_SERVICE);
        if (am == null)
            return;
        sAudioManager = am;
        sResumeOnGain = false;
        int result;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            AudioFocusRequest request = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                            .build())
                    .setOnAudioFocusChangeListener(FOCUS_LISTENER)
                    .build();
            sFocusRequest = request;
            result = am.requestAudioFocus(request);
        } else {
            // API 26 以下走老接口（26+ 标记 deprecated，但 minSdk 23 必须保留）
            result = am.requestAudioFocus(FOCUS_LISTENER, AudioManager.STREAM_MUSIC,
                    AudioManager.AUDIOFOCUS_GAIN);
        }
        sFocused = result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED;
        if (!sFocused)
            sendCommand(CMD_PAUSE, 0);
    }

    private static void abandonFocusInternal() {
        try {
            if (sAudioManager == null)
                return;
            if (sFocusRequest != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                sAudioManager.abandonAudioFocusRequest(sFocusRequest);
                sFocusRequest = null;
            } else {
                sAudioManager.abandonAudioFocus(FOCUS_LISTENER);
            }
        } catch (Throwable t) {
            Log.w(TAG, "abandon focus failed", t);
        }
        sFocused = false;
    }

    /** 主动停止播放（清空队列等）时把焦点还给系统。 */
    public static void releaseFocus() {
        abandonFocusInternal();
    }

    /** MEDIA_BUTTON 广播是全系统广播：别的应用在放歌时不该被我们切歌。 */
    public static boolean hasAudioFocus() { return sFocused; }

    // 当前播放状态快照：车机语音新协议（BydMediaBridge 的 query）用它回执
    // getPlayStatus/getPlayContent/getPlayTime。主流版没有调用方，但读取静态
    // 字段无副作用，两个变体共用这份源码。
    public static boolean isPlaying() { return sPlaying; }
    public static long currentPosition() { return sPosition; }
    public static String currentTitle() { return sTitle; }
    public static String currentArtist() { return sArtist; }

    @Override
    public void onCreate() {
        super.onCreate();
        sInstance = this;
        mNotificationManager = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        createChannel();

        mSession = new MediaSession(this, "FuruiMusic");
        mSession.setFlags(MediaSession.FLAG_HANDLES_MEDIA_BUTTONS
                | MediaSession.FLAG_HANDLES_TRANSPORT_CONTROLS);
        // MediaBrowserService 契约：token 必须在客户端 onGetRoot 前设置好，
        // 否则媒体中心 connect 直接失败，语音点歌没有 session 可连。
        setSessionToken(mSession.getSessionToken());
        mSession.setCallback(new MediaSession.Callback() {
            @Override
            public void onPlay() { sendCommand(CMD_PLAY, 0); }

            @Override
            public void onPause() { sendCommand(CMD_PAUSE, 0); }

            @Override
            public void onSkipToNext() {
                // 车机语音调试：确认小迪走的是 MediaSession transport controls
                Log.w(TAG, "session skip next");
                sendCommand(CMD_NEXT, 0);
            }

            @Override
            public void onSkipToPrevious() {
                Log.w(TAG, "session skip prev");
                sendCommand(CMD_PREV, 0);
            }

            @Override
            public void onSeekTo(long pos) { sendCommand(CMD_SEEK, pos); }

            @Override
            public void onStop() {
                sendCommand(CMD_STOP, 0);
                shutdown();
            }

            // 语音点歌标准通道：车机媒体中心/语音助手连上 MediaSession 后，
            // 「播放周杰伦的晴天」会落到这里（对应 PlaybackState 里声明的
            // ACTION_PLAY_FROM_SEARCH）。extras 可能带结构化字段
            // （MediaStore.EXTRA_MEDIA_TITLE/ARTIST...），没有就把整个
            // query 当关键词，交给 C++ 侧搜索播第一条。
            @Override
            public void onPlayFromSearch(String query, Bundle extras) {
                Log.w(TAG, "onPlayFromSearch query=" + query);
                String title = query == null ? "" : query.trim();
                String artist = "";
                if (extras != null) {
                    String t = extras.getString(MediaStore.EXTRA_MEDIA_TITLE);
                    String a = extras.getString(MediaStore.EXTRA_MEDIA_ARTIST);
                    if (t != null && !t.trim().isEmpty())
                        title = t.trim();
                    if (a != null && !a.trim().isEmpty())
                        artist = a.trim();
                }
                if (title.isEmpty() && artist.isEmpty()) {
                    // 只有口头令没有关键词：恢复播放
                    sendCommand(CMD_PLAY, 0);
                    return;
                }
                dispatchVoiceSearch(title, artist);
            }

            @Override
            public void onPlayFromMediaId(String mediaId, Bundle extras) {
                // 我们不提供浏览树，媒体中心不会拿到有效 mediaId；落日志便于取证
                Log.w(TAG, "onPlayFromMediaId id=" + mediaId);
            }
        });
        mSession.setSessionActivity(launchAppIntent());
        // 让系统把媒体按钮/语音助手的媒体命令路由到本 App 的接收器：
        // dumpsys media_session 里主流音乐 App 都有 mediaButtonReceiver，没有的话
        // 「媒体按钮会话」会落到别的应用，车机语音下一首就控制不到我们。
        Intent mbrIntent = new Intent(Intent.ACTION_MEDIA_BUTTON);
        mbrIntent.setPackage(getPackageName());
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            mSession.setMediaButtonBroadcastReceiver(
                    new ComponentName(this, MediaButtonReceiver.class));
        } else {
            int flags = PendingIntent.FLAG_UPDATE_CURRENT;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
                flags |= PendingIntent.FLAG_IMMUTABLE;
            mSession.setMediaButtonReceiver(PendingIntent.getBroadcast(this, 0, mbrIntent, flags));
        }
        mSession.setActive(true);

        // 拔出有线耳机 / 断开蓝牙耳机：系统发 ACTION_AUDIO_BECOMING_NOISY，
        // 立即暂停，避免声音突然切到扬声器外放。服务存活期常驻监听。
        mNoisyReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                Log.w(TAG, "audio becoming noisy -> pause");
                sendCommand(CMD_PAUSE, 0);
            }
        };
        registerReceiver(mNoisyReceiver,
                new IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY));

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

    // ---- MediaBrowserService：可被车机媒体中心/助手浏览连接 ----
    //
    // 我们不提供真正的内容浏览树（点歌走 playFromSearch 搜索通道），
    // 但必须实现这两个回调让 connect 成功：DiLink「选择默认使用的音乐」
    // 列表按 白名单包名 ∩ 可浏览 MediaBrowserService 过滤（汽水音乐有
    // 浏览服务但不在白名单、我们不装 browse 服务都不进列表，实测结论）。
    @Override
    public BrowserRoot onGetRoot(String clientPackageName, int clientUid, Bundle rootHints) {
        Log.w(TAG, "onGetRoot client=" + clientPackageName);
        // 放行全部客户端：车机媒体中心包名不固定，收窄白名单反而连不上
        return new BrowserRoot("furui_root", null);
    }

    @Override
    public void onLoadChildren(String parentId, Result<List<MediaBrowser.MediaItem>> result) {
        // 空浏览树：连接成功但无可浏览内容，不影响 transport controls / 点歌
        result.sendResult(new ArrayList<MediaBrowser.MediaItem>());
    }

    @Override
    public void onDestroy() {
        shutdown();
        super.onDestroy();
    }

    // 车机兼容版挂钩：BYD 小迪语音需要状态上报。主流版没有 BydVoiceHelper
    // 类，反射落空即 no-op。
    private static void notifyCarVoiceCompat() {
        try {
            Class<?> helper = Class.forName("cn.furui.music.BydVoiceHelper");
            helper.getMethod("notifyState", Context.class, boolean.class,
                            String.class, String.class)
                  .invoke(null, null, sPlaying, sTitle, sArtist);
        } catch (ClassNotFoundException | NoSuchMethodException ignored) {
            // 主流版：无 BYD 对接类，静默跳过
        } catch (Throwable t) {
            Log.w(TAG, "car compat notify failed", t);
        }
    }

    /** 相对进度跳转（秒级快进/快退）；车机语音等外部控制使用。 */
    public static void seekBy(long deltaMs) {
        long target = sPosition + deltaMs;
        if (sDuration > 0)
            target = Math.min(target, sDuration - 500);
        sendCommand(CMD_SEEK, Math.max(0, target));
    }

    // 车机实体媒体键：部分车机（比亚迪等）把 NEXT/PREV 当作普通 KeyEvent
    // 发给前台 Activity，而不走 MediaSession 的 onSkipToNext/onSkipToPrevious。
    // 这里给 Activity 一个统一入口：无论服务是否已经起来，都直接把命令送到
    // 已注册的 JNI 回调（sendCommand 本身就是静态的，不依赖服务实例）。
    public static boolean handleMediaKey(int keyCode) {
        if (keyCode == KeyEvent.KEYCODE_MEDIA_NEXT
                || keyCode == KeyEvent.KEYCODE_MEDIA_SKIP_FORWARD) {
            sendCommand(CMD_NEXT, 0);
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_MEDIA_PREVIOUS
                || keyCode == KeyEvent.KEYCODE_MEDIA_SKIP_BACKWARD) {
            sendCommand(CMD_PREV, 0);
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_MEDIA_PLAY) {
            sendCommand(CMD_PLAY, 0);
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_MEDIA_PAUSE) {
            sendCommand(CMD_PAUSE, 0);
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE) {
            sendCommand(sPlaying ? CMD_PAUSE : CMD_PLAY, 0);
            return true;
        }
        return false;
    }

    // static：音频焦点监听器与 acquireFocus 都在静态上下文里调用它，
    // 而它本身只操作静态成员（sPlaying / onNativeCommand），没有实例依赖。
    private static void sendCommand(int action, long position) {
        if (action == CMD_NEXT || action == CMD_PREV) {
            final long now = SystemClock.elapsedRealtime();
            if (now - sLastSkipAt < 500)
                return;
            sLastSkipAt = now;
            Log.w(TAG, "skip command=" + action);
        }
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

    // 车机语音点歌：BydControlReceiver 收到 AUTOVOICE_SEARCH(_PLUS) 时把关键词
    // 送进 C++（search → 搜完播第一条）。主流版没有调用方，声明不会被触发；
    // native 实现由 AndroidMediaSession.cpp 统一注册。
    private static native void onNativeSearch(String keyword);
    // 带歌手字段的语音点歌入口：优先用歌名搜，歌手用于结果里挑最匹配的一首
    private static native void onNativeSearchEx(String keyword, String artist);

    public static void dispatchVoiceSearch(String keyword) {
        dispatchVoiceSearch(keyword, "");
    }

    public static void dispatchVoiceSearch(String keyword, String artist) {
        if (keyword == null || keyword.trim().isEmpty())
            return;
        try {
            onNativeSearchEx(keyword.trim(), artist == null ? "" : artist.trim());
        } catch (Throwable t) {
            Log.w(TAG, "native search failed: " + keyword, t);
        }
    }

    // 车机歌单/模式类口令入口（BydControlReceiver 调用）：直接复用 sendCommand
    // 的 JNI 回调通路；主流版无调用方。
    public static void dispatchVoiceCommand(int command) {
        try {
            sendCommand(command, 0);
        } catch (Throwable t) {
            Log.w(TAG, "native voice command failed: " + command, t);
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
                | PlaybackState.ACTION_SEEK_TO
                // 声明支持语音点歌：媒体中心/助手按 actions 判断能否 playFromSearch
                | PlaybackState.ACTION_PLAY_FROM_SEARCH
                | PlaybackState.ACTION_PLAY_FROM_MEDIA_ID;
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
        // 服务退出（主动停止或系统回收）时不再占着音频焦点
        abandonFocusInternal();
        if (mNoisyReceiver != null) {
            try { unregisterReceiver(mNoisyReceiver); } catch (Throwable ignored) { }
            mNoisyReceiver = null;
        }
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
