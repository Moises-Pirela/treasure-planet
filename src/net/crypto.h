// The primitives the transport encrypts with, written from the specifications and
// checked against their test vectors (tests/test_net.cpp):
//   X25519 (RFC 7748)             the key exchange: each side an ephemeral key pair,
//                                 the shared secret from the other's public key
//   ChaCha20-Poly1305 (RFC 8439)  authenticated encryption of every packet after
//                                 the handshake
// Constant-time where it matters (the field arithmetic, the tag comparison), and
// no allocation anywhere.
#ifndef TP_CRYPTO_H
#define TP_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CRYPTO_KEY_SIZE 32
#define CRYPTO_NONCE_SIZE 12
#define CRYPTO_TAG_SIZE 16
#define CRYPTO_MAX_INPUT 1536        // bytes of aad + message an AEAD call accepts: a packet and change

// Operating-system randomness, for keys and nonces.
bool crypto_random(void *out, size_t size);

// X25519: scalar multiplication on Curve25519; `out` may alias neither input.
void crypto_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void crypto_x25519_base(uint8_t out[32], const uint8_t scalar[32]);
bool crypto_keypair(uint8_t secret[32], uint8_t public_key[32]);

// One ChaCha20 block (64 bytes of keystream).
void crypto_chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]);
// One-time Poly1305 over a message with a 32-byte one-time key.
void crypto_poly1305(const uint8_t key[32], const void *message, size_t size, uint8_t tag[16]);

// AEAD_CHACHA20_POLY1305. `cipher` may alias `plain`.
void crypto_aead_encrypt(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_size,
                         const void *plain, size_t size, void *cipher, uint8_t tag[16]);
// False when the tag does not match, in which case `plain` is left untouched.
bool crypto_aead_decrypt(const uint8_t key[32], const uint8_t nonce[12], const void *aad, size_t aad_size,
                         const void *cipher, size_t size, const uint8_t tag[16], void *plain);

// The two directional packet keys of a session from the shared secret and both
// public keys (so that each session, even between the same parties, is its own).
void crypto_session_keys(const uint8_t shared[32], const uint8_t client_public[32], const uint8_t server_public[32],
                         uint8_t client_to_server[32], uint8_t server_to_client[32]);

#endif
