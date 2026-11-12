#pragma once
// 福瑞音乐 - 应用版本号唯一来源
// 发版时需与 src/CMakeLists.txt 的 project(... VERSION ...) 同步修改：
//   - AppController::version() / UpdateChecker::localVersion() 读这里的宏
//   - CMake 的 PROJECT_VERSION 会注入 Android versionName/versionCode 与 CI Release 名
#define FURUI_APP_VERSION "1.3.0"
