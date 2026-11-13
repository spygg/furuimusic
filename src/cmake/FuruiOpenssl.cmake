# 福瑞音乐 - Windows Qt 5 的 OpenSSL 1.1 运行库部署
#
# Qt 5.15.2 的官方 Windows 二进制基于 OpenSSL 1.1（不是 3.x），而 Qt 安装包里
# 不带这套 DLL。缺了它 QtNetwork 的所有 HTTPS 请求直接报
# "TLS initialization failed"，表现为「音源全部取链失败」，很容易被误判成接口挂了。
#
# 因此把两个未修改的官方 DLL 随仓库分发在 src/thirdparty/openssl1.1/，本脚本负责
# 把它们放到 exe 同目录（只对 Qt 5 桌面 Windows 生效）：
#   * CMake 构建：POST_BUILD 自动 copy_if_different，增量构建不会重复写盘
#   * Qt 6 用 Schannel / OpenSSL 3，不需要 1.1，直接跳过
#   * 32 位构建跳过（随仓库只有 x64 版本），只给出提示，不阻断构建
#
# 对外（调用方作用域）变量：
#   FURUI_OPENSSL_RUNTIME_DIR   DLL 所在目录（打包脚本也可直接引用）
#   FURUI_OPENSSL_DLLS          本次构建实际要部署的 DLL 绝对路径列表
#   FURUI_OPENSSL_AVAILABLE     是否找到可用的运行库
#
# 开关：
#   -DFURUI_DEPLOY_OPENSSL=OFF       不自动拷贝（自备 OpenSSL 时用）
#   -DFURUI_OPENSSL_RUNTIME_DIR=...  指向别的目录（同样按 libssl-1_1*/libcrypto-1_1* 匹配）

include_guard(GLOBAL)

option(FURUI_DEPLOY_OPENSSL "Qt 5 Windows 构建时把 OpenSSL 1.1 运行库拷到 exe 旁" ON)
set(FURUI_OPENSSL_RUNTIME_DIR "${CMAKE_CURRENT_LIST_DIR}/../thirdparty/openssl1.1"
    CACHE PATH "随仓库分发的 OpenSSL 1.1 运行库目录")

set(FURUI_OPENSSL_DLLS "")
set(FURUI_OPENSSL_AVAILABLE OFF)

function(furui_deploy_openssl_runtime target)
    if(NOT FURUI_OPENSSL_AVAILABLE)
        return()
    endif()
    if(NOT TARGET ${target})
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${FURUI_OPENSSL_DLLS} "$<TARGET_FILE_DIR:${target}>"
        COMMENT "deploy OpenSSL 1.1 runtime to $<TARGET_FILE_DIR:${target}>"
        VERBATIM)
endfunction()

# 只有 Windows 桌面的 Qt 5 需要：Qt 6 默认 Schannel，Android 用 _3 后缀的 OpenSSL 3
if(NOT WIN32 OR ANDROID OR QT_VERSION_MAJOR GREATER_EQUAL 6 OR NOT FURUI_DEPLOY_OPENSSL)
    return()
endif()

get_filename_component(FURUI_OPENSSL_RUNTIME_DIR "${FURUI_OPENSSL_RUNTIME_DIR}" ABSOLUTE)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING
        "32 位构建：src/thirdparty/openssl1.1 只有 x64 运行库，跳过 OpenSSL 部署；"
        "Qt 5 的 HTTPS 需要在 exe 旁自备 32 位 libssl-1_1.dll / libcrypto-1_1.dll")
    return()
endif()

foreach(_ossl_name libssl-1_1-x64 libcrypto-1_1-x64)
    if(EXISTS "${FURUI_OPENSSL_RUNTIME_DIR}/${_ossl_name}.dll")
        list(APPEND FURUI_OPENSSL_DLLS "${FURUI_OPENSSL_RUNTIME_DIR}/${_ossl_name}.dll")
    endif()
endforeach()

if(FURUI_OPENSSL_DLLS)
    list(LENGTH FURUI_OPENSSL_DLLS _ossl_count)
    if(_ossl_count LESS 2)
        message(WARNING
            "OpenSSL 运行库不完整（${FURUI_OPENSSL_RUNTIME_DIR}）：只找到 ${FURUI_OPENSSL_DLLS}，"
            "TLS 可能仍然初始化失败")
    endif()
    set(FURUI_OPENSSL_AVAILABLE ON)
    message(STATUS "OpenSSL 1.1 运行库：${FURUI_OPENSSL_RUNTIME_DIR}（构建后拷到 exe 旁）")
else()
    message(WARNING
        "未在 ${FURUI_OPENSSL_RUNTIME_DIR} 找到 OpenSSL 1.1 运行库，"
        "Windows 上 Qt 5 的 HTTPS 会报 \"TLS initialization failed\"")
endif()
