@echo off
rem FuruiMusic desktop build (incremental, never cleans build output)
setlocal
set "ROOT=%~dp0.."
set "QT_DIR=E:\programes\Qt\Qt\5.15.2\mingw81_64"
set "MINGW_DIR=E:\programes\Qt\Qt\Tools\mingw810_64"
set "CMAKE_DIR=E:\programes\Qt\Qt\Tools\CMake_64\bin"
set "NINJA_DIR=E:\programes\Qt\Qt\Tools\Ninja"
set "PATH=%QT_DIR%\bin;%MINGW_DIR%\bin;%CMAKE_DIR%;%NINJA_DIR%;%PATH%"

if not exist "%ROOT%\build\desktop" mkdir "%ROOT%\build\desktop"
cd /d "%ROOT%\build\desktop"
echo [qmake] generating unified QML Makefile
qmake "%ROOT%\FuruiMusic.pro" -o Makefile || goto :fail
echo [make] building FuruiMusic (QML)
mingw32-make -f Makefile release -j%NUMBER_OF_PROCESSORS% || goto :fail

rem Build unified QML frontend with CMake
echo [cmake] configuring FuruiMusic
cmake -S "%ROOT%" -B "%ROOT%\build\cmake" -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH="%QT_DIR%" ^
    -DCMAKE_C_COMPILER="%MINGW_DIR%\bin\gcc.exe" ^
    -DCMAKE_CXX_COMPILER="%MINGW_DIR%\bin\g++.exe" || goto :fail
cmake --build "%ROOT%\build\cmake" --target FuruiMusic -j %NUMBER_OF_PROCESSORS% || goto :fail

rem Qt 5.15.2 mingw kit does not ship OpenSSL DLLs; HTTPS (login/account API) needs them next to the exe
set "SSL_DIR=E:\programes\Qt\Qt\Tools\mingw1310_64\opt\bin"
if exist "%SSL_DIR%\libssl-1_1-x64.dll" (
    copy /y "%SSL_DIR%\libssl-1_1-x64.dll" "%ROOT%\bin\" >nul
    copy /y "%SSL_DIR%\libcrypto-1_1-x64.dll" "%ROOT%\bin\" >nul
    copy /y "%SSL_DIR%\libssl-1_1-x64.dll" "%ROOT%\build\cmake\" >nul
    copy /y "%SSL_DIR%\libcrypto-1_1-x64.dll" "%ROOT%\build\cmake\" >nul
    echo [deploy] OpenSSL 1.1 DLLs copied to bin and build\cmake
) else (
    echo [deploy] WARNING: OpenSSL 1.1 DLLs not found; HTTPS requests will fail at runtime
)

echo.
echo Build finished. Desktop binary in bin and build\cmake.
echo For a redistributable folder run: windeployqt --release bin\FuruiMusic.exe
goto :eof
:fail
echo Build FAILED.
exit /b 1
