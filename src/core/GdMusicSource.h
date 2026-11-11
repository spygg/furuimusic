#pragma once
// 福瑞音乐 - GD音乐台 内置默认音源
//
// 对标 reference/AlgerMusicPlayer 的 GD音乐台聚合解析（src/renderer/api/gdmusic.ts）：
//   依次用 joox / tidal / netease 三个上游按「歌名 + 歌手」搜索，歌名歌手都匹配上
//   才取直链，宁可失败也不返回错歌（避免货不对版）；取链时 br=999 要最高可用音质。
//   接口：GET https://music-api.gdstudio.xyz/api.php
//         ?types=search&source=<src>&name=<query>&count=5&pages=1
//         ?types=url&source=<src>&id=<id>&br=999
//
// 定位：默认音源。原平台取链失败后的替代链里排在用户自建音源之后、
// LX 在线脚本音源之前；没导入任何音源时它就是开箱即用的兜底。
// 可用设置键 gdMusicSource（默认 true）整体关闭。
#include <functional>

#include "Types.h"

namespace Furui {

class GdMusicSource {
public:
    using PlayableCallback = std::function<void(const Playable &)>;
    static void resolve(const Song &song, const QString &quality, PlayableCallback cb);
};

} // namespace Furui
