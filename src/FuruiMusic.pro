# 福瑞音乐 FuruiMusic - qmake 兼容构建
# 主构建请使用 CMakeLists.txt；本文件保留用于 Qt 5.15 Android 的 APK 打包链路。
# 桌面与 Android 都只产出统一 QML 前端；Widgets 旧前端需显式 CONFIG+=legacy_widgets。
QT += core network sql multimedia qml quick
CONFIG += c++14
TEMPLATE = app
DESTDIR = $$PWD/bin
win32: CONFIG += windows

INCLUDEPATH += $$PWD/src
DEPENDPATH += $$PWD/src
msvc: QMAKE_CXXFLAGS += /utf-8

# 内嵌 QuickJS：MinGW 8.1 在 -O2 + SEH 下会让 quickjs 汇编报错，固定 -O1。
!msvc {
    QMAKE_CFLAGS += -O1 -std=gnu99 -D_GNU_SOURCE
    QMAKE_CFLAGS += -include $$PWD/src/core/3rd/quickjs/qjs_config.h
}

CORE_HEADERS += \
    src/core/Types.h \
    src/core/Settings.h \
    src/core/JsonMapper.h \
    src/core/HttpClient.h \
    src/core/SourceConfig.h \
    src/core/Library.h \
    src/core/MusicSource.h \
    src/core/CustomSource.h \
    src/core/ScriptEngine.h \
    src/core/ScriptSource.h \
    src/core/SourceManager.h \
    src/core/AuthManager.h \
    src/core/PlayerController.h \
    src/core/AppController.h \
    src/core/OpenApiServer.h \
    src/core/Aes128.h \
    src/core/UserApi.h \
    src/core/NeteaseCrypto.h \
    src/core/QqProtocol.h \
    src/core/KugouProtocol.h \
    src/core/3rd/qrcodegen.hpp

CORE_SOURCES += \
    src/core/Types.cpp \
    src/core/HttpClient.cpp \
    src/core/SourceConfig.cpp \
    src/core/Library.cpp \
    src/core/MusicSource.cpp \
    src/core/CustomSource.cpp \
    src/core/ScriptEngine.cpp \
    src/core/ScriptSource.cpp \
    src/core/3rd/quickjs/quickjs.c \
    src/core/3rd/quickjs/cutils.c \
    src/core/3rd/quickjs/libregexp.c \
    src/core/3rd/quickjs/libunicode.c \
    src/core/3rd/quickjs/libbf.c \
    src/core/SourceManager.cpp \
    src/core/AuthManager.cpp \
    src/core/PlayerController.cpp \
    src/core/AppController.cpp \
    src/core/OpenApiServer.cpp \
    src/core/Aes128.cpp \
    src/core/UserApi.cpp \
    src/core/NeteaseCrypto.cpp \
    src/core/QqProtocol.cpp \
    src/core/KugouProtocol.cpp \
    src/core/3rd/qrcodegen.cpp

HEADERS += $$CORE_HEADERS
SOURCES += $$CORE_SOURCES
RESOURCES += src/core/config.qrc

CONFIG(legacy_widgets) {
    # 迁移期保留：显式 qmake FuruiMusic.pro CONFIG+=legacy_widgets 才构建
    QT += widgets
    TARGET = FuruiMusicWidgets
    SOURCES += \
        src/desktop/main_desktop.cpp \
        src/desktop/MainWindow.cpp \
        src/desktop/Theme.cpp \
        src/desktop/ThemeStyle.cpp \
        src/desktop/PlayerWidgets.cpp \
        src/desktop/LoginDialog.cpp \
        src/desktop/SettingsDialog.cpp \
        src/desktop/LyricsWindow.cpp
    HEADERS += \
        src/desktop/MainWindow.h \
        src/desktop/Theme.h \
        src/desktop/PlayerWidgets.h \
        src/desktop/LoginDialog.h \
        src/desktop/SettingsDialog.h \
        src/desktop/LyricsWindow.h
} else {
    # 统一 QML 前端：桌面与 Android 共用同一入口和资源
    TARGET = FuruiMusic
    HEADERS += src/ui/PlatformAdapter.h src/ui/ThemeManager.h
    SOURCES += src/ui/main.cpp src/ui/PlatformAdapter.cpp src/ui/ThemeManager.cpp
    RESOURCES += src/ui/qml.qrc src/ui/icons/icons.qrc
    OTHER_FILES += \
        src/ui/qml/Main.qml \
        src/ui/qml/MainDesktop.qml \
        src/ui/qml/MainMobile.qml \
        src/ui/qml/TitleBar.qml \
        src/ui/qml/ResizeHandle.qml \
        src/ui/qml/GlyphIcon.qml \
        src/ui/qml/SearchPage.qml \
        src/ui/qml/LibraryPage.qml \
        src/ui/qml/LoginPage.qml \
        src/ui/qml/PlayerPage.qml \
        src/ui/qml/SettingsPage.qml \
        src/ui/qml/PlayBar.qml \
        src/ui/qml/QueueDrawer.qml \
        src/ui/qml/ControlButton.qml \
        src/ui/qml/NavButton.qml \
        src/ui/qml/SongRow.qml \
        src/ui/qml/AlbumCard.qml \
        src/ui/qml/PlatformChip.qml \
        src/ui/qml/EmptyState.qml \
        config/sources.json
}

android {
    ANDROID_PACKAGE_SOURCE_DIR = $$PWD/android
    ANDROID_MIN_SDK_VERSION = 21
    ANDROID_TARGET_SDK_VERSION = 30
    ANDROID_ABIS = arm64-v8a

    # Qt 5 Android 的 HTTPS 依赖预编译 OpenSSL，可用环境变量 ANDROID_SDK_ROOT 指向 SDK
    FURUI_SSL_PRI = $$(ANDROID_SDK_ROOT)/android_openssl/openssl.pri
    exists($$FURUI_SSL_PRI): include($$FURUI_SSL_PRI)
}
