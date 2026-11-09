// Standalone, Qt-free BigInt test to validate the weapi RSA modpow path.
// Cross-checks operator*, operator%, modpow against Python reference.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <ctime>

static int g_fail = 0;
static std::string u32hex(uint32_t x) { char b[16]; snprintf(b, sizeof(b), "%x", x); return b; }
#define CHECK(cond, msg) do { if(!(cond)){ printf("FAIL: %s\n", (msg).c_str()); ++g_fail; } } while(0)

struct BigInt {
    std::vector<uint32_t> v; // little-endian, base 2^32
    BigInt() = default;
    explicit BigInt(std::vector<uint32_t> &&limbs) : v(std::move(limbs)) { trim(); }

    void trim() { while (!v.empty() && v.back() == 0) v.pop_back(); }
    bool isZero() const { return v.empty() || (v.size() == 1 && v[0] == 0); }

    int bitLength() const {
        if (v.empty()) return 0;
        uint32_t top = v.back();
        int p = 0;
        while (top) { top >>= 1; ++p; }
        return int(v.size() - 1) * 32 + p;
    }
    int bitAt(int idx) const {
        int limb = idx / 32;
        if (limb < 0 || limb >= (int)v.size()) return 0;
        return (v[limb] >> (idx % 32)) & 1;
    }

    static int hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    }

    // hex (big-endian text) -> little-endian limbs
    static BigInt fromHex(const std::string &hex) {
        std::string h = hex;
        if (h.size() >= 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h = h.substr(2);
        if (h.size() % 2) h = "0" + h;
        int nbytes = (int)h.size() / 2;
        std::vector<uint32_t> limbs((nbytes + 3) / 4, 0);
        for (int i = 0; i < nbytes; ++i) {
            int strPos = (int)h.size() - 2 - 2 * i;
            uint8_t b = (uint8_t)((hexVal(h[strPos]) << 4) | hexVal(h[strPos + 1]));
            int limbIdx = i / 4;
            int byteInLimb = i % 4;
            limbs[limbIdx] |= ((uint32_t)b) << (8 * byteInLimb);
        }
        BigInt r; r.v = limbs; r.trim();
        return r;
    }

    // bytes (big-endian array) -> little-endian limbs
    static BigInt fromBytes(const std::string &bytes) {
        int n = (int)bytes.size();
        std::vector<uint32_t> limbs((n + 3) / 4, 0);
        for (int i = 0; i < n; ++i) {
            uint8_t b = (uint8_t)bytes[i];
            int fromLsb = (n - 1) - i;
            int limbIdx = fromLsb / 4;
            int byteInLimb = fromLsb % 4;
            limbs[limbIdx] |= ((uint32_t)b) << (8 * byteInLimb);
        }
        BigInt r; r.v = limbs; r.trim();
        return r;
    }

    std::string toHex() const {
        if (v.empty()) return "0";
        char buf[9];
        std::string out;
        for (int i = (int)v.size() - 1; i >= 0; --i) {
            snprintf(buf, sizeof(buf), "%08x", v[i]);
            if (i == (int)v.size() - 1) {
                int s = 0; while (buf[s] == '0' && s < 7) ++s;
                out += (buf + s);
            } else {
                out += buf;
            }
        }
        return out;
    }
    std::string toHexPadded(int hexLen) const {
        std::string h = toHex();
        if ((int)h.size() < hexLen) h = std::string(hexLen - (int)h.size(), '0') + h;
        return h;
    }

    BigInt operator*(const BigInt &o) const {
        if (v.empty() || o.v.empty()) return BigInt();
        // +1 limb headroom: the carry-propagation loop can momentarily land one
        // limb past the 2N theoretical max; trim() removes it afterwards.
        std::vector<uint32_t> r(v.size() + o.v.size() + 1, 0);
        for (size_t i = 0; i < v.size(); ++i) {
            uint64_t carry = 0;
            for (size_t j = 0; j < o.v.size(); ++j) {
                uint64_t cur = (uint64_t)v[i] * o.v[j] + r[i + j] + carry;
                r[i + j] = (uint32_t)(cur & 0xFFFFFFFFu);
                carry = cur >> 32;
            }
            size_t k = i + o.v.size();
            while (carry && k < r.size()) {
                uint64_t cur = (uint64_t)r[k] + carry;
                r[k] = (uint32_t)(cur & 0xFFFFFFFFu);
                carry = cur >> 32;
                ++k;
            }
        }
        BigInt res; res.v = r; res.trim();
        return res;
    }

    BigInt sub(const BigInt &o) const {
        BigInt r;
        r.v.assign(std::max(v.size(), o.v.size()), 0);
        int64_t borrow = 0;
        for (size_t i = 0; i < r.v.size(); ++i) {
            int64_t a = (i < v.size()) ? (int64_t)v[i] : 0;
            int64_t b = (i < o.v.size()) ? (int64_t)o.v[i] : 0;
            int64_t diff = a - b - borrow;
            if (diff < 0) { diff += ((int64_t)1 << 32); borrow = 1; }
            else borrow = 0;
            r.v[i] = (uint32_t)(diff & 0xFFFFFFFFu);
        }
        r.trim();
        return r;
    }

    bool lessThan(const BigInt &o) const {
        if (v.size() != o.v.size()) return v.size() < o.v.size();
        for (int i = (int)v.size() - 1; i >= 0; --i)
            if (v[i] != o.v[i]) return v[i] < o.v[i];
        return false;
    }

    BigInt operator%(const BigInt &m) const {
        if (m.isZero()) return BigInt();
        BigInt r;
        int bits = bitLength();
        for (int i = bits - 1; i >= 0; --i) {
            BigInt shifted;
            shifted.v.assign(r.v.size() + 1, 0);
            for (size_t k = 0; k < r.v.size(); ++k) {
                uint64_t cur = (uint64_t)r.v[k] << 1;
                shifted.v[k] |= (uint32_t)(cur & 0xFFFFFFFFu);
                shifted.v[k + 1] |= (uint32_t)(cur >> 32);
            }
            shifted.trim();
            r = shifted;
            if (bitAt(i)) {
                if (r.v.empty()) r.v.push_back(1);
                else r.v[0] |= 1u;
            }
            if (!r.lessThan(m)) r = r.sub(m);
        }
        return r;
    }

    BigInt modpow(const BigInt &exp, const BigInt &mod) const {
        BigInt base = (*this) % mod;
        BigInt result = fromBytes(std::string("\x01", 1)) % mod;
        BigInt e = exp;
        while (!e.isZero()) {
            if (e.v.front() & 1u) result = (result * base) % mod;
            base = (base * base) % mod;
            for (size_t i = 0; i < e.v.size(); ++i) {
                uint32_t carry = (i + 1 < e.v.size()) ? ((e.v[i + 1] & 1u) << 31) : 0;
                e.v[i] = (e.v[i] >> 1) | carry;
            }
            if (!e.v.empty() && e.v.back() == 0) e.v.pop_back();
        }
        return result;
    }
};

