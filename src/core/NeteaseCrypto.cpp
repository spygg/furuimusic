#include "NeteaseCrypto.h"
#include "Aes128.h"

#include <cstdio>
#include <QJsonDocument>
#include "core/QtCompat.h"
#include <QCryptographicHash>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Furui {
namespace Netease {

// ---------------------------------------------------------------------------
// 大整数（base 2^32，little-endian limbs），仅用于 RSA 模幂（e=65537）
// ---------------------------------------------------------------------------
namespace {

struct BigInt {
    std::vector<uint32_t> v; // little-endian

    BigInt() = default;
    explicit BigInt(std::vector<uint32_t> &&limbs) : v(std::move(limbs)) {}

    // 把十六进制字符串（大端）解析为 little-endian limbs。
    // 逐字节处理，对任意（偶数）长度都稳健；奇数长度时最高位不完整位会被自然忽略。
    static BigInt fromHex(const QByteArray &hex)
    {
        BigInt out;
        const int n = hex.size();
        const int nbytes = n / 2; // 整数除法：奇数长度时丢弃最高位残留的半个字节
        out.v.assign((nbytes + 3) / 4, 0);
        for (int i = 0; i < nbytes; ++i) {
            // i 从最低有效字节开始向上计数
            const int strPos = n - 2 - 2 * i;
            const char c1 = hex.at(strPos);
            const char c2 = hex.at(strPos + 1);
            const uint8_t hi = (c1 >= '0' && c1 <= '9') ? uint8_t(c1 - '0')
                             : (c1 >= 'a' && c1 <= 'f') ? uint8_t(c1 - 'a' + 10)
                             : (c1 >= 'A' && c1 <= 'F') ? uint8_t(c1 - 'A' + 10) : 0;
            const uint8_t lo = (c2 >= '0' && c2 <= '9') ? uint8_t(c2 - '0')
                             : (c2 >= 'a' && c2 <= 'f') ? uint8_t(c2 - 'a' + 10)
                             : (c2 >= 'A' && c2 <= 'F') ? uint8_t(c2 - 'A' + 10) : 0;
            const uint8_t b = uint8_t((hi << 4) | lo);
            const int limbIdx = i / 4;
            const int byteInLimb = i % 4;
            out.v[limbIdx] |= (uint32_t(b) << (8 * byteInLimb));
        }
        out.trim();
        return out;
    }

    static BigInt fromBytes(const QByteArray &bytes)
    {
        BigInt out;
        const int n = bytes.size();
        for (int i = n - 1; i >= 0; i -= 4) {
            uint32_t limb = 0;
            int low = qMax(i - 3, 0); // 组内低索引（含 i-3）为高字节
            // 组内按大端：低索引字节（字符串中更早出现）应为高位
            for (int j = low; j <= i; ++j) {
                limb = (limb << 8) | uint8_t(bytes.at(j));
            }
            out.v.push_back(limb);
        }
        out.trim();
        return out;
    }

    void trim()
    {
        while (!v.empty() && v.back() == 0) v.pop_back();
    }

    int bitLength() const
    {
        if (v.empty()) return 0;
        uint32_t top = v.back();
        int p = 0;
        while (top) { top >>= 1; ++p; } // p = 最高位在 limb 内的位置 + 1
        return int(v.size() - 1) * 32 + p;
    }

    // 大端 hex，无前导 0（除非值为 0）
    QByteArray toHex() const
    {
        if (v.empty()) return QByteArray("0");
        std::vector<uint32_t> tmp = v;
        std::reverse(tmp.begin(), tmp.end());
        QByteArray out;
        char hexd[] = "0123456789abcdef";
        for (size_t i = 0; i < tmp.size(); ++i) {
            uint32_t limb = tmp[i];
            for (int s = 28; s >= 0; s -= 4)
                out.append(hexd[(limb >> s) & 0xF]);
        }
        // 去掉前导 0（保留至少 1 个字符）
        int p = 0;
        while (p + 1 < out.size() && out.at(p) == '0') ++p;
        return out.mid(p);
    }

