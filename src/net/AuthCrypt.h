// AuthCrypt.h - the ciphers of the authentication server's protocol.
//
// Frames (both ways, after the server's first, plain ProtocolVersion frame): the payload padded with 0xCC to a multiple of 8, a
// checksum dword (the XOR of every dword before it), one more dword (not checked), then Blowfish (16 rounds, ECB, each 8-byte block
// as two little-endian dwords) over all of it. The Blowfish key is NCsoft's fixed "[;'.]94-31==-%&@!^+]" with its terminating zero
// (21 bytes) - tabula_rasa.exe holds the string, and its key schedule over the standard tables reproduces Rasa.NET's precomputed
// tables (Rasa.Utils/Cryptography/Auth/Blowfish.cs) exactly. The login request's 30-byte name / password block has its first 24 bytes
// encrypted with single DES, ECB, key "TEST" and four zeroes (ncAuthClient FUN_00aa3670 -> FUN_00a91f30; key and length as Rasa.NET's
// LoginPacket reads it).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace net {

class Blowfish {
public:
    explicit Blowfish(const uint8_t* key, size_t keyLength);
    void encryptBlock(uint32_t& left, uint32_t& right) const;
    void decryptBlock(uint32_t& left, uint32_t& right) const;
    // In place over whole 8-byte blocks (length must be a multiple of 8).
    void encrypt(uint8_t* data, size_t length) const;
    void decrypt(uint8_t* data, size_t length) const;
    uint32_t p(int i) const { return p_[i]; }
    uint32_t s(int box, int i) const { return s_[box][i]; }

private:
    uint32_t f(uint32_t x) const;
    uint32_t p_[18];
    uint32_t s_[4][256];
};

// The auth protocol's Blowfish (the fixed key above).
const Blowfish& authBlowfish();

// A payload made into an encrypted frame body (padding, checksum, trailer, Blowfish).
std::vector<uint8_t> authEncrypt(const uint8_t* payload, size_t length);
// A received frame body decrypted in place; false when its size or checksum is wrong.
bool authDecrypt(std::vector<uint8_t>& body);

// Single DES, ECB, in place over whole 8-byte blocks (FIPS 46-3 bit order).
void desEncryptEcb(const uint8_t key[8], uint8_t* data, size_t length);

// Checks the ciphers against known values (DES's FIPS 46 example, Rasa.NET's first Blowfish table entries); false and a message
// on stderr when one is wrong.
bool authCryptSelfTest();

}  // namespace net
