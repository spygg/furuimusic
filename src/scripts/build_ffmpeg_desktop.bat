@echo off
setlocal EnableExtensions
rem ==========================================================
rem  FuruiMusic - Qt5 FFmpeg playback backend (Windows wrapper)
rem  Runs the POSIX build script through Git Bash.
rem  Keep this file ASCII-only: cmd.exe parses .bat with the
rem  system ANSI codepage (GBK on zh-CN).
rem
rem  Env overrides:
rem    FURUI_MINGW_BIN   MinGW bin dir (default Qt 5.15.2 mingw810_64)
rem    FURUI_FFMPEG_JOBS parallel jobs (default 8)
rem    GIT_BASH          path to bash.exe
rem ==========================================================

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\..") do set "ROOT_DIR=%%~fI"

set "BASH_EXE=%GIT_BASH%"
if not defined BASH_EXE set "BASH_EXE=E:\programes\Git\bin\bash.exe"
if not exist "%BASH_EXE%" set "BASH_EXE=E:\programes\Git\usr\bin\bash.exe"
if not exist "%BASH_EXE%" (
    for /f "delims=" %%B in ('where bash 2^>nul') do set "BASH_EXE=%%B"
)
if not defined BASH_EXE (
    echo [ERROR] Git Bash not found. Set GIT_BASH to bash.exe.
    exit /b 1
)

echo [ffmpeg] Git Bash: %BASH_EXE%
echo [ffmpeg] Script  : %SCRIPT_DIR%build_ffmpeg_desktop.sh

"%BASH_EXE%" --login -c "'%SCRIPT_DIR%build_ffmpeg_desktop.sh' %*"
set "EXIT_CODE=%ERRORLEVEL%"
if not "%EXIT_CODE%"=="0" (
    echo [ERROR] FFmpeg build failed, exit %EXIT_CODE%
) else (
    echo [OK] FFmpeg ready: %ROOT_DIR%\build_output\ffmpeg-qt5
)
exit /b %EXIT_CODE%
