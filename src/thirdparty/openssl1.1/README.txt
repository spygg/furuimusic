FuruiMusic Windows 运行所需 OpenSSL 1.1 运行库
================================================

Qt 5.15.2 的 Windows 二进制使用 OpenSSL 1.1（不是 3.x）。Qt 官方安装包不带这套
DLL，缺少时 QtNetwork 会在所有 HTTPS 请求上报 "TLS initialization failed"，
因此作为第三方运行库随仓库放在 src/thirdparty/openssl1.1/，由构建系统自动拷到
exe 同目录：

  * CMake 构建（Qt 5 桌面）：src/cmake/FuruiOpenssl.cmake 的 POST_BUILD 步骤
    copy_if_different 到 $<TARGET_FILE_DIR:FuruiMusic>
  * qmake 构建（src/FuruiMusic.pro）：win32 下 QMAKE_POST_LINK 拷到 DESTDIR
  * src/scripts/build_windows.bat：Qt 5 分支额外拷一份到 bin_qt5
  * Qt 6 不需要：Windows 上 Qt 6 走 Schannel / 自带 OpenSSL 3
  * 32 位构建跳过（本目录只有 x64 版本），构建期给提示，不阻断构建

文件来源
--------
取自本机 Qt 工具链 Tools/mingw1310_64/opt/bin（MSYS2 mingw-w64-openssl 1.1.1k）。
纯 C ABI，仅依赖系统 DLL（KERNEL32 / msvcrt / ADVAPI32 / USER32 / WS2_32），
可被 MinGW 8.1 编译的 Qt 5.15.2 直接加载，本地已实测：
  supportsSsl=1, runtime=OpenSSL 1.1.1k，HTTPS GET 返回 200。

文件与校验
----------
libssl-1_1-x64.dll     727,997 字节
  SHA256 78C88189BBD07665F1863D0C9E25BB14A0CDBB0B28B2102297F47E40748711AC
libcrypto-1_1-x64.dll  3,437,192 字节
  SHA256 71CBA4D301EC98E623E78E79E47C0152AD32C5456CF397056AD9E9381356D596

许可证
------
OpenSSL 1.1.1 采用 OpenSSL License + SSLeay License 双许可，全文见同目录
LICENSE-OpenSSL-1.1.txt。本目录仅原样分发未修改的官方二进制。
