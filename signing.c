/* signing.c: SHA-256, canonical package payloads, envelope verification,
   and developer key-file storage for gpm signing (docs/signing.md). */

#include "signing.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define gpm_mkdir(path) _mkdir(path)
#define gpm_chmod(path, mode) (void)(path), (void)(mode)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define gpm_mkdir(path) mkdir(path, 0700)
static void gpm_chmod(const char *path, mode_t mode) {
    (void)chmod(path, mode);
}
#endif

#define SIGNING_ERROR_SIZE 512

static void signing_error(char *buffer, size_t capacity, const char *format,
                          ...) {
    if (!buffer || !capacity)
        return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(buffer, capacity, format, arguments);
    va_end(arguments);
}

/* --- SHA-256 (FIPS 180-4) ------------------------------------------------- */

typedef struct {
    uint32_t state[8];
    uint64_t length;
    uint8_t block[64];
    size_t block_used;
} GpmSha256;

static const uint32_t gpm_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static uint32_t gpm_rotr(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32 - bits));
}

static void gpm_sha256_block(GpmSha256 *ctx, const uint8_t *block) {
    uint32_t w[64];
    for (size_t index = 0; index < 16; index++)
        w[index] = ((uint32_t)block[index * 4] << 24) |
                   ((uint32_t)block[index * 4 + 1] << 16) |
                   ((uint32_t)block[index * 4 + 2] << 8) |
                   (uint32_t)block[index * 4 + 3];
    for (size_t index = 16; index < 64; index++) {
        uint32_t s0 = gpm_rotr(w[index - 15], 7) ^ gpm_rotr(w[index - 15], 18) ^
                      (w[index - 15] >> 3);
        uint32_t s1 = gpm_rotr(w[index - 2], 17) ^ gpm_rotr(w[index - 2], 19) ^
                      (w[index - 2] >> 10);
        w[index] = w[index - 16] + s0 + w[index - 7] + s1;
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2],
             d = ctx->state[3], e = ctx->state[4], f = ctx->state[5],
             g = ctx->state[6], h = ctx->state[7];
    for (size_t index = 0; index < 64; index++) {
        uint32_t s1 = gpm_rotr(e, 6) ^ gpm_rotr(e, 11) ^ gpm_rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t temp1 = h + s1 + ch + gpm_sha256_k[index] + w[index];
        uint32_t s0 = gpm_rotr(a, 2) ^ gpm_rotr(a, 13) ^ gpm_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

static void gpm_sha256_init(GpmSha256 *ctx) {
    static const uint32_t initial[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    memcpy(ctx->state, initial, sizeof(initial));
    ctx->length = 0;
    ctx->block_used = 0;
}

static void gpm_sha256_update(GpmSha256 *ctx, const uint8_t *data,
                              size_t length) {
    ctx->length += length;
    while (length) {
        size_t take = 64 - ctx->block_used;
        if (take > length)
            take = length;
        memcpy(ctx->block + ctx->block_used, data, take);
        ctx->block_used += take;
        data += take;
        length -= take;
        if (ctx->block_used == 64) {
            gpm_sha256_block(ctx, ctx->block);
            ctx->block_used = 0;
        }
    }
}

static void gpm_sha256_final(GpmSha256 *ctx, uint8_t digest[32]) {
    uint64_t bit_length = ctx->length * 8;
    uint8_t pad = 0x80;
    gpm_sha256_update(ctx, &pad, 1);
    uint8_t zero = 0;
    while (ctx->block_used != 56)
        gpm_sha256_update(ctx, &zero, 1);
    uint8_t length_bytes[8];
    for (size_t index = 0; index < 8; index++)
        length_bytes[index] = (uint8_t)(bit_length >> (56 - index * 8));
    /* Feed the length without growing ctx->length further: the padding
       above already finalized the message length accounting. */
    for (size_t index = 0; index < 8; index++) {
        ctx->block[ctx->block_used++] = length_bytes[index];
        if (ctx->block_used == 64) {
            gpm_sha256_block(ctx, ctx->block);
            ctx->block_used = 0;
        }
    }
    for (size_t index = 0; index < 8; index++) {
        digest[index * 4] = (uint8_t)(ctx->state[index] >> 24);
        digest[index * 4 + 1] = (uint8_t)(ctx->state[index] >> 16);
        digest[index * 4 + 2] = (uint8_t)(ctx->state[index] >> 8);
        digest[index * 4 + 3] = (uint8_t)(ctx->state[index]);
    }
}

bool gpm_sha256_file_hex(const char *path, char hex_out[65]) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    GpmSha256 ctx;
    gpm_sha256_init(&ctx);
    uint8_t buffer[8192];
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0)
        gpm_sha256_update(&ctx, buffer, got);
    bool ok = !ferror(file);
    fclose(file);
    if (!ok)
        return false;
    uint8_t digest[32];
    gpm_sha256_final(&ctx, digest);
    for (size_t index = 0; index < 32; index++)
        snprintf(hex_out + index * 2, 3, "%02x", digest[index]);
    hex_out[64] = '\0';
    return true;
}

/* --- canonical payload ----------------------------------------------------- */

char *gpm_package_payload(const char *alias, const char *version_or_commit,
                          const char *sha256_hex) {
    const char *prefix = "gpm-package-v1\n";
    size_t length = strlen(prefix) + strlen(alias) + 1 +
                    strlen(version_or_commit) + 1 + strlen(sha256_hex);
    char *payload = (char *)malloc(length + 1);
    if (!payload)
        return NULL;
    char *cursor = payload;
    memcpy(cursor, prefix, strlen(prefix));
    cursor += strlen(prefix);
    memcpy(cursor, alias, strlen(alias) + 1);
    cursor += strlen(alias);
    *cursor++ = '\n';
    memcpy(cursor, version_or_commit, strlen(version_or_commit) + 1);
    cursor += strlen(version_or_commit);
    *cursor++ = '\n';
    memcpy(cursor, sha256_hex, strlen(sha256_hex) + 1);
    return payload;
}

/* --- envelope verification -------------------------------------------------- */

static const GpmTrustedKey *gpm_trusted_key_find(
    const GpmPackageTrust *trust, const uint8_t key_id[GENESIS_KEY_ID_BYTES]) {
    for (size_t index = 0; index < trust->key_count; index++)
        if (!memcmp(trust->keys[index].key_id, key_id,
                    GENESIS_KEY_ID_BYTES))
            return &trust->keys[index];
    return NULL;
}

bool gpm_verify_package_signature(const char *alias,
                                  const char *version_or_commit,
                                  const char *sha256_hex,
                                  const char *envelope_text,
                                  const GpmPackageTrust *trust,
                                  char *error_buffer,
                                  size_t error_capacity) {
    uint8_t envelope_key_id[GENESIS_KEY_ID_BYTES];
    uint8_t envelope_signature[GENESIS_SIGNATURE_BYTES];
    if (!genesis_signature_envelope_parse(
            envelope_text, strlen(envelope_text), envelope_key_id,
            envelope_signature)) {
        signing_error(error_buffer, error_capacity,
                      "Package '%s' signature is not a valid "
                      "GENESIS-SIG-1 envelope.", alias);
        return false;
    }
    const GpmTrustedKey *key =
        trust ? gpm_trusted_key_find(trust, envelope_key_id) : NULL;
    if (!key) {
        signing_error(error_buffer, error_capacity,
                      "Package '%s' is signed by a key not listed in "
                      "trustedPackageKeys.", alias);
        return false;
    }
    char *payload = gpm_package_payload(alias, version_or_commit,
                                        sha256_hex);
    if (!payload) {
        signing_error(error_buffer, error_capacity, "Out of memory.");
        return false;
    }
    bool ok = genesis_signature_verify(
        (const uint8_t *)payload, strlen(payload), envelope_signature,
        key->public_key);
    free(payload);
    if (!ok) {
        signing_error(error_buffer, error_capacity,
                      "Signature verification failed for package '%s'.",
                      alias);
        return false;
    }
    return true;
}

/* --- key files --------------------------------------------------------------- */

char *gpm_keys_dir(void) {
    const char *override_dir = getenv("GPM_KEYS_DIR");
    if (override_dir && *override_dir) {
        size_t length = strlen(override_dir);
        char *copy = (char *)malloc(length + 1);
        if (copy)
            memcpy(copy, override_dir, length + 1);
        return copy;
    }
#if defined(_WIN32)
    const char *home = getenv("USERPROFILE");
    const char *relative = ".config\\genesis\\keys";
#else
    const char *home = getenv("HOME");
    const char *relative = ".config/genesis/keys";
#endif
    if (!home || !*home)
        home = ".";
    size_t length = strlen(home) + 1 + strlen(relative) + 1;
    char *dir = (char *)malloc(length);
    if (!dir)
        return NULL;
    snprintf(dir, length, "%s/%s", home, relative);
    return dir;
}

static bool gpm_keys_dir_ensure(const char *directory) {
    /* Creates <dir> and its parents with restrictive permissions. */
    char partial[4096];
    size_t length = strlen(directory);
    if (length >= sizeof(partial))
        return false;
    memcpy(partial, directory, length + 1);
    for (char *cursor = partial + 1; *cursor; cursor++) {
        char separator = *cursor;
        if (separator != '/' && separator != '\\')
            continue;
        *cursor = '\0';
        gpm_mkdir(partial);
        *cursor = separator;
    }
    gpm_mkdir(partial);
    return true;
}

bool gpm_key_file_write(const char *directory,
                        const uint8_t public_key[GENESIS_PUBLIC_KEY_BYTES],
                        const uint8_t secret_key[GENESIS_SECRET_KEY_BYTES]) {
    uint8_t key_id[GENESIS_KEY_ID_BYTES];
    genesis_signature_key_id(public_key, key_id);
    char key_id_text[GENESIS_KEY_ID_BYTES * 2 + 1];
    if (!genesis_signature_base64url_encode(
            key_id, GENESIS_KEY_ID_BYTES, key_id_text,
            sizeof(key_id_text)))
        return false;
    if (!gpm_keys_dir_ensure(directory))
        return false;
    size_t path_length = strlen(directory) + 1 + strlen(key_id_text) + 5;
    char *path = (char *)malloc(path_length);
    if (!path)
        return false;
    snprintf(path, path_length, "%s/%s.key", directory, key_id_text);
    size_t encoded_length =
        genesis_signature_base64url_length(GENESIS_SECRET_KEY_BYTES);
    char *encoded = (char *)malloc(encoded_length + 1);
    if (!encoded ||
        !genesis_signature_base64url_encode(
            secret_key, GENESIS_SECRET_KEY_BYTES, encoded,
            encoded_length + 1)) {
        free(encoded);
        free(path);
        return false;
    }
    FILE *file = fopen(path, "wb");
    bool ok = file != NULL;
    if (ok) {
        ok = fprintf(file, "%s\n", encoded) > 0;
        if (fclose(file) != 0)
            ok = false;
    }
    if (ok)
        gpm_chmod(path, 0600);
    free(encoded);
    free(path);
    return ok;
}

bool gpm_key_file_load(const char *path,
                       uint8_t secret_key[GENESIS_SECRET_KEY_BYTES]) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    char buffer[256];
    size_t used = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[used] = '\0';
    while (used && (buffer[used - 1] == '\n' || buffer[used - 1] == '\r' ||
                    buffer[used - 1] == ' ' || buffer[used - 1] == '\t'))
        buffer[--used] = '\0';
    size_t written = 0;
    return genesis_signature_base64url_decode(
               buffer, used, secret_key, GENESIS_SECRET_KEY_BYTES, &written) &&
           written == GENESIS_SECRET_KEY_BYTES;
}
