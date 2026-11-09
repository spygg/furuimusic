# 福瑞音乐架构改造方案：桌面 / 移动统一 QML 界面

> 目标：把「桌面 Qt Widgets + Android Qt Quick」两套界面收敛为一套 QML 界面，后端统一使用现有 `src/core` 的 Qt C++；最终支持 Windows / macOS / Linux / Android。
> 本文只描述与架构改造直接相关的内容，不改动业务逻辑。

## 一、现状勘察

### 1.1 本机实际可用的 Qt 套件

| 套件 | 编译器 | QML 模块 | 说明 |
| --- | --- | --- | --- |
| Qt 5.15.2 `mingw81_64` | MinGW 8.1 (Tools/mingw810_64) | QtQuick / Controls 2 / QtMultimedia 齐全 | 现有 Windows 桌面构建使用此套件，`build/desktop` 可复用 |
| Qt 5.15.2 `android` | Android 交叉编译 | QtQuick / Controls 2 齐全 | Qt 5 Android 的 APK 打包仍应走 qmake |
| Qt 6.11.1 `mingw_64` | MinGW 13.1 (Tools/mingw1310_64) | QtQuick / Controls 2 齐全 | Qt 6 桌面首选 |
| Qt 6.11.1 `android_arm64_v8a` | NDK 27 | QtQuick / Controls 2 齐全 | Qt 6 Android 走 CMake 打包 APK |

工具链：CMake 3.30.5（`Qt/Tools/CMake_64`）、Ninja（`Qt/Tools/Ninja`）、JDK 17、Android SDK / NDK 27。
本次以 **Qt 5.15.2 mingw81_64** 与 **Qt 6.11.1 mingw_64** 做编译验证；Android 只做构建配置检查，不重打 APK。

### 1.2 当前构建组织

- `FuruiMusic.pro`（qmake）
  - `android`：`FuruiMusicMobile`，Qt Quick，`src/mobile/main_mobile.cpp` + `src/mobile/qml.qrc`
  - 其他：`FuruiMusicDesktop`，Qt Widgets，`src/desktop/*`
- `CMakeLists.txt`
  - `FuruiCore` 静态库：`src/core/*`
  - 桌面：`FuruiMusicDesktop`（Widgets）+ `FuruiMusicMobile`（Quick 预览）
  - Android：仅 Qt 6 支持 `qt_add_executable` 打 APK
- `scripts/build_windows.bat` / `build_linux.sh` / `build_macos.sh` / `build_android_qt6.bat` 已存在，但分别围绕两套前端。

问题：同一份 QML 只属于 Android 目标；桌面没有 QML 入口；两套界面长期并存，功能会持续分叉。

### 1.3 核心层现状（可直接被 QML 驱动）

`src/core/AppController` 已经提供了较完整的 QML 门面：

- `QAbstractListModel`：`SongListModel`（results / queue）、`PlaylistListModel`、`AccountListModel`
- `Q_PROPERTY`：播放器状态、播放模式、音量、歌词、主题、搜索类型、音质、二维码状态、平台列表、账号 API 地址等
- `Q_INVOKABLE`：搜索、榜单、歌单、收藏/历史、播放控制、登录（Cookie / 扫码 / 密码 / 手机号）、同步用户歌单、设置、自定义音源、下载、外链打开
- 平台差异：`Settings` 已用 `QStandardPaths` 统一数据目录；`PlayerController` 用 QtMultimedia，跨平台音频后端由 Qt 插件负责

结论：**核心层到 QML 的桥已经存在，不需要重写后端**。本次只需补齐少量缺口（队列当前索引、当前收藏态、搜索历史、行级收藏角色）并加平台适配层。

### 1.4 UI 层现状

- `src/desktop/*`（约 1.1 万行 Widgets）：`MainWindow`、`ThemeStyle`、`PlayerWidgets`、`LoginDialog`、`SettingsDialog`、`LyricsWindow`
- `src/mobile/qml/*`：6 个页面（main / Search / Library / Login / Player / Settings），已覆盖搜索、歌单、收藏、历史、队列、登录、设置、播放
- QML 采用 `qrc:/qml/main.qml` 单入口 + 移动端竖向布局；每个页面各自硬编码一套主题色
- 桌面专有能力尚未迁移：宽屏三栏 / 队列抽屉、独立歌词窗口、托盘与系统通知、原生目录选择框

## 二、改造决策

### 2.1 构建系统：CMake 为唯一主构建，qmake 仅保留 Qt 5 Android 打包

- **CMake 为主**：桌面（Qt 5 / Qt 6）与 Qt 6 Android 全部由同一份 `CMakeLists.txt` 产出同一套目标，跨平台一致性最好。
- **qmake 为兼容层**：Qt 5.15 的 Android APK 打包链路（`ANDROID_PACKAGE_SOURCE_DIR`、`android_openssl`）仍依赖 qmake，因此保留 `FuruiMusic.pro`，但源文件清单与目标命名必须与 CMake 对齐，并且同样只产出 QML 前端。
- 不再把 qmake 作为桌面首选构建方式；`scripts/build_windows.bat` 后续改为 CMake 优先。

