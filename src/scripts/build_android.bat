@echo off
rem FuruiMusic Android APK - Qt 5.15.2 android kit + qmake (arm64-v8a)
rem Gradle dependencies may need a network connection on first build.
setlocal
rem 禁止 MSYS/Git-Bash 把 Windows 路径 G:\ 转成 /G/，否则 qmake 生成的 INSTALL_ROOT
rem 与 qinstall 目标路径会变成 MSYS 风格，导致 .so 复制静默失败、最终 androiddeployqt
rem 报 "Cannot find application binary"。纯 cmd 下此变量无副作用。
set "MSYS_NO_PATHCONV=1"
set "ROOT=%~dp0..\.."
set "SRC=%ROOT%\src"
set "QT_ANDROID=E:\programes\Qt\Qt\5.15.2\android"
set "MINGW_DIR=E:\programes\Qt\Qt\Tools\mingw810_64"
set "ANDROID_SDK_ROOT=E:\programes\Android\SDK"
set "ANDROID_NDK_ROOT=E:\programes\Android\SDK\ndk\21.3.6528147"
set "ANDROID_NDK_HOST=windows-x86_64"
set "ANDROID_NDK_PLATFORM=android-21"
set "JAVA_HOME=E:\programes\Android\jdk11"
set "PATH=%QT_ANDROID%\bin;%MINGW_DIR%\bin;%JAVA_HOME%\bin;%ANDROID_SDK_ROOT%\platform-tools;%PATH%"

if not exist "%ROOT%\build\android_qt5" mkdir "%ROOT%\build\android_qt5"
cd /d "%ROOT%\build\android_qt5"
echo [qmake] generating Android Makefile
qmake "%SRC%\FuruiMusic.pro" -o Makefile "ANDROID_ABIS=arm64-v8a" || goto :fail
echo [make] compiling native library
mingw32-make -f Makefile -j%NUMBER_OF_PROCESSORS% || goto :fail
set "ANDROID_BUILD=%ROOT%\build\android_qt5\android-build"
echo [install] copy native lib into android-build (explicit Windows path)
rem 不走 Makefile 的 `make apk`：其 apk_install_target 里 INSTALL_ROOT 被写死成 MSYS
rem 风格 /G/...，qinstall 无法解析导致复制静默失败。这里直接用 Windows 绝对路径调用
rem qinstall，确保 libFuruiMusic_arm64-v8a.so 落到 android-build/libs/arm64-v8a/。
"%QT_ANDROID%\bin\qmake.exe" -install qinstall -exe "%ROOT%\bin\libFuruiMusic_arm64-v8a.so" "%ANDROID_BUILD%\libs\arm64-v8a\libFuruiMusic_arm64-v8a.so" || goto :fail
echo [androiddeployqt] packaging APK
"%QT_ANDROID%\bin\androiddeployqt.exe" --input "%ROOT%\build\android_qt5\android-FuruiMusic-deployment-settings.json" --output "%ANDROID_BUILD%" --apk "%ANDROID_BUILD%\FuruiMusic.apk" || goto :fail

echo.
echo APK output: %ANDROID_BUILD%\FuruiMusic.apk
goto :eof
:fail
echo Android build FAILED.
exit /b 1
