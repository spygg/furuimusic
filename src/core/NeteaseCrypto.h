#pragma once
// 福瑞音乐 - 网易云 weapi 加密（AES-128-CBC + textbook RSA 模幂）
// 纯 C++，无第三方依赖，跨平台（Windows / Linux / macOS / Android）均可编译。
// 算法与密钥取自 NeteaseCloudMusicApi（util/crypto.js）：
//   aesEncrypt = AES-128-CBC(text, key, iv="0102030405060708")，PKCS7 填充，输出 hex（小写）
//   rsaEncrypt = 把随机 secretKey（16 字节 ASCII）反转后按大端解释为整数，做 (value ^ e) mod n，
//               结果转 hex 并左补 0 到 256 位（128 字节）
//   weapi(object) = base64( AES_CBC( base64( AES_CBC(text, "0CoJUm6Qyw8W8jud") ), randomSecretKey ) )
//                   两次链式均带 PKCS7 填充；最后把 randomSecretKey 做 RSA，得到 {params, encSecKey}
//                   （与 lx-music-desktop / NeteaseCloudMusicApi 的 crypto.js 完全一致）
// 注意：本文件只复现公开文档化的加密流程，用于本地构造请求体，不存储或分发任何密钥以外的凭证。
#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace Furui {
namespace Netease {

// 生成 weapi 请求体：返回 URL-encoded 的 "params=<hex>&encSecKey=<hex>"
QByteArray weapiForm(const QJsonObject &payload);

// 与 weapiForm 相同，但使用外部指定的 secretKey（仅用于单元测试对照）
QByteArray weapiBuild(const QJsonObject &payload, const QByteArray &secretKey);

// 仅做 AES-128-CBC（PKCS7）加密并返回小写 hex，供单元测试与 linuxapi 之外的用途复用
QByteArray aes128CbcHex(const QByteArray &plain, const QByteArray &key, const QByteArray &iv);

// 生成 eapi 请求体：返回 URL-encoded 的 "params=<大写 hex>"
// 算法（NeteaseCloudMusicApi util/crypto.js 的 eapi 分支）：
//   message = "nobody" + apiPath + "use" + json + "md5forencrypt"
//   digest  = md5(message).hex
//   data    = apiPath + "-36cd479b6b5-" + json + "-36cd479b6b5-" + digest
//   params  = AES-128-ECB(data, "e82ckenh8dichen8").hex().upper()
QByteArray eapiForm(const QString &apiPath, const QJsonObject &payload);

// 自检：AES-128（FIPS-197 ECB 块向量）+ RSA 模幂（与 Python pow 参考实现对照）。返回是否全部通过。
bool selfTest();

} // namespace Netease
} // namespace Furui
