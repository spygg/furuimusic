#pragma once
// 福瑞音乐 - AES-128-ECB 与网易云 linuxapi 签名
// 纯 C++ 实现，不依赖 OpenSSL / QCA，桌面与 Android 均可编译。
// linuxapi 算法与密钥来自洛雪音乐参考实现：
//   reference/lx-music-desktop-git/src/renderer/utils/musicSdk/wy/utils/crypto.js
//   eparams = AES-128-ECB(JSON.stringify(obj), 'rFgB&h#%2?^eDg:Q').hex().toUpperCase()
#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace Furui {
namespace Aes128 {

// PKCS7 填充后的 AES-128-ECB 加密，key 必须是 16 字节；返回大写 hex
QByteArray encryptEcbHexUpper(const QByteArray &plain, const QByteArray &key);

// 生成 /api/linux/forward 的 form 体：eparams=<大写 hex>
// payload 形如 {"method":"POST","url":"https://music.163.com/api/v3/playlist/detail","params":{...}}
QByteArray linuxapiForm(const QJsonObject &payload);

// 用 FIPS-197 标准向量自检，返回是否通过
bool selfTest();

} // namespace Aes128
} // namespace Furui