    // 大端字节，固定 length 长度，左补 0x00
    QByteArray toBytesPadded(int length) const
    {
        std::vector<uint32_t> tmp = v;
        std::reverse(tmp.begin(), tmp.end());
        QByteArray out;
        out.reserve(length);
        int produced = 0;
        for (size_t i = 0; i < tmp.size(); ++i) {
            uint8_t b[4];
            uint32_t limb = tmp[i];
            b[3] = uint8_t(limb & 0xFF);
            b[2] = uint8_t((limb >> 8) & 0xFF);
            b[1] = uint8_t((limb >> 16) & 0xFF);
            b[0] = uint8_t((limb >> 24) & 0xFF);
            for (int k = 0; k < 4; ++k) {
                if (produced < length) { out.append(char(b[k])); ++produced; }
            }
        }
        while (produced < length) { out.prepend(char(0)); ++produced; }
        return out;
    }

    // 大端 hex，固定 hexLen 字符，左补 '0'
    QByteArray toHexPadded(int hexLen) const
    {
        QByteArray h = toHex();
        if (h.size() < hexLen) h.prepend(QByteArray(hexLen - h.size(), '0'));
        return h;
    }

    BigInt modpow(const BigInt &exp, const BigInt &mod) const
    {
        BigInt base = *this % mod;
        BigInt result = BigInt::fromBytes(QByteArray("\x01", 1));
        result = result % mod;
    BigInt e = exp;
    while (!e.isZero()) {
        if (e.v.front() & 1u)
            result = (result * base) % mod;
        base = (base * base) % mod;
            // 右移 1 位
            for (size_t i = 0; i < e.v.size(); ++i) {
                uint32_t carry = (i + 1 < e.v.size()) ? (e.v[i + 1] & 1u) << 31 : 0;
                e.v[i] = (e.v[i] >> 1) | carry;
            }
            if (!e.v.empty() && e.v.back() == 0) e.v.pop_back();
        }
        return result;
    }

    bool isZero() const { return v.empty() || (v.size() == 1 && v[0] == 0); }

    BigInt operator%(const BigInt &m) const
    {
        if (m.isZero()) return BigInt();
        BigInt r; // = 0
        int bits = bitLength();
        for (int i = bits - 1; i >= 0; --i) {
            r = r.shl(1);
            if (bitAt(i)) {
                if (r.v.empty()) r.v.push_back(0);
                r.v[0] |= 1u;
            }
            r.trim();
            if (!r.isZero() && !r.lessThan(m)) r = r.sub(m);
        }
        return r;
    }

    BigInt operator*(const BigInt &o) const
    {
        BigInt r;
        // +1 limb headroom: the carry-propagation loop can momentarily land one
        // limb past the 2N theoretical max; trim() removes it afterwards.
        r.v.assign(v.size() + o.v.size() + 1, 0);
        for (size_t i = 0; i < v.size(); ++i) {
            uint64_t carry = 0;
            for (size_t j = 0; j < o.v.size(); ++j) {
                uint64_t cur = uint64_t(v[i]) * o.v[j] + r.v[i + j] + carry;
                r.v[i + j] = uint32_t(cur & 0xFFFFFFFFu);
                carry = cur >> 32;
            }
            size_t k = i + o.v.size();
            while (carry) {
                uint64_t cur = uint64_t(r.v[k]) + carry;
                r.v[k] = uint32_t(cur & 0xFFFFFFFFu);
                carry = cur >> 32;
                ++k;
            }
        }
        r.trim();
        return r;
    }

private:
    BigInt sub(const BigInt &o) const
    {
        BigInt r;
        r.v.assign(qMax(v.size(), o.v.size()), 0);
        int64_t borrow = 0;
        for (size_t i = 0; i < r.v.size(); ++i) {
            int64_t a = (i < v.size()) ? int64_t(v[i]) : 0;
            int64_t b = (i < o.v.size()) ? int64_t(o.v[i]) : 0;
            int64_t diff = a - b - borrow;
            if (diff < 0) { diff += (int64_t(1) << 32); borrow = 1; }
            else borrow = 0;
            r.v[i] = uint32_t(diff & 0xFFFFFFFFu);
        }
        r.trim();
        return r;
    }

