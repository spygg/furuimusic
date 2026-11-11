# Android OpenSSL 3 运行库

Qt 6 的 OpenSSL TLS 后端在 Android 上按 `_3` 后缀动态加载：

- `libssl_3.so`
- `libcrypto_3.so`

Android 系统自带的是 BoringSSL（没有 `OpenSSL_version_num`），不能直接复用，
因此这里放置 KDAB 提供的、与 Qt 命名约定一致的预编译 OpenSSL 3 共享库。

- 来源：https://github.com/KDAB/android_openssl （`ssl_3/arm64-v8a`）
- 许可证：Apache License 2.0（见 OpenSSL 项目）
- 当前仅包含 `arm64-v8a`；构建其他 ABI 时请补充对应目录，缺失时 CMake 会跳过打包。

`src/CMakeLists.txt` 会把它们加入 `QT_ANDROID_EXTRA_LIBS`，随 APK 一起安装。
