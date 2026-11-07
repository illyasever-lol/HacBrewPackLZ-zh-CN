// AES, SHA-256 and RSA. The console has instructions for the first two, which libnx uses. On a PC this uses AES-NI and a
// plain SHA-256, which is only there so the engine can be tested.
#pragma once

#include <map>

#include "common.h"

namespace hbp {

void aesEcbEncrypt(const u8 key[16], u8* data, size_t size);
void aesEcbDecrypt(const u8 key[16], u8* data, size_t size);

/// AES-XTS over 0x200 byte sectors, with the big endian sector number tweak NCA headers use.
void aesXts(const u8 key[32], u8* data, size_t size, bool encrypt);

/// AES-CTR as NCA sections use it: the counter is the upper IV followed by the big endian file offset / 16.
class AesCtr {
public:
    explicit AesCtr(const u8 key[16]);
    ~AesCtr();
    AesCtr(const AesCtr&) = delete;
    /// Offset and size must be 16 byte aligned.
    void crypt(u8* data, size_t size, i64 fileOffset, u64 upperIv);

private:
    struct Impl;
    Impl* _impl;
};

void sha256(const void* data, size_t size, u8 out[32]);

class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    void update(const void* data, size_t size);
    void finish(u8 out[32]);

private:
    struct Impl;
    Impl* _impl;
};

/// The key the second NCA signature is made with, the same idea as hacBrewPack's fixed key: it isn't a secret, it only has
/// to match the public key that gets patched into main.npdm.
const u8* signingModulus();

/// RSA-2048-PSS with SHA-256 over this data, with the signing key above.
void signPss(const void* data, size_t size, u8 signature[0x100]);

/// Whether this is signed with the key releases are signed with: RSA-2048-PSS with SHA-256 against the public half built into the program.
bool verifyUpdateSignature(const void* data, size_t size, const u8 signature[0x100]);

/// RSA-2048 with a private exponent.
void rsaPrivate(const u8 modulus[0x100], const u8 exponent[0x100], const u8 input[0x100], u8 output[0x100]);

/// Takes OAEP padding with SHA-256 and an empty label off. False if it doesn't check out.
bool oaepUnpad(const u8 block[0x100], Bytes& message);

class Keyset {
public:
    /// Reads prod.keys, and title.keys from the same folder if it's there.
    void load(const std::string& path);
    /// Reads a list of keys. Later ones replace earlier ones.
    static void parse(const std::string& text, std::map<std::string, Bytes>& keys);

    const u8* headerKey() const;
    const u8* keyAreaKey(int keyAreaIndex, int generation) const;
    const u8* titleKek(int generation) const;
    const u8* titleKey(const std::string& rightsIdHex) const;
    const u8* find(const std::string& name, size_t size) const;
    bool empty() const { return _keys.empty(); }

private:
    std::map<std::string, Bytes> _keys;
    std::map<std::string, Bytes> _titleKeys;
};

}  // namespace hbp
