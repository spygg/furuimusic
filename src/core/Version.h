#pragma once
// 福瑞音乐 - 应用版本号唯一人工来源（其余版本文件由 tools/release.py 自动同步）
//   - AppController::version() / UpdateChecker::localVersion() 读这里的宏
//   - src/CMakeLists.txt 启动时从这里提取注入 project(... VERSION ...)，
//     Android versionName/versionCode、macOS bundle 版本、CI Release 名自动跟随
//   - src/version.json 与 publish/version.json（更新检查清单）由
//     python tools/release.py x.y.z 一并改好，desc 字段发版时人工填写
#define FURUI_APP_VERSION "1.4.6"
