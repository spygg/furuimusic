# 福瑞音乐 FuruiMusic - 统一构建（Qt 5.15 / Qt 6）
# 主构建：CMake。目标组织：
#   FuruiCore     核心 C++ 静态库（桌面 / 移动共用）
#   FuruiMusic    统一 QML 前端（Windows / macOS / Linux / Android）
#   FuruiMusicWidgets  迁移期保留的 Widgets 旧前端（默认不构建）
# Qt 5 Android 的 APK 打包仍使用 qmake（见 FuruiMusic.pro）。
cmake_minimum_required(VERSION 3.16)
project(FuruiMusic VERSION 1.0.0 LANGUAGES C CXX)

option(FURUI_BUILD_LEGACY_WIDGETS "构建迁移期 Widgets 旧前端" OFF)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(WIN32)
    # 用 windres 把 app.ico 嵌进 exe，任务栏/Alt+Tab/资源管理器都能拿到 FuruiMusic 图标
    enable_language(RC)
endif()
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
if(MSVC)
    add_compile_options(/utf-8)
endif()

set(QT_COMPONENTS Core Gui Network Sql Multimedia Qml Quick)
if(FURUI_BUILD_LEGACY_WIDGETS AND NOT ANDROID)
    list(APPEND QT_COMPONENTS Widgets)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS ${QT_COMPONENTS})
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS ${QT_COMPONENTS})
set(QT Qt${QT_VERSION_MAJOR})

# Qt 6 的公开头文件要求 C++17；Qt 5 保持 C++14（MinGW 8.1 已验证）
if(QT_VERSION_MAJOR GREATER_EQUAL 6)
    set(CMAKE_CXX_STANDARD 17)
endif()

# sources.json 内容变化时让 AUTORCC 重新生成 qrc_config.cpp
set_source_files_properties(src/core/config.qrc PROPERTIES
    AUTORCC_DEPENDENCIES "${CMAKE_CURRENT_SOURCE_DIR}/config/sources.json")

set(CORE_FILES

    src/core/Types.h src/core/Types.cpp
    src/core/Settings.h src/core/JsonMapper.h
    src/core/HttpClient.h src/core/HttpClient.cpp
    src/core/SourceConfig.h src/core/SourceConfig.cpp
    src/core/Library.h src/core/Library.cpp
    src/core/MusicSource.h src/core/MusicSource.cpp
    src/core/CustomSource.h src/core/CustomSource.cpp
    src/core/ScriptEngine.h src/core/ScriptEngine.cpp
    src/core/ScriptSource.h src/core/ScriptSource.cpp
    src/core/3rd/quickjs/quickjs.c
    src/core/3rd/quickjs/cutils.c
    src/core/3rd/quickjs/libregexp.c
    src/core/3rd/quickjs/libunicode.c
    src/core/3rd/quickjs/libbf.c
    src/core/SourceManager.h src/core/SourceManager.cpp
    src/core/AuthManager.h src/core/AuthManager.cpp
    src/core/PlayerController.h src/core/PlayerController.cpp
    src/core/AppController.h src/core/AppController.cpp
    src/core/OpenApiServer.h src/core/OpenApiServer.cpp
    src/core/Aes128.h src/core/Aes128.cpp
    src/core/UserApi.h src/core/UserApi.cpp
    src/core/NeteaseCrypto.h src/core/NeteaseCrypto.cpp
    src/core/QqProtocol.h src/core/QqProtocol.cpp
    src/core/KugouProtocol.h src/core/KugouProtocol.cpp
    src/core/3rd/qrcodegen.hpp src/core/3rd/qrcodegen.cpp
    src/core/config.qrc
)

set(UI_FILES
    src/ui/main.cpp
    src/ui/PlatformAdapter.h src/ui/PlatformAdapter.cpp
    src/ui/ThemeManager.h src/ui/ThemeManager.cpp
    src/ui/qml.qrc
    src/ui/icons/icons.qrc
    src/ui/qml/Main.qml
    src/ui/qml/MainDesktop.qml
    src/ui/qml/TitleBar.qml
    src/ui/qml/ResizeHandle.qml
    src/ui/qml/GlyphIcon.qml
    src/ui/qml/MainMobile.qml
    src/ui/qml/SearchPage.qml
    src/ui/qml/LibraryPage.qml
    src/ui/qml/LoginPage.qml
    src/ui/qml/PlayerPage.qml
    src/ui/qml/SettingsPage.qml
    src/ui/qml/PlayBar.qml
    src/ui/qml/QueueDrawer.qml
    src/ui/qml/ControlButton.qml
    src/ui/qml/NavButton.qml
    src/ui/qml/SongRow.qml
    src/ui/qml/AlbumCard.qml
    src/ui/qml/PlatformChip.qml
    src/ui/qml/EmptyState.qml
)

