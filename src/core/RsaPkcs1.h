#pragma once
// 福瑞音乐 - 自包含 RSA 公钥加密（仅 PKCS#1 v1.5，用于咪咕等登录）
//
// 项目构建环境（Qt5.15 / MinGW 8.1）未随附 OpenSSL 开发库，且不想为一个登录端点
// 引入外部依赖，这里用 base-2^32 的小整数自行实现 RSA 公钥加密：
//   输入：十六进制 modulus + exponent（咪咕 /password/publickey 返回的就是 hex）
//   输出：十六进制密文（与官方网页端 c.encrypt() 的 hex 输出一致）
// 性能无关紧要——登录是一次性的，模幂用二进制法，1024 位密钥子秒完成。
// 正确性由 selfTest() 在启动时验证（与 Aes128 / Netease 自检同级）。
#include <QByteArray>
#include <QRandomGenerator>
#include <QString>
#include <cstdint>
#include <vector>
#include <cstdlib>

namespace Furui {

class BigInt {
public:
    BigInt() = default;
    explicit BigInt(std::vector<uint32_t> limbs) : d(std::move(limbs)) { trim(); }

    static BigInt fromHex(const QString &hex);
    static BigInt fromBytes(const QByteArray &b);   // big-endian
    QByteArray toBytes() const;                      // big-endian（零值返回单字节 0x00）
    QString toHex() const;
    BigInt modPow(const BigInt &e, const BigInt &m) const;

    int bitLength() const;
    bool isZero() const { return d.empty(); }
    bool getBit(int i) const;

private:
    static const uint64_t BASE = 0x100000000ull;     // 2^32
    std::vector<uint32_t> d;                          // base 2^32，little-endian

