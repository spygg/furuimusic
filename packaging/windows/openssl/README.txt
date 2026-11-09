FuruiMusic Windows 运行所需 OpenSSL 1.1 运行库
================================================

Qt 5.15.2 的 Windows 二进制使用 OpenSSL 1.1（不是 3.x）。Qt 官方安装包不带这套
DLL，缺少时 QtNetwork 会在所有 HTTPS 请求上报 "TLS initialization failed"，
因此随仓库提供，由 .github/workflows/release.yml 的 Windows 打包步骤复制到 exe 旁。

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