set(LEGACY_WIDGETS_FILES
    src/desktop/main_desktop.cpp
    src/desktop/MainWindow.h src/desktop/MainWindow.cpp
    src/desktop/Theme.h src/desktop/Theme.cpp src/desktop/ThemeStyle.cpp
    src/desktop/PlayerWidgets.h src/desktop/PlayerWidgets.cpp
    src/desktop/LoginDialog.h src/desktop/LoginDialog.cpp
    src/desktop/SettingsDialog.h src/desktop/SettingsDialog.cpp
    src/desktop/LyricsWindow.h src/desktop/LyricsWindow.cpp
)

add_library(FuruiCore STATIC ${CORE_FILES})
target_include_directories(FuruiCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(FuruiCore PUBLIC
    ${QT}::Core ${QT}::Gui ${QT}::Network ${QT}::Sql ${QT}::Multimedia)

if(ANDROID)
    if(QT_VERSION_MAJOR LESS 6)
        message(FATAL_ERROR "CMake 的 Android 打包需要 Qt 6；Qt 5 Android 请用 FuruiMusic.pro + qmake")
    endif()
    qt_add_executable(FuruiMusic MANUAL_FINALIZATION ${UI_FILES})
    set_target_properties(FuruiMusic PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android_qt6"
        QT_ANDROID_MIN_SDK_VERSION 23
        QT_ANDROID_TARGET_SDK_VERSION 35)
    target_link_libraries(FuruiMusic PRIVATE FuruiCore ${QT}::Qml ${QT}::Quick)
    qt_finalize_executable(FuruiMusic)
else()
    add_executable(FuruiMusic WIN32 MACOSX_BUNDLE ${UI_FILES})
    target_link_libraries(FuruiMusic PRIVATE FuruiCore ${QT}::Qml ${QT}::Quick)
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
        set_source_files_properties(src/ui/icons/FuruiMusic.icns PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources")
        target_sources(FuruiMusic PRIVATE src/ui/icons/FuruiMusic.icns)
    endif()

    if(WIN32)
        configure_file(src/ui/icons/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)
        target_sources(FuruiMusic PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/app.rc)
    endif()

    if(FURUI_BUILD_LEGACY_WIDGETS)
        add_executable(FuruiMusicWidgets WIN32 MACOSX_BUNDLE ${LEGACY_WIDGETS_FILES})
        target_link_libraries(FuruiMusicWidgets PRIVATE FuruiCore ${QT}::Widgets)
        set_target_properties(FuruiMusicWidgets PROPERTIES OUTPUT_NAME "FuruiMusicWidgets")
    endif()
endif()

set_target_properties(FuruiCore PROPERTIES POSITION_INDEPENDENT_CODE ON)

# QuickJS：内嵌 ES2020+ 引擎（Qt5 的 QJSEngine 不支持 ?./??/async，
# 现存 LX 音源脚本无一例外要用到这些语法，所以必须用独立引擎）。
# MinGW 8.1 在 -O2 + SEH 下会让 quickjs 汇编出错，固定 -O1。
set(QUICKJS_SOURCES
    src/core/3rd/quickjs/quickjs.c
    src/core/3rd/quickjs/cutils.c
    src/core/3rd/quickjs/libregexp.c
    src/core/3rd/quickjs/libunicode.c
    src/core/3rd/quickjs/libbf.c
)
if(NOT MSVC)
    set_source_files_properties(${QUICKJS_SOURCES} PROPERTIES COMPILE_FLAGS
        "-O1 -std=gnu99 -D_GNU_SOURCE -include ${CMAKE_CURRENT_SOURCE_DIR}/src/core/3rd/quickjs/qjs_config.h")
endif()
