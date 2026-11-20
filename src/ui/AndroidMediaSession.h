#pragma once

#include <QObject>
#include <QString>

namespace Furui {

struct Song;

// Android 播放前台服务 / MediaSession 的 C++ 侧桥接。
//
// 生命周期跟随整个应用：install() 在 QML 引擎加载前调用一次，之后
//   PlayerController 的换歌/播放状态变化 → 推给 Java（通知栏、锁屏、车机）；
//   Java 收到的通知/耳机/车机按键 → 回传到 PlayerController 执行。
// 非 Android 平台 install() 什么也不做（编译期剔除全部 JNI 代码）。
class AndroidMediaSession : public QObject
{
    Q_OBJECT
public:
    static void install();
    // 由 JNI 回调转进来（Java 线程 → Qt 主线程），也可供测试直接调用
    static void handleCommand(int action, qint64 position);
    // 车机语音点歌（BYD AUTOVOICE_SEARCH_PLUS → Java → 此处）：
    // 搜索关键词并自动播放第一条结果。非 Android 平台为空实现。
    static void handleVoiceSearch(const QString &keyword);
    // 车机歌单/模式类口令（播放我喜欢的歌/最近在听/随机/收藏切换），
    // 由 handleCommand 的 default 分支转入；非 Android 平台为空实现。
    static void handleVoiceListCommand(int action);
private:
    explicit AndroidMediaSession(QObject *parent = nullptr);
    // 把歌曲封面解析成本地文件路径：本地封面直接用，网络封面下载到缓存目录
    void syncCover(const Song &song);
    void pushMetadata();
    void pushPlayback();
    void ensureService();

    QString m_coverPath;   // 已落盘的封面路径（空 = 无封面）
    QString m_coverRemote; // 正在解析的原始封面地址，用于丢弃过期的下载回调
};

} // namespace Furui
