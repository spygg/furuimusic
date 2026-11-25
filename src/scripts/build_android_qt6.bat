@echo off
rem FuruiMusic Android APK - Qt 6.11.x + CMake (arm64-v8a, NDK 27, JDK 17)
rem The final signed APK lands in bin_android/ at the repo root, separate from the
rem desktop bin_qt6 / bin_qt5.
setlocal
set "ROOT=%~dp0..\.."
set "SRC=%ROOT%\src"
rem ---------- toolchain discovery ----------
rem Qt and the Android SDK live under different roots on different machines
rem (E:\programes on the original workstation, D:\software elsewhere). Probe the known
rem roots instead of hard-coding one, so the script runs unchanged on every build box.
rem Everything below is plain ASCII on purpose: cmd.exe parses .bat files in byte
rem chunks and non-ASCII bytes desynchronise the parser.
set "QTROOT="
set "QTV="
rem QT_ROOT_DIR / QT_DIR override the probe roots (Qt root or a version kit dir,
rem e.g. what install-qt-action exports). Also accept the CI version 6.8.3.
set "QT_PROBE_ROOT="
if defined QT_ROOT_DIR set "QT_PROBE_ROOT=%QT_ROOT_DIR%"
if not defined QT_PROBE_ROOT if defined QT_DIR set "QT_PROBE_ROOT=%QT_DIR%"
if defined QT_PROBE_ROOT if exist "%QT_PROBE_ROOT%\bin\qmake.bat" (
    for %%R in ("%QT_PROBE_ROOT%\..\..") do set "QT_PROBE_ROOT=%%~fR"
)
for %%R in ("%QT_PROBE_ROOT%" "E:\programes\Qt\Qt" "D:\software\Qt" "C:\Qt") do (
    if not defined QTV (
        for %%V in (6.11.3 6.11.1 6.11.2 6.10.0 6.8.3) do (
            if not defined QTV if exist "%%~R\%%V\android_arm64_v8a\bin\qmake.bat" (
                set "QTROOT=%%~R"
                set "QTV=%%V"
            )
        )
    )
)
if not defined QTV (
    echo [error] Qt for Android arm64-v8a not found. Set QT_ROOT_DIR / QT_DIR to
    echo         the Qt root, parent of the 6.x version dirs, and retry.
    goto :fail
)
set "QT_ANDROID=%QTROOT%\%QTV%\android_arm64_v8a"
set "QT_HOST=%QTROOT%\%QTV%\mingw_64"
set "CMAKE_DIR=%QTROOT%\Tools\CMake_64\bin"
set "NINJA_DIR=%QTROOT%\Tools\Ninja"

set "ANDROID_SDK_ROOT="
for %%R in ("E:\programes\Android\SDK" "D:\software\Android\SDK" "C:\Android\Sdk") do (
    if not defined ANDROID_SDK_ROOT if exist "%%~R\platform-tools\adb.exe" set "ANDROID_SDK_ROOT=%%~R"
)
if not defined ANDROID_SDK_ROOT (
    echo [error] Android SDK not found under the known roots.
    goto :fail
)

rem NDK 27 keeps the same clang/CMake layout across minor revisions; take the newest
rem one that is actually installed instead of pinning an exact revision.
set "ANDROID_NDK_ROOT="
for %%V in (27.2.12479018 27.1.12297006 27.0.12077973) do (
    if not defined ANDROID_NDK_ROOT if exist "%ANDROID_SDK_ROOT%\ndk\%%V\build\cmake\android.toolchain.cmake" set "ANDROID_NDK_ROOT=%ANDROID_SDK_ROOT%\ndk\%%V"
)
if not defined ANDROID_NDK_ROOT (
    for /d %%D in ("%ANDROID_SDK_ROOT%\ndk\27*") do set "ANDROID_NDK_ROOT=%%~fD"
)
if not defined ANDROID_NDK_ROOT (
    echo [error] Android NDK 27.x not found under %ANDROID_SDK_ROOT%\ndk.
    goto :fail
)

