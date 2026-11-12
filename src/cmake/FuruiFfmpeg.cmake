# 福瑞音乐 - Qt 5 桌面「自编译 FFmpeg 播放后端」探测与自动编译
#
# 由 src/CMakeLists.txt 在 find_package(Qt) 之后 include，只处理 Qt 5 桌面：
#   * configure 阶段调用 src/scripts/build_ffmpeg_desktop.sh 编出静态库
#     （链接前库必须已存在；脚本自己增量判断，第二次 configure 几乎零开销）
#   * 按编译器 target / 版本分产物目录，杜绝把 x64 的 .a 链进 x86 的 exe
#   * 任何失败都只降级：Qt 5 依旧能构建，MV 退回 DirectShow 提示
#
# 对外（调用方作用域）变量：
#   FURUI_HAVE_FFMPEG / FURUI_FFMPEG_INCLUDE_DIR
#   FURUI_FFMPEG_SOURCES / FURUI_FFMPEG_LINK_LIBS
#
# 开关：
#   -DFURUI_AUTO_BUILD_FFMPEG=OFF      只探测、不编译（离线 / 已有产物）
#   -DFURUI_FFMPEG_ROOT=/path/prefix   显式指定产物前缀（跳过 ABI 分目录）
#   -DFURUI_FFMPEG_JOBS=16             编译并行度
#   -DFURUI_FFMPEG_RECONFIGURE=ON      强制重跑 FFmpeg configure（不删产物）

include_guard(GLOBAL)

option(FURUI_AUTO_BUILD_FFMPEG "Qt 5 构建时自动编译 FFmpeg 播放后端" ON)
option(FURUI_FFMPEG_RECONFIGURE "强制重跑 FFmpeg configure（不删除已有产物）" OFF)
set(FURUI_FFMPEG_VERSION "6.1.2" CACHE STRING
    "FFmpeg 源码版本，对应 src/thirdparty/ffmpeg/FFmpeg-n<version>")
include(ProcessorCount)
ProcessorCount(_ff_cpu)
if(NOT _ff_cpu GREATER 0)
    set(_ff_cpu 8)
endif()
set(FURUI_FFMPEG_JOBS "${_ff_cpu}" CACHE STRING "FFmpeg 编译并行度")
set(FURUI_FFMPEG_ROOT "" CACHE PATH
    "Qt5 FFmpeg 安装前缀（留空 = 按工具链自动选择；目录下应有 include/lib）")

set(FURUI_HAVE_FFMPEG OFF)
set(FURUI_FFMPEG_SOURCES "")
set(FURUI_FFMPEG_LINK_LIBS "")
set(FURUI_FFMPEG_INCLUDE_DIR "")

# Qt 6 用 QtMultimedia 官方 FFmpeg 插件；Android 的 Qt 5 走 qmake，也不能链桌面 .a
if(QT_VERSION_MAJOR GREATER_EQUAL 6 OR ANDROID)
    return()
endif()

get_filename_component(_ff_src_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(_ff_repo_root "${_ff_src_root}/.." ABSOLUTE)
file(TO_CMAKE_PATH "${_ff_src_root}" _ff_src_root)
file(TO_CMAKE_PATH "${_ff_repo_root}" _ff_repo_root)
set(_ff_source "${_ff_src_root}/thirdparty/ffmpeg/FFmpeg-n${FURUI_FFMPEG_VERSION}")
set(_ff_script "${_ff_src_root}/scripts/build_ffmpeg_desktop.sh")
set(_ff_legacy "${_ff_repo_root}/build_output/ffmpeg-qt5")

# ---- 工具链指纹 ---------------------------------------------------------
# 与脚本写在 <prefix>/.toolchain 的内容逐字比对。旧产物没有标记文件时
# 不敢复用（无法证明 ABI 一致），CMake 自动换到带 tag 的目录重新编译，
# 旧目录原样保留，绝不删除任何已有产物。
set(_ff_platform linux)
if(WIN32)
    set(_ff_platform windows)
elseif(APPLE)
    set(_ff_platform macos)
endif()

set(_ff_cc_target "")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
                    OUTPUT_VARIABLE _ff_cc_target
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET TIMEOUT 10)
endif()
set(_ff_stamp "target=${_ff_cc_target}\nversion=${CMAKE_C_COMPILER_VERSION}\nplatform=${_ff_platform}\nffmpeg=${FURUI_FFMPEG_VERSION}\n")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_ff_bits "x64")
else()
    set(_ff_bits "x86")
