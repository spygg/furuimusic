# 福瑞音乐 FuruiMusic - 统一构建（Qt 5.15 / Qt 6）
# 主构建：CMake。目标组织：
#   FuruiCore     核心 C++ 静态库（桌面 / 移动共用）
#   FuruiMusic    统一 QML 前端（Windows / macOS / Linux / Android）
#   FuruiMusicWidgets  弃用中的 Widgets 旧前端（默认不构建）
# Qt 5 Android 的 APK 打包仍使用 qmake（见 FuruiMusic.pro）。
#
# src/ 目录职责（构建所需的一切都在本目录内）：
#   core/      业务后端：音源、账号、播放、曲库、开放 API、HTTP
#   ui/        QML 前端 + UI 侧 C++ 适配（含 main.cpp、图标、qml.qrc）
#   config/    声明式配置（sources.json、version.json 也在 src 根）
#   platform/  各平台打包资源：android-qt5/（qmake）、android-qt6/（CMake）
#   legacy/    已弃用、默认不编译的代码：widgets/（老 Widgets 前端）
#   thirdparty/随仓库分发的第三方源码/运行库（ffmpeg、openssl1.1）
#   cmake/     CMake 辅助模块（FFmpeg 自动编译、OpenSSL 运行库部署）
#   scripts/   各平台构建脚本
#   tools/     账号 API / 自定义音源示例服务
#   docs/      架构与调研文档
cmake_minimum_required(VERSION 3.16)
# 版本号的唯一人工来源是 core/Version.h（qmake 构建也直接读它，见发版脚本
# tools/release.py）。这里读出宏值注入 project(... VERSION ...)，Android 的
# versionName/versionCode、macOS bundle 版本与 CI Release 命名全部自动跟随，
# CMakeLists 不再单独维护版本号。
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/core/Version.h" _furui_version_h)
string(REGEX MATCH "#define FURUI_APP_VERSION \"([0-9]+\\.[0-9]+\\.[0-9]+)\""
       _furui_version_match "${_furui_version_h}")
if(NOT _furui_version_match)
    message(FATAL_ERROR "无法从 core/Version.h 解析 FURUI_APP_VERSION（应由 tools/release.py 维护该宏）")
endif()
project(FuruiMusic VERSION "${CMAKE_MATCH_1}" LANGUAGES C CXX)

# 启用 ASM：FuruiBreakpad 在 UNIX 分支编译 common/linux/breakpad_getcontext.S
# （预处理汇编，提供 breakpad_getcontext 符号）。ASM 未启用时 CMake 会把 .S
# 当成「无法确定语言」的文件排除出构建，链接 FuruiMusic 时才报
# undefined reference to breakpad_getcontext。各工具链均自带汇编器，无条件启用安全。
enable_language(ASM)

option(FURUI_BUILD_LEGACY_WIDGETS "构建迁移期 Widgets 旧前端" OFF)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(WIN32)
    # 用 windres 把 app.ico 嵌进 exe，任务栏/Alt+Tab/资源管理器都能拿到 FuruiMusic 图标
    # 只给 CMAKE_C_COMPILER 绝对路径、工具链 bin 又不在 PATH 时，CMake 会把
    # windres 记成裸命令名，构建期报「不是内部或外部命令」。从编译器目录推导绝对路径。
    if(CMAKE_RC_COMPILER AND NOT IS_ABSOLUTE("${CMAKE_RC_COMPILER}"))
        get_filename_component(_furui_cc_dir "${CMAKE_C_COMPILER}" DIRECTORY)
        if(EXISTS "${_furui_cc_dir}/windres.exe")
            set(CMAKE_RC_COMPILER "${_furui_cc_dir}/windres.exe"
                CACHE FILEPATH "Windows 资源编译器 (windres)" FORCE)
            unset(CMAKE_RC_COMPILER_WORKS CACHE)
        endif()
    endif()
    enable_language(RC)
endif()
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
if(MSVC)
    add_compile_options(/utf-8)
endif()

set(QT_COMPONENTS Core Gui Network Sql Multimedia Qml Quick QuickControls2)
if(FURUI_BUILD_LEGACY_WIDGETS AND NOT ANDROID)
    list(APPEND QT_COMPONENTS Widgets)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS ${QT_COMPONENTS})
