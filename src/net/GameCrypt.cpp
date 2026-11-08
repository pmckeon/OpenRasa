#include "GameCrypt.h"

#include "BlowfishTables.h"

#include <cstdio>
#include <cstring>
#include <random>

namespace net {

// ---- Big numbers: modular power by Montgomery multiplication (an odd modulus; 32-bit limbs, least significant first) ----------------

namespace {

using Limbs = std::vector<uint32_t>;

Limbs toLimbs(const std::vector<uint8_t>& bigEndian, size_t count) {
    Limbs out(count, 0);
    for (size_t i = 0; i < bigEndian.size(); ++i) {
        const size_t bit = (bigEndian.size() - 1 - i) * 8;
        if (bit / 32 < count) out[bit / 32] |= uint32_t(bigEndian[i]) << (bit % 32);
    }
    return out;
}

std::vector<uint8_t> toBytes(const Limbs& v) {
    std::vector<uint8_t> out(v.size() * 4);
    for (size_t i = 0; i < v.size(); ++i)
        for (int b = 0; b < 4; ++b) out[out.size() - 1 - (i * 4 + b)] = uint8_t(v[i] >> (8 * b));
    return trimLeadingZeros(out);
}

bool geq(const Limbs& a, const Limbs& b) {   // same length
    for (size_t i = a.size(); i-- > 0;)
        if (a[i] != b[i]) return a[i] > b[i];
    return true;
}

void subtract(Limbs& a, const Limbs& b) {
    uint64_t borrow = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const uint64_t d = uint64_t(a[i]) - b[i] - borrow;
        a[i] = uint32_t(d);
        borrow = (d >> 63) & 1;
    }
}

// a x b x R^-1 mod m (CIOS); inputs below R with a x b < m R.
Limbs montMul(const Limbs& a, const Limbs& b, const Limbs& m, uint32_t mInv) {
    const size_t n = m.size();
    std::vector<uint64_t> t(n + 2, 0);
    for (size_t i = 0; i < n; ++i) {
        uint64_t carry = 0;
        for (size_t j = 0; j < n; ++j) {
            const uint64_t s = t[j] + uint64_t(a[j]) * b[i] + carry;
            t[j] = uint32_t(s);
            carry = s >> 32;
        }
        uint64_t s = t[n] + carry;
        t[n] = uint32_t(s);
        t[n + 1] = s >> 32;
        const uint32_t q = uint32_t(t[0]) * mInv;
        carry = (t[0] + uint64_t(q) * m[0]) >> 32;
        for (size_t j = 1; j < n; ++j) {
            const uint64_t s2 = t[j] + uint64_t(q) * m[j] + carry;
            t[j - 1] = uint32_t(s2);
            carry = s2 >> 32;
        }
        s = t[n] + carry;
        t[n - 1] = uint32_t(s);
        t[n] = t[n + 1] + (s >> 32);
    }
    Limbs out(n);
    for (size_t i = 0; i < n; ++i) out[i] = uint32_t(t[i]);
    if (t[n] != 0 || geq(out, m)) subtract(out, m);
    return out;
}

}  // namespace

std::vector<uint8_t> trimLeadingZeros(std::vector<uint8_t> v) {
    size_t z = 0;
    while (z < v.size() && v[z] == 0) ++z;
    v.erase(v.begin(), v.begin() + z);
    return v;
}

