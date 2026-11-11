@echo off
rem FuruiMusic Android APK - Qt 6.11.1 + CMake (arm64-v8a, NDK 27, JDK 17)
setlocal
set "ROOT=%~dp0.."
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
cmake -S "%ROOT%" -B "%ROOT%\build\android_qt6" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH="%QT_ANDROID%" ^
    -DQT_HOST_PATH="%QT_HOST%" ^
    -DCMAKE_TOOLCHAIN_FILE="%ANDROID_NDK_ROOT%\build\cmake\android.toolchain.cmake" ^
    -DANDROID_ABI=arm64-v8a ^
    -DANDROID_PLATFORM=android-23 ^
    -DANDROID_SDK_ROOT="%ANDROID_SDK_ROOT%" ^
    -DANDROID_NDK_ROOT="%ANDROID_NDK_ROOT%" || goto :fail
echo [cmake] building APK
cmake --build "%ROOT%\build\android_qt6" --target FuruiMusic_make_apk -j %NUMBER_OF_PROCESSORS% || goto :fail
echo.
echo APK output under: %ROOT%\build\android_qt6\android-build\build\outputs\apk\
goto :eof
:fail
echo Android Qt6 build FAILED.
exit /b 1