# QtWebView（内置浏览器）是可选功能：Qt 5 没有该模块；CI / 部分发行包未安装
# qtwebview 模块时，降级为外部浏览器导入（BrowserController 已有兜底），不阻断构建。
set(FURUI_HAVE_WEBVIEW FALSE)
if(QT_VERSION_MAJOR GREATER_EQUAL 6)
    find_package(Qt6 QUIET COMPONENTS WebView)
    if(Qt6WebView_FOUND)
        list(APPEND QT_COMPONENTS WebView)
        set(FURUI_HAVE_WEBVIEW TRUE)
    else()
        message(WARNING
            "[webview] 未找到 Qt6 WebView 模块，内置浏览器降级为外部浏览器导入")
    endif()
endif()
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS ${QT_COMPONENTS})
set(QT Qt${QT_VERSION_MAJOR})

# Windows MinGW 自编译 QtWebView 的 WebView2 后端（MSVC/Android 用 Qt 自带后端）
include(cmake/FuruiWebView2.cmake)
furui_configure_webview2()

# Qt 6 的公开头文件要求 C++17；Qt 5 保持 C++14（MinGW 8.1 已验证）
if(QT_VERSION_MAJOR GREATER_EQUAL 6)
    set(CMAKE_CXX_STANDARD 17)
endif()

# ---- Qt5 自编译 FFmpeg 播放后端 -----------------------------------------
# 具体逻辑见 cmake/FuruiFfmpeg.cmake：Qt 5 桌面在 configure 阶段自动调用
# build_ffmpeg_desktop.sh，从 src/thirdparty/ffmpeg 编出静态库（按编译器 ABI
# 分目录），检测通过则定义 FURUI_HAVE_FFMPEG 并链接 FfmpegVideoPlayer。
# Qt 6 用 QtMultimedia 官方 FFmpeg 插件，完全不走这里；没编出产物也不算
# 错误，Qt 5 仍可构建，只是 MV 退回 DirectShow 降级说明。
include(cmake/FuruiFfmpeg.cmake)

# ---- Windows Qt 5 的 OpenSSL 1.1 运行库 --------------------------------
# Qt 5.15.2 的 Windows 二进制依赖 OpenSSL 1.1，Qt 安装包又不带这两个 DLL，
# 缺了它所有 HTTPS 请求报 "TLS initialization failed"。运行库随仓库放在
# src/thirdparty/openssl1.1/，Qt 5 构建后自动拷到 exe 旁（Qt 6 不需要）。
include(cmake/FuruiOpenssl.cmake)

# ---- Google Breakpad 崩溃转储 ------------------------------------------
# 产出 FuruiBreakpad 静态库（Windows/Linux x86_64 平台 client + crashhandler.cpp），
# 并定义 FURUI_HAVE_BREAKPAD。Android/macOS 与其它 Linux 架构自动跳过，
# 源码缺失时 WARNING 后跳过，不阻断构建。详见 cmake/FuruiBreakpad.cmake。
include(cmake/FuruiBreakpad.cmake)
furui_configure_breakpad()

# sources.json / version.json 内容变化时让 AUTORCC 重新生成 qrc_config.cpp
set_source_files_properties(core/config.qrc PROPERTIES
    AUTORCC_DEPENDENCIES "${CMAKE_CURRENT_SOURCE_DIR}/config/sources.json;${CMAKE_CURRENT_SOURCE_DIR}/version.json")

set(CORE_FILES
    core/QtCompat.h
    core/Types.h core/Types.cpp
    core/Version.h
    core/Settings.h core/JsonMapper.h
    core/HttpClient.h core/HttpClient.cpp
    core/UpdateChecker.h core/UpdateChecker.cpp
    core/SourceConfig.h core/SourceConfig.cpp
    core/Library.h core/Library.cpp
    core/MusicSource.h core/MusicSource.cpp
    core/CustomSource.h core/CustomSource.cpp
    core/GdMusicSource.h core/GdMusicSource.cpp
    core/ScriptEngine.h core/ScriptEngine.cpp
    core/ScriptSource.h core/ScriptSource.cpp
    core/3rd/quickjs/quickjs.c
    core/3rd/quickjs/cutils.c
    core/3rd/quickjs/libregexp.c
    core/3rd/quickjs/libunicode.c
    core/3rd/quickjs/libbf.c
    core/SourceManager.h core/SourceManager.cpp
    core/AuthManager.h core/AuthManager.cpp
    core/PlayerController.h core/PlayerController.cpp
    core/AppController.h core/AppController.cpp
    core/BrowserController.h core/BrowserController.cpp
    core/OpenApiServer.h core/OpenApiServer.cpp
    core/OpenApiWeb.h
    core/Logger.h core/Logger.cpp
    core/Aes128.h core/Aes128.cpp
    core/UserApi.h core/UserApi.cpp
    core/NeteaseCrypto.h core/NeteaseCrypto.cpp
    core/QqProtocol.h core/QqProtocol.cpp
    core/RsaPkcs1.h
    core/KugouProtocol.h core/KugouProtocol.cpp
    core/KugouLoginCrypto.h core/KugouLoginCrypto.cpp
    core/3rd/qrcodegen.hpp core/3rd/qrcodegen.cpp
    core/config.qrc
)

