# 多平台音乐聚合与登录后歌单 —— 只读调研与落地方案

> 适用工程：FuruiMusic（Qt 5.7 / QML + Qt C++，桌面与移动端同一套代码）
> 调研时间：2026-11-07
> 调研范围：网易云音乐、QQ 音乐、酷狗音乐、酷我音乐、咪咕音乐；附 B 站、汽水音乐。
> 参考实现：仓库内 `reference/lx-music-desktop-git`（洛雪音乐桌面版源码）。

---

## 0. 证据等级与边界声明

本文只做**只读调研**，未修改 `src/`、未改构建配置、未构建、未提交 git，也未向任何平台发起线上请求。文中每条结论都标注证据等级：

| 标记 | 含义 |
|---|---|
| `【仓】` | 本仓库代码或 `config/sources.json` 中已存在；仓库注释明确写过“实测”的，文中会单独注明 |
| `【洛】` | 洛雪音乐 `reference/lx-music-desktop-git` 源码中存在该实现，本次只做了代码核对，**未复测** |
| `【未验证】` | 常见公开做法或社区资料，但本次既未在仓库、也未在洛雪代码中找到确证；不可直接当作稳定契约 |
| `【实测·仓注】` | 仓库代码注释中记录了当时的线上实测结果，本次未复测 |

**重要原则**：各平台这些接口都不是公开 API，随时可能变更、加签、风控或下线。本文的目标是给出“当前可依据的接口形状 + 风险点”，而不是承诺长期可用。

---

## 1. 结论摘要（先看这 6 条）

1. **五个平台的“搜索 / 歌词 / 榜单 / 歌单分类 / 歌单详情”元数据基本都能匿名获取**，但难度递增：网易云（weapi/eapi）< 酷狗（MD5 签名）< 酷我（部分接口要 csrf）< 咪咕（固定 sign + deviceId）< QQ（musicu 签名 + 付费/登录限制）。
2. **播放地址是最大的不确定项**。洛雪当前版本的内置 SDK 已不再内置各平台播放地址，`getMusicUrl` 全部委托给 `apis(source)`（用户导入的 User API / 自定义源），`api-source-info.ts` 中的官方地址实现基本被注释。结论：**播放地址必须按“可插拔解析器”设计，不能当作内置稳定能力**。
3. **登录后歌单**：本仓库已打通网易云、QQ、酷狗的原生登录 + 用户歌单；酷我、咪咕目前只有 Cookie 导入，二维码/手机号要依赖自建账号 API 服务；**最近播放五个平台都未实现**。
4. 现有聚合层是“**一个平台一个 MusicSource 实例、只保留一个登录账号、搜索并发后简单排序**”，无法支撑“多账号隔离 + 跨平台歌单 + 能力差异”。需要把会话从“按平台”升级为“按账号”，并增加能力声明与去重合并。
5. **不建议在 MVP 引入洛雪式 JS 音源插件**。洛雪脚本需要 `lx.request`/`lx.crypto`/`lx.zlib` 运行时，且能接触请求上下文；在 C++/QML 中引入等于引入一个可执行第三方代码的沙箱。第一阶段用现有 HTTP 自定义源契约即可，第二阶段再评估 `QJSEngine` 受限沙箱。
6. **合规边界**：只做“用户自带账号/Cookie 的客户端”，不内置绕过 VIP/DRM 的解析，不批量抓取，不把临时播放地址缓存或分发；本地 Cookie 目前只是混淆存储，不是强加密。

---

## 2. 本仓库现状盘点

### 2.1 已有模块

