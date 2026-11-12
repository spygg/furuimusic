// 福瑞音乐 - Qt 5 自编译 FFmpeg 播放后端实现
// 仅在 FURUI_HAVE_FFMPEG 定义时参与编译（见 CMakeLists.txt / FuruiMusic.pro）。
#ifdef FURUI_HAVE_FFMPEG

#include "FfmpegVideoPlayer.h"

#include <QPainter>
#include <QMetaObject>
#include <QMutexLocker>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

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

} // namespace

FfmpegVideoPlayer::FfmpegVideoPlayer(QQuickItem *parent)
    : QQuickPaintedItem(parent)
{
    setFillColor(QColor(Qt::black));
    setOpaquePainting(true);
    m_syncTimer = new QTimer(this);
    m_syncTimer->setInterval(80);
    connect(m_syncTimer, &QTimer::timeout, this, &FfmpegVideoPlayer::syncState);
    m_syncTimer->start();
}

FfmpegVideoPlayer::~FfmpegVideoPlayer()
{
    stopDecode();
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
    FfmpegVideoPlayer *self = static_cast<FfmpegVideoPlayer *>(opaque);
    return (self && self->m_stop.load()) ? 1 : 0;
}

void FfmpegVideoPlayer::stopDecode()
{
    if (!m_thread.joinable())
        return;
    m_stop.store(true);
    m_pause.store(false);
    m_waitCv.notify_all();
    m_thread.join();
}

void FfmpegVideoPlayer::open(const QUrl &url)
{
    stopDecode();
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
    refreshAtomicVolume();
    update();
    syncState();
    // file:///G:/... 交给 FFmpeg 的 file protocol 会变成 "///G:/..." 打不开，
    // 本地路径统一还原成 "G:/..."；网络 URL 保留 toEncoded() 以免签名参数被解码。
    const QString mediaPath = url.isLocalFile()
            ? url.toLocalFile()
            : QString::fromUtf8(url.toEncoded());
    m_thread = std::thread(&FfmpegVideoPlayer::decodeLoop, this, mediaPath);
}

void FfmpegVideoPlayer::close()
{
    stopDecode();
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

void FfmpegVideoPlayer::decodeLoop(const QString &urlText)
{
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
    fmt->interrupt_callback.opaque = this;

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
    QAudioOutput *audioOut = nullptr;
    QIODevice *audioIo = nullptr;
    int outRate = 48000;
    if (actx) {
        QAudioFormat af;
        af.setSampleRate(outRate);
        af.setChannelCount(2);
        af.setSampleSize(16);
        af.setCodec(QStringLiteral("audio/pcm"));
        af.setByteOrder(QAudioFormat::LittleEndian);
        af.setSampleType(QAudioFormat::SignedInt);
        const QAudioDeviceInfo dev = QAudioDeviceInfo::defaultOutputDevice();
        const QAudioFormat usable = dev.isFormatSupported(af) ? af : dev.nearestFormat(af);
        outRate = usable.sampleRate() > 0 ? usable.sampleRate() : outRate;
        audioOut = new QAudioOutput(dev, usable, nullptr);   // 与解码线程同生共死
        audioOut->setBufferSize(64 * 1024);
        audioOut->setVolume(m_vol.load() / 1000.0);
        audioIo = audioOut->start();
        if (!audioIo) {           // 无音频设备时降级为「只有画面」，不算播放失败
            delete audioOut;
            audioOut = nullptr;
        }
    }

    SwrContext *swr = nullptr;
    SwsContext *sws = nullptr;
    int swsW = 0, swsH = 0;
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    if (!frame || !pkt) {
        publishError(QStringLiteral("无法分配解码缓冲区"));
        av_frame_free(&frame);
        av_packet_free(&pkt);
        if (audioOut) { audioOut->stop(); delete audioOut; }
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
    int audioWriteFailures = 0;
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
        if (audioOut)
            audioOut->suspend();
        {
            std::unique_lock<std::mutex> lk(m_waitMutex);
            m_waitCv.wait(lk, [this] { return !m_pause.load() || m_stop.load(); });
        }
        if (audioOut)
            audioOut->resume();
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
            if (audioOut) {
                audioOut->stop();
                audioIo = audioOut->start();
            }
            clockAnchorMs = wallMs();
            clockBaseMs = targetMs;
            m_pos.store(targetMs);
        }
    };

    while (!m_stop.load()) {
        if (!waitIfPaused())
            break;
        const int seek = m_seekTarget.exchange(-1);
        if (seek >= 0)
            doSeek(seek);
        if (audioOut)
            audioOut->setVolume(m_vol.load() / 1000.0);

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
                    if (audioOut && audioIo) {
                        if (!swr) {
                            AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
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
                                nullptr, 2, outSamples, AV_SAMPLE_FMT_S16, 1);
                            QByteArray pcm(bufSize > 0 ? bufSize : 0, Qt::Uninitialized);
                            uint8_t *out[1] = { reinterpret_cast<uint8_t *>(pcm.data()) };
                            const int n = swr_convert(swr, out, outSamples,
                                const_cast<const uint8_t **>(frame->extended_data),
                                frame->nb_samples);
                            if (n > 0) {
                                int left = n * 2 * 2;  // 2ch * S16
                                const char *p = pcm.constData();
                                bool ok = true;
                                while (left > 0 && !m_stop.load()) {
                                    const qint64 written = audioIo->write(p, left);
                                    if (written <= 0) { ok = false; break; }
                                    p += written;
                                    left -= int(written);
                                }
                                audioWriteFailures = ok ? 0 : audioWriteFailures + 1;
                                // 音频设备连续写不进去就停掉音频只留画面，
                                // 否则每帧都白等设备，拖慢整个解码循环
                                if (audioWriteFailures > 5) {
                                    FFLOG("audio device stalled, disable audio output\n");
                                    audioOut->stop();
                                    delete audioOut;
                                    audioOut = nullptr;
                                    audioIo = nullptr;
                                }
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
    if (audioOut) {
        audioOut->stop();
        delete audioOut;
    }
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
