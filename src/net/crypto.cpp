#include "crypto.h"
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

bool crypto_random(void *out, size_t size) {
#ifdef _WIN32
    return BCryptGenRandom(NULL, (PUCHAR)out, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    unsigned char *p = (unsigned char *)out;
    size_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, p + got, size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    return got == size;
#endif
}

// ---------------------------------------------------------------------------
// Curve25519, after TweetNaCl (public domain): 16 limbs of 16 bits in int64s,
// carried lazily. Branch-free on secret data.
// ---------------------------------------------------------------------------
typedef int64_t gf[16];
static const gf GF_121665 = {0xDB41, 1};
static const uint8_t BASE_POINT[32] = {9};

static void car25519(gf o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}
static void sel25519(gf p, gf q, int64_t b) {
    int64_t c = ~(b - 1);
    for (int i = 0; i < 16; ++i) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}
static void pack25519(uint8_t *o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int64_t b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}
static void unpack25519(gf o, const uint8_t *n) {
    for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}
static void add(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i]; }
static void sub(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i]; }
static void mul(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; ++i) t[i] = 0;
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    car25519(o);
    car25519(o);
}
static void square(gf o, const gf a) { mul(o, a, a); }
static void inv25519(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; ++a) c[a] = i[a];
    for (int a = 253; a >= 0; --a) {
        square(c, c);
        if (a != 2 && a != 4) mul(c, c, i);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

void crypto_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t z[32];
    gf x, a, b, c, d, e, f;
    for (int i = 0; i < 31; ++i) z[i] = scalar[i];
    z[31] = (uint8_t)((scalar[31] & 127) | 64);
    z[0] &= 248;
    unpack25519(x, point);
    for (int i = 0; i < 16; ++i) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int64_t r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        add(e, a, c);
        sub(a, a, c);
        add(c, b, d);
        sub(b, b, d);
        square(d, e);
        square(f, a);
        mul(a, c, a);
        mul(c, b, e);
        add(e, a, c);
        sub(a, a, c);
        square(b, a);
        sub(c, d, f);
        mul(a, c, GF_121665);
        add(a, a, d);
        mul(c, c, a);
        mul(a, d, f);
        mul(d, b, x);
        square(b, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }
    inv25519(c, c);
    mul(a, a, c);
    pack25519(out, a);
}

void crypto_x25519_base(uint8_t out[32], const uint8_t scalar[32]) { crypto_x25519(out, scalar, BASE_POINT); }

bool crypto_keypair(uint8_t secret[32], uint8_t public_key[32]) {
    if (!crypto_random(secret, 32)) return false;
    crypto_x25519_base(public_key, secret);
    return true;
}

// ---------------------------------------------------------------------------
// ChaCha20
// ---------------------------------------------------------------------------
static uint32_t load32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void store32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
#define QUARTER(a, b, c, d) \
    do { \
        a += b; d ^= a; d = rotl32(d, 16); \
        c += d; b ^= c; b = rotl32(b, 12); \
        a += b; d ^= a; d = rotl32(d, 8); \
        c += d; b ^= c; b = rotl32(b, 7); \
    } while (0)

void crypto_chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; ++i) s[4 + i] = load32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; ++i) s[13 + i] = load32(nonce + 4 * i);
    uint32_t x[16];
    memcpy(x, s, sizeof(x));
    for (int i = 0; i < 10; ++i) {
        QUARTER(x[0], x[4], x[8], x[12]);
        QUARTER(x[1], x[5], x[9], x[13]);
        QUARTER(x[2], x[6], x[10], x[14]);
        QUARTER(x[3], x[7], x[11], x[15]);
        QUARTER(x[0], x[5], x[10], x[15]);
        QUARTER(x[1], x[6], x[11], x[12]);
        QUARTER(x[2], x[7], x[8], x[13]);
        QUARTER(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; ++i) store32(out + 4 * i, x[i] + s[i]);
}

static void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], const uint8_t *in,
                         uint8_t *out, size_t size) {
    uint8_t block[64];
    for (size_t done = 0; done < size; done += 64) {
        crypto_chacha20_block(key, counter++, nonce, block);
        size_t n = size - done < 64 ? size - done : 64;
        for (size_t i = 0; i < n; ++i) out[done + i] = (uint8_t)(in[done + i] ^ block[i]);
    }
}