set(UI_FILES
    ui/main.cpp
    ui/PlatformAdapter.h ui/PlatformAdapter.cpp
    ui/ThemeManager.h ui/ThemeManager.cpp
    ui/GlyphImageProvider.h ui/GlyphImageProvider.cpp
    ui/qml.qrc
    ui/icons/icons.qrc
    ui/qml/Main.qml
    ui/qml/MainDesktop.qml
    ui/qml/TitleBar.qml
    ui/qml/ResizeHandle.qml
    ui/qml/GlyphIcon.qml
    ui/qml/MainMobile.qml
    ui/qml/LibraryPage.qml
    ui/qml/LoginPage.qml
    ui/qml/BrowserWindow.qml
    ui/qml/WebLoginView.qml
    ui/qml/CaptchaField.qml
    ui/qml/MvPage.qml
    ui/qml/MvMedia5.qml
    ui/qml/MvMedia5Ffmpeg.qml
    ui/qml/MvMedia6.qml
    ui/qml/SoftShadow.qml
    ui/qml/Toast.qml
    ui/qml/PlayerPage.qml
    ui/qml/SettingsPage.qml
    ui/qml/PlayBar.qml
    ui/qml/QueueDrawer.qml
    ui/qml/NavButton.qml
    ui/qml/SongRow.qml
    ui/qml/AlbumCard.qml
    ui/qml/EmptyState.qml
    ${FURUI_FFMPEG_SOURCES}
)

set(LEGACY_WIDGETS_FILES
    legacy/widgets/main_desktop.cpp
    legacy/widgets/MainWindow.h legacy/widgets/MainWindow.cpp
    legacy/widgets/Theme.h legacy/widgets/Theme.cpp legacy/widgets/ThemeStyle.cpp
    legacy/widgets/PlayerWidgets.h legacy/widgets/PlayerWidgets.cpp
    legacy/widgets/LoginDialog.h legacy/widgets/LoginDialog.cpp
    legacy/widgets/SettingsDialog.h legacy/widgets/SettingsDialog.cpp
    legacy/widgets/LyricsWindow.h legacy/widgets/LyricsWindow.cpp
)