int main() {
    std::srand((unsigned)std::time(0));
    fprintf(stderr, "[stage] start\n"); fflush(stderr);

    // 1) small known case: 13^65537 mod 17 = 13
    {
        fprintf(stderr, "[stage] small rsa\n"); fflush(stderr);
        BigInt a = BigInt::fromHex("0d");
        BigInt e = BigInt::fromHex("010001");
        BigInt m = BigInt::fromHex("11");
        BigInt r = a.modpow(e, m);
        CHECK(r.toHex() == "d", std::string("13^65537 mod 17 == 13, got ") + r.toHex());
    }

    // 2) random cross-check, small modulus, brute-force reference
    for (int t = 0; t < 200; ++t) {
        if (t % 50 == 0) { fprintf(stderr, "[stage] rand t=%d\n", t); fflush(stderr); }
        uint32_t modv = 2 + (std::rand() % 60000);
        uint32_t bv = std::rand() % modv;
        uint32_t ev = std::rand() % 30;
        uint32_t br = 1 % modv;
        uint32_t bb = bv % modv;
        for (uint32_t k = 0; k < ev; ++k) br = (uint64_t(br) * bb) % modv;
        BigInt B = BigInt::fromHex(u32hex(bv));
        BigInt E = BigInt::fromHex(u32hex(ev));
        BigInt M = BigInt::fromHex(u32hex(modv));
        BigInt R = B.modpow(E, M);
        std::string expHex = u32hex(br);
        CHECK(R.toHex() == expHex, std::string("modpow small t=") + std::to_string(t) +
             " R=" + R.toHex() + " exp=" + expHex);
    }

    // 3) Real Netease weapi RSA
    {
        fprintf(stderr, "[stage] real weapi rsa\n"); fflush(stderr);
        const char *modulus =
            "00e0b509f6259df8642dbc35662901477df22677ec152b5ff68ace615bb7b725152b3ab17a8"
            "76aea8a5aa76d2e417629ec4ee341f56135fccf695280104e0312ecbda92557c93870114af6"
            "c9d05c4f7f0c3685b7a46bee255932575cce10b424d813cfe4875d3e82047b97ddef52741d5"
            "46b8e289dc6935b3ece0462db0a22b8e7";
        std::string sk = "abcdefghijklmnop";
        std::string rev(sk.rbegin(), sk.rend());
        BigInt bi = BigInt::fromBytes(rev);
        BigInt e = BigInt::fromHex("010001");
        BigInt n = BigInt::fromHex(modulus);
        fprintf(stderr, "[stage] bi limbs=%zu n limbs=%zu\n", bi.v.size(), n.v.size()); fflush(stderr);
        BigInt r = bi.modpow(e, n);
        std::string enc = r.toHexPadded(256);
        printf("C++ encSecKey = %s\n", enc.c_str());
        // canonical 256-char modulus (value V), for copying into NeteaseCrypto.cpp
        printf("CANON_MODULUS = %s\n", n.toHexPadded(256).c_str());
        std::string cm = n.toHexPadded(256);
        printf("CANON_LEN = %zu\n", cm.size());
        // also a form guaranteed even and == V: prepend a '0'
        printf("CANON0 = 0%s\n", cm.c_str());
        printf("CANON0_LEN = %zu\n", cm.size() + 1);
        printf("expected       = d15a1683c992095d0c234c19966605c5c5964911268bbeda8cb8d08d834913e59d53b32358903a121b5fca784c1f5ae44951fd02524df58ecc98e52cc7cf8689b42c2e93ddf05b0592512d87f5960467e2f086c018849d76014d323500e30f13ef4cafbb0cf5a66731a3f1776c75ca35d0062dac70a3e33245afabcf47938487\n");
        CHECK(enc ==
            "d15a1683c992095d0c234c19966605c5c5964911268bbeda8cb8d08d834913e59d53b32358903a121b5fca784c1f5ae44951fd02524df58ecc98e52cc7cf8689b42c2e93ddf05b0592512d87f5960467e2f086c018849d76014d323500e30f13ef4cafbb0cf5a66731a3f1776c75ca35d0062dac70a3e33245afabcf47938487",
            std::string("real weapi encSecKey mismatch"));
    }

    printf(g_fail == 0 ? "ALL PASS\n" : "SOME FAIL\n");
    return g_fail == 0 ? 0 : 1;
}