std::vector<uint8_t> modPow(const std::vector<uint8_t>& base, const std::vector<uint8_t>& exponent, const std::vector<uint8_t>& modulus) {
    const std::vector<uint8_t> mod = trimLeadingZeros(modulus);
    if (mod.empty() || (mod.back() & 1) == 0) return {};   // Montgomery needs an odd modulus (the shipped primes are)
    const size_t n = (mod.size() + 3) / 4;
    const Limbs m = toLimbs(mod, n);
    // -m^-1 mod 2^32 by Newton's iteration.
    uint32_t inv = 1;
    for (int i = 0; i < 5; ++i) inv *= 2 - m[0] * inv;
    const uint32_t mInv = ~inv + 1;
    // R^2 mod m, R = 2^(32 n): 1 doubled 2 x 32 n times.
    Limbs r2(n, 0);
    r2[0] = 1;
    for (size_t i = 0; i < 64 * n; ++i) {
        uint32_t carry = 0;
        for (size_t j = 0; j < n; ++j) {
            const uint32_t next = r2[j] >> 31;
            r2[j] = (r2[j] << 1) | carry;
            carry = next;
        }
        if (carry || geq(r2, m)) subtract(r2, m);
    }
    Limbs b = toLimbs(trimLeadingZeros(base), n);
    while (geq(b, m)) subtract(b, m);   // a base below R; this is only for one at or above m
    const Limbs one = [&] {
        Limbs o(n, 0);
        o[0] = 1;
        return o;
    }();
    Limbs x = montMul(b, r2, m, mInv);        // base in Montgomery form
    Limbs acc = montMul(one, r2, m, mInv);    // 1 in Montgomery form
    for (uint8_t byte : exponent)
        for (int bit = 7; bit >= 0; --bit) {
            acc = montMul(acc, acc, m, mInv);
            if ((byte >> bit) & 1) acc = montMul(acc, x, m, mInv);
        }
    return toBytes(montMul(acc, one, m, mInv));
}

// ---- MD5 (RFC 1321) -----------------------------------------------------------------------------------------------------------------

