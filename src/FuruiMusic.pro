# 福瑞音乐 FuruiMusic - qmake 兼容构建
# 主构建请使用 CMakeLists.txt；本文件保留用于 Qt 5.15 Android 的 APK 打包链路。
# 桌面与 Android 都只产出统一 QML 前端；Widgets 旧前端需显式 CONFIG+=legacy_widgets。
QT += core network sql multimedia qml quick quickcontrols2
CONFIG += c++14
TEMPLATE = app
# 桌面主交付已切 Qt 6（QtMultimedia 官方 FFmpeg 后端，见 scripts/build_windows.bat）。
# Qt 5 qmake 保留为兼容构建，可用 FURUI_QT5_OUTDIR 指定独立输出目录，
# 避免覆盖 bin 里的 Qt 6 产物（Qt5/Qt6 运行库混放会互相污染）。
DESTDIR = $$PWD/../bin
!isEmpty(FURUI_QT5_OUTDIR): DESTDIR = $$FURUI_QT5_OUTDIR
win32: CONFIG += windows

INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD
msvc: QMAKE_CXXFLAGS += /utf-8

# 内嵌 QuickJS：MinGW 8.1 在 -O2 + SEH 下会让 quickjs 汇编报错（.seh_savexmm offset
# is negative），必须固定 -O1。注意 qmake 会把 QMAKE_CFLAGS_RELEASE 的 -O2 排在后面，
# 只往 QMAKE_CFLAGS 里加 -O1 会被 -O2 覆盖，所以要从 RELEASE 变量里摘掉 -O2。
# 项目里的 .c 文件只有 quickjs，因此这条只影响它。
!msvc {
    QMAKE_CFLAGS_RELEASE -= -O2
    QMAKE_CFLAGS_RELEASE += -O1
    QMAKE_CFLAGS += -O1 -std=gnu99 -D_GNU_SOURCE
    QMAKE_CFLAGS += -include $$PWD/core/3rd/quickjs/qjs_config.h
}

CORE_HEADERS += \
    core/QtCompat.h \
    core/Types.h \
    core/Version.h \
    core/Settings.h \
    core/JsonMapper.h \
    core/HttpClient.h \
    core/UpdateChecker.h \
    core/SourceConfig.h \
    core/Library.h \
    core/MusicSource.h \
    core/CustomSource.h \
    core/GdMusicSource.h \
    core/ScriptEngine.h \
    core/ScriptSource.h \
    core/SourceManager.h \
    core/AuthManager.h \
    core/PlayerController.h \
    core/AppController.h \
    core/OpenApiServer.h \
    core/OpenApiWeb.h \
    core/Logger.h \
    core/Aes128.h \
    core/UserApi.h \
    core/NeteaseCrypto.h \
    core/QqProtocol.h \
    core/RsaPkcs1.h \
    core/KugouProtocol.h \
    core/KugouLoginCrypto.h \
    core/3rd/qrcodegen.hpp

CORE_SOURCES += \
    core/Types.cpp \
    core/HttpClient.cpp \
    core/UpdateChecker.cpp \
    core/SourceConfig.cpp \
    core/Library.cpp \
    core/MusicSource.cpp \
    core/CustomSource.cpp \
    core/GdMusicSource.cpp \
    core/ScriptEngine.cpp \
    core/ScriptSource.cpp \
    core/3rd/quickjs/quickjs.c \
    core/3rd/quickjs/cutils.c \
    core/3rd/quickjs/libregexp.c \
    core/3rd/quickjs/libunicode.c \
    core/3rd/quickjs/libbf.c \
    core/SourceManager.cpp \
    core/AuthManager.cpp \
    core/PlayerController.cpp \
    core/AppController.cpp \
    core/OpenApiServer.cpp \
    core/Logger.cpp \
    core/Aes128.cpp \
    core/UserApi.cpp \
    core/NeteaseCrypto.cpp \
    core/QqProtocol.cpp \
    core/KugouProtocol.cpp \
    core/KugouLoginCrypto.cpp \
    core/3rd/qrcodegen.cpp

HEADERS += $$CORE_HEADERS
SOURCES += $$CORE_SOURCES
RESOURCES += core/config.qrc

