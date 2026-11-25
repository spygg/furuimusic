// 福瑞音乐 - Qt 5 自编译 FFmpeg 播放后端实现
// 仅在 FURUI_HAVE_FFMPEG 定义时参与编译（见 CMakeLists.txt / FuruiMusic.pro）。
#ifdef FURUI_HAVE_FFMPEG

#include "FfmpegVideoPlayer.h"

#include <QPainter>
#include <QMetaObject>
#include <QMutexLocker>
#include <QAudioDeviceInfo>
#include <QAudioOutput>
#include <QIODevice>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace Furui {
namespace {

QString ffError(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

double ptsToMs(qint64 pts, const AVRational &tb)
{
    if (pts == AV_NOPTS_VALUE)
        return 0.0;
    return double(pts) * av_q2d(tb) * 1000.0;
}

// FURUI_MV_DEBUG=1 时把解码链路关键节点打到 stderr，用于无人值守验证
static bool ffDebug()
{
    static const bool on = qEnvironmentVariableIntValue("FURUI_MV_DEBUG") > 0;
    return on;
}
#define FFLOG(...) do { if (ffDebug()) fprintf(stderr, "[ffmv] " __VA_ARGS__); } while (0)

// QAudioOutput / QIODevice 都是 QObject，必须在 GUI 线程创建和控制。解码线程只调用
// push()（仅操作自己的互斥缓冲），不会再跨线程碰 Qt 音频对象，也不会阻塞在 write() 上。
class PcmDevice : public QIODevice {
public:
    explicit PcmDevice(QObject *parent = nullptr) : QIODevice(parent)
    { open(QIODevice::ReadOnly | QIODevice::Unbuffered); }

    qint64 readData(char *data, qint64 maxLen) override
    {
        QMutexLocker locker(&m_mutex);
        const qint64 n = qMin<qint64>(maxLen, m_buffer.size());
        if (n > 0) {
            std::memcpy(data, m_buffer.constData(), static_cast<std::size_t>(n));
            m_buffer.remove(0, int(n));
        }
        return n;
    }
    qint64 writeData(const char *, qint64) override { return -1; }

    // 解码线程调用：缓冲满就丢弃，绝不等待，避免 stopDecode() 的 join 卡死。
    void push(const char *data, qint64 len)
    {
        if (len <= 0) return;
        {
            QMutexLocker locker(&m_mutex);
            if (m_buffer.size() + len > kMaxBuffer) return;
            m_buffer.append(data, int(len));
        }
        QMetaObject::invokeMethod(this, [this]() { emit readyRead(); }, Qt::QueuedConnection);
    }
    void clearBuffer()
    {
        QMutexLocker locker(&m_mutex);
        m_buffer.clear();
    }

private:
    static const qint64 kMaxBuffer = 256 * 1024;
    QMutex m_mutex;
    QByteArray m_buffer;
};

// 传给 FFmpeg interrupt_callback 的每代上下文：必须按代次判断是否需要中断，
// 不能只看全局 m_stop——open() 会在起新线程前把 m_stop 清零，已 detach 的旧
// 线程若还阻塞在 av_read_frame，就会因为读到 false 而无法唤醒。
struct InterruptCtx {
    FfmpegVideoPlayer *self;
    int gen;
};

} // namespace

// 头文件里前向声明的是 Furui::AudioSink，定义必须放在同一个命名空间（不能放匿名命名空间）。
class AudioSink : public QObject {
public:
    explicit AudioSink(QObject *parent = nullptr)
        : QObject(parent), m_device(new PcmDevice(this)) { prepare(); }