// ---------------------------------------------------------------------------
// Poly1305, with 26-bit limbs.
// ---------------------------------------------------------------------------
void crypto_poly1305(const uint8_t key[32], const void *message, size_t size, uint8_t tag[16]) {
    const uint8_t *m = (const uint8_t *)message;
    uint32_t r0 = load32(key) & 0x3ffffff, r1 = (load32(key + 3) >> 2) & 0x3ffff03, r2 = (load32(key + 6) >> 4) & 0x3ffc0ff;
    uint32_t r3 = (load32(key + 9) >> 6) & 0x3f03fff, r4 = (load32(key + 12) >> 8) & 0x00fffff;
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;
    while (size) {
        uint8_t block[16] = {0};
        size_t n = size < 16 ? size : 16;
        memcpy(block, m, n);
        uint32_t hibit = 1u << 24;
        if (n < 16) {
            block[n] = 1;
            hibit = 0;
        }
        h0 += load32(block) & 0x3ffffff;
        h1 += (load32(block + 3) >> 2) & 0x3ffffff;
        h2 += (load32(block + 6) >> 4) & 0x3ffffff;
        h3 += (load32(block + 9) >> 6) & 0x3ffffff;
        h4 += (load32(block + 12) >> 8) | hibit;
        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        uint32_t c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
        h1 += c;
        m += n;
        size -= n;
    }
    // Full carry, then h mod p by comparing with h - p.
    uint32_t c = h1 >> 26; h1 &= 0x3ffffff;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;   // all ones when h >= p (g4 did not go negative)
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
    // h as four 32-bit words, plus the second half of the key.
    uint64_t f0 = ((h0) | (h1 << 26)) & 0xffffffffu;
    uint64_t f1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffffu;
    uint64_t f2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffffu;
    uint64_t f3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffffu;
    uint64_t f = f0 + load32(key + 16); store32(tag, (uint32_t)f);
    f = f1 + load32(key + 20) + (f >> 32); store32(tag + 4, (uint32_t)f);
    f = f2 + load32(key + 24) + (f >> 32); store32(tag + 8, (uint32_t)f);
    f = f3 + load32(key + 28) + (f >> 32); store32(tag + 12, (uint32_t)f);
}

// ---------------------------------------------------------------------------
// AEAD
// ---------------------------------------------------------------------------
static void aead_tag(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_size,
                     const void *cipher, size_t size, uint8_t tag[16]) {
    // Poly1305 key: the first 32 bytes of block 0. MAC over aad, cipher (each padded
    // to 16) and their lengths.
    uint8_t block[64], otk[32];
    crypto_chacha20_block(key, 0, nonce, block);
    memcpy(otk, block, 32);
    // Poly1305 is streamed through the same function by assembling the padded input
    // in one buffer: the sizes here are a packet at most.
    uint8_t buffer[CRYPTO_MAX_INPUT + 64];
    if (aad_size + size > CRYPTO_MAX_INPUT) {   // never for a packet; refuse rather than overrun
        memset(tag, 0, 16);
        return;
    }
    size_t at = 0;
    memcpy(buffer + at, aad, aad_size); at += aad_size;
    while (at % 16) buffer[at++] = 0;
    memcpy(buffer + at, cipher, size); at += size;
    while (at % 16) buffer[at++] = 0;
    for (int i = 0; i < 8; ++i) buffer[at++] = (uint8_t)((uint64_t)aad_size >> (8 * i));
    for (int i = 0; i < 8; ++i) buffer[at++] = (uint8_t)((uint64_t)size >> (8 * i));
    crypto_poly1305(otk, buffer, at, tag);
}

void crypto_aead_encrypt(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_size,
                         const void *plain, size_t size, void *cipher, uint8_t tag[16]) {
    chacha20_xor(key, 1, nonce, (const uint8_t *)plain, (uint8_t *)cipher, size);
    aead_tag(key, nonce, aad, aad_size, cipher, size, tag);
}

bool crypto_aead_decrypt(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_size,
                         const void *cipher, size_t size, const uint8_t tag[16], void *plain) {
    uint8_t expected[16];
    aead_tag(key, nonce, aad, aad_size, cipher, size, expected);
    unsigned diff = 0;
    for (int i = 0; i < 16; ++i) diff |= (unsigned)(expected[i] ^ tag[i]);
    if (diff) return false;
    chacha20_xor(key, 1, nonce, (const uint8_t *)cipher, (uint8_t *)plain, size);
    return true;
}

void crypto_session_keys(const uint8_t shared[32], const uint8_t client_public[32], const uint8_t server_public[32],
                         uint8_t client_to_server[32], uint8_t server_to_client[32]) {
    // Keystream of the shared secret under a nonce drawn from both public keys: a
    // different pair of keys for every session, even between the same two parties.
    uint8_t nonce[12];
    for (int i = 0; i < 12; ++i) nonce[i] = (uint8_t)(client_public[i] ^ server_public[i + 12]);
    uint8_t block[64];
    crypto_chacha20_block(shared, 0, nonce, block);
    memcpy(client_to_server, block, 32);
    memcpy(server_to_client, block + 32, 32);
}