endif()
string(MAKE_C_IDENTIFIER "${CMAKE_C_COMPILER_ID}_${CMAKE_C_COMPILER_VERSION}" _ff_cc_tag)
set(_ff_tag "${_ff_bits}-${_ff_cc_tag}")
set(_ff_auto "${_ff_repo_root}/build_output/ffmpeg-qt5-${_ff_tag}")

set(_ff_prefix "")
if(FURUI_FFMPEG_ROOT)
    set(_ff_prefix "${FURUI_FFMPEG_ROOT}")
elseif(FURUI_AUTO_BUILD_FFMPEG)
    if(EXISTS "${_ff_legacy}/.toolchain")
        file(READ "${_ff_legacy}/.toolchain" _ff_old_stamp)
        string(REPLACE "\r\n" "\n" _ff_old_stamp "${_ff_old_stamp}")
        if(_ff_old_stamp STREQUAL _ff_stamp)
            set(_ff_prefix "${_ff_legacy}")
        endif()
    endif()
    if(NOT _ff_prefix)
        set(_ff_prefix "${_ff_auto}")
    endif()
else()
    set(_ff_prefix "${_ff_legacy}")
endif()

# ---- configure 阶段自动编译 ---------------------------------------------
if(FURUI_AUTO_BUILD_FFMPEG)
    if(MSVC)
        message(STATUS "Qt5 FFmpeg 自动编译：MSVC 不支持 FFmpeg configure，"
                       "可用 -DFURUI_FFMPEG_ROOT=<prefix> 指向手工编译的静态库")
    elseif(NOT EXISTS "${_ff_source}/configure")
        message(WARNING "FFmpeg 源码缺失：${_ff_source}/configure（请检出 src/thirdparty/ffmpeg），跳过自动编译")
    elseif(NOT EXISTS "${_ff_script}")
        message(WARNING "构建脚本缺失：${_ff_script}，跳过自动编译")
    else()
        # Windows 靠 Git Bash 提供 sh / perl / cygpath；Linux/macOS 用系统 sh
        find_program(FURUI_FFMPEG_SHELL NAMES sh bash
            HINTS "E:/programes/Git/usr/bin" "E:/programes/Git/bin"
                  "C:/Program Files/Git/usr/bin" "C:/Program Files/Git/bin"
                  /bin /usr/bin
            DOC "执行 build_ffmpeg_desktop.sh 的 POSIX shell")
        if(NOT FURUI_FFMPEG_SHELL)
            message(WARNING "未找到 sh/bash（Windows 需要 Git Bash），Qt5 MV 将退回 DirectShow")
        else()
            get_filename_component(_ff_cc_dir "${CMAKE_C_COMPILER}" DIRECTORY)
            file(TO_CMAKE_PATH "${CMAKE_C_COMPILER}" _ff_cc)
            file(TO_CMAKE_PATH "${_ff_cc_dir}" _ff_cc_dir)
            file(TO_CMAKE_PATH "${_ff_prefix}" _ff_prefix_cmake)
            file(TO_CMAKE_PATH "${_ff_source}" _ff_source_cmake)
            set(_ff_build "${CMAKE_BINARY_DIR}/ffmpeg-qt5-${_ff_tag}")
            # 指纹文件由 CMake 生成，脚本编译成功后拷到 <prefix>/.toolchain；
            # 下次 configure 比对一致才复用，避免链接到错误 ABI 的 .a。
            set(_ff_stamp_file "${CMAKE_BINARY_DIR}/ffmpeg-qt5-${_ff_tag}.toolchain")
            file(WRITE "${_ff_stamp_file}" "${_ff_stamp}")

            set(_ff_env
                "FURUI_FFMPEG_ROOT=${_ff_prefix_cmake}"
                "FURUI_FFMPEG_BUILD=${_ff_build}"
                "FURUI_FFMPEG_SRC=${_ff_source_cmake}"
                "FURUI_FFMPEG_VERSION=${FURUI_FFMPEG_VERSION}"
                "FURUI_FFMPEG_JOBS=${FURUI_FFMPEG_JOBS}"
                "FURUI_FFMPEG_CC=${_ff_cc}"
                "FURUI_MINGW_BIN=${_ff_cc_dir}"
                "FURUI_FFMPEG_STAMP_FILE=${_ff_stamp_file}")
            if(FURUI_FFMPEG_RECONFIGURE)
                list(APPEND _ff_env "FURUI_FFMPEG_RECONFIGURE=1")
            endif()

            # Git Bash 里把 G:/... 写成 /g/...，避免盘符/反斜杠被 sh -c 吞掉
            set(_ff_script_sh "${_ff_script}")
            if(WIN32 AND _ff_script_sh MATCHES "^([A-Za-z]):/(.*)$")
                string(TOLOWER "${CMAKE_MATCH_1}" _ff_drive)
                set(_ff_script_sh "/${_ff_drive}/${CMAKE_MATCH_2}")
            endif()

            if(WIN32)
                set(_ff_cmd "${CMAKE_COMMAND}" -E env ${_ff_env}
                    "${FURUI_FFMPEG_SHELL}" --login -c
                    "'${_ff_script_sh}' -j ${FURUI_FFMPEG_JOBS}")
            else()
                set(_ff_cmd "${CMAKE_COMMAND}" -E env ${_ff_env}
                    "${FURUI_FFMPEG_SHELL}" "${_ff_script_sh}"
                    -j "${FURUI_FFMPEG_JOBS}")
            endif()

            message(STATUS "Qt5 FFmpeg 后端：检查/编译 ${_ff_prefix_cmake}"
                           "（首次数分钟，之后增量跳过；日志在 ${_ff_build}）")
            execute_process(COMMAND ${_ff_cmd}
                            WORKING_DIRECTORY "${_ff_repo_root}"
                            RESULT_VARIABLE _ff_rc
                            OUTPUT_VARIABLE _ff_out
                            ERROR_VARIABLE _ff_err)
            if(NOT _ff_rc EQUAL 0)
                message(WARNING "FFmpeg 自动编译失败 (rc=${_ff_rc})，Qt5 MV 退回 DirectShow。\n"
                                "stdout: ${_ff_out}\nstderr: ${_ff_err}")
            else()
                message(STATUS "Qt5 FFmpeg 后端：构建脚本完成")
            endif()
        endif()
    endif()