    PcmDevice *device() const { return m_device; }
    // GUI 线程调用：按“当前”默认输出设备定一次格式。必须和解码线程的 swr
    // 目标格式一致——QAudioOutput 不做重采样，swr 固定输出 48k 而设备只支持
    // 44.1k 时声音会变调/加速甚至直接打开失败。
    // 只存原子整数：即使上一代 detached 线程还在读，也不会和这里的写入竞争。
    void prepare()
    {
        QAudioFormat af;
        af.setSampleRate(48000);
        af.setChannelCount(2);
        af.setSampleSize(16);
        af.setCodec(QStringLiteral("audio/pcm"));
        af.setByteOrder(QAudioFormat::LittleEndian);
        af.setSampleType(QAudioFormat::SignedInt);
        const QAudioDeviceInfo dev = QAudioDeviceInfo::defaultOutputDevice();
        const QAudioFormat chosen = dev.isFormatSupported(af) ? af : dev.nearestFormat(af);
        m_sampleRate.store(chosen.sampleRate());
        m_channels.store(chosen.channelCount());
    }
    int sampleRate() const { return m_sampleRate.load(); }
    int channelCount() const { return m_channels.load(); }

    void start()
    {
        // 旧输出可能还在（open 后未 close 直接换歌），先关掉再开，避免叠加两路音频。
        if (m_out) { m_out->stop(); delete m_out; m_out = nullptr; }
        const int rate = m_sampleRate.load();
        const int channels = m_channels.load();
        if (rate <= 0 || channels <= 0)
            return;   // open() 时没有可用音频设备：降级为只有画面
        QAudioFormat af;
        af.setSampleRate(rate);
        af.setChannelCount(channels);
        af.setSampleSize(16);
        af.setCodec(QStringLiteral("audio/pcm"));
        af.setByteOrder(QAudioFormat::LittleEndian);
        af.setSampleType(QAudioFormat::SignedInt);
        const QAudioDeviceInfo dev = QAudioDeviceInfo::defaultOutputDevice();
        m_out = new QAudioOutput(dev, af, this);
        m_out->setBufferSize(64 * 1024);
        m_out->setVolume(m_volume);
        m_out->start(m_device);
    }
    void suspend() { if (m_out) m_out->suspend(); }
    void resume() { if (m_out) m_out->resume(); }
    void setVolume(double v) { m_volume = v; if (m_out) m_out->setVolume(v); }
    void restart()
    {
        m_device->clearBuffer();
        if (m_out) { m_out->stop(); delete m_out; m_out = nullptr; }
        start();
    }
    void stop()
    {
        if (m_out) { m_out->stop(); delete m_out; m_out = nullptr; }
        m_device->clearBuffer();
    }

private:
    QAudioOutput *m_out = nullptr;
    PcmDevice *m_device = nullptr;
    std::atomic<int> m_sampleRate{0};
    std::atomic<int> m_channels{0};
    double m_volume = 1.0;
};

FfmpegVideoPlayer::FfmpegVideoPlayer(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setFillColor(QColor(Qt::black));
    setOpaquePainting(true);
    m_audioSink = new AudioSink(this);   // GUI 线程创建，QAudioOutput 只在这里存活
    m_syncTimer = new QTimer(this);
    m_syncTimer->setInterval(80);
    connect(m_syncTimer, &QTimer::timeout, this, &FfmpegVideoPlayer::syncState);
    m_syncTimer->start();
}

FfmpegVideoPlayer::~FfmpegVideoPlayer()
{
    stopDecode();
    // stopDecode() 超时会 detach：线程可能还阻塞在 FFmpeg 读里，稍后返回时仍会
    // 访问 m_stop / m_frame / publishError 等成员。对象一旦析构这些写入就是 UAF，
    // 所以析构路径必须等所有解码线程真正退出（无法 join 的 detached 线程也计入）。
    std::unique_lock<std::mutex> lk(m_exitMutex);
    m_exitCv.wait(lk, [this] { return m_activeDecoders.load() == 0; });
}

void FfmpegVideoPlayer::paint(QPainter *painter)
{
    QMutexLocker locker(&m_frameMutex);
    if (m_frame.isNull())
        return;
    const QRectF rect = boundingRect();
    QSizeF draw = m_frame.size();
    draw.scale(rect.width(), rect.height(), Qt::KeepAspectRatio);
    const QRectF target(rect.x() + (rect.width() - draw.width()) / 2.0,
                        rect.y() + (rect.height() - draw.height()) / 2.0,
                        draw.width(), draw.height());
    painter->drawImage(target, m_frame);
}

void FfmpegVideoPlayer::requestUpdate()
{
    update();
}

void FfmpegVideoPlayer::refreshAtomicVolume()
{
    const int v = m_muted ? 0 : qBound(0, int(m_volume * 1000.0), 1000);
    m_vol.store(v);
}

void FfmpegVideoPlayer::setVolume(qreal v)
{
    const qreal nv = qBound(qreal(0), v, qreal(1));
    if (qFuzzyCompare(nv, m_volume))
        return;
    m_volume = nv;
    refreshAtomicVolume();
    emit volumeChanged();
}

void FfmpegVideoPlayer::setMuted(bool m)
{
    if (m == m_muted)
        return;
    m_muted = m;
    refreshAtomicVolume();
    emit mutedChanged();
}

void FfmpegVideoPlayer::publishError(const QString &msg)
{
    {
        QMutexLocker locker(&m_errorMutex);
        m_error = msg;
    }
    m_failed.store(true);
    m_busy.store(false);
    m_playing.store(false);
}

int FfmpegVideoPlayer::interruptCallback(void *opaque)
{
    const InterruptCtx *ctx = static_cast<const InterruptCtx *>(opaque);
    if (!ctx || !ctx->self) return 0;
    // 本代被停 或 已被更新一代取代：两种情况都要立刻中断 FFmpeg 的阻塞读。
    // 后者是关键——m_stop 是全局标志，起新线程时会被清零，光看它会漏掉旧线程。
    return (ctx->self->m_stop.load()
            || ctx->gen != ctx->self->m_decodeGen.load()) ? 1 : 0;
}

void FfmpegVideoPlayer::stopDecode()
{
    // 递增代次：即使超时后只能 detach，旧线程也会在下一轮循环自行退出，
    // 不会和新开的解码线程同时写同一个对象。
    const int gen = m_decodeGen.fetch_add(1);
    // 即使线程已 detach 也要置停止位：FFmpeg 的 interruptCallback 读它，
    // 能让残留线程尽快从阻塞读里退出。
    m_stop.store(true);
    m_pause.store(false);
    m_waitCv.notify_all();
    if (!m_thread.joinable())
        return;
    // 不再无界 join：FFmpeg 阻塞读可能拖住线程，GUI 线程一直等就是卡死/ANR。
    // 给 2s 收尾；超时则 detach（PCM 写已改非阻塞，正常路径会立刻退出）。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (m_threadDone.load() != gen && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (m_threadDone.load() == gen)
        m_thread.join();
    else
        m_thread.detach();
    // detach 后不能再用 m_thread 判断，但线程会在 DoneGuard 里把 activeDecoders
    // 减回去；析构时会等它归零。
}

void FfmpegVideoPlayer::open(const QUrl &url)
{
    stopDecode();
    // 旧解码线程超时残留时，它稍后发出的 stop 会被代次门禁忽略；这里在 GUI 线程
    // 直接关一次旧音频设备，避免换歌后旧视频的 PCM 还留在缓冲里继续出声。
    if (m_audioSink)
        m_audioSink->stop();
    {
        QMutexLocker locker(&m_frameMutex);
        m_frame = QImage();
    }
    {
        QMutexLocker locker(&m_errorMutex);
        m_error.clear();
    }
    m_failed.store(false);
    m_ended.store(false);
    m_seekable.store(false);
    m_busy.store(true);
    m_playing.store(true);
    m_pos.store(0);
    m_dur.store(0);
    m_seekTarget.store(-1);
    m_pause.store(false);
    m_stop.store(false);
    if (m_audioSink)
        m_audioSink->prepare();   // 在起线程前定好设备格式，swr 才能对上
    refreshAtomicVolume();
    update();
    syncState();
    // file:///G:/... 交给 FFmpeg 的 file protocol 会变成 "///G:/..." 打不开，
    // 本地路径统一还原成 "G:/..."；网络 URL 保留 toEncoded() 以免签名参数被解码。
    const QString mediaPath = url.isLocalFile()
            ? url.toLocalFile()
            : QString::fromUtf8(url.toEncoded());
    const int gen = m_decodeGen.load();
    // 先记账再起线程：线程可能在 open() 返回前就结束，晚记账会把它自己的
    // DoneGuard 减成负数，析构等待也会误判。
    m_activeDecoders.fetch_add(1);
    try {
        m_thread = std::thread(&FfmpegVideoPlayer::decodeLoop, this, mediaPath, gen);
    } catch (const std::exception &e) {
        // 线程创建失败（句柄/内存耗尽）：必须回滚计数，否则析构会永久等待。
        m_activeDecoders.fetch_sub(1);
        publishError(QStringLiteral("无法启动解码线程：%1")
                         .arg(QString::fromLocal8Bit(e.what())));
    }
}

void FfmpegVideoPlayer::close()
{
    stopDecode();
    if (m_audioSink)
        m_audioSink->stop();
    m_playing.store(false);
    m_busy.store(false);
    m_ended.store(false);
    m_failed.store(false);
    {
        QMutexLocker locker(&m_frameMutex);
        m_frame = QImage();
    }
    update();
    syncState();
}

void FfmpegVideoPlayer::seekTo(int ms)
{
    if (m_stop.load())
        return;
    m_seekTarget.store(qMax(0, ms));
    m_pos.store(qMax(0, ms));
    m_ended.store(false);
    m_waitCv.notify_all();
    syncState();
}

void FfmpegVideoPlayer::togglePlay()
{
    if (m_stop.load())
        return;
    if (m_ended.load()) {
        replay();
        return;
    }
    const bool paused = !m_pause.load();
    m_pause.store(paused);
    m_playing.store(!paused);
    m_waitCv.notify_all();
    syncState();
}

void FfmpegVideoPlayer::replay()
{
    if (m_stop.load())
        return;
    m_seekTarget.store(0);
    m_pos.store(0);
    m_ended.store(false);
    m_pause.store(false);
    m_playing.store(true);
    m_waitCv.notify_all();
    syncState();
}

void FfmpegVideoPlayer::syncState()
{
    bool stateDirty = false;
    bool posDirty = false;
    auto syncBool = [&stateDirty](bool &cached, const std::atomic<bool> &src) {
        const bool v = src.load();
        if (cached != v) {
            cached = v;
            stateDirty = true;
        }
    };
    syncBool(m_propPlaying, m_playing);
    syncBool(m_propBusy, m_busy);
    syncBool(m_propEnded, m_ended);
    syncBool(m_propSeekable, m_seekable);
    syncBool(m_propFailed, m_failed);

    QString err;
    {
        QMutexLocker locker(&m_errorMutex);
        err = m_error;
    }
    if (err != m_propError) {
        m_propError = err;
        stateDirty = true;
    }

    const int p = int(m_pos.load());
    const int d = int(m_dur.load());
    if (p != m_propPosition || d != m_propDuration) {
        m_propPosition = p;
        m_propDuration = d;
        posDirty = true;
    }
    if (stateDirty)
        emit stateChanged();
    if (posDirty)
        emit positionChanged();
}

void FfmpegVideoPlayer::decodeLoop(const QString &urlText, int gen)
{
    // 任何提前返回都要通知 stopDecode()，否则 GUI 线程会白等满 2s。
    // 同时维护 activeDecoders：析构要等包括 detached 残留线程在内的所有线程退出。
    struct DoneGuard {
        FfmpegVideoPlayer *self;
        int gen;
        ~DoneGuard()
        {
            // 用 CAS-max 记录完成代次：旧 detached 线程晚退出时不能把新线程
            // 已经写入的更大代次覆盖回去，否则 stopDecode 会白等满 2s。
            int prev = self->m_threadDone.load();
            while (prev < gen
                   && !self->m_threadDone.compare_exchange_weak(prev, gen)) {}
            self->m_activeDecoders.fetch_sub(1);
            { std::lock_guard<std::mutex> lk(self->m_exitMutex); }
            self->m_exitCv.notify_all();
        }
    } doneGuard{this, gen};
    // 栈上上下文：只在本线程执行 decodeLoop 期间有效，FFmpeg 的回调同步发生在
    // 本线程内，函数返回（含各提前 return）前 avformat_close_input 已完成。
    InterruptCtx interruptCtx{this, gen};
    PcmDevice *pcmDevice = m_audioSink ? m_audioSink->device() : nullptr;
    // QAudioOutput 只能在 GUI 线程控制，解码线程通过 queued 调用下发指令。
    // 带上代次：被新歌顶掉的残留线程不能再 start/stop/setVolume，否则会把
    // 新视频的音频设备关掉或改音量。
    auto sinkCall = [this, gen](std::function<void(AudioSink *)> fn) {
        if (m_audioSink)
            QMetaObject::invokeMethod(m_audioSink,
                [this, gen, fn]() {
                    if (gen != m_decodeGen.load()) return;
                    fn(m_audioSink);
                }, Qt::QueuedConnection);
    };

    // 网络协议必须先初始化：Windows 上要 WSAStartup + SChannel 全局状态，
    // 否则 avformat_open_input 对 http/https 直接报 "Protocol not found"。
    static std::once_flag networkOnce;
    std::call_once(networkOnce, [] {
        const int r = avformat_network_init();
        FFLOG("avformat_network_init = %d\n", r);
    });

    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) {
        publishError(QStringLiteral("无法分配 FFmpeg 上下文"));
        return;
    }
    // 网络阻塞时能立刻响应 close()，否则要等 rw_timeout 到期才能退出线程
    fmt->interrupt_callback.callback = &FfmpegVideoPlayer::interruptCallback;
    fmt->interrupt_callback.opaque = &interruptCtx;

    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "user_agent",
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36", 0);
    av_dict_set(&opts, "referer", "https://music.163.com/", 0);
    av_dict_set(&opts, "rw_timeout", "20000000", 0);  // 20s 读超时
    const QByteArray rawUrl = urlText.toUtf8();
    FFLOG("open %s\n", rawUrl.constData());
    if (ffDebug()) {
        void *opaque = nullptr;
        const char *name = nullptr;
        QByteArray list;
        while ((name = avio_enum_protocols(&opaque, 0))) {
            list.append(name);
            list.append(' ');
        }
        FFLOG("protocols=%s find=%s\n", list.constData(),
              avio_find_protocol_name(rawUrl.constData()));
    }
    int ret = avformat_open_input(&fmt, rawUrl.constData(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        FFLOG("avformat_open_input failed: %s\n", qPrintable(ffError(ret)));
        publishError(QStringLiteral("打开媒体失败：%1").arg(ffError(ret)));
        avformat_free_context(fmt);
        return;
    }

    ret = avformat_find_stream_info(fmt, nullptr);
    if (ret < 0) {
        publishError(QStringLiteral("读取媒体信息失败：%1").arg(ffError(ret)));
        avformat_close_input(&fmt);
        return;
    }

    m_dur.store(fmt->duration > 0 ? fmt->duration / (AV_TIME_BASE / 1000) : 0);
    m_seekable.store(m_dur.load() > 0);
    m_busy.store(false);

    const int vIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int aIndex = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    FFLOG("duration=%lldms video=%d audio=%d format=%s\n",
          (long long)m_dur.load(), vIndex, aIndex,
          fmt->iformat ? fmt->iformat->name : "?");
    if (vIndex < 0 && aIndex < 0) {
        publishError(QStringLiteral("媒体中没有可解码的视频或音频流"));
        avformat_close_input(&fmt);
        return;
    }

    AVCodecContext *vctx = nullptr;
    AVCodecContext *actx = nullptr;
    auto openStream = [&](int index, AVCodecContext **out) {
        if (index < 0)
            return true;
        const AVCodecParameters *par = fmt->streams[index]->codecpar;
        const AVCodec *codec = avcodec_find_decoder(par->codec_id);
        if (!codec) {
            publishError(QStringLiteral("缺少解码器（编码 ID %1）").arg(int(par->codec_id)));
            return false;
        }
        *out = avcodec_alloc_context3(codec);
        if (!*out)
            return false;
        if (avcodec_parameters_to_context(*out, par) < 0 || avcodec_open2(*out, codec, nullptr) < 0) {
            publishError(QStringLiteral("初始化解码器失败"));
            return false;
        }
        return true;
    };
    if (!openStream(vIndex, &vctx) || !openStream(aIndex, &actx)) {
        avcodec_free_context(&vctx);
        avcodec_free_context(&actx);
        avformat_close_input(&fmt);
        return;
    }

    // ---- 音频输出（Qt5 QAudioOutput，纯 PCM，不依赖任何 mediaservice 插件）----
    // QAudioOutput 由 GUI 线程的 AudioSink 持有；解码线程只推 PCM，不跨线程调 Qt API。
    const bool hasAudioStream = (actx != nullptr);
    // swr 的目标采样率必须等于 AudioSink 实际打开的设备格式：QAudioOutput 不会
    // 替我们重采样，48k 源硬写进 44.1k 设备会变调/加速。
    const int outRate = (m_audioSink && m_audioSink->sampleRate() > 0)
                            ? m_audioSink->sampleRate() : 48000;
    const int outChannels = (m_audioSink && m_audioSink->channelCount() > 0)
                                ? m_audioSink->channelCount() : 2;
    if (hasAudioStream && pcmDevice)
        sinkCall([](AudioSink *s) { s->start(); });

    SwrContext *swr = nullptr;
    SwsContext *sws = nullptr;
    int swsW = 0, swsH = 0;
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    if (!frame || !pkt) {
        publishError(QStringLiteral("无法分配解码缓冲区"));
        av_frame_free(&frame);
        av_packet_free(&pkt);
        if (hasAudioStream) sinkCall([](AudioSink *s) { s->stop(); });
        avcodec_free_context(&vctx);
        avcodec_free_context(&actx);
        avformat_close_input(&fmt);
        return;
    }

    bool firstFrame = false;
    const std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::now();
    double pausedTotalMs = 0.0;
    double clockBaseMs = 0.0;     // seek 后的媒体时间基准
    double clockAnchorMs = 0.0;   // 该基准对应的墙钟时刻
    int frameCounter = 0;

    auto elapsedRawMs = [&]() -> double {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - startedAt).count();
    };
    auto wallMs = [&]() -> double {
        return elapsedRawMs() - pausedTotalMs;
    };
    // 主时钟用单调墙钟。Qt5 的 QAudioOutput::processedUSecs() 在没有事件
    // 循环的解码线程里可能恒为 0，拿它当主时钟会让视频永久卡在第一帧
    //（实测：截图 9 秒后仍停在 00:00）。音频写入是阻塞式的，设备按自身
    // 速率消费，天然提供背压；音频设备异常时视频也能继续正常推进。
    auto masterClockMs = [&]() -> double {
        return clockBaseMs + (wallMs() - clockAnchorMs);
    };

