#include "KugouLoginCrypto.h"

#include "RsaPkcs1.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRandomGenerator>

#include <cstring>

namespace Furui {
namespace KugouLoginCrypto {

namespace {

const quint8 kSBox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

// 酷狗客户端内置的 1024-bit RSA 公钥（SPKI -> modulus / exponent = 65537）
const char *kRsaModulusHex =
    "c8006ed03842d2628209bd314984ca5ed6cfe06e30c95f9d4704d9c49791d7a935ba950"
    "ecb0bc8ebf5f5994f0bac927a7eb151b3c1de343303fa539c83136eccfd7d7e511e2dbc"
    "e18eaa9f784c9b50d443e75865979e0a5e216e46c684066a8d6b998580bbaa22d73f5790"
    "286bb14742e83244e44db6d707ffe162c5c7002d45";

quint8 gmul(quint8 a, quint8 b)
{
    quint8 p = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1) p ^= a;
        const bool hi = (a & 0x80) != 0;
        a = static_cast<quint8>(a << 1);
        if (hi) a ^= 0x1b;
        b = static_cast<quint8>(b >> 1);
    }
    return p;
}

quint8 invSBoxOf(quint8 v)
{
    for (int i = 0; i < 256; ++i)
        if (kSBox[i] == v) return static_cast<quint8>(i);
    return 0;
}

void addRoundKey(quint8 s[16], const quint8 *w, int round)
{
    for (int i = 0; i < 16; ++i) s[i] ^= w[round * 16 + i];
}

// AES-256：Nk=8，Nr=14，扩展 60 个 word = 240 字节
void expandKey256(const quint8 key[32], quint8 w[240])
{
    std::memcpy(w, key, 32);
    static const quint8 rcon[8] = {0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40};
    int generated = 32;
    int rconIter = 1;
    quint8 t[4];
    while (generated < 240) {
        std::memcpy(t, w + generated - 4, 4);
        if (generated % 32 == 0) {
            const quint8 tmp = t[0];
            t[0] = kSBox[t[1]] ^ rcon[rconIter++];
            t[1] = kSBox[t[2]];
            t[2] = kSBox[t[3]];
            t[3] = kSBox[tmp];
        } else if (generated % 32 == 16) {
            for (int i = 0; i < 4; ++i) t[i] = kSBox[t[i]];
        }
        for (int i = 0; i < 4; ++i) {
            w[generated] = w[generated - 32] ^ t[i];
            ++generated;
        }
    }
}

void subBytes(quint8 s[16])
{
    for (int i = 0; i < 16; ++i) s[i] = kSBox[s[i]];
}

void invSubBytes(quint8 s[16])
{
    for (int i = 0; i < 16; ++i) s[i] = invSBoxOf(s[i]);
}

// 状态按列存储 s[r + 4*c]
void shiftRows(quint8 s[16])
{
    quint8 t;
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
}

void invShiftRows(quint8 s[16])
{
    quint8 t;
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    t = s[2]; s[2] = s[10]; s[10] = t;
    t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
}

void mixColumns(quint8 s[16])
{
    for (int c = 0; c < 4; ++c) {
        quint8 *p = s + 4 * c;
        const quint8 a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = static_cast<quint8>(gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
        p[1] = static_cast<quint8>(a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
        p[2] = static_cast<quint8>(a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
        p[3] = static_cast<quint8>(gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
    }
}

void invMixColumns(quint8 s[16])
{
    for (int c = 0; c < 4; ++c) {
        quint8 *p = s + 4 * c;
        const quint8 a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = static_cast<quint8>(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
        p[1] = static_cast<quint8>(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
        p[2] = static_cast<quint8>(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
        p[3] = static_cast<quint8>(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
    }
}

void encryptBlock(const quint8 in[16], quint8 out[16], const quint8 w[240])
{
    quint8 s[16];
    std::memcpy(s, in, 16);
    addRoundKey(s, w, 0);
    for (int r = 1; r < 14; ++r) {
        subBytes(s); shiftRows(s); mixColumns(s); addRoundKey(s, w, r);
    }
    subBytes(s); shiftRows(s); addRoundKey(s, w, 14);
    std::memcpy(out, s, 16);
}

void decryptBlock(const quint8 in[16], quint8 out[16], const quint8 w[240])
{
    quint8 s[16];
    std::memcpy(s, in, 16);
    addRoundKey(s, w, 14);
    for (int r = 13; r >= 1; --r) {
        invShiftRows(s); invSubBytes(s); addRoundKey(s, w, r); invMixColumns(s);
    }
    invShiftRows(s); invSubBytes(s); addRoundKey(s, w, 0);
    std::memcpy(out, s, 16);
}

} // namespace

QByteArray toHexLower(const QByteArray &bytes)
{
    static const char *hex = "0123456789abcdef";
    QByteArray out;
    out.reserve(bytes.size() * 2);
    for (char ch : bytes) {
        const uchar c = uchar(ch);
        out.append(hex[c >> 4]);
        out.append(hex[c & 0x0f]);
    }
    return out;
}

QByteArray aes256CbcEncrypt(const QByteArray &plain, const QByteArray &key, const QByteArray &iv)
{
    if (key.size() != 32 || iv.size() != 16) return QByteArray();
    quint8 w[240];
    quint8 k[32];
    std::memcpy(k, key.constData(), 32);
    expandKey256(k, w);

    const int pad = 16 - (plain.size() % 16);   // PKCS7：整除时补一整块
    QByteArray padded = plain;
    padded.append(QByteArray(pad, static_cast<char>(pad)));

    quint8 prev[16];
    std::memcpy(prev, iv.constData(), 16);
    QByteArray out;
    out.reserve(padded.size());
    for (int off = 0; off < padded.size(); off += 16) {
        quint8 block[16], enc[16];
        for (int i = 0; i < 16; ++i)
            block[i] = static_cast<quint8>(uchar(padded[off + i])) ^ prev[i];
        encryptBlock(block, enc, w);
        out.append(reinterpret_cast<const char *>(enc), 16);
        std::memcpy(prev, enc, 16);
    }
    return out;
}

QByteArray aes256CbcDecrypt(const QByteArray &cipher, const QByteArray &key, const QByteArray &iv)
{
    if (key.size() != 32 || iv.size() != 16 || cipher.isEmpty() || cipher.size() % 16 != 0)
        return QByteArray();
    quint8 w[240];
    quint8 k[32];
    std::memcpy(k, key.constData(), 32);
    expandKey256(k, w);

    quint8 prev[16];
    std::memcpy(prev, iv.constData(), 16);
    QByteArray out;
    out.reserve(cipher.size());
    for (int off = 0; off < cipher.size(); off += 16) {
        quint8 block[16], dec[16];
        std::memcpy(block, cipher.constData() + off, 16);
        decryptBlock(block, dec, w);
        for (int i = 0; i < 16; ++i)
            out.append(char(static_cast<uchar>(dec[i]) ^ prev[i]));
        std::memcpy(prev, block, 16);
    }
    const int pad = int(uchar(out.at(out.size() - 1)));
    if (pad < 1 || pad > 16 || pad > out.size()) return QByteArray();
    for (int i = out.size() - pad; i < out.size(); ++i)
        if (int(uchar(out.at(i))) != pad) return QByteArray();
    out.chop(pad);
    return out;
}

QString randomTempKey()
{
    QString out;
    out.reserve(16);
    while (out.size() < 16)
        out += QStringLiteral("%1").arg(quint32(QRandomGenerator::global()->generate()), 8, 16,
                                        QLatin1Char('0'));
    return out.left(16).toLower();
}

QByteArray keyFromTempKey(const QString &tempKey)
{
    return QCryptographicHash::hash(tempKey.toUtf8(), QCryptographicHash::Md5)
        .toHex().left(32);
}

QByteArray ivFromKey(const QByteArray &key)
{
    return key.right(16);
}

QString rsaNoPaddingHex(const QByteArray &plain)
{
    BigInt n = BigInt::fromHex(QString::fromLatin1(kRsaModulusHex));
    BigInt e = BigInt::fromHex(QStringLiteral("10001"));
    const int k = n.toBytes().size();
    if (k <= 0 || plain.size() > k) return QString();

    QByteArray em = plain;
    em.append(QByteArray(k - plain.size(), char(0)));   // RSA_NO_PADDING：右侧补零
    QByteArray cb = BigInt::fromBytes(em).modPow(e, n).toBytes();
    if (cb.size() < k)
        cb.prepend(QByteArray(k - cb.size(), char(0)));
    QString hex;
    for (char ch : cb)
        hex += QStringLiteral("%1").arg(int(uchar(ch)), 2, 16, QLatin1Char('0')).toUpper();
    return hex;
}

bool selfTest()
{
    // FIPS-197 Appendix C.3 AES-256 标准向量（单块，手动 PKCS7 后取第一块密文）
    const QByteArray key = QByteArray::fromHex(
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
    const QByteArray iv(16, char(0));
    const QByteArray plain = QByteArray::fromHex("00112233445566778899aabbccddeeff");
    const QByteArray ct = aes256CbcEncrypt(plain, key, iv);
    if (ct.size() < 16 || toHexLower(ct.left(16)) != "8ea2b7ca516745bfeafc49904b496089")
        return false;
    // 解密必须能还原明文
    if (aes256CbcDecrypt(ct, key, iv) != plain) return false;
    // RSA_NO_PADDING 输出必须是固定 256 位 hex
    const QString rsa = rsaNoPaddingHex(QByteArrayLiteral("hi"));
    return rsa.size() == 256;
}

} // namespace KugouLoginCrypto
} // namespace Furui