    bool lessThan(const BigInt &o) const
    {
        if (v.size() != o.v.size()) return v.size() < o.v.size();
        for (int i = int(v.size()) - 1; i >= 0; --i)
            if (v[i] != o.v[i]) return v[i] < o.v[i];
        return false;
    }

    bool bitAt(int idx) const
    {
        int limb = idx / 32;
        if (limb < 0 || limb >= int(v.size())) return false;
        return (v[limb] >> (idx % 32)) & 1u;
    }

    BigInt shl(int n) const
    {
        BigInt r;
        if (n <= 0) return *this;
        int wordShift = n / 32;
        int bitShift = n % 32;
        r.v.assign(v.size() + wordShift + (bitShift ? 1 : 0), 0);
        for (size_t i = 0; i < v.size(); ++i) {
            size_t dst = i + wordShift;
            r.v[dst] |= (v[i] << bitShift);
            if (bitShift) r.v[dst + 1] |= (v[i] >> (32 - bitShift));
        }
        r.trim();
        return r;
    }
};

QByteArray toHexLower(const QByteArray &b)
{
    QByteArray out;
    const char h[] = "0123456789abcdef";
    out.reserve(b.size() * 2);
    for (char c : b) {
        uint8_t u = uint8_t(c);
        out.append(h[u >> 4]);
        out.append(h[u & 0xF]);
    }
    return out;
}

// 网易云 weapi RSA 模数（1024-bit，256 个十六进制字符，无多余前导 0）。
// 注意：必须恰好 256 个十六进制字符，否则下面的 fromHex 解析会错位。
const char kModulusHex[] =
    "e0b509f6259df8642dbc35662901477df22677ec152b5ff68ace615bb7b725152b3ab17a876aea8a5aa76d2e417629ec4ee341f56135fccf695280104e0312ecbda92557c93870114af6c9d05c4f7f0c3685b7a46bee255932575cce10b424d813cfe4875d3e82047b97ddef52741d546b8e289dc6935b3ece0462db0a22b8e7";

} // namespace

// ---------------------------------------------------------------------------
// AES-128（ECB 块 + CBC + PKCS7），自包含实现（FIPS-197 / SP800-38A，向量已校验）
namespace {
const uint8_t kSboxV2[256] = {
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
const uint8_t kRconV2[11] = {0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

static inline uint8_t xtimeV2(uint8_t x)
{
    uint8_t r = uint8_t(x << 1);
    if (x & 0x80) r ^= 0x1b;
    return r;
}

static inline uint8_t gmulV2(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1) p ^= a;
        bool hi = (a & 0x80);
        a = uint8_t(a << 1);
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

static void keyExpansionV2(const uint8_t key[16], uint8_t w[176])
{
    for (int i = 0; i < 16; ++i) w[i] = key[i];
    for (int i = 4; i < 44; ++i) {
        uint8_t temp[4];
        for (int j = 0; j < 4; ++j) temp[j] = w[(i - 1) * 4 + j];
        if (i % 4 == 0) {
            uint8_t t = temp[0]; temp[0]=temp[1]; temp[1]=temp[2]; temp[2]=temp[3]; temp[3]=t;
            for (int j = 0; j < 4; ++j) temp[j] = kSboxV2[temp[j]];
            temp[0] ^= kRconV2[i / 4];
        }
        for (int j = 0; j < 4; ++j) w[i * 4 + j] = w[(i - 4) * 4 + j] ^ temp[j];
    }
}

static void aesEncBlockV2(const uint8_t in[16], const uint8_t w[176], uint8_t out[16])
{
    uint8_t s[16];
    for (int i = 0; i < 16; ++i) s[i] = in[i];
    for (int i = 0; i < 16; ++i) s[i] ^= w[i]; // AddRoundKey(round 0)
    for (int r = 1; r <= 10; ++r) {
        for (int i = 0; i < 16; ++i) s[i] = kSboxV2[s[i]]; // SubBytes
        uint8_t t; // ShiftRows
        t = s[1]; s[1]=s[5]; s[5]=s[9]; s[9]=s[13]; s[13]=t;
        t = s[2]; s[2]=s[10]; s[10]=t; t = s[6]; s[6]=s[14]; s[14]=t;
        t = s[3]; s[3]=s[15]; s[15]=s[11]; s[11]=s[7]; s[7]=t;
        if (r != 10) { // MixColumns（最后一轮不做）
            for (int c = 0; c < 4; ++c) {
                uint8_t a0=s[c*4+0], a1=s[c*4+1], a2=s[c*4+2], a3=s[c*4+3];
                s[c*4+0] = gmulV2(a0,2)^gmulV2(a1,3)^a2^a3;
                s[c*4+1] = a0^gmulV2(a1,2)^gmulV2(a2,3)^a3;
                s[c*4+2] = a0^a1^gmulV2(a2,2)^gmulV2(a3,3);
                s[c*4+3] = gmulV2(a0,3)^a1^a2^gmulV2(a3,2);
            }
        }
        for (int i = 0; i < 16; ++i) s[i] ^= w[r * 16 + i]; // AddRoundKey
    }
    for (int i = 0; i < 16; ++i) out[i] = s[i];
}
} // namespace

QByteArray aes128CbcRawV2(const QByteArray &plain, const QByteArray &key, const QByteArray &iv)
{
    if (key.size() != 16 || iv.size() != 16) return QByteArray();
    uint8_t kk[16];
    for (int i = 0; i < 16; ++i) kk[i] = uint8_t(key[i]);
    uint8_t w[176];
    keyExpansionV2(kk, w);

    int pad = 16 - (plain.size() % 16);
    if (pad == 0) pad = 16;
    QByteArray padded = plain;
    int newSize = plain.size() + pad;
    padded.resize(newSize);
    for (int i = plain.size(); i < newSize; ++i) padded[i] = char(pad);

    QByteArray out;
    out.reserve(padded.size());
    uint8_t prev[16];
    for (int i = 0; i < 16; ++i) prev[i] = uint8_t(iv[i]);
    for (int off = 0; off < padded.size(); off += 16) {
        uint8_t block[16];
        for (int i = 0; i < 16; ++i) block[i] = uint8_t(padded[off + i]) ^ prev[i];
        uint8_t enc[16];
        aesEncBlockV2(block, w, enc);
        out.append(reinterpret_cast<char*>(enc), 16);
        for (int i = 0; i < 16; ++i) prev[i] = enc[i];
    }
    return out;
}

QByteArray aes128CbcHex(const QByteArray &plain, const QByteArray &key, const QByteArray &iv)
{
    return toHexLower(aes128CbcRawV2(plain, key, iv));
}

QByteArray weapiBuild(const QJsonObject &payload, const QByteArray &secretKey)
{
    static const QByteArray iv = "0102030405060708";
    static const QByteArray nonce = "0CoJUm6Qyw8W8jud";
    QByteArray text = QJsonDocument(payload).toJson(QJsonDocument::Compact);

    // 标准 weapi 链式（与 lx-music-desktop / NeteaseCloudMusicApi 一致）：
    //   内层 = AES_CBC(json 原文, presetKey)            → 原始密文
    //   中间 = 内层密文 .toString('base64')             → base64 字符串
    //   外层 = AES_CBC(中间字符串, secretKey)           → 原始密文
    //   params = 外层密文 .toString('base64')
    QByteArray inner = aes128CbcRawV2(text, nonce, iv);
    QByteArray innerB64 = inner.toBase64();
    QByteArray outer = aes128CbcRawV2(innerB64, secretKey, iv);
    QByteArray encText = outer.toBase64();

    // RSA：反转并大端解释，再做 (value ^ e) mod n
    QByteArray reversed = secretKey;
    std::reverse(reversed.begin(), reversed.end());
    BigInt bi = BigInt::fromBytes(reversed);
    BigInt e = BigInt::fromHex("010001");
    BigInt n = BigInt::fromHex(kModulusHex);
    BigInt r = bi.modpow(e, n);
    QByteArray encSecKey = r.toHexPadded(256); // 256 hex 字符 = 128 字节

    // 必须自己按 form-urlencoded 编码：QUrlQuery 会保留 base64 里的 '+'，
    // 而服务端把 '+' 解码成空格，密文被破坏后网易云直接返回空 body（二维码登录会随机失败）。
    static const QByteArray unreserved =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    return QByteArray("params=") + encText.toPercentEncoding(unreserved)
           + "&encSecKey=" + encSecKey;
}

QByteArray weapiForm(const QJsonObject &payload)
{
    QString charset = QStringLiteral("abcdefghijklmnopqrstuvwxyz"
                                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    QByteArray secretKey;
    for (int i = 0;i < 16;++i)
        secretKey.append(char(charset.at(QRandomGenerator::global()->bounded(charset.size())).toLatin1()));
    return weapiBuild(payload, secretKey);
}

QByteArray eapiForm(const QString &apiPath, const QJsonObject &payload)
{
    // 与 NeteaseCloudMusicApi util/crypto.js 的 eapi 分支逐字节一致：
    //   message = nobody + apiPath + use + json + md5forencrypt
    //   params  = AES-128-ECB(apiPath + "-36cd479b6b5-" + json + "-36cd479b6b5-" + md5(message), "e82ckenh8dichen8")
    const QByteArray json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    const QString message = QStringLiteral("nobody") + apiPath + QStringLiteral("use")
                            + QString::fromUtf8(json) + QStringLiteral("md5forencrypt");
    const QByteArray digest = QCryptographicHash::hash(message.toUtf8(), QCryptographicHash::Md5)
                                  .toHex();
    const QByteArray data = apiPath.toUtf8() + QByteArrayLiteral("-36cd479b6b5-") + json
                            + QByteArrayLiteral("-36cd479b6b5-") + digest;
    static const QByteArray eapiKey = QByteArrayLiteral("e82ckenh8dichen8");
    // 必须是真正的 ECB（多块之间不链），不能用零 IV 的 CBC 冒充
    return QByteArrayLiteral("params=") + Aes128::encryptEcbHexUpper(data, eapiKey);
}

bool selfTest()
{
    bool ok = true;
    // FIPS-197 附录 C.1：key=000102...0f，plain=00112233...eeff。
    // CBC 配全零 IV 的首块等价于 ECB 单块；aes128CbcRawV2 会补 PKCS7，故取前 16 字节。
    {
        QByteArray key(16, '\0'), plain(16, '\0');
        for (int i = 0; i < 16; ++i) {
            key[i] = char(i);
            plain[i] = char(i * 17);
        }
        const QByteArray cipher = aes128CbcRawV2(plain, key, QByteArray(16, '\0'));
        ok &= (toHexLower(cipher).left(32) == QByteArray("69c4e0d86a7b0430d8cdb78070b4c55a"));
    }
    // NIST SP800-38A CBC 向量： key=2b7e151628aed2a6, iv=000102030405060708090a0b0c0d0e0f
    {
        uint8_t k[16] = {0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
        QByteArray iv = QByteArray::fromHex(QByteArray("000102030405060708090a0b0c0d0e0f"));
        QByteArray plaintext = QByteArray::fromHex(QByteArray(
            "6bc1bee22e409f96e93d7e117393172a")); // 单块
        QByteArray key(reinterpret_cast<char *>(k), 16);
        QByteArray cipher = aes128CbcRawV2(plaintext, key, iv);
        QByteArray hex = toHexLower(cipher);
        // aes128CbcRawV2 内部做 PKCS7 填充，16 字节明文会被补成 32 字节；
        // NIST SP800-38A 该向量的期望是第一个 CBC 块（前 32 个十六进制字符）。
        ok &= (hex.left(32) == QByteArray("7649abac8119b246cee98e9b12e9197d"));
    }
    // RSA：13 ^ 65537 mod 17 = ?
    {
        BigInt base = BigInt::fromBytes(QByteArray("\x0d", 1));
        BigInt e = BigInt::fromHex("010001");
        BigInt m = BigInt::fromBytes(QByteArray("\x11", 1));
        BigInt r = base.modpow(e, m);
        // 13^65537 mod 17: 13 mod 17, 13^2=169=16 mod17, ... 由 Fermat 13^16=1 mod17, 65537 mod16=1 => 13^1=13
        ok &= (r.toHex() == QByteArray("d"));
    }
    return ok;
}

} // namespace Netease
} // namespace Furui