| 模块 | 作用 | 关键事实 |
|---|---|---|
| [MusicSource.h](file:///G:/free_music_hunyuan/src/core/MusicSource.h#L18-L50) | 配置驱动的平台适配器 | 搜索、歌单搜索、榜单、歌单详情、播放地址、歌词、用户歌单、收藏全部由 `sources.json` 的“端点 + 字段映射”驱动 |
| [SourceManager.cpp](file:///G:/free_music_hunyuan/src/core/SourceManager.cpp#L37-L82) | 多源聚合入口 | `refreshSessions()` 每个平台只取**第一个已登录账号**注入 `setSession`；`search()` 并发请求后仅按 `normalizedKey().size()` 稳定排序，**没有跨源去重/分组** |
| [Types.h](file:///G:/free_music_hunyuan/src/core/Types.h#L20-L62) | Song / Playlist / Account | `Account` 只有 `source/uid/nickname/avatar/cookie/token/logged/...`，没有“账号主键/多账号”维度；`Song` 有 `otherSource` 用于跨源换源 |
| [Library.cpp](file:///G:/free_music_hunyuan/src/core/Library.cpp#L50-L50) | SQLite 持久化 | `accounts` 表主键是 `source`，即**一个平台只能存一个账号**；Cookie/Token 走 `obfuscate()` 轻量混淆（[Library.h](file:///G:/free_music_hunyuan/src/core/Library.h#L50-L52) 明确“并非强加密”） |
| [AuthManager.cpp](file:///G:/free_music_hunyuan/src/core/AuthManager.cpp#L118-L230) | 登录编排 | 内置二维码登录覆盖网易云/QQ/酷狗；其它平台走账号 API；成功判据按平台区分 |
| [UserApi.cpp](file:///G:/free_music_hunyuan/src/core/UserApi.cpp#L271-L343) | 登录后用户数据 | 三种驱动：`netease`（linuxapi 转发）、`direct`（复用 MusicSource）、`http`（自建账号 API） |
| [CustomSource.cpp](file:///G:/free_music_hunyuan/src/core/CustomSource.cpp) | 自定义 HTTP 音源 | HTTP 契约 + 本地目录匹配 + 换源兜底；**没有 JS 脚本插件运行时** |
| [sources.json](file:///G:/free_music_hunyuan/config/sources.json) | 六个源配置 | 网易云、QQ、酷狗、酷我、咪咕、汽水；部分端点是“示例模板”，酷狗歌单详情注释称“已实测通过” |

### 2.2 现有实现能回答“登录后能不能拿歌单”

| 平台 | 内置登录 | 用户资料 | 收藏歌单列表 | 自建歌单列表 | 我喜欢的音乐 | 最近播放 |
|---|---|---|---|---|---|---|
| 网易云 | 二维码 / 手机号或邮箱密码 / Cookie `【仓】` | ✅ | ✅ | ✅ | ✅ 名称包含匹配 `【仓】` | ❌ |
| QQ | 二维码 / Cookie `【仓】`；手机号/密码未实现 | ✅ | ✅ | ✅ | ✅ 名称包含“我喜欢的音乐” `【仓】` | ❌ |
| 酷狗 | 二维码 / Cookie `【仓】`；手机号/密码未实现 | ✅ | ✅ | ✅ | ✅ 名称匹配 `【仓】` | ❌ |
| 酷我 | 仅 Cookie `【仓】`；二维码/手机号靠账号 API `【未验证】` | 依赖账号 API | ❌ 内置直连缺失 | ❌ | ❌ | ❌ |
| 咪咕 | 仅 Cookie `【仓】`；二维码/手机号靠账号 API `【未验证】` | 依赖账号 API | ❌ 内置直连缺失 | ❌ | ❌ | ❌ |

> 结论：**“登录后获取歌单”目前只有网易云/QQ/酷狗是原生可用的**；酷我/咪咕必须补外置账号 API，否则只能导入 Cookie 后做很有限的事。最近播放需要新增接口与库表，不属于“已支持”。

---

## 3. 各平台接口明细

### 3.1 网易云音乐

**接口形状**（`【仓】` 配置 + `【洛】` 代码）：

| 能力 | 接口 | 方法 | 关键参数 | 关键返回 |
|---|---|---|---|---|
| 搜索（旧/通用） | `https://music.163.com/api/search/get` | GET/POST | `s`、`type=1/1000`、`limit`、`offset` | `result.songs[]` / `result.playlists[]` |
| 搜索（洛雪当前） | `POST http://interface.music.163.com/eapi/batch`，业务路径 `/api/search/song/list/page` | POST | `keyword`、`needCorrect=1`、`channel=typing`、`offset`、`limit`、`scene=normal`、`total` | `data.resources[]`、`data.totalCount` `【洛】` |
| 歌曲详情 | `https://music.163.com/api/song/detail?ids=[id]` | GET | `ids` | `songs[]`：`id/name/ar[]/al/dt` `【未验证】` |
| 播放地址 | `https://music.163.com/api/song/enhance/player/url?ids=[id]&br=320000` | GET | `ids`、`br` | `data[0].url/code/br` `【仓】`（高音质/VIP 需登录态 `【未验证】`） |
| 歌词 | `POST https://interface3.music.163.com/eapi/song/lyric/v1` | POST | `id`、`lv/kv/tv`、`cp=1` | `lrc.lyric`、`tlyric.lyric` `【洛】` |
| 榜单列表 | `POST https://music.163.com/weapi/toplist` | POST | weapi 空对象 | 榜单列表 `【洛】` |
| 榜单/歌单详情 | `POST https://music.163.com/weapi/v3/playlist/detail` | POST | `id`、`n`、`s` | `playlist.tracks[]` `【洛】` |
| 歌单分类 | `POST https://music.163.com/weapi/playlist/catalogue` | POST | weapi 空对象 | `categories/sub[]` `【洛】` |
| 歌单列表 | `POST https://music.163.com/weapi/playlist/list` | POST | `cat`、`order=hot/new`、`offset`、`limit` | 歌单列表 `【洛】` |
| 歌单搜索 | 同搜索接口 `type=1000` | GET/POST | 同上 | `result.playlists[]` `【仓】` |
| 用户资料 | `POST /api/nuser/account/get`（经 linuxapi 转发） | POST | 登录 Cookie | `profile` `【仓】` |
| 用户歌单 | `/api/user/playlist?uid=&limit=&offset=` | GET | `uid` | `playlist[]`，含“我喜欢的音乐”等特殊歌单 `【仓】` |
| 歌单歌曲 | `/api/v3/playlist/detail?id=&n=&s=8` | GET | `id` | `playlist.tracks[]` `【仓】` |

**加密/签名 `【仓】`**：

- weapi：`params = base64(AES-128-CBC(base64(AES-128-CBC(text, "0CoJUm6Qyw8W8jud")), randomKey))` + `encSecKey = RSA(randomKey)`，见 [NeteaseCrypto.h](file:///G:/free_music_hunyuan/src/core/NeteaseCrypto.h#L2-L23)。
- 洛雪当前搜索/歌词用 eapi（AES-ECB + 固定 key 派生），本仓库未实现 eapi，只有 weapi。要跟洛雪搜索路径，需要补 eapi。
- 请求头需要 `Referer: https://music.163.com`、PC UA；登录态依赖 `MUSIC_U` Cookie，写操作还需要 `__csrf`。

**登录 `【仓】` / `【实测·仓注】`**：

- 二维码：`POST /weapi/login/qrcode/unikey`（`type=1`）取 `unikey` → 轮询 `POST /weapi/login/qrcode/client/login`（`key`、`type=1`）。代码注释明确记录：旧的 `/weapi/login/qrcode/create` 已下线（404），出码接口是 `unikey`（[AuthManager.cpp](file:///G:/free_music_hunyuan/src/core/AuthManager.cpp#L140-L144)）。
- 手机号：`POST /weapi/login/cellphone`，`phone` + `password=md5(明文)` + `countrycode`。
- 邮箱：`POST /weapi/login`，`username` + `password=md5(明文)`（[AuthManager.cpp](file:///G:/free_music_hunyuan/src/core/AuthManager.cpp#L861-L892)）。
- Cookie 导入：直接保存，后续请求带 `MUSIC_U`。

**限制/风险**：

- 播放地址对 VIP/版权歌曲会返回 `code=404/-1/401` 或空 `url`；高音质通常要求登录 Cookie。
- weapi 的 `encSecKey` 是固定模幂，平台若换 key 会整体失效。
- 搜索接口有频控，短时间大量请求容易 460/403 `【未验证】`。

---

### 3.2 QQ 音乐

**接口形状**（`【洛】` + `【仓】`）：

| 能力 | 接口 | 方法 | 关键参数 | 关键返回 |
|---|---|---|---|---|
| 搜索（旧） | `https://c.y.qq.com/soso/fcgi-bin/client_search_cp` | GET | `w`、`p`、`n`、`t=0`、`format=json` | `data.song.list[]`：`songmid/songname/singer[]/albumname/interval/albummid` `【仓】` |
| 搜索（musicu） | `POST https://u.y.qq.com/cgi-bin/musics.fcg?sign=zzc…` | POST | module `music.search.SearchCgiService` / method `DoSearchForQQMusicDesktop`；`query/page_num/num_per_page/searchid` | `body.song.list[]` `【洛】` |
| 歌曲详情 | `POST https://u.y.qq.com/cgi-bin/musicu.fcg` | POST | module `music.pf_song_detail_svr` / method `get_song_detail_yqq`；`song_mid` | `track_info` `【洛】` |
| 播放地址 | 无内置；洛雪委托 `apis('tx').getMusicUrl` | - | - | 本仓库配置走 `fcg_music_express_mobile3.fcg`，属**示例模板**，多数曲目拿不到有效 vkey `【未验证】` |
| 歌词 | `POST https://u.y.qq.com/cgi-bin/musicu.fcg` | POST | module `music.musichallSong.PlayLyricInfo` / method `GetPlayLyricInfo`；`songMID/songID`、`qrc=1/trans=1` | `lyric/trans` `【洛】` |
| 榜单列表 | `https://c.y.qq.com/v8/fcg-bin/fcg_myqq_toplist.fcg` | GET | `format=json`、`uin=0` | `data.topList[]` `【洛】` |
| 榜单详情 | `POST https://u.y.qq.com/cgi-bin/musicu.fcg` | POST | module `musicToplist.ToplistInfoServer` / method `GetDetail` | `data.song[]` `【洛】` |
| 歌单分类 | `https://u.y.qq.com/cgi-bin/musicu.fcg?...data=…` | GET | module `playlist.PlaylistAllCategoriesServer` / `get_all_categories` | 分类树 `【洛】` |
| 分类歌单 | 同上 | GET | module `playlist.PlayListCategoryServer` / `get_category_content`；`titleid/page/size` | `data.content` `【洛】` |
| 歌单详情 | `https://c.y.qq.com/qzone/fcg-bin/fcg_ucc_getcdinfo_byids_cp.fcg` | GET | `disstid`、`type=1`、`json=1`、`onlysong=0` | `cdlist[0].songlist[]` `【洛】`、`【仓】` |
| 歌单搜索 | `http://c.y.qq.com/soso/fcgi-bin/client_music_search_songlist` | GET | `query/page_no/num_per_page/format=json` | 歌单列表 `【洛】` |
| 用户歌单 | `https://c.y.qq.com/rsc/fcgi-bin/fcg_user_created_diss` | GET | `hostuin`、`sin`、`size`、`g_tk` | `data.disslist[]` `【仓】` |
| 收藏歌单歌曲 | `fcg_ucc_getcdinfo_byids_cp` | GET | `disstid`、`onlysong=0` | `songlist[]` `【仓】` |

**签名/加密 `【洛】`/`【仓】`**：

- `sign = zzcSign(data)`：SHA1 后按固定索引取片段、base64 混淆，输出 `zzc…`；请求 `musics.fcg?sign=…`（[tx/utils/crypto.js](file:///G:/free_music_hunyuan/reference/lx-music-desktop-git/src/renderer/utils/musicSdk/tx/utils/crypto.js)）。
- `comm` 公共参数：`cv/ct/uin/format`；匿名 `uin=0`。
- `g_tk = hash33(qqmusic_key|qm_keyst|p_skey|skey)`；扫码 `ptqrtoken = hash33(qrsig)`，见 [QqProtocol.h](file:///G:/free_music_hunyuan/src/core/QqProtocol.h#L5-L21)。
- 设备标识 `guid`，登录后接口需要 `uin` + `g_tk` + Cookie。

**登录 `【仓】`**：

1. `ptlogin2.qq.com/ptqrshow` 取二维码（`appid`、`e`、`l`、`s`、`d`、`t`、`v`、`daid` 等）。
2. `ptqrlogin` 轮询，成功返回 `ptuiCB` 重定向链，提取授权 `code`。
3. `POST https://u.y.qq.com/cgi-bin/musicu.fcg`，module `QQConnectLogin.LoginServer` / method `QQLogin`，用 `code` 换 `musickey/musicid/openid/access_token`。
4. 保存 Cookie（`qqmusic_key`/`qm_keyst` 等），后续计算 `g_tk`。
5. 手机号/密码登录**未实现**（需要 RSA、验证码、风控，不建议优先做）。

**限制/风险**：

- 搜索旧接口随时可能被风控；musicu 接口要 `sign` 与 `comm`，字段名一变就挂。
- 播放地址：无登录/无 vkey 基本不可用；即使有 vkey，VIP/付费曲仍返回空。洛雪的“全外置”策略就是对这个现实的承认。
- 用户歌单需要登录 Cookie 与 `g_tk`；`我喜欢的音乐` 是特殊歌单，按名称包含匹配可用，但更稳的是识别 `specialType` 或 `tid`（`【未验证】` 具体字段）。

---

### 3.3 酷狗音乐

**接口形状**（`【洛】` + `【仓】`）：

| 能力 | 接口 | 方法 | 关键参数 | 关键返回 |
|---|---|---|---|---|
| 搜索 | `http://songsearch.kugou.com/song_search_v2` | GET | `platform=AndroidFilter`、`keyword`、`page`、`pagesize`、`hifiquality=0`、`PrivilegeFilter=0` | `data.lists[]`：`FileHash/SongName/SingerName/AlbumName/AlbumID/Duration/Audioid/MixSongID` |
| 歌曲详情 | `POST http://gateway.kugou.com/v3/album_audio/audio` | POST | `appid=1005`、`clientver`、`dfid/mid/uuid`、`clienttime`、`hash` | `data[].play_url/lyrics` `【洛】` |
| 播放地址 | `https://www.kugou.com/yy/index.php?r=play/getdata&hash=&album_id=` | GET | `hash`、`album_id` | `data.play_url` `【仓】`（是否稳定可用 `【未验证】`） |
| 歌词搜索 | `http://lyrics.kugou.com/search` | GET | `ver=1`、`man=yes`、`client=pc`、`keyword`、`hash`、`timelength`、`lrctxt=1`；头 `KG-RC/KG-THash` | `candidates[]`：`id/accesskey/krctype` `【洛】` |
| 歌词下载 | `http://lyrics.kugou.com/download` | GET | `id`、`accesskey`、`fmt=lrc/krc`、`charset=utf8` | `content`（base64；krc 需解密） `【洛】` |
| 榜单列表 | `http://mobilecdnbj.kugou.com/api/v5/rank/list` | GET | `version=9108`、`plat=0`、`showtype=2`、`parentid=0`、`apiver=6`、`area_code=1` | `data.info[]` `【洛】` |
| 榜单歌曲 | `http://mobilecdnbj.kugou.com/api/v3/rank/song` | GET | `rankid`、`page`、`pagesize`、`ranktype=1`、`area_code=1` | `data.info[]` `【洛】` |
| 歌单分类 | `http://www2.kugou.kugou.com/yueku/v9/special/getSpecial` | GET | `is_ajax=1`、`cdn=cdn`、`t`、`c`、`p` | 标签/歌单 `【洛】` |
| 歌单详情 | `https://pubsongscdn.kugou.com/v2/get_other_list_file` | GET | `specialid`、`page`、`pagesize` + 签名 | `data.info[]`（`remark/singerinfo/albuminfo/timelen/hash`） `【仓】`（仓库注释称已实测通过） |
| 歌单搜索 | `http://msearchretry.kugou.com/api/v3/search/special` | GET | `keyword`、`page`、`pagesize`、`showtype=10` | 歌单列表 `【洛】` |
| 用户歌单 | `POST https://gateway.kugou.com/v7/get_all_list` | POST | `userid`、`token`、`type=2`、`page/pagesize`；头 `x-router: cloudlist.service.kugou.com` | `data.info[]` `【仓】` |
| 收藏歌单歌曲 | `https://gateway.kugou.com/pubsongs/v2/get_other_list_file_nofilt` | GET | `global_collection_id`、`begin_idx`、`pagesize` | `data.info[]` `【仓】` |

**签名/加密 `【洛】`/`【仓】`**：

- 公式：`signature = md5(keyparam + 按 key 排序后的 "k=v" 直接拼接 + body 原文 + keyparam)`；`signature` 自身不参与签名；参与签名的 body 必须与实际发送字节完全一致（[KugouProtocol.h](file:///G:/free_music_hunyuan/src/core/KugouProtocol.h#L5-L6)）。
- 两把 keyparam：Web 接口 `NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt`；App/gateway 接口 `OIlwieks28dk2k092lksi2UIkp`（[kg/util.js](file:///G:/free_music_hunyuan/reference/lx-music-desktop-git/src/renderer/utils/musicSdk/kg/util.js#L13-L18)）。
- 公共参数：`dfid/mid/uuid/appid/clientver/clienttime`，扫码 Web 端一次会话内固定（[KugouProtocol.h](file:///G:/free_music_hunyuan/src/core/KugouProtocol.h#L22-L32)）。
- 登录态：Cookie 需要 `userid` + `token`（`token` 来自扫码结果）。

**登录 `【仓】`**：

1. `https://login-user.kugou.com/v2/qrcode`（`appid=1005`、`qrcode_txt`）取二维码图片与 `qrcode` key。
2. 轮询 `https://login-user.kugou.com/v2/get_userinfo_qrcode`（`qrcode`、`appid`），按状态码判断扫码/确认/成功。
3. 成功后保存 `userid/token` 到 Cookie。
4. 手机号/密码**未实现**（需要 App 端 RSA/签名参数）。

**限制/风险**：

- `song_search_v2` 返回的 `FileHash` 与播放接口的 `hash` 需要对应，混用 `Audioid/MixSongID` 会拿不到地址。
- 播放接口对版权/付费曲返回空或试听；Web key 与 App key 混用会签名失败。
- 用户歌单接口依赖 `token`，过期后需要重新扫码；`get_all_list` 的 `type` 与 `global_collection_id` 语义需按实测确认 `【未验证】`。

---

### 3.4 酷我音乐

**接口形状**（`【洛】` + `【仓】`）：

| 能力 | 接口 | 方法 | 关键参数 | 关键返回 |
|---|---|---|---|---|
| 搜索 | `http://search.kuwo.cn/r.s` | GET | `client=kt`、`all`、`pn`（从 0 开始）、`rn`、`ft=music`、`rformat=json`、`encoding=utf8`、`uid=794762570` | `abslist[]`：`MUSICRID`、`SONGNAME`、`ARTIST`、`ALBUM`、`DURATION`、`N_MINFO` |
| 搜索（Web） | `https://www.kuwo.cn/api/www/search/searchMusicBykeyWord` | GET | `key`、`pn`、`rn`、`httpsStatus=1`；头 `csrf` + Cookie `kw_token` | `data.list[]` `【仓】`（未实测） |
| 歌曲详情 | `http://www.kuwo.cn/api/www/music/musicInfo` | GET | `mid`；需要 `csrf`/`kw_token` | `data` `【洛】` |
| 播放地址 | `http://www.kuwo.cn/api/v1/www/music/playUrl` | GET | `mid`、`type=music`、`httpsStatus=1`；需要 `csrf`/`kw_token` | `data.url` `【仓】`（未实测） |
| 歌词 | `http://mlyric.kuwo.cn/mobi.s` | GET | `f=web`、`type=lyric`、`lrcx=1`、`rid`、`encode=utf8` | 压缩 + base64 + XOR（key `yeelion`） `【洛】` |
| 榜单树 | `http://qukudata.kuwo.cn/q.k` | GET | `op=query`、`cont=tree`、`node=2`、`pn=0`、`rn=1000`、`fmt=json`、`level=2` | 榜单树 `【洛】` |
| 榜单详情 | `https://wbd.kuwo.cn/api/bd/bang/bang_info` | GET | `data`（AES-128-ECB 加密的 JSON：`id/pn/rn/...`）、`time`、`appId`、`sign` | 加密返回，需 `wbdCrypto.decodeData` `【洛】` |
| 歌单分类 | `http://wapi.kuwo.cn/api/pc/classify/playlist/getTagList` | GET | `cmd=rcm_keyword_playlist`、`appUid=76039576`、`loginUid=0` | 标签树 `【洛】` |
| 分类歌单 | `http://wapi.kuwo.cn/api/pc/classify/playlist/getTagPlayList` | GET | `id`、`pn`、`rn`、`appUid` | 歌单列表 `【洛】` |
| 推荐歌单 | `http://wapi.kuwo.cn/api/pc/classify/playlist/getRcmPlayList` | GET | `pn`、`rn`、`order` | 歌单列表 `【洛】` |
| 歌单详情 | `http://nplserver.kuwo.cn/pl.svc` | GET | `op=getlistinfo`、`pid`、`pn`、`rn`、`encode=utf8`、`keyset=pl2012` | `musiclist[]` `【洛】` |
| 歌单搜索 | `http://search.kuwo.cn/r.s` | GET | `ft=playlist` + 搜索参数 | 歌单列表 `【洛】` |
| 用户歌单 | 无内置 | - | - | 需账号 API `【未验证】` |
| 收藏/最近播放 | 无内置 | - | - | `【未验证】` |

**签名/加密 `【洛】`**：

- 歌词：返回体需按 `tp=content` 头做解压，再 base64 解码，再用 `yeelion` 做 XOR。
- 榜单：`wbdCrypto` = AES-128-ECB 加密请求 JSON + MD5 签名（常量在 [kw/util.js](file:///G:/free_music_hunyuan/reference/lx-music-desktop-git/src/renderer/utils/musicSdk/kw/util.js)）。
- Web 接口：必须带 `csrf` 头与 `kw_token` Cookie，否则 403/空数据。

**登录 `【仓】`/`【未验证】`**：

- 仓库内置只有 Cookie 导入；二维码/手机号由账号 API 服务的 `extraMethods` 提供，本仓库没有酷我原生登录实现。
- 酷我网页登录需要 `kw_token` 与 `csrf`；具体登录端点本次未在仓库/洛雪中找到，标 `【未验证】`。

**限制/风险**：

- 公开搜索/榜单/歌单可用，但播放地址依赖 Web 登录态，匿名基本不可用。
- 用户歌单/收藏/最近播放缺少公开契约，需自建账号 API 或抓包补充；**不要把猜测的端点写进生产代码**。

---

### 3.5 咪咕音乐

**接口形状**（`【洛】` + `【仓】`）：

| 能力 | 接口 | 方法 | 关键参数 | 关键返回 |
|---|---|---|---|---|
| 搜索 | `https://jadeite.migu.cn/music_search/v3/search/searchAll` | GET | `isCorrect=0`、`isCopyright=1`、`searchSwitch`、`pageSize`、`text`、`pageNo`、`sort=0`、`sid=USS`；头 `uiVersion/deviceId/timestamp/sign/channel` | 经 `filterData` 归一化 `【洛】` |
| 搜索（仓库模板） | `https://m.music.migu.cn/migu/remoting/scr_search_tag` | GET | `keyword`、`type=2`、`rows`、`pgc` | `musics[]` `【仓】`（示例模板，未实测） |
| 歌曲详情 | `https://c.musicapp.migu.cn/MIGUM2.0/v1.0/content/resourceinfo.do` | GET | `resourceType=2`、`resourceId` | `resource` `【洛】` |
| 播放地址 | `https://app.c.nf.migu.cn/MIGUM2.0/v1.0/content/sub/listenUrl.do` | GET | `copyrightId`、`resourceType=2`、`toneFlag` | `url` `【仓】`（未实测；洛雪中相关实现被注释 `【洛】`） |
| 歌词 | 搜索/详情返回的 `lrcUrl/mrcUrl/trcUrl` 直接下载 | GET | 头 `Referer: https://app.c.nf.migu.cn/`、`channel: 0146921` | LRC / MRC `【洛】` |
| 榜单列表 | `https://app.c.nf.migu.cn/pc/bmw/rank/rank-index/v1.0` | GET | 头 `Referer`/`channel` | 榜单列表 `【洛】` |
| 榜单详情 | `https://app.c.nf.migu.cn/MIGUM2.0/v1.0/content/querycontentbyId.do` | GET | `columnId`、`needAll=0` | 榜单歌曲 `【洛】` |
| 歌单分类 | `https://app.c.nf.migu.cn/pc/v1.0/template/musiclistplaza-taglist/release` | GET | 头 `Referer`/`channel` | 标签 `【洛】` |
| 推荐歌单 | `https://app.c.nf.migu.cn/pc/bmw/page-data/playlist-square-recommend/v1.0` | GET | `templateVersion=2`、`pageNo` | 歌单列表 `【洛】` |
| 分类歌单 | `https://app.c.nf.migu.cn/pc/v1.0/template/musiclistplaza-listbytag/release` | GET | `pageNumber`、`templateVersion=2`、`tagId` | 歌单列表 `【洛】` |
| 歌单详情 | `https://app.c.nf.migu.cn/MIGUM3.0/resource/playlist/song/v2.0` | GET | `pageNo`、`pageSize`、`playlistId` | 歌曲列表 `【洛】` |
| 歌单信息 | `https://c.musicapp.migu.cn/MIGUM3.0/resource/playlist/v2.0` | GET | `playlistId` | 歌单元数据 `【洛】` |
| 歌单搜索 | 搜索接口 `searchSwitch.songlist=1` | GET | 同上 | 歌单列表 `【洛】` |
| 用户歌单 | 无内置 | - | - | 需账号 API `【未验证】` |

**签名/加密 `【洛】`**：

- `sign = md5(text + "6cdc72a439cef99a3418d2a78aa28c73" + "yyapp2d16148780a1dcc7408e06336b98cfd50" + deviceId + timestamp)`；`deviceId` 为固定值 `963B7AA0D21511ED807EE5846EC87D20`（[mg/musicSearch.js](file:///G:/free_music_hunyuan/reference/lx-music-desktop-git/src/renderer/utils/musicSdk/mg/musicSearch.js#L6-L9)）。
- 请求头必须带 `uiVersion/deviceId/timestamp/sign/channel`；`channel=0146921`，UA 伪装 Android。
- 歌词/歌单等 `app.c.nf.migu.cn` 接口要求 `Referer: https://app.c.nf.migu.cn/`。

**登录 `【仓】`/`【未验证】`**：

- 仓库内置只有 Cookie 导入；二维码/手机号依赖账号 API 的 `extraMethods`。
- 咪咕原生登录端点本次未在仓库/洛雪中找到，标 `【未验证】`。

**限制/风险**：

- `deviceId` 与 `signatureMd5` 是硬编码，平台换常量会整体失效；不能视为长期契约。
- 播放地址版权限制严格，匿名/无登录多数只能拿试听或空。
- 用户体系接口缺失，想支持“收藏/最近播放”必须先补账号 API。

---

### 3.6 附录：B 站与汽水音乐

**B 站（可选，未接入）**

- 搜索：`https://api.bilibili.com/x/web-interface/wbi/search/type?search_type=video&keyword=&page=`，需要 **WBI 签名**（`w_rid`、`wts`）与 `buvid3` Cookie `【未验证】`。
- 播放：`https://api.bilibili.com/x/player/wbi/playurl?bvid=&cid=&fnval=16`，需要 WBI + `Referer: https://www.bilibili.com` + 登录 Cookie `【未验证】`。
- 歌词：B 站没有官方歌词，只能从视频字幕/第三方补 `【未验证】`。
- “歌单”实际是收藏夹：`https://api.bilibili.com/x/v3/fav/resource/list?media_id=&pn=&ps=`，需要登录 `【未验证】`。
- 结论：B 站应作为独立源后置，先解决 WBI 签名与登录，不建议混入五平台 MVP。

**汽水音乐（仓库已有配置）**

- `sources.json` 已存在 `qishui` 源，但注释说明匿名搜索返回 200 空数据，需要 Cookie 与 `X-Helios/X-Medusa` 等签名头，仓库未实现该签名 `【仓】`。
- 结论：可作为“已占位、未可用”源保留；进入生产前必须补齐签名与登录态，否则应在 UI 标记为不可用。

---

## 4. 跨平台登录与歌单能力矩阵

| 能力 | 网易云 | QQ | 酷狗 | 酷我 | 咪咕 |
|---|---|---|---|---|---|
| 二维码登录 | ✅ 原生 | ✅ 原生 | ✅ 原生 | ⚠ 账号 API | ⚠ 账号 API |
| 手机号验证码 | ❌ 未实现 | ❌ | ❌ | ⚠ 账号 API | ⚠ 账号 API |
| 账号密码 | ✅ 手机/邮箱 | ❌ | ❌ | ⚠ | ⚠ |
| Cookie 导入 | ✅ | ✅ | ✅ | ✅ | ✅ |
| 用户资料 | ✅ | ✅ | ✅ | ⚠ | ⚠ |
| 收藏歌单列表 | ✅ | ✅ | ✅ | ❌ | ❌ |
| 自建歌单列表 | ✅ | ✅ | ✅ | ❌ | ❌ |
| 我喜欢的音乐 | ✅ | ✅ | ✅ | ❌ | ❌ |
| 最近播放 | ❌ | ❌ | ❌ | ❌ | ❌ |
| 歌单写回（新建/收藏） | ❌ | ❌ | ❌ | ❌ | ❌ |

**读法**：✅ = 仓库已有实现；⚠ = 只有外置账号 API 路径，且本次未验证；❌ = 明确没有。

**建议产品口径**：

- 首版只承诺“网易云/QQ/酷狗登录后拉取歌单与收藏”。
- 酷我/咪咕登录入口先做 Cookie 导入，二维码/手机号标注“实验中”；没有账号 API 时在 UI 禁用“我的歌单”。
- “最近播放”作为独立里程碑，先做本地最近播放，再逐平台补远端。

---

## 5. 洛雪音乐（lx-music-desktop）做法拆解

### 5.1 它做了什么

- `musicSdk/<source>/` 按平台拆分：`musicSearch`、`musicInfo`、`lyric`、`songList`、`leaderboard`、`singer`、`album`、`comment` 等模块；每个模块只负责“取元数据 + 归一化字段”。
- 平台差异用目录隔离，公共入口 `musicSdk/index.js` 提供统一 `searchMusic/findMusic`。
- 跨源匹配（[index.js](file:///G:/free_music_hunyuan/reference/lx-music-desktop-git/src/renderer/utils/musicSdk/index.js#L60-L135)）：
  - 歌名/歌手/专辑做去标点、去空白、转小写；
  - 歌手按 `、/&/;/，/|` 拆分后排序再比较；
  - 时长容忍 ±5 秒；
  - 版本关键字（live/remix 等）必须一致；
  - 先精确匹配，再包含匹配，最后按平台顺序保留候选。
- **没有账号登录/云歌单同步模块**：从本次参考的 `musicSdk` 代码看，只有匿名接口；洛雪的“同步”是它自建的同步服务（`src/main/modules/sync`），不是各平台账号体系。

### 5.2 播放地址策略（最关键）

- 各平台 `index.js` 的 `getMusicUrl` 统一委托 `apis(source)`（如 [tx/index.js](file:///G:/free_music_hunyuan/reference/lx-music-desktop-git/src/renderer/utils/musicSdk/tx/index.js)、[kw/index.js](file:///G:/free_music_hunyuan/reference/lx-music-desktop-git/src/renderer/utils/musicSdk/kw/index.js#L62-L63)）。
- `api-source-info.ts` 中官方地址实现基本被注释，意思是：**洛雪把“播放地址”变成用户导入的 User API / 自定义源职责**，内置代码只保留元数据能力。
- User API 脚本契约（[user_api.d.ts](file:///G:/free_music_hunyuan/reference/lx-music-desktop-git/src/common/types/user_api.d.ts)）：脚本在 `lx.request` 上发请求，返回 `musicUrl/lyric/pic` 等 action；脚本可使用 `lx.crypto`、`lx.zlib`。

### 5.3 对本项目的启示

1. **元数据与播放地址分层**：`SourceManager` 负责聚合元数据；`PlaybackResolver` 负责按“平台 + 歌曲 ID + 音质”解析地址，可插拔（内置直连 / 账号 API / 自定义 HTTP / 未来 JS）。
2. **跨源换源复用洛雪的匹配规则**：本仓库 `Song::normalizedKey()` 已有类似思路，但需要补“歌手拆分排序 + 时长容差 + 版本关键字”。
3. **不要照搬洛雪的账号层**：它没有，照搬不了；登录/歌单必须按本仓库 `AuthManager + UserApi + Library` 自建。
4. **JS 插件不是免费能力**：洛雪脚本依赖运行时与网络权限，直接引入会带来安全与跨端（Android/iOS）复杂度。

---

## 6. 聚合层统一抽象建议

### 6.1 需要扩展的接口（按优先级）

现状 [MusicSource.h](file:///G:/free_music_hunyuan/src/core/MusicSource.h#L35-L44) 已有 `search/searchPlaylist/rank/playlistDetail/resolveUrl/lyric/userPlaylists/userFavorites/userPlaylistDetail`。建议增加：

```cpp
// 1) 能力声明：UI 按能力隐藏/禁用入口，而不是请求失败后弹错
struct SourceCaps {
    bool search = false;
    bool searchPlaylist = false;
    bool rank = false;
    bool playlistCategories = false;
    bool playlistDetail = false;
    bool songDetail = false;
    bool musicUrl = false;
    bool lyric = false;
    bool userLibrary = false;   // 登录后才为 true
    bool userRecent = false;
    bool userPlaylistWrite = false;
    QStringList qualities;      // 例：{"128k","320k","flac"}
};

// 2) 分页/错误要可判别，不能让“空结果”和“失败”都变成空数组
struct PageResult {
    QVector<Song> songs;
    int total = -1;
    bool hasMore = false;
    QString cursor;             // 部分平台是 offset，部分是 page，部分是不透明游标
    int errorCode = 0;          // 0=成功；非 0 需要 UI 区分“未登录/限流/无版权”
    QString errorMessage;
};

// 3) 登录后用户库按账号维度查询
virtual void userPlaylists(const Account &account, PageResultCallback cb);
virtual void userRecent(const Account &account, int limit, PageResultCallback cb);
```

同时建议保留现有回调签名做兼容包装，避免一次性改动所有 QML 调用点。

### 6.2 数据模型改造

**账号（最大硬伤）**：`accounts` 表主键是 `source`，一个平台只能存一个账号（[Library.cpp](file:///G:/free_music_hunyuan/src/core/Library.cpp#L50-L50)）。需要：

```cpp
struct Account {
    QString id;         // 新增：source + ":" + uid，作为稳定主键
    QString source;
    QString uid;
    // ... 现有字段
    bool active = false; // 当前选中的账号
    QString scope;       // "remote" | "local"（本地游客）
};
```

`CREATE TABLE accounts (id TEXT PRIMARY KEY, source TEXT, uid TEXT, ...)`，索引 `(source, uid)`。SQLite 迁移建议“新建表 + 数据搬迁 + 重命名”，不要直接改主键。

**歌单**：`Playlist` 增加 `accountUid` 与 `remote`：

- 远端歌单：`remote=true`，只读；ID 用 `source:uid:playlistId`，避免多账号同 ID 冲突。
- 本地/聚合歌单：`remote=false`，`accountUid` 为空，允许用户自由增删。
- 跨平台歌单：新增 `kind="aggregate"`，歌曲条目保留原平台 `source/sourceId`，同时记录可替代的 `otherSource`。

**歌曲**：`Song::otherSource` 已能存跨源等价 ID，建议统一成：

```cpp
QVector<SourceRef> variants; // {source, songId, qualityHint}
```

这样聚合去重后仍能换源。

### 6.3 聚合搜索算法

现状 [SourceManager.cpp](file:///G:/free_music_hunyuan/src/core/SourceManager.cpp#L49-L82)：并发请求 + 按 `normalizedKey().size()` 升序排序；没有去重，也没有“平台权重/精确度”概念。建议：

1. **归一化**：去标点空白、小写；歌手按分隔符拆分排序；保留版本关键字（live/remix/伴奏）。
2. **分组键**：`归一化歌名 + "|" + 归一化歌手`；时长 ±5 秒只作为“同一首”的确认条件，不进入分组键（避免翻唱/同名前奏被错误合并）。
3. **打分**：
   - 歌名完全相等 +3；包含 +2；
   - 歌手完全相等 +2；包含 +1；
   - 时长差 ≤2 秒 +1；
   - 平台优先级权重（用户可配置，默认按已登录平台 > 匿名平台）。
4. **合并**：同组保留得分最高的一条为主条目，其余写入 `variants`；UI 显示“可切换源”。
5. **分页**：每个源独立维护 `page/cursor`；聚合第 N 页 = 各源第 N 页，合并后去重；`total` 是各源 `total` 之和（若未知则为 -1）。
6. **并发与限流**：`QSemaphore` 控制同时在途请求（建议 4-6）；每源最小请求间隔（建议 300-800ms）+ 指数退避；遇到 403/429/-460 进入冷却，不要自动重试风暴。
7. **失败隔离**：单源失败不阻塞整体；`PageResult.errorCode` 上报，UI 显示“某平台暂不可用”。

### 6.4 跨平台歌单

- **只读优先**：远端歌单以平台为准，本地只做缓存；刷新时按 `remote=false/true` 区分。
- **写回**：五个平台都没有验证过的“新建歌单/收藏歌单/加歌”接口，**首版一律不提供写回**，按钮置灰并提示“平台未开放”。
- **聚合歌单**：用户可在本地创建“跨平台歌单”，条目保存 `variants`；播放时按 `SourceManager::resolve` 的兜底顺序找可播地址；找不到时标灰而不是删除。
- **收藏同步方向**：只做“拉取”，不做“推送”；避免用户本地删除被误同步到平台。

### 6.5 登录态隔离

现状 [SourceManager.cpp](file:///G:/free_music_hunyuan/src/core/SourceManager.cpp#L37-L47)：每个平台只取**第一个已登录账号**，`MusicSource` 只有一个 `m_session`。这会导致：

- 同平台第二个账号无法登录/切换；
- 切换账号后旧请求可能带着新 Cookie 发出；
- QML 里所有页面共享同一份会话，无法做“工作账号/私人账号”隔离。

建议：

1. `SourceManager` 维护 `QHash<QString /*source*/, QVector<Account>>` 与 `QHash<QString /*source*/, QString /*activeAccountId*/>`。
2. **会话按账号实例化**：`SourceSession { MusicSource *source; Account account; }`；不要在一个 `MusicSource` 上反复 `setSession`。
3. `AuthManager` 的登录状态机按 `loginId`（source + method + 临时 key）而不是 `source` 维度管理；支持并行扫码。
4. 所有用户库请求显式传 `account`，不允许隐式读 `m_session`。
5. Library 的 `accounts`、`playlists`、`playlist_songs` 增加 `account_uid` 作用域；本地歌单 `account_uid` 为空。
6. QML 账号选择器：每平台一个“当前账号”下拉 + “添加账号”；未登录时显示“登录后可用”。

### 6.6 自定义音源插件是否值得引入

现状 `CustomSource` 是 **HTTP 契约 + 本地目录匹配**，没有 JS 运行时（[CustomSource.h](file:///G:/free_music_hunyuan/src/core/CustomSource.h)）。洛雪式脚本源需要：

- JS 引擎（Electron 里是 Node/浏览器上下文）；
- `lx.request` 网络代理；
- `lx.crypto`（AES/RSA/MD5）、`lx.zlib`；
- 脚本签名/来源校验、权限与超时。

**建议分三阶段**：

| 阶段 | 方案 | 理由 |
|---|---|---|
| MVP | 保留现有 HTTP 自定义源 + 跨源换源 | 零新增运行时、跨端一致、风险最低 |
| 二期 | 增加“外部解析器 HTTP 契约”（类似洛雪 User API 的 HTTP 版）：`POST /resolve {source, songId, quality}` 返回 `{url, expiresAt}` | 让高级用户自行部署，不把代码执行放进客户端 |
| 三期 | 可选 `QJSEngine` 受限沙箱：只暴露 `request/crypto/zlib`，禁止文件/进程/任意网络；脚本必须用户显式导入并展示权限 | Qt 5.7 自带 QJSEngine，桌面/Android 可用；但安全审计成本高 |

**红线**：不要把账号 Cookie/Token 暴露给脚本；脚本只能拿到“本次解析所需的最小上下文”，且必须能一键禁用。

---

## 7. 建议落地顺序

1. **P0 数据模型**：`accounts` 改多账号主键；`Playlist/Song` 增加账号作用域与 variants；补迁移脚本与回归测试。
2. **P0 能力声明**：`SourceCaps` + `PageResult`；UI 按能力隐藏入口；先把“失败/未登录/无版权”区分开。
3. **P1 登录补齐**：网易云/QQ/酷狗多账号切换；酷我/咪咕先 Cookie，账号 API 可选。
4. **P1 用户库**：网易云/QQ/酷狗“收藏/自建/我喜欢/歌单歌曲”统一到账号维度；最近播放先做本地。
5. **P2 聚合搜索**：归一化 + 分组去重 + variants + 分页 + 限流；把现有 `stable_sort` 替换为评分排序。
6. **P2 跨平台歌单**：本地聚合歌单 + 只读远端歌单 + 换源播放。
7. **P3 解析器**：HTTP 外部解析器；再评估 JS 沙箱。
8. **P3 平台扩展**：B 站（WBI）、汽水（签名）在五平台稳定后再做。

---

## 8. 风险与合规

### 8.1 接口稳定性

- 这些接口均为**非公开契约**：字段改名、加签、风控、下线都发生过（例如仓库注释记录网易云 `qrcode/create` 已 404）。
- 洛雪把播放地址外置，本身就是对“内置地址不可长期维护”的工程结论。
- 建议：所有端点集中在 `sources.json`，允许热更新；代码里不散落 URL；每个源独立降级。

### 8.2 频率限制与风控

- 网易云可能返回 460/403；QQ 可能触发登录/风控；酷狗签名错误直接空数据；咪咕 `deviceId` 固定但可被识别；酷我 Web 接口无 `csrf` 直接 403。
- 建议：每源最小间隔 + 并发上限 + 指数退避 + 冷却；搜索输入防抖；不做全量爬取；不并发翻页轰炸。

### 8.3 账号与凭据

- 本地 Cookie/Token 目前是**混淆存储**，不是加密（[Library.h](file:///G:/free_music_hunyuan/src/core/Library.h#L50-L52)）。
- 建议后续接系统安全存储：Windows DPAPI、macOS Keychain、Android Keystore；至少做到“导出日志不含 Cookie”。
- 账号 API 服务会收到用户 Cookie，必须由用户自建/可信部署；客户端要明确提示。

### 8.4 版权与合规

- 不内置绕过 VIP/DRM/付费墙的解析；不提供版权内容下载；不缓存/分发临时播放地址。
- 播放地址通常有时效，过期即失效；不要持久化 URL，只持久化 `songId/source/quality`。
- 用户须使用自己的账号；对“账号 API 服务”与“JS 音源脚本”做免责声明与来源校验。

### 8.5 跨端一致性

- Qt 5.7 + QML 同一套代码：C++ 层负责网络/加密/存储，QML 只做展示；不要把平台签名逻辑写到 QML。
- Android 上 Cookie/文件路径、系统安全存储、后台网络策略与桌面不同；`QJSEngine` 方案需单独验证性能与合规。
- 桌面与移动端都应支持“无账号匿名浏览”，登录只影响用户库与高音质解析。

---

## 9. 明确“未验证”清单（开发前必须补测）

| 项 | 状态 | 建议验证方式 |
|---|---|---|
| 网易云 eapi 搜索/歌词 | `【未验证】` | 用仓库 HttpClient 复刻 `eapi`，对比匿名/登录返回 |
| 网易云高音质播放地址 | `【未验证】` | 匿名 vs 登录 Cookie，检查 `code/url/br` |
| QQ musicu 搜索与 vkey | `【未验证】` | 先验 `musics.fcg` 签名，再验 `vkey` 模块与付费曲 |
| QQ `我喜欢的音乐` 识别字段 | `【未验证】` | 抓取 `disslist`，确认 `specialType/tid/name` |
| 酷狗 Web 播放地址稳定性 | `【未验证】` | 抽样 20 首免费/付费曲，统计 `play_url` 空值率 |
| 酷狗 `get_all_list` 分页与 `type` | `【未验证】` | 登录账号对比 `data.info` 与网页歌单 |
| 酷我 `csrf/kw_token` 获取与刷新 | `【未验证】` | 浏览器登录后抓包，验证 403/200 边界 |
| 酷我用户歌单/最近播放 | `【未验证】` | 需登录抓包；未确认前不做 UI |
| 咪咕 `listenUrl` 与版权参数 | `【未验证】` | 抽样验证 `toneFlag/copyrightId` |
| 咪咕用户体系 | `【未验证】` | 需登录抓包或账号 API |
| B 站 WBI 签名 | `【未验证】` | 单独 spike，不阻塞五平台 |
| 汽水签名头 | `【未验证】` | 需 Cookie + `X-Helios/X-Medusa`，先标不可用 |
| 各平台“最近播放” | `【未验证】` | 本次未在仓库/洛雪找到可用契约 |
| 洛雪 User API 脚本运行时 | `【洛】`（仅代码） | 若引入需单独安全评估，不在 MVP |

---

## 10. 参考资料

**本仓库**

- [MusicSource.h](file:///G:/free_music_hunyuan/src/core/MusicSource.h) / [MusicSource.cpp](file:///G:/free_music_hunyuan/src/core/MusicSource.cpp)
- [SourceManager.cpp](file:///G:/free_music_hunyuan/src/core/SourceManager.cpp)
- [AuthManager.cpp](file:///G:/free_music_hunyuan/src/core/AuthManager.cpp)
- [UserApi.cpp](file:///G:/free_music_hunyuan/src/core/UserApi.cpp)
- [Library.cpp](file:///G:/free_music_hunyuan/src/core/Library.cpp)
- [CustomSource.cpp](file:///G:/free_music_hunyuan/src/core/CustomSource.cpp)
- [NeteaseCrypto.h](file:///G:/free_music_hunyuan/src/core/NeteaseCrypto.h) / [QqProtocol.h](file:///G:/free_music_hunyuan/src/core/QqProtocol.h) / [KugouProtocol.h](file:///G:/free_music_hunyuan/src/core/KugouProtocol.h)
- [config/sources.json](file:///G:/free_music_hunyuan/config/sources.json)

**洛雪音乐（参考实现）**

- 公共入口与跨源匹配：[musicSdk/index.js](file:///G:/free_music_hunyuan/reference/lx-music-desktop-git/src/renderer/utils/musicSdk/index.js)
- 网易云：[wy/](file:///G:/free_music_hunyuan/reference/lx-music-desktop-git/src/renderer/utils/musicSdk/wy)
- QQ：[tx/](file:///G:/free_music_hunyuan/reference/lx-music-desktop-git/src/renderer/utils/musicSdk/tx)
- 酷狗：[kg/](file:///G:/free_music_hunyuan/reference/lx-music-desktop-git/src/renderer/utils/musicSdk/kg)
- 酷我：[kw/](file:///G:/free_music_hunyuan/reference/lx-music-desktop-git/src/renderer/utils/musicSdk/kw)
- 咪咕：[mg/](file:///G:/free_music_hunyuan/reference/lx-music-desktop-git/src/renderer/utils/musicSdk/mg)
- 播放地址外置：[api-source.js](file:///G:/free_music_hunyuan/reference/lx-music-desktop-git/src/renderer/utils/musicSdk/api-source.js)、[api-source-info.ts](file:///G:/free_music_hunyuan/reference/lx-music-desktop-git/src/renderer/utils/musicSdk/api-source-info.ts)
- User API 契约：[user_api.d.ts](file:///G:/free_music_hunyuan/reference/lx-music-desktop-git/src/common/types/user_api.d.ts)

---

## 11. 一句话给后续开发

**先把“账号维度”和“播放解析可插拔”两件事做对，再谈聚合体验。** 元数据聚合可以快速出效果；登录后歌单必须按平台分别验收，酷我/咪咕没有账号 API 就不要假装支持；播放地址永远按“可能失败”设计。

## 12. 外部参考项目可借鉴性结论

对两个外部来源做了实际抓取与代码结构核对，结论只保留可落地部分：

**`https://www.jamecling.com/archives/3553`（洛雪音源教程向文章）**

- 没有可直接复用的宿主机实现，文章层面只是使用/分发说明，不能当代码依赖。
- 可借鉴的是交互流程：导入前明示脚本会获得哪些能力（网络请求、加解密），展示脚本来源与哈希，再让用户二次确认。
- 本工程现状：导入即执行、状态区展示能力与失败原因，但**权限清单 + 哈希绑定 + 二次确认尚未实现**，列为后续安全项。

**`https://github.com/chthollyphile/folia-major`**

- 技术栈为 Electron + React，属 AGPL 授权，与本工程 Qt/C++ 完全不同，**代码不可抄**（许可与架构都不兼容）。
- 可借鉴的设计：provider 契约统一搜索/取链/歌词的返回结构；由 capabilities 驱动界面显示哪些操作；错误按类型分类给用户看。
- 本工程现状：`ScriptSourceInfo::actions` + `capability` 已实现 capabilities 驱动 UI；`errorType`（syntax/runtime/timeout/network/tls/http/empty）已实现错误分类。待补的是**每脚本请求配额**（防止脚本无限发请求）。

**明确不抄**：Electron/React 源码、AGPL 代码、把账号 Cookie 交给脚本使用的设计（本工程脚本只拿到宿主转发的请求能力，不暴露登录态）。
