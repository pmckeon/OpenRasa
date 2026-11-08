// GameCrypt.h - the game connection's key exchange and encryption (tabula_rasa.exe TRasa::DHKeyExchanger and
// TRasa::BlowfishLiteEncryptor; Rasa.NET's server side: Rasa.Utils/Cryptography/DHKeyExchange.cs, GameCryptManager.cs, Game/Blowfish.cs).
//
// Key exchange (DHKeyExchanger, vtable 0x00bf9a88): the server's prime and generator are big-endian byte strings (slot 7 FUN_0070f7f0
// -> BN_bin2bn); the client draws its secret and sends its public key g^b mod p as minimal big-endian bytes (slot 4); the shared key
// is the server's public key ^ b mod p, minimal big-endian bytes (slot 8 FUN_0070f8a0).
// Encryptor (BlowfishLiteEncryptor, vtable 0x00bf9af4): initialised from the shared key string (slot 1 FUN_00710150): a 4-round
// Blowfish keyed by its first 8 bytes (the string repeated until it has 8; P = standard P[0..3] and P[16..17]; Rasa.NET Game/Blowfish.cs)
// and the MD5 of the whole string as the CBC starting block. Encrypt (slot 3 FUN_00710270): the message grows to (length & ~7) + 8,
// byte 0 = the padding count (1..8), the padding zeroed, the message at the end; then CBC, each block's two words big-endian, XORed
// with the previous ciphertext block (the first with MD5 bytes 0..7). Decrypt (slot 5 FUN_007103f0): a whole number of blocks; the
// first (byte 0 & 15) bytes dropped (more than 8: refused).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace net {

// Big-endian byte strings as unsigned numbers.
std::vector<uint8_t> modPow(const std::vector<uint8_t>& base, const std::vector<uint8_t>& exponent, const std::vector<uint8_t>& modulus);
std::vector<uint8_t> trimLeadingZeros(std::vector<uint8_t> v);

void md5(const uint8_t* data, size_t length, uint8_t out[16]);

class GameCipher {
public:
    void init(const std::vector<uint8_t>& sharedKey);
    std::vector<uint8_t> encrypt(const uint8_t* message, size_t length) const;
    bool decrypt(const std::vector<uint8_t>& frame, std::vector<uint8_t>& message) const;
    uint32_t p0() const { return p_[0]; }

private:
    void encryptBlock(uint32_t& l, uint32_t& r) const;
    void decryptBlock(uint32_t& l, uint32_t& r) const;
    uint32_t f(uint32_t x) const;
    uint32_t p_[6];
    uint32_t s_[4][256];
    uint32_t ivLeft_ = 0, ivRight_ = 0;
};

// The client's half of the exchange: a fresh secret below p, the public key to send, the shared key from the server's public key.
struct DhExchange {
    std::vector<uint8_t> prime, generator, secret;
    void start(const std::vector<uint8_t>& primeBytes, const std::vector<uint8_t>& generatorBytes);
    std::vector<uint8_t> publicKey() const;
    std::vector<uint8_t> sharedKey(const std::vector<uint8_t>& serverPublicKey) const;
};

// modPow, MD5 and the cipher against known values; false and a message on stderr when one is wrong.
bool gameCryptSelfTest();

}  // namespace net
