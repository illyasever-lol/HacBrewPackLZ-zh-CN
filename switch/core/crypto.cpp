#include "crypto.h"

#include <cctype>

#include "sys.h"

#ifdef __SWITCH__
#include <switch.h>
#else
#include <immintrin.h>
#if defined(__GNUC__) || defined(__clang__)
#define AES_TARGET AES_TARGET
#else
#define AES_TARGET
#endif
#endif

namespace hbp {

#ifdef __SWITCH__

void aesEcbEncrypt(const u8 key[16], u8* data, size_t size) {
    Aes128Context context;
    aes128ContextCreate(&context, key, true);
    for (size_t i = 0; i + 16 <= size; i += 16)
        aes128EncryptBlock(&context, data + i, data + i);
}

void aesEcbDecrypt(const u8 key[16], u8* data, size_t size) {
    Aes128Context context;
    aes128ContextCreate(&context, key, false);
    for (size_t i = 0; i + 16 <= size; i += 16)
        aes128DecryptBlock(&context, data + i, data + i);
}

void aesXts(const u8 key[32], u8* data, size_t size, bool encrypt) {
    Aes128XtsContext context;
    aes128XtsContextCreate(&context, key, key + 16, encrypt);
    for (size_t sector = 0; sector * 0x200 < size; sector++) {
        aes128XtsContextResetSector(&context, sector, true);
        if (encrypt)
            aes128XtsEncrypt(&context, data + sector * 0x200, data + sector * 0x200, 0x200);
        else
            aes128XtsDecrypt(&context, data + sector * 0x200, data + sector * 0x200, 0x200);
    }
}

struct AesCtr::Impl {
    Aes128CtrContext context;
};

AesCtr::AesCtr(const u8 key[16]) : _impl(new Impl) {
    u8 zero[16] = {};
    aes128CtrContextCreate(&_impl->context, key, zero);
}

AesCtr::~AesCtr() { delete _impl; }

void AesCtr::crypt(u8* data, size_t size, i64 fileOffset, u64 upperIv) {
    u8 counter[16];
    wrBe64(counter, upperIv);
    wrBe64(counter + 8, (u64)fileOffset >> 4);
    // A copy of its own, as several threads can be doing this with the one key.
    Aes128CtrContext context = _impl->context;
    aes128CtrContextResetCtr(&context, counter);
    aes128CtrCrypt(&context, data, data, size);
}

void sha256(const void* data, size_t size, u8 out[32]) { sha256CalculateHash(out, data, size); }

struct Sha256::Impl {
    Sha256Context context;
};

Sha256::Sha256() : _impl(new Impl) { sha256ContextCreate(&_impl->context); }
Sha256::~Sha256() { delete _impl; }
void Sha256::update(const void* data, size_t size) { sha256ContextUpdate(&_impl->context, data, size); }
void Sha256::finish(u8 out[32]) { sha256ContextGetHash(&_impl->context, out); }

#else

namespace {

struct AesKeys {
    __m128i encrypt[11];
    __m128i decrypt[11];
};

template <int Rcon>
AES_TARGET __m128i expandStep(__m128i key) {
    __m128i assist = _mm_aeskeygenassist_si128(key, Rcon);
    assist = _mm_shuffle_epi32(assist, 0xFF);
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    key = _mm_xor_si128(key, _mm_slli_si128(key, 4));
    return _mm_xor_si128(key, assist);
}

AES_TARGET void expandKey(const u8 key[16], AesKeys& keys) {
    __m128i* e = keys.encrypt;
    e[0] = _mm_loadu_si128((const __m128i*)key);
    e[1] = expandStep<0x01>(e[0]);
    e[2] = expandStep<0x02>(e[1]);
    e[3] = expandStep<0x04>(e[2]);
    e[4] = expandStep<0x08>(e[3]);
    e[5] = expandStep<0x10>(e[4]);
    e[6] = expandStep<0x20>(e[5]);
    e[7] = expandStep<0x40>(e[6]);
    e[8] = expandStep<0x80>(e[7]);
    e[9] = expandStep<0x1B>(e[8]);
    e[10] = expandStep<0x36>(e[9]);
    keys.decrypt[0] = e[10];
    for (int i = 1; i < 10; i++)
        keys.decrypt[i] = _mm_aesimc_si128(e[10 - i]);
    keys.decrypt[10] = e[0];
}

AES_TARGET inline __m128i encryptBlock(const AesKeys& keys, __m128i block) {
    block = _mm_xor_si128(block, keys.encrypt[0]);
    for (int i = 1; i < 10; i++)
        block = _mm_aesenc_si128(block, keys.encrypt[i]);
    return _mm_aesenclast_si128(block, keys.encrypt[10]);
}

AES_TARGET inline __m128i decryptBlock(const AesKeys& keys, __m128i block) {
    block = _mm_xor_si128(block, keys.decrypt[0]);
    for (int i = 1; i < 10; i++)
        block = _mm_aesdec_si128(block, keys.decrypt[i]);
    return _mm_aesdeclast_si128(block, keys.decrypt[10]);
}

}  // namespace

AES_TARGET void aesEcbEncrypt(const u8 key[16], u8* data, size_t size) {
    AesKeys keys;
    expandKey(key, keys);
    for (size_t i = 0; i + 16 <= size; i += 16)
        _mm_storeu_si128((__m128i*)(data + i), encryptBlock(keys, _mm_loadu_si128((const __m128i*)(data + i))));
}

AES_TARGET void aesEcbDecrypt(const u8 key[16], u8* data, size_t size) {
    AesKeys keys;
    expandKey(key, keys);
    for (size_t i = 0; i + 16 <= size; i += 16)
        _mm_storeu_si128((__m128i*)(data + i), decryptBlock(keys, _mm_loadu_si128((const __m128i*)(data + i))));
}

void aesXts(const u8 key[32], u8* data, size_t size, bool encrypt) {
    for (size_t sector = 0; sector * 0x200 < size; sector++) {
        u8 tweak[16] = {};
        wrBe64(tweak + 8, sector);
        aesEcbEncrypt(key + 16, tweak, 16);

        u8 masks[0x200];
        for (int block = 0; block < 0x200; block += 16) {
            memcpy(masks + block, tweak, 16);
            int carry = 0;
            for (int i = 0; i < 16; i++) {
                int next = tweak[i] >> 7;
                tweak[i] = (u8)(tweak[i] << 1 | carry);
                carry = next;
            }
            if (carry)
                tweak[0] ^= 0x87;
        }

        u8* s = data + sector * 0x200;
        for (int i = 0; i < 0x200; i++)
            s[i] ^= masks[i];
        if (encrypt)
            aesEcbEncrypt(key, s, 0x200);
        else
            aesEcbDecrypt(key, s, 0x200);
        for (int i = 0; i < 0x200; i++)
            s[i] ^= masks[i];
    }
}

struct AesCtr::Impl {
    AesKeys keys;
};

AesCtr::AesCtr(const u8 key[16]) : _impl(new Impl) { expandKey(key, _impl->keys); }
AesCtr::~AesCtr() { delete _impl; }

AES_TARGET void AesCtr::crypt(u8* data, size_t size, i64 fileOffset, u64 upperIv) {
    u64 block = (u64)fileOffset >> 4;
    u8 counter[16];
    wrBe64(counter, upperIv);
    for (size_t i = 0; i + 16 <= size; i += 16) {
        wrBe64(counter + 8, block++);
        __m128i mask = encryptBlock(_impl->keys, _mm_loadu_si128((const __m128i*)counter));
        _mm_storeu_si128((__m128i*)(data + i), _mm_xor_si128(_mm_loadu_si128((const __m128i*)(data + i)), mask));
    }
}

namespace {

const u32 ShaConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline u32 rotr(u32 x, int n) { return x >> n | x << (32 - n); }

void shaBlocks(u32 state[8], const u8* data, size_t blocks) {
    while (blocks--) {
        u32 w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (u32)data[i * 4] << 24 | (u32)data[i * 4 + 1] << 16 | (u32)data[i * 4 + 2] << 8 | data[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ w[i - 15] >> 3;
            u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ w[i - 2] >> 10;
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; i++) {
            u32 t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + ShaConstants[i] + w[i];
            u32 t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
        data += 64;
    }
}

}  // namespace

struct Sha256::Impl {
    u32 state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    u8 partial[64];
    size_t partialSize = 0;
    u64 total = 0;
};

Sha256::Sha256() : _impl(new Impl) {}
Sha256::~Sha256() { delete _impl; }

void Sha256::update(const void* data, size_t size) {
    const u8* d = (const u8*)data;
    _impl->total += size;
    if (_impl->partialSize > 0) {
        size_t n = 64 - _impl->partialSize < size ? 64 - _impl->partialSize : size;
        memcpy(_impl->partial + _impl->partialSize, d, n);
        _impl->partialSize += n;
        d += n;
        size -= n;
        if (_impl->partialSize < 64)
            return;
        shaBlocks(_impl->state, _impl->partial, 1);
        _impl->partialSize = 0;
    }
    shaBlocks(_impl->state, d, size / 64);
    d += size / 64 * 64;
    size %= 64;
    memcpy(_impl->partial, d, size);
    _impl->partialSize = size;
}

void Sha256::finish(u8 out[32]) {
    u64 bits = _impl->total * 8;
    u8 pad[72] = {0x80};
    size_t padding = (_impl->total % 64 < 56 ? 56 : 120) - _impl->total % 64;
    update(pad, padding);
    u8 length[8];
    wrBe64(length, bits);
    update(length, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (u8)(_impl->state[i] >> 24);
        out[i * 4 + 1] = (u8)(_impl->state[i] >> 16);
        out[i * 4 + 2] = (u8)(_impl->state[i] >> 8);
        out[i * 4 + 3] = (u8)_impl->state[i];
    }
}

void sha256(const void* data, size_t size, u8 out[32]) {
    Sha256 hash;
    hash.update(data, size);
    hash.finish(out);
}

#endif

// -------------------------------------------------------------------------------------------------------------------
// RSA: Montgomery multiplication over 32 bit digits, which needs nothing from the compiler and is quick enough for the
// one signature a title takes.

namespace {

constexpr int Digits = 64;

struct Big {
    u32 d[Digits];
};

void fromBytes(Big& out, const u8 bytes[0x100]) {
    for (int i = 0; i < Digits; i++) {
        const u8* b = bytes + 0x100 - 4 - i * 4;
        out.d[i] = (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
    }
}

void toBytes(u8 bytes[0x100], const Big& in) {
    for (int i = 0; i < Digits; i++) {
        u8* b = bytes + 0x100 - 4 - i * 4;
        b[0] = (u8)(in.d[i] >> 24);
        b[1] = (u8)(in.d[i] >> 16);
        b[2] = (u8)(in.d[i] >> 8);
        b[3] = (u8)in.d[i];
    }
}

bool lessThan(const Big& a, const Big& b) {
    for (int i = Digits - 1; i >= 0; i--) {
        if (a.d[i] != b.d[i])
            return a.d[i] < b.d[i];
    }
    return false;
}

void subtract(Big& a, const Big& b) {
    u64 borrow = 0;
    for (int i = 0; i < Digits; i++) {
        u64 value = (u64)a.d[i] - b.d[i] - borrow;
        a.d[i] = (u32)value;
        borrow = value >> 63;
    }
}

u32 inverseOfDigit(u32 n0) {
    u32 x = 1;
    for (int i = 0; i < 5; i++)
        x *= 2 - n0 * x;
    return (u32)(0 - x);
}

/// out = a * b / 2^2048 mod n
void montgomery(Big& out, const Big& a, const Big& b, const Big& n, u32 n0inv) {
    u32 t[Digits + 2] = {};
    for (int i = 0; i < Digits; i++) {
        u64 carry = 0;
        for (int j = 0; j < Digits; j++) {
            u64 value = (u64)a.d[j] * b.d[i] + t[j] + carry;
            t[j] = (u32)value;
            carry = value >> 32;
        }
        u64 top = (u64)t[Digits] + carry;
        t[Digits] = (u32)top;
        t[Digits + 1] = (u32)(top >> 32);

        u32 m = t[0] * n0inv;
        u64 value = (u64)m * n.d[0] + t[0];
        carry = value >> 32;
        for (int j = 1; j < Digits; j++) {
            value = (u64)m * n.d[j] + t[j] + carry;
            t[j - 1] = (u32)value;
            carry = value >> 32;
        }
        top = (u64)t[Digits] + carry;
        t[Digits - 1] = (u32)top;
        t[Digits] = t[Digits + 1] + (u32)(top >> 32);
    }

    Big result;
    memcpy(result.d, t, sizeof(result.d));
    if (t[Digits] != 0 || !lessThan(result, n))
        subtract(result, n);
    out = result;
}

}  // namespace

void rsaPrivate(const u8 modulus[0x100], const u8 exponent[0x100], const u8 input[0x100], u8 output[0x100]) {
    Big n, base;
    fromBytes(n, modulus);
    fromBytes(base, input);
    u32 inv = inverseOfDigit(n.d[0]);

    // R mod n and R^2 mod n, by doubling.
    Big one = {};
    {
        // R mod n = 2^2048 mod n
        Big r = {};
        r.d[0] = 1;
        for (int i = 0; i < 2048; i++) {
            u32 carry = 0;
            for (int j = 0; j < Digits; j++) {
                u32 next = r.d[j] >> 31;
                r.d[j] = r.d[j] << 1 | carry;
                carry = next;
            }
            if (carry || !lessThan(r, n))
                subtract(r, n);
        }
        one = r;
    }
    Big rr = one;
    for (int i = 0; i < 2048; i++) {
        u32 carry = 0;
        for (int j = 0; j < Digits; j++) {
            u32 next = rr.d[j] >> 31;
            rr.d[j] = rr.d[j] << 1 | carry;
            carry = next;
        }
        if (carry || !lessThan(rr, n))
            subtract(rr, n);
    }

    Big result = one;
    Big power;
    montgomery(power, base, rr, n, inv);
    for (int byte = 0; byte < 0x100; byte++) {
        for (int bit = 7; bit >= 0; bit--) {
            montgomery(result, result, result, n, inv);
            if (exponent[byte] >> bit & 1)
                montgomery(result, result, power, n, inv);
        }
    }

    Big unit = {};
    unit.d[0] = 1;
    montgomery(result, result, unit, n, inv);
    toBytes(output, result);
}

namespace {

// Made for this tool. It isn't a secret: see signingModulus.
#include "signing_key.inc"

#include "update_key.inc"

void mgf1(const u8* seed, size_t seedSize, u8* mask, size_t maskSize) {
    u8 block[32];
    Bytes input(seedSize + 4);
    memcpy(input.data(), seed, seedSize);
    for (u32 counter = 0, done = 0; done < maskSize; counter++) {
        input[seedSize] = (u8)(counter >> 24);
        input[seedSize + 1] = (u8)(counter >> 16);
        input[seedSize + 2] = (u8)(counter >> 8);
        input[seedSize + 3] = (u8)counter;
        sha256(input.data(), input.size(), block);
        for (u32 i = 0; i < 32 && done < maskSize; i++, done++)
            mask[done] ^= block[i];
    }
}

}  // namespace

const u8* signingModulus() { return SigningModulus; }

void signPss(const void* data, size_t size, u8 signature[0x100]) {
    constexpr int HashSize = 32;
    constexpr int SaltSize = 32;
    constexpr int DbSize = 0x100 - HashSize - 1;

    u8 messageHash[HashSize];
    sha256(data, size, messageHash);
    u8 salt[SaltSize];
    sys::randomBytes(salt, sizeof(salt));

    u8 prefixed[8 + HashSize + SaltSize] = {};
    memcpy(prefixed + 8, messageHash, HashSize);
    memcpy(prefixed + 8 + HashSize, salt, SaltSize);
    u8 h[HashSize];
    sha256(prefixed, sizeof(prefixed), h);

    u8 block[0x100] = {};
    block[DbSize - SaltSize - 1] = 1;
    memcpy(block + DbSize - SaltSize, salt, SaltSize);
    mgf1(h, HashSize, block, DbSize);
    block[0] &= 0x7F;
    memcpy(block + DbSize, h, HashSize);
    block[0xFF] = 0xBC;

    rsaPrivate(SigningModulus, SigningExponent, block, signature);
}

bool verifyUpdateSignature(const void* data, size_t size, const u8 signature[0x100]) {
    constexpr int HashSize = 32;
    constexpr int SaltSize = 32;
    constexpr int DbSize = 0x100 - HashSize - 1;

    u8 publicExponent[0x100] = {};
    publicExponent[0xFD] = 1;
    publicExponent[0xFF] = 1;
    u8 block[0x100];
    rsaPrivate(UpdateModulus, publicExponent, signature, block);
    if (block[0xFF] != 0xBC || (block[0] & 0x80) != 0)
        return false;

    const u8* h = block + DbSize;
    mgf1(h, HashSize, block, DbSize);
    block[0] &= 0x7F;
    for (int i = 0; i < DbSize - SaltSize - 1; i++) {
        if (block[i] != 0)
            return false;
    }
    if (block[DbSize - SaltSize - 1] != 1)
        return false;

    u8 prefixed[8 + HashSize + SaltSize] = {};
    sha256(data, size, prefixed + 8);
    memcpy(prefixed + 8 + HashSize, block + DbSize - SaltSize, SaltSize);
    u8 expected[HashSize];
    sha256(prefixed, sizeof(prefixed), expected);
    return memcmp(expected, h, HashSize) == 0;
}

bool oaepUnpad(const u8 block[0x100], Bytes& message) {
    constexpr int HashSize = 32;
    u8 seed[HashSize];
    u8 db[0x100 - HashSize - 1];
    memcpy(seed, block + 1, HashSize);
    memcpy(db, block + 1 + HashSize, sizeof(db));
    mgf1(db, sizeof(db), seed, HashSize);
    mgf1(seed, HashSize, db, sizeof(db));

    u8 emptyHash[HashSize];
    sha256("", 0, emptyHash);
    if (block[0] != 0 || memcmp(db, emptyHash, HashSize) != 0)
        return false;

    size_t i = HashSize;
    while (i < sizeof(db) && db[i] == 0)
        i++;
    if (i >= sizeof(db) || db[i] != 1)
        return false;
    message.assign(db + i + 1, db + sizeof(db));
    return true;
}

// -------------------------------------------------------------------------------------------------------------------

void Keyset::parse(const std::string& text, std::map<std::string, Bytes>& keys) {
    size_t position = 0;
    while (position < text.size()) {
        size_t end = text.find('\n', position);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(position, end - position);
        position = end + 1;

        auto trim = [](std::string value) {
            while (!value.empty() && isspace((u8)value.back()))
                value.pop_back();
            size_t start = 0;
            while (start < value.size() && isspace((u8)value[start]))
                start++;
            return value.substr(start);
        };
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0)
            continue;

        std::string name = trim(line.substr(0, eq));
        for (char& c : name)
            c = (char)tolower((u8)c);
        Bytes value;
        if (fromHex(trim(line.substr(eq + 1)), value))
            keys[name] = value;
    }
}

static std::string readWholeFile(const std::string& path) {
    sys::File file(path, sys::File::Mode::Read);
    std::string text((size_t)file.size(), '\0');
    if (!text.empty())
        file.read(0, &text[0], text.size());
    return text;
}

void Keyset::load(const std::string& path) {
    _keys.clear();
    _titleKeys.clear();
    parse(readWholeFile(path), _keys);

    size_t slash = path.find_last_of("/\\");
    std::string titleKeys = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "title.keys";
    if (sys::exists(titleKeys))
        parse(readWholeFile(titleKeys), _titleKeys);
}

const u8* Keyset::find(const std::string& name, size_t size) const {
    auto found = _keys.find(name);
    return found != _keys.end() && found->second.size() == size ? found->second.data() : nullptr;
}

const u8* Keyset::headerKey() const {
    const u8* key = find("header_key", 32);
    if (!key)
        throw ToolError("密钥文件中缺少 header_key");
    return key;
}

const u8* Keyset::keyAreaKey(int keyAreaIndex, int generation) const {
    static const char* const kinds[] = {"application", "ocean", "system"};
    if (keyAreaIndex < 0 || keyAreaIndex > 2)
        return nullptr;
    return find(format("key_area_key_%s_%02x", kinds[keyAreaIndex], generation), 16);
}

const u8* Keyset::titleKek(int generation) const { return find(format("titlekek_%02x", generation), 16); }

const u8* Keyset::titleKey(const std::string& rightsIdHex) const {
    auto found = _titleKeys.find(rightsIdHex);
    return found != _titleKeys.end() && found->second.size() == 16 ? found->second.data() : nullptr;
}

}  // namespace hbp