add_library(FuruiCore STATIC ${CORE_FILES})
target_include_directories(FuruiCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(FuruiCore PUBLIC
    ${QT}::Core ${QT}::Gui ${QT}::Network ${QT}::Sql ${QT}::Multimedia)
if(FURUI_HAVE_WEBVIEW)
    target_link_libraries(FuruiCore PUBLIC ${QT}::WebView)
    target_compile_definitions(FuruiCore PUBLIC FURUI_HAVE_WEBVIEW=1)
endif()

if(ANDROID)
    if(QT_VERSION_MAJOR LESS 6)
        message(FATAL_ERROR "CMake 的 Android 打包需要 Qt 6；Qt 5 Android 请用 FuruiMusic.pro + qmake")
    endif()
    qt_add_executable(FuruiMusic MANUAL_FINALIZATION ${UI_FILES})
    # androiddeployqt 不会自动读 project() 的 VERSION，缺省写死 1.0/1；
    # 这里显式传进去，保证「应用关于页显示的版本 = git tag」。
    math(EXPR FURUI_ANDROID_VERSION_CODE
         "${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")
    set_target_properties(FuruiMusic PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/platform/android-qt6"
        QT_ANDROID_VERSION_NAME "${PROJECT_VERSION}"
        QT_ANDROID_VERSION_CODE ${FURUI_ANDROID_VERSION_CODE}
        QT_ANDROID_MIN_SDK_VERSION 23
        QT_ANDROID_TARGET_SDK_VERSION 35)

    # Android 系统只提供 BoringSSL，Qt 6 的 OpenSSL 后端会按 _3 后缀查找
    # libssl_3.so / libcrypto_3.so。缺失时 QSslSocket::supportsSsl() 为 false，
    # 所有 HTTPS 音源和在线脚本都会取链失败，因此把预编译库随 APK 一起打包。
    set(FURUI_ANDROID_OPENSSL_LIBS
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/android-qt6/libs/${ANDROID_ABI}/libssl_3.so"
        "${CMAKE_CURRENT_SOURCE_DIR}/platform/android-qt6/libs/${ANDROID_ABI}/libcrypto_3.so")
    foreach(_ssl_lib IN LISTS FURUI_ANDROID_OPENSSL_LIBS)
        if(EXISTS "${_ssl_lib}")
            set_property(TARGET FuruiMusic APPEND PROPERTY
                QT_ANDROID_EXTRA_LIBS "${_ssl_lib}")
        endif()
    endforeach()

    target_link_libraries(FuruiMusic PRIVATE FuruiCore ${QT}::Qml ${QT}::Quick ${QT}::QuickControls2)
    qt_finalize_executable(FuruiMusic)
else()
    add_executable(FuruiMusic WIN32 MACOSX_BUNDLE ${UI_FILES})
    target_link_libraries(FuruiMusic PRIVATE FuruiCore ${QT}::Qml ${QT}::Quick ${QT}::QuickControls2)
    set_target_properties(FuruiMusic PROPERTIES OUTPUT_NAME "FuruiMusic")

    if(APPLE)
        set_target_properties(FuruiMusic PROPERTIES
            MACOSX_BUNDLE_BUNDLE_NAME "FuruiMusic"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "cn.furui.music"
            MACOSX_BUNDLE_ICON_FILE "FuruiMusic.icns"
        )
        # macOS 的 Dock 图标只认 bundle 里的 .icns，运行时 setWindowIcon 不生效
        set_source_files_properties(ui/icons/FuruiMusic.icns PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources")
        target_sources(FuruiMusic PRIVATE ui/icons/FuruiMusic.icns)
    endif()

    if(WIN32)
        configure_file(ui/icons/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)
        target_sources(FuruiMusic PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/app.rc)
        furui_deploy_openssl_runtime(FuruiMusic)
        furui_deploy_webview2(FuruiMusic)
    endif()

    if(FURUI_BUILD_LEGACY_WIDGETS)
        add_executable(FuruiMusicWidgets WIN32 MACOSX_BUNDLE ${LEGACY_WIDGETS_FILES})
        target_link_libraries(FuruiMusicWidgets PRIVATE FuruiCore ${QT}::Widgets)
        set_target_properties(FuruiMusicWidgets PROPERTIES OUTPUT_NAME "FuruiMusicWidgets")
        if(WIN32)
            furui_deploy_openssl_runtime(FuruiMusicWidgets)
        endif()
    endif()
endif()

if(FURUI_HAVE_FFMPEG)
    target_compile_definitions(FuruiMusic PRIVATE FURUI_HAVE_FFMPEG=1)
    target_include_directories(FuruiMusic PRIVATE "${FURUI_FFMPEG_INCLUDE_DIR}")
    target_link_libraries(FuruiMusic PRIVATE ${FURUI_FFMPEG_LINK_LIBS})
    find_package(Threads QUIET)
    if(Threads_FOUND)
        target_link_libraries(FuruiMusic PRIVATE Threads::Threads)
    endif()
endif()

if(FURUI_HAVE_BREAKPAD)
    target_compile_definitions(FuruiMusic PRIVATE FURUI_HAVE_BREAKPAD=1)
    # 显式把 breakpad 根目录加进 include 路径，保证 main.cpp 能
    # #include "thirdparty/breakpad/crashhandler.h"（与 core/QtCompat.h 同一规则）
    target_include_directories(FuruiMusic PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/breakpad")
    target_link_libraries(FuruiMusic PRIVATE FuruiBreakpad)
endif()

set_target_properties(FuruiCore PROPERTIES POSITION_INDEPENDENT_CODE ON)

# QuickJS：内嵌 ES2020+ 引擎（Qt5 的 QJSEngine 不支持 ?./??/async，
# 现存 LX 音源脚本无一例外要用到这些语法，所以必须用独立引擎）。
# MinGW 8.1 在 -O2 + SEH 下会让 quickjs 汇编出错，固定 -O1。
set(QUICKJS_SOURCES
    core/3rd/quickjs/quickjs.c
    core/3rd/quickjs/cutils.c
    core/3rd/quickjs/libregexp.c
    core/3rd/quickjs/libunicode.c
    core/3rd/quickjs/libbf.c
)
if(NOT MSVC)
    set_source_files_properties(${QUICKJS_SOURCES} PROPERTIES COMPILE_FLAGS
        "-O1 -std=gnu99 -D_GNU_SOURCE -include ${CMAKE_CURRENT_SOURCE_DIR}/core/3rd/quickjs/qjs_config.h")
endif()