目标组织（两端一致）：

| 目标 | 类型 | 内容 | 平台 |
| --- | --- | --- | --- |
| `FuruiCore` | 静态库 | `src/core/*` + `config.qrc` | 全部 |
| `FuruiMusic` | 可执行 | `src/ui/main.cpp` + 共享 QML 资源 | Windows / macOS / Linux / Android |
| `FuruiMusicWidgets` | 可选可执行 | `src/desktop/*`（迁移期保留） | 桌面，默认不构建 |

### 2.2 目录组织（不使用下划线命名）

```
src/
  core/                 # 保持不变：纯 C++ 后端
  ui/                   # 新增：桌面与移动共用
    main.cpp            # 统一 QML 入口
    PlatformAdapter.h/.cpp
    qml.qrc
    qml/
      Main.qml          # 响应式外壳：宽屏侧栏 / 窄屏底部标签
      SearchPage.qml
      LibraryPage.qml
      LoginPage.qml
      PlayerPage.qml
      SettingsPage.qml
      components/       # 后续：PlayBar / SongRow / QueueDrawer / Theme
  desktop/              # 迁移期保留的 Widgets 旧前端
  mobile/               # 迁移完成后删除
```

- `src/mobile/qml/*` 整体迁入 `src/ui/qml/*`，QML 中的相对导入不变。
- `src/ui/main.cpp` 同时服务桌面与 Android，不再保留两份 `main_*`。

### 2.3 QML 组织：一套资源、两种外壳、同一批页面

- 单入口 `qrc:/qml/Main.qml`。
- 外壳按容器宽度切换：
  - 窄屏（< 760dp）：沿用现有移动结构（顶部迷你播放条 + StackLayout + TabBar）
  - 宽屏（>= 760dp）：左侧导航 + 页面内容 + 底部播放栏 + 可开合队列抽屉
- 页面组件本身不感知移动/桌面，只通过 `app` 与 `platform` 两个上下文对象取数据。
- 主题色先沿用现有页面内的色值；第二阶段再抽 `Theme.qml` 单例，避免这次改动同时做全局换肤导致 diff 过大。

### 2.4 核心层需要补齐的 QML 接口（本次实施）

| 能力 | 形式 | 说明 |
| --- | --- | --- |
| 队列当前索引 | `Q_PROPERTY(int currentQueueIndex)` | 宽屏播放栏 / 队列高亮 |
| 当前歌曲是否收藏 | `Q_PROPERTY(bool currentFavorite)` | 收藏按钮状态由后端驱动 |
| 行级收藏态 | `SongListModel::FavoriteRole` | 搜索/队列行直接显示心形状态 |
| 搜索历史 | `Q_INVOKABLE QStringList searchHistory()` | 搜索页历史补全 |
| 平台信息 | `PlatformAdapter` | `isMobile` / `platformName` / `dataDir` / `downloadsDir` / `toFileUrl` |

已存在且验证可用的接口不重复包装，避免门面膨胀。

### 2.5 平台差异隔离

- **路径**：`Settings::dataDir()` 已基于 `QStandardPaths`；`PlatformAdapter` 只读暴露目录与 `toFileUrl()`，QML 不再手拼 `file:///`。
- **托盘 / 通知**：桌面使用 `Qt.labs.platform`（`SystemTrayIcon` / `Notification`），Android 后续接原生通知；通过 `platform.supportsTray` / `supportsNotification` 能力开关，不在页面里写 `#ifdef`。
- **音频**：继续由 `PlayerController + QtMultimedia` 承担，平台后端由 Qt 插件选择；QML 不感知。
- **原生对话框**：QML 使用 `QtQuick.Dialogs` 的 `FolderDialog` / `FileDialog`，桌面和 Android 共用同一 QML 代码。

### 2.6 Widgets 层退役路径

| 阶段 | 动作 |
| --- | --- |
| P1（本次） | `FuruiMusic` QML 外壳成为默认桌面入口；Widgets 目标改为可选，不阻塞构建 |
| P2 | 迁移宽屏三栏、队列抽屉、独立歌词窗口、托盘 |
| P3 | 迁移设置/登录的桌面原生体验（目录选择、快捷键、窗口记忆） |
| P4 | 删除 `src/desktop/*` 与 `FuruiMusicWidgets` 目标，统一只保留 QML |

本次只做 P1 与部分 P2（宽屏外壳、底部播放栏、队列抽屉、现有 5 个页面复用）。

## 三、实施阶段与验收

### 阶段 A：方案与勘察（本文档）
验收：本机套件、现状、目标组织、API 补齐清单、平台隔离方式均明确。

### 阶段 B：统一构建配置
验收：
- CMake 同时支持 Qt 5.15.2 mingw 与 Qt 6.11.1 mingw，产出 `FuruiMusic`。
- qmake 桌面与 Android 均指向同一 QML 入口与同一批源文件。
- Widgets 旧前端仅在显式 `FURUI_BUILD_LEGACY_WIDGETS` / `CONFIG+=legacy_widgets` 时构建。