endif()

# ---- 探测产物 -----------------------------------------------------------
# 直接用确定路径，避免 find_library 的 CACHE 在切换工具链后仍指向旧 .a。
set(_ff_ok TRUE)
set(_ff_libs "")
set(_ff_avutil "")
foreach(_l avformat avcodec swscale swresample avutil)
    set(_ff_lib "${_ff_prefix}/lib/lib${_l}.a")
    if(NOT EXISTS "${_ff_lib}")
        unset(FURUI_FF_LIB_${_l} CACHE)
        find_library(FURUI_FF_LIB_${_l} NAMES ${_l} lib${_l}
                     PATHS "${_ff_prefix}/lib" NO_DEFAULT_PATH)
        if(FURUI_FF_LIB_${_l})
            set(_ff_lib "${FURUI_FF_LIB_${_l}}")
        else()
            set(_ff_ok FALSE)
        endif()
    endif()
    if(NOT _ff_ok)
        break()
    endif()
    list(APPEND _ff_libs "${_ff_lib}")
    if(_l STREQUAL "avutil")
        set(_ff_avutil "${_ff_lib}")
    endif()
endforeach()
if(EXISTS "${_ff_prefix}/include/libavformat/avformat.h")
    set(FURUI_FFMPEG_INCLUDE_DIR "${_ff_prefix}/include")
else()
    set(_ff_ok FALSE)
endif()

if(_ff_ok)
    set(FURUI_HAVE_FFMPEG ON)
    set(FURUI_FFMPEG_SOURCES ui/FfmpegVideoPlayer.h ui/FfmpegVideoPlayer.cpp)
    # 静态库循环依赖：末尾再重复一次 avutil 兜底
    list(APPEND _ff_libs "${_ff_avutil}")
    if(WIN32)
        # SChannel TLS + 网络 + 数学/系统库（configure 用了 --disable-autodetect）
        list(APPEND _ff_libs ws2_32 secur32 crypt32 bcrypt m z psapi
             advapi32 shell32 ole32 user32 gdi32)
    elseif(APPLE)
        foreach(_fw CoreFoundation Security CoreVideo CoreMedia VideoToolbox AudioToolbox)
            find_library(FURUI_FW_${_fw} ${_fw})
            if(FURUI_FW_${_fw})
                list(APPEND _ff_libs "${FURUI_FW_${_fw}}")
            endif()
        endforeach()
        list(APPEND _ff_libs m z)
    else()
        list(APPEND _ff_libs m z pthread dl)
        find_package(OpenSSL QUIET)
        if(OPENSSL_FOUND)
            list(APPEND _ff_libs OpenSSL::SSL OpenSSL::Crypto)
        endif()
    endif()
    set(FURUI_FFMPEG_LINK_LIBS "${_ff_libs}")
    message(STATUS "Qt5 FFmpeg 播放后端：已启用 (${_ff_prefix})")
else()
    message(STATUS "Qt5 FFmpeg 播放后端：未找到 ${_ff_prefix}，MV 退回 DirectShow")
endif()
