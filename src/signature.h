#ifndef GENESIS_SIGNATURE_H
#define GENESIS_SIGNATURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Genesis artifact signing: detached Ed25519 signatures per
   docs/signing.md. The implementation is vendored TweetNaCl
   (public domain) under src/vendor/tweetnacl/. */

#define GENESIS_SIGNATURE_ALGORITHM "ed25519"
#define GENESIS_SIGNATURE_BYTES 64
#define GENESIS_PUBLIC_KEY_BYTES 32
#define GENESIS_SECRET_KEY_BYTES 64
#define GENESIS_KEY_ID_BYTES 16

/* Verifies a detached signature over payload. signature is the raw
   64-byte Ed25519 signature; public_key is the raw 32-byte key.
   Returns false on malformed inputs or verification failure. */
bool genesis_signature_verify(const uint8_t *payload, size_t payload_length,
                              const uint8_t signature[GENESIS_SIGNATURE_BYTES],
                              const uint8_t public_key[GENESIS_PUBLIC_KEY_BYTES]);

/* Derives the 16-byte key identifier published in .sig envelopes and
   genesis.json trusted-key maps: the first 16 bytes of SHA-512 over the
   raw public key. */
void genesis_signature_key_id(const uint8_t public_key[GENESIS_PUBLIC_KEY_BYTES],
                              uint8_t key_id[GENESIS_KEY_ID_BYTES]);

/* Generates a keypair for signing tools (gpm keygen). public_key is 32
   bytes; secret_key is the 64-byte TweetNaCl secret key (seed || public
   key) accepted by genesis_signature_sign. Returns false when the
   platform entropy source is unavailable. */
bool genesis_signature_keypair(uint8_t public_key[GENESIS_PUBLIC_KEY_BYTES],
                               uint8_t secret_key[GENESIS_SECRET_KEY_BYTES]);

/* Signs payload with secret_key, producing the raw 64-byte signature. */
void genesis_signature_sign(const uint8_t *payload, size_t payload_length,
                            const uint8_t secret_key[GENESIS_SECRET_KEY_BYTES],
                            uint8_t signature[GENESIS_SIGNATURE_BYTES]);

/* base64url without padding, used by .sig envelopes and genesis.json
   trusted-key maps. encode writes the NUL-terminated string and returns
   false when output is too small (required size is
   genesis_signature_base64url_length(data_length) + 1). decode writes
   raw bytes and reports the count through written; it rejects padding,
   non-alphabet characters, and lengths that are not a whole number of
   6-bit groups. */
size_t genesis_signature_base64url_length(size_t data_length);
bool genesis_signature_base64url_encode(const uint8_t *data, size_t length,
                                        char *output, size_t capacity);
bool genesis_signature_base64url_decode(const char *text, size_t length,
                                        uint8_t *output, size_t capacity,
                                        size_t *written);

/* .sig envelope from docs/signing.md:
     GENESIS-SIG-1
     keyId: <base64url 16-byte key fingerprint>
     alg: ed25519
     sig: <base64url 64-byte signature>
   parse accepts LF or CRLF endings and a single trailing newline;
   anything else is rejected. format returns a malloc'd envelope for
   signing tools (caller frees) or NULL on allocation failure. */
bool genesis_signature_envelope_parse(
    const char *text, size_t length,
    uint8_t key_id[GENESIS_KEY_ID_BYTES],
    uint8_t signature[GENESIS_SIGNATURE_BYTES]);
char *genesis_signature_envelope_format(
    const uint8_t key_id[GENESIS_KEY_ID_BYTES],
    const uint8_t signature[GENESIS_SIGNATURE_BYTES]);

#ifdef __cplusplus
}
#endif

#endif