set "FOUND_JDK="
for %%R in ("E:\programes\Android" "D:\software\Android" "C:\Program Files\Java") do (
    if not defined FOUND_JDK (
        for /d %%J in ("%%~R\jdk-17*") do set "FOUND_JDK=%%~fJ"
    )
)
if defined FOUND_JDK (
    set "JAVA_HOME=%FOUND_JDK%"
) else (
    echo [warn] JDK 17 not found under the known roots; using the inherited JAVA_HOME.
)
set "PATH=%QT_HOST%\bin;%CMAKE_DIR%;%NINJA_DIR%;%JAVA_HOME%\bin;%ANDROID_SDK_ROOT%\platform-tools;%PATH%"

if not exist "%ROOT%\build\android_qt6" mkdir "%ROOT%\build\android_qt6"
echo [cmake] configuring Android build
cmake -S "%SRC%" -B "%ROOT%\build\android_qt6" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH="%QT_ANDROID%" ^
    -DQT_HOST_PATH="%QT_HOST%" ^
    -DCMAKE_TOOLCHAIN_FILE="%ANDROID_NDK_ROOT%\build\cmake\android.toolchain.cmake" ^
    -DANDROID_ABI=arm64-v8a ^
    -DANDROID_PLATFORM=android-23 ^
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH ^
    -DANDROID_SDK_ROOT="%ANDROID_SDK_ROOT%" ^
    -DANDROID_NDK_ROOT="%ANDROID_NDK_ROOT%" || goto :fail
echo [cmake] building APK
cmake --build "%ROOT%\build\android_qt6" --target FuruiMusic_make_apk -j %NUMBER_OF_PROCESSORS% || goto :fail
rem The gradle daemon inherits the build pipe's stdout handle, so an external
rem `| tail` never sees EOF and looks "stuck" (the APK is actually produced).
rem Stop the daemon right after the build to release the handle.
echo [gradle] stopping daemon to release pipe handles
pushd "%ROOT%\build\android_qt6\android-build" && (call gradlew.bat --stop >nul 2>&1 & popd)
echo.
echo [sign] zipalign + apksigner (debug keystore)
rem Newest installed build-tools that actually ships apksigner (first hit wins).
set "BT="
for %%V in (37.0.0 36.0.0 35.0.0 34.0.0 33.0.1 31.0.0 30.0.3) do (
    if not defined BT if exist "%ANDROID_SDK_ROOT%\build-tools\%%V\apksigner.bat" set "BT=%ANDROID_SDK_ROOT%\build-tools\%%V"
)
if not defined BT (
    echo [error] Android build-tools with apksigner not found.
    goto :fail
)
rem Fixed signing identity shared with CI (base64 in src/platform/android-qt6).
rem Generating a fresh random keystore per build makes in-app updates fail with
rem "same signature already installed" / signature mismatch on the device.
set "KS=%ROOT%\build\android_qt6\furui-release.keystore"
if not exist "%KS%" (
    echo [sign] restoring fixed debug keystore from repository
    powershell -NoProfile -Command "[IO.File]::WriteAllBytes('%KS%', [Convert]::FromBase64String([IO.File]::ReadAllText('%SRC%\platform\android-qt6\furui-debug.keystore.b64')))"
)
if not exist "%KS%" (
    echo [error] failed to restore signing keystore
    goto :fail
)
set "KSALIAS=androiddebugkey"
:signAPK
set "UNSIGNED=%ROOT%\build\android_qt6\android-build\build\outputs\apk\release\android-build-release-unsigned.apk"
set "ALIGNED=%ROOT%\build\android_qt6\android-build\FuruiMusic-aligned.apk"
set "OUT=%ROOT%\bin_android\FuruiMusic-android-arm64-v8a.apk"
if not exist "%UNSIGNED%" (
    echo [error] Qt-produced unsigned release APK not found: %UNSIGNED%
    goto :fail
)
if not exist "%ROOT%\bin_android" mkdir "%ROOT%\bin_android"
"%BT%\zipalign.exe" -f -p 4 "%UNSIGNED%" "%ALIGNED%" || goto :fail
call "%BT%\apksigner.bat" sign --ks "%KS%" --ks-key-alias "%KSALIAS%" --ks-pass pass:android --key-pass pass:android --v1-signing-enabled true --v2-signing-enabled true --out "%OUT%" "%ALIGNED%" || goto :fail
call "%BT%\apksigner.bat" verify "%OUT%" || goto :fail
echo.
echo APK (signed / installable): %OUT%
echo unsigned intermediate    : %UNSIGNED%
goto :eof
:fail
echo Android Qt6 build FAILED.
exit /b 1
