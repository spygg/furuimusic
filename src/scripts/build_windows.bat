@echo off
rem FuruiMusic desktop build (incremental, never cleans build output)
rem
rem 主交付 = Qt 6.11.1 mingw_64 + CMake。Qt 6 的 QtMultimedia 走官方 FFmpeg
rem 插件（ffmpegmediaplugin.dll + avcodec-61.dll 等），MV 在应用内解码 MP4/H.264。
rem Qt 5.15 桌面只有 DirectShow，放不了 MP4：兼容构建会先跑
rem build_ffmpeg_desktop.bat 自编译 FFmpeg 静态库，链接 C++ 自解码后端
rem（FfmpegVideoPlayer），这样 Qt5 也能在应用内播 MV，且跨平台。
rem Qt 5 qmake 输出到独立目录，最终分发产物按版本落到 bin_qt6 / bin_qt5，互不覆盖。
setlocal
set "ROOT=%~dp0..\.."
set "SRC=%ROOT%\src"
set "QT6_DIR=E:\programes\Qt\Qt\6.11.1\mingw_64"
set "MINGW6_DIR=E:\programes\Qt\Qt\Tools\mingw1310_64"
set "QT5_DIR=E:\programes\Qt\Qt\5.15.2\mingw81_64"
set "MINGW5_DIR=E:\programes\Qt\Qt\Tools\mingw810_64"
set "CMAKE_DIR=E:\programes\Qt\Qt\Tools\CMake_64\bin"
set "NINJA_DIR=E:\programes\Qt\Qt\Tools\Ninja"
set "PATH=%QT6_DIR%\bin;%MINGW6_DIR%\bin;%CMAKE_DIR%;%NINJA_DIR%;%PATH%"

if not exist "%ROOT%\bin_qt6" mkdir "%ROOT%\bin_qt6"

rem ---------- 主构建：Qt 6 + CMake/Ninja ----------
echo [cmake] configuring FuruiMusic (Qt 6 / FFmpeg multimedia backend)
cmake -S "%SRC%" -B "%ROOT%\build\desktop-qt6" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH="%QT6_DIR%" ^
    -DCMAKE_C_COMPILER="%MINGW6_DIR%\bin\gcc.exe" ^
    -DCMAKE_CXX_COMPILER="%MINGW6_DIR%\bin\g++.exe" || goto :fail
cmake --build "%ROOT%\build\desktop-qt6" --target FuruiMusic -j %NUMBER_OF_PROCESSORS% || goto :fail
copy /y "%ROOT%\build\desktop-qt6\FuruiMusic.exe" "%ROOT%\bin_qt6\" >nul || goto :fail

rem windeployqt: --qmldir is REQUIRED. Without it QtQuick/Controls/Layouts/Window QML
rem plugins are NOT deployed and the app fails to load qrc:/qml/Main.qml.
rem Qt 6 版本会一并部署 plugins/multimedia/ffmpegmediaplugin.dll 与 avcodec-61.dll
rem 等 FFmpeg 运行库（MV 解码就靠它们），不再需要手工拷 OpenSSL 1.1。
windeployqt --no-translations --qmldir "%SRC%\ui\qml" "%ROOT%\bin_qt6\FuruiMusic.exe" || goto :fail
echo [deploy] Qt 6 QML + multimedia (FFmpeg) plugins deployed into bin_qt6

rem MinGW 版 Qt 不带 QtWebView 后端插件；CMake POST_BUILD 已把它和 WebView2Loader.dll
rem 放进 build\desktop-qt6，随 exe 一起拷到分发目录，内置浏览器才可用。
if exist "%ROOT%\build\desktop-qt6\plugins\webview\qtwebview_webview2.dll" (
    if not exist "%ROOT%\bin_qt6\plugins\webview" mkdir "%ROOT%\bin_qt6\plugins\webview"
    copy /y "%ROOT%\build\desktop-qt6\plugins\webview\qtwebview_webview2.dll" "%ROOT%\bin_qt6\plugins\webview\" >nul || goto :fail
    copy /y "%ROOT%\build\desktop-qt6\WebView2Loader.dll" "%ROOT%\bin_qt6\" >nul || goto :fail
    echo [deploy] QtWebView WebView2 backend + loader deployed into bin_qt6
)

rem ---------- 可选：Qt 5.15 兼容构建（自编译 FFmpeg 播放后端）----------
if not "%FURUI_BUILD_QT5%"=="1" goto :skip_qt5
set "FURUI_MINGW_BIN=%MINGW5_DIR%\bin"
echo [ffmpeg] building Qt5 FFmpeg playback backend (incremental)
call "%SRC%\scripts\build_ffmpeg_desktop.bat" -j %NUMBER_OF_PROCESSORS% || goto :fail
if not exist "%ROOT%\build\desktop-qt5" mkdir "%ROOT%\build\desktop-qt5"
set "PATH=%QT5_DIR%\bin;%MINGW5_DIR%\bin;%PATH%"
cd /d "%ROOT%\build\desktop-qt5"
echo [qmake] generating Qt 5 compatibility Makefile (FURUI_HAVE_FFMPEG)
qmake "%SRC%\FuruiMusic.pro" -o Makefile "FURUI_QT5_OUTDIR=%ROOT%\build\desktop-qt5" || goto :fail
echo [make] building Qt 5 compatibility binary
mingw32-make -f Makefile release -j%NUMBER_OF_PROCESSORS% || goto :fail
if not exist "%ROOT%\bin_qt5" mkdir "%ROOT%\bin_qt5"
copy /y "%ROOT%\build\desktop-qt5\FuruiMusic.exe" "%ROOT%\bin_qt5\" >nul || goto :fail
rem Qt 5 的 windeployqt 会带 Qt5Core/Qt5Quick/Qt5QuickControls2 等 DLL、qwindows
rem 与 QML 模块；FFmpeg 是静态链接进 exe 的，运行时不需要额外带 FFmpeg DLL。
windeployqt --no-translations --qmldir "%SRC%\ui\qml" "%ROOT%\bin_qt5\FuruiMusic.exe" || goto :fail
rem Qt 5.15.2 的 Windows 二进制依赖 OpenSSL 1.1，Qt 安装包不自带：不拷这两个 DLL
rem 时所有 HTTPS 请求报 "TLS initialization failed"（音源全挂的样子）。
if exist "%SRC%\thirdparty\openssl1.1\libssl-1_1-x64.dll" (
    xcopy /y /q "%SRC%\thirdparty\openssl1.1\*.dll" "%ROOT%\bin_qt5\" >nul || goto :fail
    echo [qt5] OpenSSL 1.1 runtime copied into bin_qt5
) else (
    echo [qt5] WARNING: src\thirdparty\openssl1.1 has no OpenSSL 1.1 DLL - HTTPS will fail
)
echo [qt5] binary + Qt 5 runtime in bin_qt5 (self-built FFmpeg backend: in-app MV enabled)
:skip_qt5

echo.
echo Build finished. Qt 6 binary in bin_qt6, Qt 5 binary in bin_qt5 (MV enabled).
echo For a redistributable folder run: windeployqt --no-translations --qmldir "%SRC%\ui\qml" "%ROOT%\bin_qt6\FuruiMusic.exe"
goto :eof
:fail
echo Build FAILED.
exit /b 1
