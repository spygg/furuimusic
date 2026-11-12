@echo off
rem FuruiMusic Android APK - Qt 6.11.1 + CMake (arm64-v8a, NDK 27, JDK 17)
rem 最终签名 APK 落在仓库根的 bin_android/，与桌面版 bin_qt6 / bin_qt5 分开。
setlocal
set "ROOT=%~dp0..\.."
set "SRC=%ROOT%\src"
set "QT_ANDROID=E:\programes\Qt\Qt\6.11.1\android_arm64_v8a"
set "QT_HOST=E:\programes\Qt\Qt\6.11.1\mingw_64"
set "CMAKE_DIR=E:\programes\Qt\Qt\Tools\CMake_64\bin"
set "NINJA_DIR=E:\programes\Qt\Qt\Tools\Ninja"
set "ANDROID_SDK_ROOT=E:\programes\Android\SDK"
set "ANDROID_NDK_ROOT=E:\programes\Android\SDK\ndk\27.1.12297006"
set "JAVA_HOME=E:\programes\Android\jdk-17.0.17.10-hotspot"
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
rem gradle 守护进程会继承构建管道的 stdout 句柄，导致外部 `| tail` 等不到 EOF
rem 表现为"构建卡死"（APK 其实已产出）。构建完立即 --stop 释放句柄。
echo [gradle] stopping daemon to release pipe handles
pushd "%ROOT%\build\android_qt6\android-build" && (call gradlew.bat --stop >nul 2>&1 & popd)
echo.
echo [sign] zipalign + apksigner (debug keystore)
set "BT=%ANDROID_SDK_ROOT%\build-tools\35.0.0"
if not exist "%BT%\apksigner.bat" set "BT=%ANDROID_SDK_ROOT%\build-tools\37.0.0"
set "KS=%USERPROFILE%\.android\debug.keystore"
if exist "%KS%" goto :haveKS
echo [sign] 生成临时 debug keystore
if not exist "%ROOT%\build\android_qt6" mkdir "%ROOT%\build\android_qt6"
keytool -genkeypair -v -keystore "%ROOT%\build\android_qt6\furui-debug.keystore" -alias furui -keyalg RSA -keysize 2048 -validity 10000 -storepass android -keypass android -dname "CN=FuruiMusic Debug, OU=Local, O=Furui, C=CN" >nul || goto :fail
set "KS=%ROOT%\build\android_qt6\furui-debug.keystore"
set "KSALIAS=furui"
goto :signAPK
:haveKS
set "KSALIAS=androiddebugkey"
:signAPK
set "UNSIGNED=%ROOT%\build\android_qt6\android-build\build\outputs\apk\release\android-build-release-unsigned.apk"
set "ALIGNED=%ROOT%\build\android_qt6\android-build\FuruiMusic-aligned.apk"
set "OUT=%ROOT%\bin_android\FuruiMusic-android-arm64-v8a.apk"
if not exist "%UNSIGNED%" (
    echo [error] 未找到 Qt 产出的未签名 release APK: %UNSIGNED%
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
