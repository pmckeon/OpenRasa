#include "AuthCrypt.h"

#include "BlowfishTables.h"

#include <cstdio>
#include <cstring>

namespace net {

// ---- Blowfish -----------------------------------------------------------------------------------------------------------------------

Blowfish::Blowfish(const uint8_t* key, size_t keyLength) {
    std::memcpy(p_, kBlowfishP, sizeof p_);
    std::memcpy(s_, kBlowfishS, sizeof s_);
    size_t j = 0;
    for (int i = 0; i < 18; ++i) {
        uint32_t d = 0;
        for (int k = 0; k < 4; ++k) {
            d = (d << 8) | key[j];
            j = (j + 1) % keyLength;
        }
        p_[i] ^= d;
    }
    uint32_t l = 0, r = 0;
    for (int i = 0; i < 18; i += 2) {
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
}

uint32_t Blowfish::f(uint32_t x) const {
    return ((s_[0][x >> 24] + s_[1][(x >> 16) & 0xFF]) ^ s_[2][(x >> 8) & 0xFF]) + s_[3][x & 0xFF];
}

void Blowfish::encryptBlock(uint32_t& left, uint32_t& right) const {
    uint32_t l = left, r = right;
    for (int i = 0; i < 16; ++i) {
        l ^= p_[i];
        r ^= f(l);
        std::swap(l, r);
    }
    std::swap(l, r);
    r ^= p_[16];
    l ^= p_[17];
    left = l;
    right = r;
}

void Blowfish::decryptBlock(uint32_t& left, uint32_t& right) const {
    uint32_t l = left, r = right;
    for (int i = 17; i > 1; --i) {
        l ^= p_[i];
        r ^= f(l);
        std::swap(l, r);
    }
    std::swap(l, r);
    r ^= p_[1];
    l ^= p_[0];
    left = l;
    right = r;
}

static uint32_t readLe(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
static void writeLe(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}

void Blowfish::encrypt(uint8_t* data, size_t length) const {
    for (size_t i = 0; i + 8 <= length; i += 8) {
        uint32_t l = readLe(data + i), r = readLe(data + i + 4);
        encryptBlock(l, r);
        writeLe(data + i, l);
        writeLe(data + i + 4, r);
    }
}

void Blowfish::decrypt(uint8_t* data, size_t length) const {
    for (size_t i = 0; i + 8 <= length; i += 8) {
        uint32_t l = readLe(data + i), r = readLe(data + i + 4);
        decryptBlock(l, r);
        writeLe(data + i, l);
        writeLe(data + i + 4, r);
    }
}

const Blowfish& authBlowfish() {
    static const uint8_t key[] = "[;'.]94-31==-%&@!^+]";   // 20 characters + the terminating zero = 21 bytes
    static const Blowfish cipher(key, sizeof key);
    return cipher;
}

std::vector<uint8_t> authEncrypt(const uint8_t* payload, size_t length) {
    size_t padded = (length + 7) & ~size_t(7);
    std::vector<uint8_t> body(padded + 8, 0);
    std::memcpy(body.data(), payload, length);
    for (size_t i = length; i < padded; ++i) body[i] = 0xCC;
    uint32_t sum = 0;
    for (size_t i = 0; i < padded; i += 4) sum ^= readLe(&body[i]);
    writeLe(&body[padded], sum);
    authBlowfish().encrypt(body.data(), body.size());
    return body;
}

bool authDecrypt(std::vector<uint8_t>& body) {
    if (body.size() < 8 || body.size() % 8 != 0) return false;
    authBlowfish().decrypt(body.data(), body.size());
    uint32_t sum = 0;
    for (size_t i = 0; i + 4 < body.size(); i += 4) sum ^= readLe(&body[i]);   // up to and including the checksum dword
    return sum == 0;
}

// ---- DES (FIPS 46-3) ----------------------------------------------------------------------------------------------------------------

namespace {

const uint8_t kIp[64] = {58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6,
                         64, 56, 48, 40, 32, 24, 16, 8, 57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3,
                         61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};
const uint8_t kFp[64] = {40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22, 62, 30,
                         37, 5, 45, 13, 53, 21, 61, 29, 36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
                         34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25};
const uint8_t kE[48] = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
                        16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
const uint8_t kP[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
                        2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6, 22, 11, 4,  25};
const uint8_t kPc1[56] = {57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18, 10, 2,  59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
                          63, 55, 47, 39, 31, 23, 15, 7,  62, 54, 46, 38, 30, 22, 14, 6,  61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4};
const uint8_t kPc2[48] = {14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,  26, 8,  16, 7,  27, 20, 13, 2,
                          41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
const uint8_t kShifts[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
const uint8_t kS[8][64] = {
    {14, 4,  13, 1, 2,  15, 11, 8,  3,  10, 6,  12, 5,  9,  0, 7,  0, 15, 7,  4,  14, 2,  13, 1,  10, 6, 12, 11, 9,  5,  3,  8,
     4,  1,  14, 8, 13, 6,  2,  11, 15, 12, 9,  7,  3,  10, 5, 0,  15, 12, 8,  2,  4,  9,  1,  7,  5,  11, 3,  14, 10, 0, 6,  13},
    {15, 1,  8,  14, 6,  11, 3,  4,  9,  7, 2,  13, 12, 0, 5,  10, 3, 13, 4,  7,  15, 2,  8,  14, 12, 0, 1,  10, 6,  9,  11, 5,
     0,  14, 7,  11, 10, 4,  13, 1,  5,  8, 12, 6,  9,  3, 2,  15, 13, 8, 10, 1,  3,  15, 4,  2,  11, 6, 7,  12, 0,  5,  14, 9},
    {10, 0,  9,  14, 6, 3,  15, 5,  1,  13, 12, 7,  11, 4,  2,  8,  13, 7,  0,  9,  3,  4,  6,  10, 2,  8,  5,  14, 12, 11, 15, 1,
     13, 6,  4,  9,  8, 15, 3,  0,  11, 1,  2,  12, 5,  10, 14, 7,  1,  10, 13, 0,  6,  9,  8,  7,  4,  15, 14, 3,  11, 5,  2,  12},
    {7,  13, 14, 3, 0,  6,  9,  10, 1,  2, 8, 5,  11, 12, 4,  15, 13, 8,  11, 5,  6,  15, 0,  3,  4,  7,  2,  12, 1,  10, 14, 9,
     10, 6,  9,  0, 12, 11, 7,  13, 15, 1, 3, 14, 5,  2,  8,  4,  3,  15, 0,  6,  10, 1,  13, 8,  9,  4,  5,  11, 12, 7,  2,  14},
    {2,  12, 4,  1,  7,  10, 11, 6,  8,  5,  3,  15, 13, 0, 14, 9,  14, 11, 2,  12, 4,  7,  13, 1,  5,  0,  15, 10, 3,  9,  8,  6,
     4,  2,  1,  11, 10, 13, 7,  8,  15, 9,  12, 5,  6,  3, 0,  14, 11, 8,  12, 7,  1,  14, 2,  13, 6,  15, 0,  9,  10, 4,  5,  3},
    {12, 1,  10, 15, 9, 2,  6,  8,  0,  13, 3,  4,  14, 7,  5,  11, 10, 15, 4,  2,  7,  12, 9,  5,  6,  1,  13, 14, 0,  11, 3,  8,
     9,  14, 15, 5,  2, 8,  12, 3,  7,  0,  4,  10, 1,  13, 11, 6,  4,  3,  2,  12, 9,  5,  15, 10, 11, 14, 1,  7,  6,  0,  8,  13},
    {4,  11, 2,  14, 15, 0, 8,  13, 3,  12, 9, 7,  5,  10, 6, 1,  13, 0,  11, 7,  4,  9,  1,  10, 14, 3,  5,  12, 2,  15, 8,  6,
     1,  4,  11, 13, 12, 3, 7,  14, 10, 15, 6, 8,  0,  5,  9, 2,  6,  11, 13, 8,  1,  4,  10, 7,  9,  5,  0,  15, 14, 2,  3,  12},
    {13, 2,  8,  4, 6,  15, 11, 1,  10, 9,  3,  14, 5,  0,  12, 7,  1,  15, 13, 8,  10, 3,  7,  4,  12, 5,  6,  11, 0,  14, 9,  2,
     7,  11, 4,  1, 9,  12, 14, 2,  0,  6,  10, 13, 15, 3,  5,  8,  2,  1,  14, 7,  4,  10, 8,  13, 15, 12, 9,  0,  3,  5,  6,  11}};

// Bit n (1-based, 1 = the most significant) of an inBits-wide value, gathered by a table into an outBits-wide value.
uint64_t permute(uint64_t in, int inBits, const uint8_t* table, int outBits) {
    uint64_t out = 0;
    for (int i = 0; i < outBits; ++i) out = (out << 1) | ((in >> (inBits - table[i])) & 1);
    return out;
}

}  // namespace

void desEncryptEcb(const uint8_t key[8], uint8_t* data, size_t length) {
    uint64_t k = 0;
    for (int i = 0; i < 8; ++i) k = (k << 8) | key[i];
    uint64_t cd = permute(k, 64, kPc1, 56);
    uint32_t c = uint32_t(cd >> 28) & 0x0FFFFFFF, d = uint32_t(cd) & 0x0FFFFFFF;
    uint64_t subkeys[16];
    for (int round = 0; round < 16; ++round) {
        for (int s = 0; s < kShifts[round]; ++s) {
            c = ((c << 1) | (c >> 27)) & 0x0FFFFFFF;
            d = ((d << 1) | (d >> 27)) & 0x0FFFFFFF;
        }
        subkeys[round] = permute((uint64_t(c) << 28) | d, 56, kPc2, 48);
    }
    for (size_t off = 0; off + 8 <= length; off += 8) {
        uint64_t block = 0;
        for (int i = 0; i < 8; ++i) block = (block << 8) | data[off + i];
        block = permute(block, 64, kIp, 64);
        uint32_t l = uint32_t(block >> 32), r = uint32_t(block);
        for (int round = 0; round < 16; ++round) {
            uint64_t x = permute(r, 32, kE, 48) ^ subkeys[round];
            uint32_t sOut = 0;
            for (int s = 0; s < 8; ++s) {
                uint32_t six = uint32_t(x >> (42 - 6 * s)) & 0x3F;
                uint32_t row = ((six >> 4) & 2) | (six & 1), col = (six >> 1) & 0xF;
                sOut = (sOut << 4) | kS[s][row * 16 + col];
            }
            uint32_t fOut = uint32_t(permute(sOut, 32, kP, 32));
            uint32_t next = l ^ fOut;
            l = r;
            r = next;
        }
        block = permute((uint64_t(r) << 32) | l, 64, kFp, 64);
        for (int i = 7; i >= 0; --i) {
            data[off + i] = uint8_t(block);
            block >>= 8;
        }
    }
}

bool authCryptSelfTest() {
    bool ok = true;
    // FIPS 46 worked example: key 133457799BBCDFF1, plaintext 0123456789ABCDEF -> 85E813540F0AB405.
    const uint8_t key[8] = {0x13, 0x34, 0x57, 0x79, 0x9B, 0xBC, 0xDF, 0xF1};
    uint8_t block[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    const uint8_t expected[8] = {0x85, 0xE8, 0x13, 0x54, 0x0F, 0x0A, 0xB4, 0x05};
    desEncryptEcb(key, block, 8);
    if (std::memcmp(block, expected, 8) != 0) {
        std::fprintf(stderr, "[net] DES self-test failed\n");
        ok = false;
    }
    // Rasa.NET's precomputed auth tables (Rasa.Utils/Cryptography/Auth/Blowfish.cs): PBox[0], PBox[17], SBox0[0], SBox1[0].
    const Blowfish& bf = authBlowfish();
    if (bf.p(0) != 0xAE915BC6 || bf.p(17) != 0x7B584F44 || bf.s(0, 0) != 0x8F9ED754 || bf.s(1, 0) != 0xD34E8403) {
        std::fprintf(stderr, "[net] Blowfish key schedule self-test failed\n");
        ok = false;
    }
    // A frame round trip.
    const uint8_t payload[10] = {5, 1, 2, 3, 4, 5, 6, 7, 8, 1};
    std::vector<uint8_t> body = authEncrypt(payload, sizeof payload);
    if (body.size() != 24 || !authDecrypt(body) || std::memcmp(body.data(), payload, sizeof payload) != 0) {
        std::fprintf(stderr, "[net] auth frame round trip failed\n");
        ok = false;
    }
    return ok;
}

}  // namespace net