    void trim();
    static int cmp(const BigInt &a, const BigInt &b);
    static BigInt add(const BigInt &a, const BigInt &b);
    static BigInt sub(const BigInt &a, const BigInt &b);   // 要求 a >= b
    static BigInt mul(const BigInt &a, const BigInt &b);
    static BigInt mulSmall(const BigInt &a, uint32_t k);
    static BigInt addSmall(const BigInt &a, uint32_t k);
    static BigInt shl1(const BigInt &a);                   // *2
    static BigInt mod(const BigInt &a, const BigInt &m);   // 二进制减法法
    static BigInt divmodSmall(const BigInt &a, uint32_t m, uint32_t &rem);
};

inline void BigInt::trim()
{
    while (!d.empty() && d.back() == 0) d.pop_back();
}

inline int BigInt::cmp(const BigInt &a, const BigInt &b)
{
    if (a.d.size() != b.d.size()) return a.d.size() < b.d.size() ? -1 : 1;
    for (int i = int(a.d.size()) - 1; i >= 0; --i) {
        if (a.d[i] != b.d[i]) return a.d[i] < b.d[i] ? -1 : 1;
    }
    return 0;
}

inline BigInt BigInt::add(const BigInt &a, const BigInt &b)
{
    BigInt r;
    r.d.resize(std::max(a.d.size(), b.d.size()) + 1);
    uint64_t carry = 0;
    for (size_t i = 0; i < r.d.size(); ++i) {
        uint64_t s = carry
                   + (i < a.d.size() ? a.d[i] : 0)
                   + (i < b.d.size() ? b.d[i] : 0);
        r.d[i] = uint32_t(s & 0xFFFFFFFFu);
        carry = s >> 32;
    }
    r.trim();
    return r;
}

inline BigInt BigInt::sub(const BigInt &a, const BigInt &b)
{
    // 调用方保证 a >= b
    BigInt r;
    r.d = a.d;
    int64_t borrow = 0;
    for (size_t i = 0; i < r.d.size(); ++i) {
        int64_t s = int64_t(r.d[i]) - (i < b.d.size() ? int64_t(b.d[i]) : 0) - borrow;
        if (s < 0) { s += BASE; borrow = 1; } else { borrow = 0; }
        r.d[i] = uint32_t(s & 0xFFFFFFFFu);
    }
    r.trim();
    return r;
}

inline BigInt BigInt::mul(const BigInt &a, const BigInt &b)
{
    if (a.isZero() || b.isZero()) return BigInt();
    BigInt r;
    r.d.assign(a.d.size() + b.d.size(), 0);
    for (size_t i = 0; i < a.d.size(); ++i) {
        uint64_t carry = 0;
        uint64_t ai = a.d[i];
        for (size_t j = 0; j < b.d.size(); ++j) {
            uint64_t cur = ai * b.d[j] + r.d[i + j] + carry;
            r.d[i + j] = uint32_t(cur & 0xFFFFFFFFu);
            carry = cur >> 32;
        }
        size_t k = i + b.d.size();
        while (carry) {
            uint64_t cur = uint64_t(r.d[k]) + carry;
            r.d[k] = uint32_t(cur & 0xFFFFFFFFu);
            carry = cur >> 32;
            ++k;
        }
    }
    r.trim();
    return r;
}

inline BigInt BigInt::mulSmall(const BigInt &a, uint32_t k)
{
    BigInt r;
    r.d.resize(a.d.size() + 1);
    uint64_t carry = 0;
    for (size_t i = 0; i < a.d.size(); ++i) {
        uint64_t cur = uint64_t(a.d[i]) * k + carry;
        r.d[i] = uint32_t(cur & 0xFFFFFFFFu);
        carry = cur >> 32;
    }
    r.d[a.d.size()] = uint32_t(carry);
    r.trim();
    return r;
}

inline BigInt BigInt::addSmall(const BigInt &a, uint32_t k)
{
    BigInt r = a;
    if (r.d.empty()) r.d.resize(1, 0);
    uint64_t carry = k;
    for (size_t i = 0; i < r.d.size() && carry; ++i) {
        uint64_t cur = uint64_t(r.d[i]) + carry;
        r.d[i] = uint32_t(cur & 0xFFFFFFFFu);
        carry = cur >> 32;
    }
    if (carry) r.d.push_back(uint32_t(carry));
    r.trim();
    return r;
}

inline BigInt BigInt::shl1(const BigInt &a)
{
    BigInt r = a;
    uint64_t carry = 0;
    for (size_t i = 0; i < r.d.size(); ++i) {
        uint64_t cur = uint64_t(r.d[i]) * 2 + carry;
        r.d[i] = uint32_t(cur & 0xFFFFFFFFu);
        carry = cur >> 32;
    }
    if (carry) r.d.push_back(uint32_t(carry));
    r.trim();
    return r;
}

inline BigInt BigInt::mod(const BigInt &a, const BigInt &m)
{
    if (cmp(a, m) < 0) return a;
    BigInt r;
    int bl = a.bitLength();
    for (int i = bl - 1; i >= 0; --i) {
        r = shl1(r);
        if (a.getBit(i)) r = addSmall(r, 1);
        if (cmp(r, m) >= 0) r = sub(r, m);
    }
    return r;
}

inline BigInt BigInt::modPow(const BigInt &e, const BigInt &m) const
{
    BigInt result = BigInt(std::vector<uint32_t>{1});
    BigInt base = mod(*this, m);
    int eb = e.bitLength();
    for (int i = eb - 1; i >= 0; --i) {
        result = mod(mul(result, result), m);
        if (e.getBit(i)) result = mod(mul(result, base), m);
    }
    return result;
}

inline int BigInt::bitLength() const
{
    if (d.empty()) return 0;
    uint32_t top = d.back();
    int bits = 32;
    while (top) { --bits; top >>= 1; }
    return int(d.size() - 1) * 32 + (32 - bits);
}

inline bool BigInt::getBit(int i) const
{
    int limb = i >> 5;
    if (limb < 0 || limb >= int(d.size())) return false;
    return (d[limb] >> (i & 31)) & 1u;
}

inline BigInt BigInt::fromHex(const QString &hex)
{
    BigInt x;
    QString h = hex.trimmed();
    if (h.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) h = h.mid(2);
    for (const QChar &ch : h) {
        ushort c = ch.unicode();
        int v = -1;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        if (v < 0) continue;
        x = mulSmall(x, 16);
        x = addSmall(x, uint32_t(v));
    }
    return x;
}

inline BigInt BigInt::fromBytes(const QByteArray &b)
{
    BigInt x;
    for (char ch : b) {
        x = mulSmall(x, 256);
        x = addSmall(x, uint32_t(uchar(ch)));
    }
    return x;
}

inline BigInt BigInt::divmodSmall(const BigInt &a, uint32_t m, uint32_t &rem)
{
    BigInt q;
    q.d.resize(a.d.size());
    uint64_t r = 0;
    for (int i = int(a.d.size()) - 1; i >= 0; --i) {
        uint64_t cur = r * BASE + a.d[i];
        q.d[i] = uint32_t(cur / m);
        r = cur % m;
    }
    rem = uint32_t(r);
    q.trim();
    return q;
}

inline QByteArray BigInt::toBytes() const
{
    if (d.empty()) return QByteArray(1, char(0));
    BigInt n = *this;
    QByteArray out;
    while (!n.d.empty()) {
        uint32_t rem = 0;
        n = divmodSmall(n, 256, rem);
        out.insert(0, char(uchar(rem)));
    }
    return out;
}

inline QString BigInt::toHex() const
{
    QByteArray bytes = toBytes();
    QString hex;
    for (uchar ch : bytes) hex += QStringLiteral("%1").arg(int(ch), 2, 16, QLatin1Char('0'));
    return hex;
}

// RSA 公钥加密（PKCS#1 v1.5）
class RsaPkcs1 {
public:
    // modulusHex / exponentHex：十六进制公钥（咪咕返回 hex）。plain：待加密明文（字节）。
    // 返回大写十六进制密文；k 为 modulus 字节长度，明文不得超过 k-11。
    static QString encryptHex(const QString &modulusHex, const QString &exponentHex,
                              const QByteArray &plain);
    static bool selfTest();
};

inline QString RsaPkcs1::encryptHex(const QString &modulusHex, const QString &exponentHex,
                                    const QByteArray &plain)
{
    BigInt n = BigInt::fromHex(modulusHex);
    BigInt e = BigInt::fromHex(exponentHex);
    if (n.isZero() || e.isZero()) return QString();
    const int k = n.toBytes().size();
    const int mLen = plain.size();
    if (mLen > k - 11) return QString();          // PKCS#1 v1.5 最少 11 字节填充

    // EM = 0x00 || 0x02 || PS || 0x00 || M
    QByteArray em;
    em.reserve(k);
    em.append(char(0x00));
    em.append(char(0x02));
    for (int i = 0; i < k - 3 - mLen; ++i) {
        char b = char(0);
        while (b == char(0)) b = char(QRandomGenerator::global()->bounded(1, 256));
        em.append(b);
    }
    em.append(char(0x00));
    em.append(plain);

    BigInt m = BigInt::fromBytes(em);
    BigInt c = m.modPow(e, n);
    QByteArray cb = c.toBytes();
    if (cb.size() < k) {
        QByteArray pad(k - cb.size(), char(0x00));
        cb = pad + cb;                              // 左侧补零到 k 字节
    }
    QString hex;
    for (uchar ch : cb)
        hex += QStringLiteral("%1").arg(int(ch), 2, 16, QLatin1Char('0')).toUpper();
    return hex;
}

inline bool RsaPkcs1::selfTest()
{
    // 大整数运算自检
    BigInt a = BigInt::fromHex(QStringLiteral("7"));
    BigInt b = BigInt::fromHex(QStringLiteral("d"));      // 13
    BigInt m = BigInt::fromHex(QStringLiteral("64"));     // 100
    if (a.modPow(b, m).toBytes() != QByteArray::fromHex("07")) return false;  // 7^13 mod 100 = 7

    BigInt p = BigInt::fromHex(QStringLiteral("2"));
    BigInt q = BigInt::fromHex(QStringLiteral("a"));      // 10
    BigInt r = BigInt::fromHex(QStringLiteral("3e8"));    // 1000
    if (p.modPow(q, r).toBytes() != QByteArray::fromHex("18")) return false;  // 2^10 mod 1000 = 24

    // PKCS#1 v1.5 结构自检：模数取 1024 位（与咪咕一致），密文必须是固定 128 字节（256 位十六进制）
    const QString mod = QString(256, QLatin1Char('f'));   // 2^1024-1，任意合法 1024 位模数
    QString enc = encryptHex(mod, QStringLiteral("10001"), QByteArray("hi"));
    if (enc.isEmpty()) return false;
    if (enc.size() != 256) return false;
    // 明文超过 k-11 时必须拒绝，否则会产出无法被服务端解密的块
    if (!encryptHex(mod, QStringLiteral("10001"), QByteArray(118, 'x')).isEmpty()) return false;
    return true;
}

} // namespace Furui
