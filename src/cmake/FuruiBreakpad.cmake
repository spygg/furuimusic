# 福瑞音乐 - Google Breakpad 崩溃转储接入
#
# CrashHandler（thirdparty/breakpad/crashhandler.{h,cpp}）已按 Q_OS 自行门控：
#   Windows 桌面（MinGW / MSVC） -> 编译 client/windows
#   Linux 桌面 x86_64            -> 编译 client/linux（非 Android）
# 以下平台自动跳过（CrashHandler 退化为空操作，且不编译 client 源码）：
#   Android  -> Google Breakpad 在 Android 上需 NDK 单独集成，本项目暂不接入
#   macOS    -> breakpad 无 client 源码
#   其它 Linux 架构（aarch64 等）-> breakpad_getcontext.S 未覆盖，避免汇编不兼容
#
# 接入流程：
#   1) 本文件在 find_package(Qt...) 之后被 include，调用 furui_configure_breakpad()
#   2) 它产出 FuruiBreakpad 静态库，并定义缓存变量 FURUI_HAVE_BREAKPAD（ON/OFF）
#   3) CMakeLists 在构建 FuruiMusic 时按 FURUI_HAVE_BREAKPAD 链接并定义宏
#
# 崩溃转储落点：<应用数据目录>/crashes（见 src/ui/main.cpp 的 Init 调用）
# 注：minidump 是二进制转储，需配合 dump_syms + minidump_stackwalk 离线符号化，
#     该步骤在 CI / 开发者本机独立进行，不进应用。

option(FURUI_BREAKPAD "启用 Google Breakpad 崩溃转储" ON)

# 注意：必须是 function 而非 macro。macro 内的 return() 会在「顶层调用处」
# 直接终止整个 CMakeLists.txt（Android/macOS 分支会 return），导致 FuruiMusic
# 目标与 _make_apk 永不生成。function 的 return() 只结束自身，安全。
function(furui_configure_breakpad)
    set(FURUI_HAVE_BREAKPAD OFF PARENT_SCOPE)
    if(NOT FURUI_BREAKPAD)
        message(STATUS "[breakpad] 已通过 FURUI_BREAKPAD=OFF 关闭")
        return()
    endif()

    if(ANDROID OR APPLE)
        message(STATUS "[breakpad] 平台不支持（Android/macOS），跳过崩溃转储接入")
        return()
    endif()

    if(UNIX AND NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        message(STATUS "[breakpad] Linux 非 x86_64（${CMAKE_SYSTEM_PROCESSOR}），跳过以避免汇编不兼容")
        return()
    endif()

    set(_bp_root "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/breakpad")
    if(NOT EXISTS "${_bp_root}/src/client")
        message(WARNING "[breakpad] 源码缺失（${_bp_root}），跳过崩溃转储接入")
        return()
    endif()

    set(_bp_sources "${_bp_root}/crashhandler.cpp")

    if(WIN32)
        list(APPEND _bp_sources
            ${_bp_root}/src/client/windows/handler/exception_handler.cc
            ${_bp_root}/src/common/windows/string_utils.cc
            ${_bp_root}/src/common/windows/guid_string.cc
            ${_bp_root}/src/client/windows/crash_generation/crash_generation_client.cc)
    elseif(UNIX)
        list(APPEND _bp_sources
            ${_bp_root}/src/client/linux/handler/exception_handler.cc
            ${_bp_root}/src/client/linux/handler/minidump_descriptor.cc
            ${_bp_root}/src/client/linux/crash_generation/crash_generation_client.cc
            ${_bp_root}/src/client/linux/minidump_writer/minidump_writer.cc
            ${_bp_root}/src/client/linux/minidump_writer/linux_dumper.cc
            ${_bp_root}/src/client/linux/minidump_writer/linux_ptrace_dumper.cc
            ${_bp_root}/src/client/linux/log/log.cc
            ${_bp_root}/src/client/minidump_file_writer.cc
            ${_bp_root}/src/common/linux/linux_libc_support.cc
            ${_bp_root}/src/common/linux/file_id.cc
            ${_bp_root}/src/common/linux/memory_mapped_file.cc
            ${_bp_root}/src/common/linux/safe_readlink.cc
            ${_bp_root}/src/common/linux/guid_creator.cc
            ${_bp_root}/src/common/linux/elfutils.cc
            ${_bp_root}/src/common/string_conversion.cc
            ${_bp_root}/src/common/convert_UTF.cc
            ${_bp_root}/src/client/linux/microdump_writer/microdump_writer.cc
            ${_bp_root}/src/client/linux/dump_writer_common/ucontext_reader.cc
            ${_bp_root}/src/client/linux/dump_writer_common/thread_info.cc
            ${_bp_root}/src/common/linux/breakpad_getcontext.S)
    endif()

    add_library(FuruiBreakpad STATIC ${_bp_sources})
    target_include_directories(FuruiBreakpad PUBLIC
        ${_bp_root} ${_bp_root}/src ${_bp_root}/src/google_breakpad)
    target_link_libraries(FuruiBreakpad PUBLIC ${QT}::Core)
    # 第三方代码：关掉所有告警（仅静默，不 -Werror），避免污染构建输出
    if(MSVC)
        target_compile_options(FuruiBreakpad PRIVATE /w)
    else()
        target_compile_options(FuruiBreakpad PRIVATE -w)
    endif()
    if(WIN32)
        target_link_libraries(FuruiBreakpad PUBLIC dbghelp version)
    elseif(UNIX)
        # 可符号化的 minidump 需要二进制带调试段；只给 breakpad 自身加 -g，
        # 不波及整个可执行文件的体积（应用本体 release 仍按默认优化）。
        target_compile_options(FuruiBreakpad PRIVATE -g)
        find_package(Threads QUIET)
        if(Threads_FOUND)
            target_link_libraries(FuruiBreakpad PUBLIC Threads::Threads)
        endif()
    endif()
    set(FURUI_HAVE_BREAKPAD ON PARENT_SCOPE)
    message(STATUS "[breakpad] 已接入崩溃转储（${QT}::Core + 平台 client，落点见 main.cpp）")
endfunction()