CONFIG(legacy_widgets) {
    # 迁移期保留：显式 qmake FuruiMusic.pro CONFIG+=legacy_widgets 才构建
    QT += widgets
    TARGET = FuruiMusicWidgets
    SOURCES += \
        desktop/main_desktop.cpp \
        desktop/MainWindow.cpp \
        desktop/Theme.cpp \
        desktop/ThemeStyle.cpp \
        desktop/PlayerWidgets.cpp \
        desktop/LoginDialog.cpp \
        desktop/SettingsDialog.cpp \
        desktop/LyricsWindow.cpp
    HEADERS += \
        desktop/MainWindow.h \
        desktop/Theme.h \
        desktop/PlayerWidgets.h \
        desktop/LoginDialog.h \
        desktop/SettingsDialog.h \
        desktop/LyricsWindow.h
} else {
    # 统一 QML 前端：桌面与 Android 共用同一入口和资源
    TARGET = FuruiMusic
    HEADERS += ui/PlatformAdapter.h ui/ThemeManager.h ui/GlyphImageProvider.h
    SOURCES += ui/main.cpp ui/PlatformAdapter.cpp ui/ThemeManager.cpp ui/GlyphImageProvider.cpp
    RESOURCES += ui/qml.qrc ui/icons/icons.qrc

    # Qt5 自编译 FFmpeg 播放后端：src/scripts/build_ffmpeg_desktop.bat 产出
    # build_output/ffmpeg-qt5。检测到静态库就启用 C++ 自解码播放器，
    # 完全绕开 DirectShow；没检测到仍可构建，MV 只是给出降级说明。
    # Android 的 Qt5 qmake 链路不能链接桌面 MinGW 编译的 .a，显式排除
    !android {
    FURUI_FF_ROOT = $$(FURUI_FFMPEG_ROOT)
    isEmpty(FURUI_FF_ROOT): FURUI_FF_ROOT = $$PWD/../build_output/ffmpeg-qt5
    exists($$FURUI_FF_ROOT/lib/libavformat.a) {
        DEFINES += FURUI_HAVE_FFMPEG
        INCLUDEPATH += $$FURUI_FF_ROOT/include
        HEADERS += ui/FfmpegVideoPlayer.h
        SOURCES += ui/FfmpegVideoPlayer.cpp
        # 静态库顺序：被依赖的 avutil 放最后，再重复一次兜底循环依赖
        LIBS += -L$$FURUI_FF_ROOT/lib \
                -lavformat -lavcodec -lswscale -lswresample -lavutil -lavutil
        win32 {
            LIBS += -lws2_32 -lsecur32 -lcrypt32 -lbcrypt -lm -lz \
                    -lpsapi -ladvapi32 -lshell32 -lole32 -luser32 -lgdi32
        } else {
            LIBS += -lm -lz -lpthread -ldl
        }
        message("Qt5 FFmpeg playback backend enabled: $$FURUI_FF_ROOT")
    } else {
        message("Qt5 FFmpeg backend not found - MV falls back to DirectShow")
    }
    }
    OTHER_FILES += \
        ui/qml/Main.qml \
        ui/qml/MainDesktop.qml \
        ui/qml/MainMobile.qml \
        ui/qml/TitleBar.qml \
        ui/qml/ResizeHandle.qml \
        ui/qml/GlyphIcon.qml \
        ui/qml/LibraryPage.qml \
        ui/qml/LoginPage.qml \
        ui/qml/CaptchaField.qml \
        ui/qml/MvPage.qml \
        ui/qml/MvMedia5.qml \
        ui/qml/MvMedia5Ffmpeg.qml \
        ui/qml/MvMedia6.qml \
        ui/qml/SoftShadow.qml \
        ui/qml/Toast.qml \
        ui/qml/PlayerPage.qml \
        ui/qml/SettingsPage.qml \
        ui/qml/PlayBar.qml \
        ui/qml/QueueDrawer.qml \
        ui/qml/NavButton.qml \
        ui/qml/SongRow.qml \
        ui/qml/AlbumCard.qml \
        ui/qml/EmptyState.qml \
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

# Windows 可执行文件图标（MinGW/VS 都用 windres 编译本 rc，把 app.ico 嵌进 exe）。
# CMake 走 app.rc.in（绝对路径），这里是 qmake 用的相对路径版本。
win32 {
    RC_FILE = $$PWD/ui/icons/app.rc

    # Qt 5.15.2 的 Windows 二进制基于 OpenSSL 1.1（不是 3.x），Qt 安装包不带这套 DLL，
    # 缺了它 QtNetwork 的所有 HTTPS 请求报 "TLS initialization failed"。运行库随仓库
    # 放在 src/thirdparty/openssl1.1/，链接后拷到目标目录；CMake 构建由
    # cmake/FuruiOpenssl.cmake 做同一件事（Qt 6 用 Schannel，不需要）。
    FURUI_OPENSSL_LIBS = $$PWD/thirdparty/openssl1.1
    contains(QT_ARCH, x86_64) {
        # 不用 foreach：Qt 5 的 qmake 不认（会报 'foreach' is not a recognized test function）
        exists($$FURUI_OPENSSL_LIBS/libssl-1_1-x64.dll) {
            QMAKE_POST_LINK += $$QMAKE_COPY $$shell_path($$FURUI_OPENSSL_LIBS/libssl-1_1-x64.dll) $$shell_path($$DESTDIR) $$escape_expand(\n\t)
        }
        exists($$FURUI_OPENSSL_LIBS/libcrypto-1_1-x64.dll) {
            QMAKE_POST_LINK += $$QMAKE_COPY $$shell_path($$FURUI_OPENSSL_LIBS/libcrypto-1_1-x64.dll) $$shell_path($$DESTDIR) $$escape_expand(\n\t)
        }
    } else {
        message("32-bit build: src/thirdparty/openssl1.1 only ships x64 OpenSSL runtime, skipping copy")
    }
}