void md5(const uint8_t* data, size_t length, uint8_t out[16]) {
    static const uint32_t k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
        0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
        0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int shift[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                                  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    std::vector<uint8_t> msg(data, data + length);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    const uint64_t bits = uint64_t(length) * 8;
    for (int i = 0; i < 8; ++i) msg.push_back(uint8_t(bits >> (8 * i)));
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(msg[chunk + i * 4]) | uint32_t(msg[chunk + i * 4 + 1]) << 8 | uint32_t(msg[chunk + i * 4 + 2]) << 16 |
                   uint32_t(msg[chunk + i * 4 + 3]) << 24;
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t fv;
            int g;
            if (i < 16) fv = (b & c) | (~b & d), g = i;
            else if (i < 32) fv = (d & b) | (~d & c), g = (5 * i + 1) % 16;
            else if (i < 48) fv = b ^ c ^ d, g = (3 * i + 5) % 16;
            else fv = c ^ (b | ~d), g = (7 * i) % 16;
            const uint32_t tmp = d;
            d = c;
            c = b;
            const uint32_t sum = a + fv + k[i] + w[g];
            b = b + ((sum << shift[i]) | (sum >> (32 - shift[i])));
            a = tmp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = uint8_t(h[i] >> (8 * j));
}

// ---- The 4-round Blowfish in CBC ----------------------------------------------------------------------------------------------------

uint32_t GameCipher::f(uint32_t x) const {
    return ((s_[0][x >> 24] + s_[1][(x >> 16) & 0xFF]) ^ s_[2][(x >> 8) & 0xFF]) + s_[3][x & 0xFF];
}

// Rasa.NET Game/Blowfish.cs EncryptBlock (the words returned swapped, as it stores them).
void GameCipher::encryptBlock(uint32_t& first, uint32_t& second) const {
    uint32_t l = first ^ p_[0], r = second;
    r ^= p_[1] ^ f(l);
    l ^= p_[2] ^ f(r);
    r ^= p_[3] ^ f(l);
    l ^= p_[4] ^ f(r);
    r ^= p_[5];
    first = r;
    second = l;
}

void GameCipher::decryptBlock(uint32_t& first, uint32_t& second) const {
    uint32_t l = first ^ p_[5], r = second;
    r ^= p_[4] ^ f(l);
    l ^= p_[3] ^ f(r);
    r ^= p_[2] ^ f(l);
    l ^= p_[1] ^ f(r);
    r ^= p_[0];
    first = r;
    second = l;
}

void GameCipher::init(const std::vector<uint8_t>& sharedKey) {
    std::vector<uint8_t> key = sharedKey;
    while (!sharedKey.empty() && key.size() < 8) key.insert(key.end(), sharedKey.begin(), sharedKey.end());
    // The 4-round cipher's P: the standard P-array's first four entries and its last two (Rasa.NET Game/Blowfish.cs BfInit).
    const uint32_t initial[6] = {kBlowfishP[0], kBlowfishP[1], kBlowfishP[2], kBlowfishP[3], kBlowfishP[16], kBlowfishP[17]};
    std::memcpy(p_, initial, sizeof p_);
    std::memcpy(s_, kBlowfishS, sizeof s_);
    size_t at = 0;
    for (int i = 0; i < 6; ++i) {
        uint32_t d = 0;
        for (int b = 0; b < 4; ++b) {
            d = (d << 8) | (key.empty() ? 0 : key[at]);
            at = (at + 1) % 8;
        }
        p_[i] ^= d;
    }
    uint32_t l = 0, r = 0;
    // Rasa.NET's SetKey keeps the block in its stored (swapped) order between steps.
    for (int i = 0; i < 6; i += 2) {
        encryptBlock(l, r);
        p_[i] = l;
        p_[i + 1] = r;
    }
    for (int box = 0; box < 4; ++box)
        for (int i = 0; i < 256; i += 2) {
            encryptBlock(l, r);
            s_[box][i] = l;
            s_[box][i + 1] = r;
        }
    uint8_t digest[16];
    md5(sharedKey.data(), sharedKey.size(), digest);
    ivLeft_ = uint32_t(digest[0]) << 24 | uint32_t(digest[1]) << 16 | uint32_t(digest[2]) << 8 | digest[3];
    ivRight_ = uint32_t(digest[4]) << 24 | uint32_t(digest[5]) << 16 | uint32_t(digest[6]) << 8 | digest[7];
}

static uint32_t readBe(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
static void writeBe(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

std::vector<uint8_t> GameCipher::encrypt(const uint8_t* message, size_t length) const {
    const size_t size = (length & ~size_t(7)) + 8;
    std::vector<uint8_t> out(size, 0);
    out[0] = uint8_t(size - length);
    std::memcpy(out.data() + (size - length), message, length);
    uint32_t x = ivLeft_, y = ivRight_;
    for (size_t i = 0; i < size; i += 8) {
        uint32_t first = readBe(&out[i]) ^ x, second = readBe(&out[i + 4]) ^ y;
        encryptBlock(first, second);
        x = first;
        y = second;
        writeBe(&out[i], first);
        writeBe(&out[i + 4], second);
    }
    return out;
}

bool GameCipher::decrypt(const std::vector<uint8_t>& frame, std::vector<uint8_t>& message) const {
    if (frame.empty() || frame.size() % 8 != 0) return false;
    std::vector<uint8_t> plain(frame.size());
    uint32_t x = ivLeft_, y = ivRight_;
    for (size_t i = 0; i < frame.size(); i += 8) {
        uint32_t first = readBe(&frame[i]), second = readBe(&frame[i + 4]);
        const uint32_t nextX = first, nextY = second;
        decryptBlock(first, second);
        writeBe(&plain[i], first ^ x);
        writeBe(&plain[i + 4], second ^ y);
        x = nextX;
        y = nextY;
    }
    const size_t padding = plain[0] & 0xF;
    if (padding > 8) return false;
    message.assign(plain.begin() + padding, plain.end());
    return true;
}

// ---- The client's half of the key exchange ------------------------------------------------------------------------------------------

void DhExchange::start(const std::vector<uint8_t>& primeBytes, const std::vector<uint8_t>& generatorBytes) {
    prime = trimLeadingZeros(primeBytes);
    generator = trimLeadingZeros(generatorBytes);
    // A secret one bit shorter than the prime (as OpenSSL's DH_generate_key draws it), never 0.
    std::random_device rd;
    secret.assign(prime.size(), 0);
    for (uint8_t& b : secret) b = uint8_t(rd());
    if (!secret.empty()) {
        secret[0] &= uint8_t(prime[0] >> 1);
        secret.back() |= 1;
    }
}

std::vector<uint8_t> DhExchange::publicKey() const { return modPow(generator, secret, prime); }
std::vector<uint8_t> DhExchange::sharedKey(const std::vector<uint8_t>& serverPublicKey) const {
    return modPow(serverPublicKey, secret, prime);
}

bool gameCryptSelfTest() {
    bool ok = true;
    if (modPow({4}, {13}, {0x01, 0xF1}) != std::vector<uint8_t>{0x01, 0xBD}) {   // 4^13 mod 497 = 445
        std::fprintf(stderr, "[net] modPow self-test failed\n");
        ok = false;
    }
    uint8_t digest[16];
    md5(reinterpret_cast<const uint8_t*>("abc"), 3, digest);
    const uint8_t expected[16] = {0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0, 0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72};
    if (std::memcmp(digest, expected, 16) != 0) {
        std::fprintf(stderr, "[net] MD5 self-test failed\n");
        ok = false;
    }
    // Rasa.NET's group: both sides reach the same key.
    static const std::vector<uint8_t> prime = {
        0x98, 0x0F, 0x91, 0xEA, 0xAD, 0xAD, 0x8E, 0x7D, 0xF9, 0xEC, 0x43, 0x1D, 0xD4, 0x1C, 0xEF, 0x3F, 0xBE, 0xCF, 0xD1, 0xAE, 0xD2, 0x77,
        0x1C, 0xCF, 0xF8, 0x5E, 0xF8, 0x85, 0x3E, 0x2F, 0x9B, 0xC8, 0x30, 0x2E, 0xD3, 0xC4, 0x7F, 0xE6, 0x29, 0x72, 0xE0, 0x08, 0xE9, 0x32,
        0x53, 0x97, 0xDB, 0x41, 0x37, 0x98, 0xB3, 0x8A, 0xDC, 0xB8, 0xAF, 0xD3, 0x6A, 0x69, 0xD5, 0x12, 0xEC, 0x32, 0x61, 0xAF};
    DhExchange a, b;
    a.start(prime, {5});
    b.start(prime, {5});
    if (a.sharedKey(b.publicKey()) != b.sharedKey(a.publicKey())) {
        std::fprintf(stderr, "[net] Diffie-Hellman self-test failed\n");
        ok = false;
    }
    GameCipher cipher;
    cipher.init(a.sharedKey(b.publicKey()));
    const uint8_t text[] = "\x02\x00" "ENC OK";
    std::vector<uint8_t> back;
    if (!cipher.decrypt(cipher.encrypt(reinterpret_cast<const uint8_t*>("ENC OK"), 6), back) || back.size() != 6 ||
        std::memcmp(back.data(), "ENC OK", 6) != 0) {
        std::fprintf(stderr, "[net] game cipher round trip failed\n");
        ok = false;
    }
    (void)text;
    // Known values from an independent reimplementation of Rasa.NET's GameCryptManager (key = bytes 1..64): MD5 of the key, and
    // "\x02\x00ENC OK" encrypted.
    std::vector<uint8_t> key(64);
    for (int i = 0; i < 64; ++i) key[i] = uint8_t(i + 1);
    md5(key.data(), key.size(), digest);
    const uint8_t keyDigest[16] = {0x50, 0xe8, 0x45, 0xbf, 0xe6, 0xc7, 0x0f, 0xee, 0x66, 0x3a, 0xb5, 0x50, 0x0f, 0xd0, 0xc4, 0x11};
    if (std::memcmp(digest, keyDigest, 16) != 0) {
        std::fprintf(stderr, "[net] MD5 (64 bytes) self-test failed\n");
        ok = false;
    }
    GameCipher known;
    known.init(key);
    const std::vector<uint8_t> frame = known.encrypt(reinterpret_cast<const uint8_t*>("ENC OK"), 6);
    const uint8_t expectedFrame[8] = {0x8d, 0xa0, 0xa7, 0x78, 0xec, 0xbd, 0x29, 0x8b};
    if (frame.size() != 8 || std::memcmp(frame.data(), expectedFrame, 8) != 0) {
        std::fprintf(stderr, "[net] game cipher known-value self-test failed:");
        for (uint8_t c : frame) std::fprintf(stderr, " %02x", c);
        std::fprintf(stderr, " (P0 %08x)\n", known.p0());
        ok = false;
    }
    return ok;
}

}  // namespace net
