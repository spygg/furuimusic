@echo off
rem FuruiMusic Android APK - Qt 5.15.2 android kit + qmake (arm64-v8a)
rem Gradle dependencies may need a network connection on first build.
setlocal
rem Prevent MSYS/Git-Bash from rewriting Windows paths like G:\ into /G/, which would
rem make qmake's INSTALL_ROOT and qinstall target paths MSYS-style and silently fail
rem the .so copy, finally making androiddeployqt report "Cannot find application
rem binary". This env var is a no-op under plain cmd.
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
rem Do NOT use Makefile's `make apk`: its apk_install_target hardcodes INSTALL_ROOT as
rem MSYS-style /G/..., which qinstall cannot resolve and silently fails the copy. Here
rem we call qinstall with an explicit Windows absolute path so
rem libFuruiMusic_arm64-v8a.so lands in android-build/libs/arm64-v8a/.
"%QT_ANDROID%\bin\qmake.exe" -install qinstall -exe "%ROOT%\bin\libFuruiMusic_arm64-v8a.so" "%ANDROID_BUILD%\libs\arm64-v8a\libFuruiMusic_arm64-v8a.so" || goto :fail
echo [androiddeployqt] packaging APK
"%QT_ANDROID%\bin\androiddeployqt.exe" --input "%ROOT%\build\android_qt5\android-FuruiMusic-deployment-settings.json" --output "%ANDROID_BUILD%" --apk "%ANDROID_BUILD%\FuruiMusic.apk" || goto :fail

echo.
echo APK output: %ANDROID_BUILD%\FuruiMusic.apk
goto :eof
:fail
echo Android build FAILED.
exit /b 1
