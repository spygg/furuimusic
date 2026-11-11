#pragma once
// 福瑞音乐 - 酷狗账号密码登录所需的加密原语
//
// 纯 C++ 实现，不依赖 OpenSSL / QCA，桌面与 Android 均可编译。
// 协议依据 MakcRe/KuGouMusicApi 的 util/crypto.js 与 module/login.js，
// 已于 2026-11-10 用真实请求验证（无签名退 20006，签名正确后账号字段被服务端解析）。
//
//   1) tempKey = 16 位随机十六进制字符串（小写）
//      key     = md5(tempKey).hex 前 32 字符（作为 32 字节 ASCII 密钥）
//      iv      = key 的后 16 字符
//   2) params  = AES-256-CBC( {"pwd":...,"code":"","clienttime_ms":...}, key, iv ).hex 小写
//   3) pk      = RSA_NO_PADDING( {"clienttime_ms":...,"key":tempKey} ).hex 大写
//      RSA 明文右侧补零到 128 字节，公钥为酷狗客户端内置的 1024-bit SPKI。
//   4) 登录成功后 data.secu_params 用同一 key/iv 解密，取 token / userid。
#include <QByteArray>
#include <QString>

namespace Furui {
namespace KugouLoginCrypto {

// PKCS7 填充的 AES-256-CBC。key 必须 32 字节、iv 必须 16 字节；失败返回空。
QByteArray aes256CbcEncrypt(const QByteArray &plain, const QByteArray &key, const QByteArray &iv);
// 解密并去掉 PKCS7 填充；填充非法返回空。
QByteArray aes256CbcDecrypt(const QByteArray &cipher, const QByteArray &key, const QByteArray &iv);

QByteArray toHexLower(const QByteArray &bytes);

// 生成 16 位小写十六进制随机串（酷狗的 tempKey）
QString randomTempKey();
// tempKey -> 32 字节 key / 16 字节 iv
QByteArray keyFromTempKey(const QString &tempKey);
QByteArray ivFromKey(const QByteArray &key);

// RSA_NO_PADDING：明文右侧补零到 modulus 长度（128 字节）后模幂，返回大写 hex。
QString rsaNoPaddingHex(const QByteArray &plain);

// 标准向量自检
bool selfTest();

} // namespace KugouLoginCrypto
} // namespace Furui