    auto waitIfPaused = [&]() -> bool {
        if (m_stop.load())
            return false;
        if (!m_pause.load())
            return true;
        const double pauseStart = elapsedRawMs();
        if (hasAudioStream) sinkCall([](AudioSink *s) { s->suspend(); });
        {
            std::unique_lock<std::mutex> lk(m_waitMutex);
            m_waitCv.wait(lk, [this] { return !m_pause.load() || m_stop.load(); });
        }
        if (hasAudioStream) sinkCall([](AudioSink *s) { s->resume(); });
        // 暂停时长不计入播放时钟
        pausedTotalMs += elapsedRawMs() - pauseStart;
        return !m_stop.load();
    };

    auto waitForPts = [&](double ptsMs) -> bool {
        const double deadline = elapsedRawMs() + 2000.0;
        for (;;) {
            if (m_stop.load())
                return false;
            if (m_seekTarget.load() >= 0)
                return true;                    // seek 打断当前帧，回到主循环处理
            const double diff = ptsMs - masterClockMs();
            if (diff <= 5.0)
                return true;
            // 任何时钟异常都不能让解码线程永久卡在一帧上
            if (elapsedRawMs() > deadline)
                return true;
            if (m_pause.load() && !waitIfPaused())
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(
                int(std::min(40.0, std::max(1.0, diff)))));
        }
    };

    auto doSeek = [&](int targetMs) {
        const qint64 ts = qint64(targetMs) * (AV_TIME_BASE / 1000);
        if (av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD) >= 0) {
            if (vctx) avcodec_flush_buffers(vctx);
            if (actx) avcodec_flush_buffers(actx);
            if (swr) { swr_free(&swr); }
            if (hasAudioStream) sinkCall([](AudioSink *s) { s->restart(); });
            clockAnchorMs = wallMs();
            clockBaseMs = targetMs;
            m_pos.store(targetMs);
        }
    };

    while (!m_stop.load() && gen == m_decodeGen.load()) {
        if (!waitIfPaused())
            break;
        const int seek = m_seekTarget.exchange(-1);
        if (seek >= 0)
            doSeek(seek);
        if (hasAudioStream) {
            const double vol = m_vol.load() / 1000.0;
            sinkCall([vol](AudioSink *s) { s->setVolume(vol); });
        }

        ret = av_read_frame(fmt, pkt);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
            FFLOG("end of stream ret=%d frames=%s\n", ret, firstFrame ? "yes" : "no");
            m_ended.store(true);
            m_playing.store(false);
            break;
        }
        if (ret < 0) {
            FFLOG("av_read_frame failed: %s\n", qPrintable(ffError(ret)));
            if (!m_stop.load())
                publishError(QStringLiteral("读取媒体数据失败：%1").arg(ffError(ret)));
            break;
        }

        if (pkt->stream_index == vIndex && vctx) {
            if (avcodec_send_packet(vctx, pkt) == 0) {
                while (avcodec_receive_frame(vctx, frame) == 0) {
                    const double ptsMs = ptsToMs(frame->best_effort_timestamp,
                                                 fmt->streams[vIndex]->time_base);
                    if (!waitForPts(ptsMs)) {
                        av_frame_unref(frame);
                        break;
                    }
                    if (m_seekTarget.load() < 0 && !m_stop.load()) {
                        const int srcW = frame->width;
                        const int srcH = frame->height;
                        if (srcW > 0 && srcH > 0) {
                            // 1080p 缩到 1280 宽足够 MV 窗口用，降低 CPU 与纹理上传开销
                            const int dstW = std::min(srcW, 1280);
                            const int dstH = std::max(1, int(double(srcH) * dstW / srcW) & ~1);
                            if (!sws || swsW != srcW || swsH != srcH) {
                                sws_freeContext(sws);
                                sws = sws_getContext(srcW, srcH, AVPixelFormat(frame->format),
                                                     dstW, dstH, AV_PIX_FMT_BGRA,
                                                     SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                                swsW = srcW; swsH = srcH;
                            }
                            if (sws) {
                                // QImage::Format_RGB32 在小端上就是 BGRA 字节序
                                QImage image(dstW, dstH, QImage::Format_RGB32);
                                uint8_t *dst[1] = { image.bits() };
                                int dstStride[1] = { int(image.bytesPerLine()) };
                                sws_scale(sws, frame->data, frame->linesize, 0, srcH,
                                          dst, dstStride);
                                {
                                    QMutexLocker locker(&m_frameMutex);
                                    m_frame = image;
                                }
                                QMetaObject::invokeMethod(this, "requestUpdate", Qt::QueuedConnection);
                                if (!firstFrame) {
                                    firstFrame = true;
                                    FFLOG("first video frame %dx%d -> %dx%d\n",
                                          srcW, srcH, dstW, dstH);
                                }
                            }
                        }
                        m_pos.store(qint64(std::max(0.0, ptsMs)));
                        if (++frameCounter % 60 == 0)
                            FFLOG("frame %d pos=%lldms clock=%.0fms\n", frameCounter,
                                  (long long)m_pos.load(), masterClockMs());
                    }
                    av_frame_unref(frame);
                    if (m_stop.load() || m_seekTarget.load() >= 0)
                        break;
                }
            }
        } else if (pkt->stream_index == aIndex && actx) {
            if (avcodec_send_packet(actx, pkt) == 0) {
                while (avcodec_receive_frame(actx, frame) == 0) {
                    const double apts = ptsToMs(frame->best_effort_timestamp,
                                                fmt->streams[aIndex]->time_base);
                    if (pcmDevice) {
                        if (!swr) {
                            // AV_CHANNEL_LAYOUT_* 是 C 聚合初始化宏，不能放进三目运算符
                            // （Qt5 用 C++14 会报 "expected primary-expression"）。
                            AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
                            if (outChannels == 1) outLayout = AV_CHANNEL_LAYOUT_MONO;
                            swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, outRate,
                                                &frame->ch_layout, AVSampleFormat(frame->format),
                                                frame->sample_rate, 0, nullptr);
                            if (swr) swr_init(swr);
                        }
                        if (swr) {
                            const int outSamples = av_rescale_rnd(
                                swr_get_delay(swr, frame->sample_rate) + frame->nb_samples,
                                outRate, frame->sample_rate, AV_ROUND_UP);
                            const int bufSize = av_samples_get_buffer_size(
                                nullptr, outChannels, outSamples, AV_SAMPLE_FMT_S16, 1);
                            QByteArray pcm(bufSize > 0 ? bufSize : 0, Qt::Uninitialized);
                            uint8_t *out[1] = { reinterpret_cast<uint8_t *>(pcm.data()) };
                            const int n = swr_convert(swr, out, outSamples,
                                const_cast<const uint8_t **>(frame->extended_data),
                                frame->nb_samples);
                            // 代次已变说明这是被顶掉的残留线程，不能再往共用的
                            // PcmDevice 里塞旧歌 PCM（会和新歌声音交错）。
                            if (n > 0 && gen == m_decodeGen.load()) {
                                // 非阻塞推送：缓冲满就丢帧，解码线程不再等音频设备。
                                // 这是 stopDecode() 能快速收尾的关键（以前会卡在 write()）。
                                pcmDevice->push(pcm.constData(),
                                                qint64(n) * outChannels * 2);  // S16
                            }
                        }
                    } else if (vIndex < 0) {
                        // 纯音频且无音频设备：仍然推进进度条
                        m_pos.store(qint64(std::max(0.0, apts)));
                    }
                    av_frame_unref(frame);
                    if (m_stop.load())
                        break;
                }
            }
        }
        av_packet_unref(pkt);
    }

    m_playing.store(false);
    m_busy.store(false);
    if (hasAudioStream) sinkCall([](AudioSink *s) { s->stop(); });
    if (swr) swr_free(&swr);
    if (sws) sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&vctx);
    avcodec_free_context(&actx);
    avformat_close_input(&fmt);
}

} // namespace Furui

#endif // FURUI_HAVE_FFMPEG
