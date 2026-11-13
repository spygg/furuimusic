@echo off
rem FuruiMusic desktop build (incremental, never cleans build output)
rem
rem Primary deliverable = Qt 6.11.1 mingw_64 + CMake. Qt 6 QtMultimedia uses the
rem official FFmpeg plugin (ffmpegmediaplugin.dll + avcodec-61.dll etc.); MV decodes
rem MP4/H.264 in-app.
rem Qt 5.15 desktop only has DirectShow, which cannot play MP4: the compatibility
rem build (FURUI_BUILD_QT5=1) first runs build_ffmpeg_desktop.bat to self-build an
rem FFmpeg static lib and link the C++ FfmpegVideoPlayer backend, so Qt5 can also
rem play MV in-app and cross-platform.
rem Qt 5 qmake outputs to a separate dir; final distributables land in bin_qt6 /
rem bin_qt5 per version, never overwriting each other.
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

rem ---------- primary build: Qt 6 + CMake/Ninja ----------
echo [cmake] configuring FuruiMusic (Qt 6 / FFmpeg multimedia backend)
cmake -S "%SRC%" -B "%ROOT%\build\desktop-qt6" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH="%QT6_DIR%" ^
    -DCMAKE_C_COMPILER="%MINGW6_DIR%\bin\gcc.exe" ^
    -DCMAKE_CXX_COMPILER="%MINGW6_DIR%\bin\g++.exe" || goto :fail
cmake --build "%ROOT%\build\desktop-qt6" --target FuruiMusic -j %NUMBER_OF_PROCESSORS% || goto :fail
copy /y "%ROOT%\build\desktop-qt6\FuruiMusic.exe" "%ROOT%\bin_qt6\" >nul || goto :fail

rem windeployqt: --qmldir is REQUIRED. Without it QtQuick/Controls/Layouts/Window
rem QML plugins are NOT deployed and the app fails to load qrc:/qml/Main.qml.
rem Qt 6 also deploys plugins/multimedia/ffmpegmediaplugin.dll and avcodec-61.dll
rem etc. (MV decode depends on them); no need to copy OpenSSL 1.1 by hand anymore.
windeployqt --no-translations --qmldir "%SRC%\ui\qml" "%ROOT%\bin_qt6\FuruiMusic.exe" || goto :fail
echo [deploy] Qt 6 QML + multimedia (FFmpeg) plugins deployed into bin_qt6

rem MinGW Qt ships no QtWebView backend plugin; the CMake POST_BUILD step already
rem placed it and WebView2Loader.dll into build\desktop-qt6, copied next to the exe
rem so the in-app browser works.
if exist "%ROOT%\build\desktop-qt6\plugins\webview\qtwebview_webview2.dll" (
    if not exist "%ROOT%\bin_qt6\plugins\webview" mkdir "%ROOT%\bin_qt6\plugins\webview"
    copy /y "%ROOT%\build\desktop-qt6\plugins\webview\qtwebview_webview2.dll" "%ROOT%\bin_qt6\plugins\webview\" >nul || goto :fail
    copy /y "%ROOT%\build\desktop-qt6\WebView2Loader.dll" "%ROOT%\bin_qt6\" >nul || goto :fail
    echo [deploy] QtWebView WebView2 backend + loader deployed into bin_qt6
)

rem ---------- optional: Qt 5.15 compatibility build (self-built FFmpeg backend) ----------
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
rem Qt 5 windeployqt brings Qt5Core/Qt5Quick/Qt5QuickControls2 DLLs, qwindows and
rem QML modules; FFmpeg is statically linked into the exe, so no extra FFmpeg DLL
rem is needed at runtime.
windeployqt --no-translations --qmldir "%SRC%\ui\qml" "%ROOT%\bin_qt5\FuruiMusic.exe" || goto :fail
rem Qt 5.15.2 Windows binaries depend on OpenSSL 1.1, which the Qt installer does
rem not bundle: without these two DLLs every HTTPS request fails with
rem "TLS initialization failed" (all music sources appear dead).
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
