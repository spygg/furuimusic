# 福瑞音乐 - Windows MinGW 下自建 QtWebView 的 WebView2 后端
#
# Qt 6.11.1 的官方 MinGW 二进制把 QtWebView 五个后端全关了
# (include/QtWebView/qtwebview-config.h: webview_webview2_plugin = -1)，
# 而 QtWebEngine 官方从不支持 MinGW。为了让 QML 能统一 `import QtWebView`，
# 这里用 MinGW 自行编译 Qt 6.11.1 自带的 WebView2 后端插件源码
# (src/platform/windows/webview2-plugin/)，并随 exe 部署。
#
# MSVC 套件自带 qtwebview_webview2.dll，不需要本模块；Android 走系统
# WebView 后端，也不需要。缺 SDK 时只 WARNING，降级为外部浏览器导入。

include_guard(GLOBAL)

option(FURUI_WEBVIEW2_PLUGIN "MinGW 下自编译并部署 QtWebView 的 WebView2 后端" ON)

set(FURUI_WEBVIEW2_AVAILABLE FALSE)
set(FURUI_WEBVIEW2_SDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../thirdparty/webview2"
    CACHE PATH "随仓库分发的 WebView2 SDK 头与加载器")
set(FURUI_WEBVIEW2_PLUGIN_DIR "${CMAKE_CURRENT_LIST_DIR}/../platform/windows/webview2-plugin"
    CACHE PATH "MinGW 兼容版 QtWebView WebView2 后端源码")

function(furui_configure_webview2)
    if(NOT FURUI_WEBVIEW2_PLUGIN)
        return()
    endif()
    if(NOT QT_VERSION_MAJOR GREATER_EQUAL 6)
        return() # Qt 5.15.2 没有 QtWebView 模块
    endif()
    if(NOT WIN32 OR ANDROID)
        return()
    endif()
    if(NOT MINGW)
        message(STATUS "[webview2] MSVC: Qt 官方已提供 WebView2 后端，跳过自编译")
        return()
    endif()
    if(TARGET qtwebview_webview2)
        set(FURUI_WEBVIEW2_AVAILABLE TRUE PARENT_SCOPE)
        return()
    endif()

    set(_inc "${FURUI_WEBVIEW2_SDK_DIR}/include")
    set(_loader "${FURUI_WEBVIEW2_SDK_DIR}/x64/WebView2Loader.dll")
    set(_private_config "${Qt6_DIR}/../Qt6WebViewPrivate/Qt6WebViewPrivateConfig.cmake")
    get_filename_component(_private_config "${_private_config}" ABSOLUTE)

    if(NOT EXISTS "${_inc}/WebView2.h" OR NOT EXISTS "${_loader}"
       OR NOT EXISTS "${FURUI_WEBVIEW2_PLUGIN_DIR}/qwebview2webview.cpp")
        message(WARNING "[webview2] SDK 或插件源码缺失，内置浏览器将不可用")
        return()
    endif()
    if(NOT EXISTS "${_private_config}")
        message(WARNING "[webview2] 找不到 Qt6WebViewPrivateConfig.cmake，内置浏览器将不可用")
        return()
    endif()

    include("${_private_config}")
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets WebView)

    add_library(qtwebview_webview2 MODULE
        "${FURUI_WEBVIEW2_PLUGIN_DIR}/qwebview2webview.cpp"
        "${FURUI_WEBVIEW2_PLUGIN_DIR}/qwebview2webviewplugin.cpp"
        "${FURUI_WEBVIEW2_PLUGIN_DIR}/webview2_loader_compat.cpp"
        "${FURUI_WEBVIEW2_PLUGIN_DIR}/webview2_cookie_bridge.cpp"
    )
    set_target_properties(qtwebview_webview2 PROPERTIES
        PREFIX ""
        OUTPUT_NAME "qtwebview_webview2"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/webview"
        AUTOMOC ON)
    target_include_directories(qtwebview_webview2 PRIVATE
        "${FURUI_WEBVIEW2_PLUGIN_DIR}" "${_inc}")
    target_link_libraries(qtwebview_webview2 PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::WebView Qt6::WebViewPrivate
        urlmon ole32)

    set(FURUI_WEBVIEW2_LOADER "${_loader}" CACHE FILEPATH "WebView2Loader.dll" FORCE)
    set(FURUI_WEBVIEW2_AVAILABLE TRUE PARENT_SCOPE)
    message(STATUS "[webview2] MinGW 自编译 WebView2 后端已启用")
endfunction()

function(furui_deploy_webview2 target)
    if(NOT FURUI_WEBVIEW2_AVAILABLE OR NOT TARGET ${target})
        return()
    endif()
    add_dependencies(${target} qtwebview_webview2)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/plugins/webview"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:qtwebview_webview2>"
                "$<TARGET_FILE_DIR:${target}>/plugins/webview/qtwebview_webview2.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FURUI_WEBVIEW2_LOADER}" "$<TARGET_FILE_DIR:${target}>/WebView2Loader.dll"
        COMMENT "deploy QtWebView WebView2 backend + loader"
        VERBATIM)
endfunction()