### 阶段 C：统一 QML 入口可启动
验收：
- 桌面 Qt 5 与 Qt 6 编译出的 `FuruiMusic` 可启动，`qrc:/qml/Main.qml` 加载成功。
- 窄屏/宽屏外壳都能在桌面窗口内切换。
- 原有移动页面（搜索/音乐库/账号/播放/设置）在桌面外壳中可正常打开。

### 阶段 D：核心层 QML 接口打通
验收：
- 播放、暂停、进度、音量、模式、歌词、收藏、队列、搜索、歌单、登录状态均由 `app` 属性/信号驱动。
- `FavoriteRole`、`currentQueueIndex`、`currentFavorite`、`searchHistory` 在 QML 中可用。
- `platform` 上下文对象提供平台判断与路径转换。

### 阶段 E：Widgets 页面迁移第一批
验收：
- 宽屏桌面外壳使用 QML 实现：左侧导航、搜索/音乐库/账号/设置页面、底部播放栏、队列抽屉。
- Widgets 旧前端仍可按需构建，但默认构建不再产出。

### 阶段 F：验证
验收：
- 独立临时目录 `build_check_<时间戳>` 编译通过，不触碰现有 `build/`。
- qmllint 对全部 QML 无错误。
- 启动冒烟：进程能进入事件循环，无 QML 加载失败。

## 四、风险与规避

1. **Qt 5 / Qt 6 QML 差异**：统一使用 QtQuick 2.15 / Controls 2.15 导入；Qt 6 兼容该版本导入。避免使用仅 Qt 6 的 API。
2. **静态库资源丢失**：已存在 `Q_INIT_RESOURCE` 修复，构建改动不得移除 `config.qrc`，并保留 CMake 的 `AUTORCC_DEPENDENCIES`。
3. **HTTPS 运行时依赖**：Qt 5.15.2 mingw 不自带 OpenSSL，沿用现有 DLL 复制步骤。
4. **Android Qt 5 打包**：只保留 qmake 链路；CMake 在 Qt 5 Android 下明确报错提示改用 qmake。
5. **迁移期双前端**：Widgets 目标默认关闭，避免两套 UI 同时演进；业务逻辑仍只在 `src/core` 修改。
6. **不清理 build/**：所有验证在 `build_check_<时间戳>` 中进行，现有构建产物保持不变。

## 五、第一批改动清单（本次执行）

1. 新增 `src/ui/` 共享入口、`PlatformAdapter`、共享 QML 资源与响应式外壳。
2. 迁移 `src/mobile/qml/*` 到 `src/ui/qml/*`，删除旧 `src/mobile`。
3. 重写 `CMakeLists.txt`：`FuruiCore` + `FuruiMusic`（全平台）+ 可选 `FuruiMusicWidgets`。
4. 重写 `FuruiMusic.pro`：桌面与 Android 都产出同一 QML 前端，Widgets 改为可选。
5. `AppController` / `SongListModel` 补齐 2.4 表格中的接口。
6. 更新构建脚本中的目标名与说明。
7. 用 Qt 5.15.2 与 Qt 6.11.1 在各独立临时目录编译，并用 `-platform offscreen` 做启动冒烟。

## 六、实施结果（本次）

| 项目 | 结果 |
| --- | --- |
| Qt 5.15.2 mingw81_64 + CMake（Ninja） | 编译通过；`-platform offscreen` 启动冒烟通过，stderr 为空 |
| Qt 5.15.2 mingw81_64 + qmake | 编译通过，产出 `bin/FuruiMusic.exe`；启动冒烟通过 |
| Qt 6.11.1 mingw_64 + CMake（Ninja） | 编译通过；启动冒烟通过，stderr 为空 |
| qmllint（Qt 5.15.2） | 12 个 QML 文件全部通过 |
| 宽屏桌面外壳 | 实机截图验证：左侧导航、搜索页、底部播放栏、队列入口正常 |
| 窄屏外壳 | 实机窗口缩放验证：顶部迷你播放条、页面区、底部标签栏正常 |
| Android | 仅完成 CMake（Qt 6）/ qmake（Qt 5）配置对齐，本次未实跑 APK 打包 |

验证目录：`build_check_20261107_212548`（Qt 5 CMake）、`build_check_qmake_20261107`（Qt 5 qmake）、`build_check_20261107_213000_qt6`（Qt 6 CMake）。未触碰现有 `build/`。

Qt 6 兼容修正：`HttpClient.cpp` 中的 `QNetworkRequest::FollowRedirectsAttribute` 在 Qt 6.11 已移除，改为仅使用 `RedirectPolicyAttribute`（Qt 5.9+ / Qt 6 均可用）。

说明：验证期间检测到其他会话正在并发修改 `src/core/MusicSource.cpp`，曾出现一次瞬时编译失败；待其补齐 `shortError` 后重试通过，本次未改动该文件。

