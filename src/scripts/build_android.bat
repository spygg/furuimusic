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
rem ---------- toolchain discovery ----------
rem Caller overrides win: QT_ANDROID (full .../5.15.2/android kit), MINGW_DIR,
rem ANDROID_SDK_ROOT / ANDROID_HOME, ANDROID_NDK_ROOT / ANDROID_NDK, JAVA_HOME.
rem QT_ROOT_DIR / QT_DIR should be the Qt root (parent of 5.15.2); use QT_ANDROID for a kit dir.
rem Without overrides, probe the known roots instead of hard-coding one machine's paths.
rem Plain ASCII only: cmd.exe parses .bat files byte-wise and non-ASCII desynchronises it.
if not defined QT_ANDROID if defined QT_ROOT_DIR set "QT_ANDROID=%QT_ROOT_DIR%\5.15.2\android"
if not defined QT_ANDROID if defined QT_DIR set "QT_ANDROID=%QT_DIR%\5.15.2\android"
if defined QT_ANDROID if exist "%QT_ANDROID%\bin\qmake.exe" goto :have_qt
set "QT_ANDROID="
for %%R in ("E:\programes\Qt\Qt" "D:\software\Qt" "C:\Qt") do (
    if not defined QT_ANDROID if exist "%%~R\5.15.2\android\bin\qmake.exe" set "QT_ANDROID=%%~R\5.15.2\android"
)
if not defined QT_ANDROID (
    echo [error] Qt 5.15.2 Android kit not found. Set QT_ANDROID to the .../5.15.2/android directory.
    goto :fail
)
:have_qt

if not defined MINGW_DIR if defined QT_ROOT_DIR set "MINGW_DIR=%QT_ROOT_DIR%\Tools\mingw810_64"
if not defined MINGW_DIR if defined QT_DIR set "MINGW_DIR=%QT_DIR%\Tools\mingw810_64"
if defined MINGW_DIR if exist "%MINGW_DIR%\bin\mingw32-make.exe" goto :have_mingw
set "MINGW_DIR="
for %%R in ("E:\programes\Qt\Qt" "D:\software\Qt" "C:\Qt") do (
    if not defined MINGW_DIR if exist "%%~R\Tools\mingw810_64\bin\mingw32-make.exe" set "MINGW_DIR=%%~R\Tools\mingw810_64"
)
if not defined MINGW_DIR (
    echo [error] MinGW 8.1 not found. Set MINGW_DIR to the Qt Tools/mingw810_64 directory.
    goto :fail
)
:have_mingw

if not defined ANDROID_SDK_ROOT if defined ANDROID_HOME set "ANDROID_SDK_ROOT=%ANDROID_HOME%"
if not defined ANDROID_SDK_ROOT (
    for %%R in ("E:\programes\Android\SDK" "D:\software\Android\SDK" "C:\Android\Sdk") do (
        if not defined ANDROID_SDK_ROOT if exist "%%~R\platform-tools\adb.exe" set "ANDROID_SDK_ROOT=%%~R"
    )
)
if not defined ANDROID_SDK_ROOT (
    echo [error] Android SDK not found. Set ANDROID_SDK_ROOT to the SDK directory.
    goto :fail
)
if not defined ANDROID_NDK_ROOT if defined ANDROID_NDK set "ANDROID_NDK_ROOT=%ANDROID_NDK%"
if not defined ANDROID_NDK_ROOT if exist "%ANDROID_SDK_ROOT%\ndk\21.3.6528147\build\cmake\android.toolchain.cmake" set "ANDROID_NDK_ROOT=%ANDROID_SDK_ROOT%\ndk\21.3.6528147"
if not defined ANDROID_NDK_ROOT (
    for /d %%D in ("%ANDROID_SDK_ROOT%\ndk\21*") do set "ANDROID_NDK_ROOT=%%~fD"
)
if not defined ANDROID_NDK_ROOT (
    echo [error] Android NDK 21.x not found under %ANDROID_SDK_ROOT%\ndk. Set ANDROID_NDK_ROOT.
    goto :fail
)
if not defined ANDROID_NDK_HOST set "ANDROID_NDK_HOST=windows-x86_64"
if not defined ANDROID_NDK_PLATFORM set "ANDROID_NDK_PLATFORM=android-21"

if defined JAVA_HOME if exist "%JAVA_HOME%\bin\java.exe" goto :have_java
set "FOUND_JDK="
for %%R in ("E:\programes\Android" "D:\software\Android" "C:\Program Files\Java") do (
    if not defined FOUND_JDK for /d %%J in ("%%~R\jdk-11*" "%%~R\jdk11*") do set "FOUND_JDK=%%~fJ"
)
if defined FOUND_JDK (
    set "JAVA_HOME=%FOUND_JDK%"
) else (
    echo [error] JDK 11 not found. Set JAVA_HOME to a JDK 11 installation.
    goto :fail
)
:have_java
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
