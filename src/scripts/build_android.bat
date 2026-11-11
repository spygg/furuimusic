@echo off
rem FuruiMusic Android APK - Qt 5.15.2 android kit + qmake (arm64-v8a)
rem Gradle dependencies may need a network connection on first build.
setlocal
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
echo [androiddeployqt] packaging APK
mingw32-make -f Makefile apk || goto :fail

echo.
echo APK output: %ROOT%\build\android_qt5\android-build\build\outputs\apk\debug\
goto :eof
:fail
echo Android build FAILED.
exit /b 1
