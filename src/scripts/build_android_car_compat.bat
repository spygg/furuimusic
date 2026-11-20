@echo off
rem FuruiMusic CAR-COMPAT TEST APK - Qt 6.11.x + CMake (arm64-v8a, NDK 27, JDK 17)
rem
rem FOR TESTING ONLY. This variant impersonates com.netease.cloudmusic (BYD voice
rem whitelist) and contains the Byd* voice integration classes. It must NOT be
rem mixed with the mainstream release:
rem   - builds in build\android_qt6_carcompat (separate gradle project / caches)
rem   - signed APK lands in bin_android_carcompat\
rem   - mainstream build remains src\scripts\build_android_qt6.bat -> bin_android\
rem
rem Installing this over the mainstream APK (or vice versa) is impossible by
rem design: different applicationId. On the car, uninstall the genuine NetEase
rem app first (same package name, different signature).
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
for %%R in ("E:\programes\Qt\Qt" "D:\software\Qt" "C:\Qt") do (
    if not defined QTV (
        for %%V in (6.11.3 6.11.1 6.11.2 6.10.0) do (
            if not defined QTV if exist "%%~R\%%V\android_arm64_v8a\bin\qmake.bat" (
                set "QTROOT=%%~R"
                set "QTV=%%V"
            )
        )
    )
)
if not defined QTV (
    echo [error] Qt for Android arm64-v8a not found under the known roots.
    echo         Edit the probe list at the top of this script.
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

if not exist "%ROOT%\build\android_qt6_carcompat" mkdir "%ROOT%\build\android_qt6_carcompat"
echo [cmake] configuring CAR-COMPAT Android build
cmake -S "%SRC%" -B "%ROOT%\build\android_qt6_carcompat" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DFURUI_ANDROID_CAR_COMPAT=ON ^
    -DCMAKE_PREFIX_PATH="%QT_ANDROID%" ^
    -DQT_HOST_PATH="%QT_HOST%" ^
    -DCMAKE_TOOLCHAIN_FILE="%ANDROID_NDK_ROOT%\build\cmake\android.toolchain.cmake" ^
    -DANDROID_ABI=arm64-v8a ^
    -DANDROID_PLATFORM=android-23 ^
    -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH ^
    -DANDROID_SDK_ROOT="%ANDROID_SDK_ROOT%" ^
    -DANDROID_NDK_ROOT="%ANDROID_NDK_ROOT%" || goto :fail
echo [cmake] building CAR-COMPAT APK
cmake --build "%ROOT%\build\android_qt6_carcompat" --target FuruiMusic_make_apk -j %NUMBER_OF_PROCESSORS% || goto :fail
rem The gradle daemon inherits the build pipe's stdout handle, so an external
rem `| tail` never sees EOF and looks "stuck" (the APK is actually produced).
rem Stop the daemon right after the build to release the handle.
echo [gradle] stopping daemon to release pipe handles
pushd "%ROOT%\build\android_qt6_carcompat\android-build" && (call gradlew.bat --stop >nul 2>&1 & popd)
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
rem Same fixed signing identity as the mainstream build (base64 in src/platform/android-qt6).
set "KS=%ROOT%\build\android_qt6_carcompat\furui-release.keystore"
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
set "UNSIGNED=%ROOT%\build\android_qt6_carcompat\android-build\build\outputs\apk\release\android-build-release-unsigned.apk"
set "ALIGNED=%ROOT%\build\android_qt6_carcompat\android-build\FuruiMusic-carcompat-aligned.apk"
set "OUT=%ROOT%\bin_android_carcompat\FuruiMusic-carcompat-netease-arm64-v8a.apk"
if not exist "%UNSIGNED%" (
    echo [error] Qt-produced unsigned release APK not found: %UNSIGNED%
    goto :fail
)
if not exist "%ROOT%\bin_android_carcompat" mkdir "%ROOT%\bin_android_carcompat"
"%BT%\zipalign.exe" -f -p 4 "%UNSIGNED%" "%ALIGNED%" || goto :fail
call "%BT%\apksigner.bat" sign --ks "%KS%" --ks-key-alias "%KSALIAS%" --ks-pass pass:android --key-pass pass:android --v1-signing-enabled true --v2-signing-enabled true --out "%OUT%" "%ALIGNED%" || goto :fail
call "%BT%\apksigner.bat" verify "%OUT%" || goto :fail
echo.
echo CAR-COMPAT APK (signed / installable): %OUT%
goto :eof
:fail
echo CAR-COMPAT Android Qt6 build FAILED.
exit /b 1
