#include "signature.h"

#include "platform.h"
#include "vendor/tweetnacl/tweetnacl.h"

#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
#include <stdlib.h>
#elif defined(_WIN32)
#include <stdlib.h>
#elif defined(__linux__) || defined(__ANDROID__)
#include <errno.h>
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>
#endif

/* TweetNaCl requires the application to provide randombytes(). Verify-only
   consumers never call it; signing tools (gpm keygen) need real entropy.
   Signature matches tweetnacl.c's `extern void randombytes(u8 *, u64)`. */
void randombytes(uint8_t *out, unsigned long long length) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)
    arc4random_buf(out, (size_t)length);
#elif defined(_WIN32)
    for (uint64_t index = 0; index < length; index++) {
        unsigned int value = 0;
        rand_s(&value);
        out[index] = (uint8_t)value;
    }
#elif defined(__linux__) || defined(__ANDROID__)
    size_t offset = 0;
    while (offset < length) {
        ssize_t got = getrandom(out + offset, length - offset, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            int fd = open("/dev/urandom", O_RDONLY);
            if (fd < 0)
                abort();
            ssize_t read_bytes = read(fd, out + offset, length - offset);
            close(fd);
            if (read_bytes <= 0)
                abort();
            got = read_bytes;
        }
        offset += (size_t)got;
    }
#else
    /* No portable entropy source on this platform (e.g. WASM/WASI):
       signing is unavailable; verify-only use is unaffected. */
    memset(out, 0, (size_t)length);
#endif
}

bool genesis_signature_verify(const uint8_t *payload, size_t payload_length,
                              const uint8_t signature[GENESIS_SIGNATURE_BYTES],
                              const uint8_t public_key[GENESIS_PUBLIC_KEY_BYTES]) {
    if (!signature || !public_key || (!payload && payload_length != 0))
        return false;
    /* TweetNaCl verifies a contiguous signature||message buffer and needs
       a separate output buffer, so assemble both once. */
    size_t signed_length = GENESIS_SIGNATURE_BYTES + payload_length;
    uint8_t *signed_message = (uint8_t *)malloc(signed_length);
    uint8_t *message = (uint8_t *)malloc(signed_length);
    if (!signed_message || !message) {
        free(signed_message);
        free(message);
        return false;
    }
    memcpy(signed_message, signature, GENESIS_SIGNATURE_BYTES);
    if (payload_length)
        memcpy(signed_message + GENESIS_SIGNATURE_BYTES, payload,
               payload_length);
    unsigned long long message_length = 0;
    int result = crypto_sign_open(message, &message_length, signed_message,
                                  (uint64_t)signed_length, public_key);
    free(signed_message);
    free(message);
    return result == 0;
}

void genesis_signature_key_id(const uint8_t public_key[GENESIS_PUBLIC_KEY_BYTES],
                              uint8_t key_id[GENESIS_KEY_ID_BYTES]) {
    /* crypto_hash is SHA-512 in TweetNaCl; the first 16 bytes fingerprint
       the key well enough for manifest keyIds. */
    uint8_t digest[64];
    crypto_hash(digest, public_key, GENESIS_PUBLIC_KEY_BYTES);
    memcpy(key_id, digest, GENESIS_KEY_ID_BYTES);
}

bool genesis_signature_keypair(uint8_t public_key[GENESIS_PUBLIC_KEY_BYTES],
                               uint8_t secret_key[GENESIS_SECRET_KEY_BYTES]) {
#if defined(__EMSCRIPTEN__) || defined(__wasi__)
    (void)public_key;
    (void)secret_key;
    return false;
#else
    return crypto_sign_keypair(public_key, secret_key) == 0;
#endif
}

void genesis_signature_sign(const uint8_t *payload, size_t payload_length,
                            const uint8_t secret_key[GENESIS_SECRET_KEY_BYTES],
                            uint8_t signature[GENESIS_SIGNATURE_BYTES]) {
    size_t signed_length = GENESIS_SIGNATURE_BYTES + payload_length;
    uint8_t *signed_message = (uint8_t *)malloc(signed_length);
    if (!signed_message)
        abort();
    unsigned long long produced = 0;
    crypto_sign(signed_message, &produced, payload, (uint64_t)payload_length,
                secret_key);
    memcpy(signature, signed_message, GENESIS_SIGNATURE_BYTES);
    free(signed_message);
}

static int base64url_value(char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '-')
        return 62;
    if (c == '_')
        return 63;
    return -1;
}

size_t genesis_signature_base64url_length(size_t data_length) {
    return (data_length * 8 + 5) / 6;
}

bool genesis_signature_base64url_encode(const uint8_t *data, size_t length,
                                        char *output, size_t capacity) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t needed = genesis_signature_base64url_length(length) + 1;
    if (!output || capacity < needed)
        return false;
    size_t out = 0;
    for (size_t index = 0; index < length; index += 3) {
        uint32_t chunk = (uint32_t)data[index] << 16;
        int remaining = (int)(length - index);
        if (remaining > 1)
            chunk |= (uint32_t)data[index + 1] << 8;
        if (remaining > 2)
            chunk |= data[index + 2];
        output[out++] = alphabet[(chunk >> 18) & 63];
        output[out++] = alphabet[(chunk >> 12) & 63];
        if (remaining > 1)
            output[out++] = alphabet[(chunk >> 6) & 63];
        if (remaining > 2)
            output[out++] = alphabet[chunk & 63];
    }
    output[out] = '\0';
    return true;
}

bool genesis_signature_base64url_decode(const char *text, size_t length,
                                        uint8_t *output, size_t capacity,
                                        size_t *written) {
    if (written)
        *written = 0;
    if (!text || !output || length % 4 == 1)
        return false;
    size_t full_groups = length / 4;
    size_t tail = length % 4;
    /* Without padding every 4-char group yields 3 bytes and a tail of
       2 or 3 chars yields 1 or 2 bytes. */
    size_t expected = full_groups * 3 + (tail == 2 ? 1 : tail == 3 ? 2 : 0);
    if (capacity < expected)
        return false;
    size_t out = 0;
    size_t index = 0;
    for (; index + 4 <= length; index += 4) {
        int a = base64url_value(text[index]);
        int b = base64url_value(text[index + 1]);
        int c = base64url_value(text[index + 2]);
        int d = base64url_value(text[index + 3]);
        if ((a | b | c | d) < 0)
            return false;
        uint32_t chunk = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                         ((uint32_t)c << 6) | (uint32_t)d;
        output[out++] = (uint8_t)(chunk >> 16);
        output[out++] = (uint8_t)(chunk >> 8);
        output[out++] = (uint8_t)chunk;
    }
    if (tail == 2) {
        int a = base64url_value(text[index]);
        int b = base64url_value(text[index + 1]);
        if ((a | b) < 0)
            return false;
        output[out++] = (uint8_t)(((uint32_t)a << 2) | ((uint32_t)b >> 4));
    } else if (tail == 3) {
        int a = base64url_value(text[index]);
        int b = base64url_value(text[index + 1]);
        int c = base64url_value(text[index + 2]);
        if ((a | b | c) < 0)
            return false;
        output[out++] = (uint8_t)(((uint32_t)a << 2) | ((uint32_t)b >> 4));
        output[out++] =
            (uint8_t)((((uint32_t)b & 15) << 4) | ((uint32_t)c >> 2));
    }
    if (written)
        *written = out;
    return true;
}

static bool envelope_line(const char *text, size_t *cursor, size_t length,
                          char *line, size_t line_capacity) {
    size_t start = *cursor;
    size_t end = start;
    while (end < length && text[end] != '\n')
        end++;
    if (end == length && start == length)
        return false;
    size_t line_length = end - start;
    if (line_length && text[end - 1] == '\r')
        line_length--;
    if (line_length >= line_capacity)
        return false;
    memcpy(line, text + start, line_length);
    line[line_length] = '\0';
    *cursor = end < length ? end + 1 : end;
    return true;
}

bool genesis_signature_envelope_parse(
    const char *text, size_t length,
    uint8_t key_id[GENESIS_KEY_ID_BYTES],
    uint8_t signature[GENESIS_SIGNATURE_BYTES]) {
    if (!text || !key_id || !signature)
        return false;
    size_t cursor = 0;
    char line[128];
    if (!envelope_line(text, &cursor, length, line, sizeof(line)) ||
        strcmp(line, "GENESIS-SIG-1") != 0)
        return false;
    enum {
        FIELD_NONE = 0,
        FIELD_KEY_ID = 1,
        FIELD_ALGORITHM = 2,
        FIELD_SIGNATURE = 4
    };
    int seen_fields = FIELD_NONE;
    while (cursor < length) {
        if (!envelope_line(text, &cursor, length, line, sizeof(line)))
            return false;
        if (!*line)
            continue;
        const char *value = NULL;
        int field = FIELD_NONE;
        if (strncmp(line, "keyId: ", 7) == 0) {
            field = FIELD_KEY_ID;
            value = line + 7;
        } else if (strncmp(line, "alg: ", 5) == 0) {
            field = FIELD_ALGORITHM;
            value = line + 5;
        } else if (strncmp(line, "sig: ", 5) == 0) {
            field = FIELD_SIGNATURE;
            value = line + 5;
        } else {
            return false;
        }
        if (seen_fields & field)
            return false;
        seen_fields |= field;
        if (field == FIELD_ALGORITHM) {
            if (strcmp(value, GENESIS_SIGNATURE_ALGORITHM) != 0)
                return false;
            continue;
        }
        uint8_t *target = field == FIELD_KEY_ID ? key_id : signature;
        size_t target_size = field == FIELD_KEY_ID ? GENESIS_KEY_ID_BYTES
                                                 : GENESIS_SIGNATURE_BYTES;
        size_t decoded = 0;
        if (!genesis_signature_base64url_decode(value, strlen(value), target,
                                                target_size, &decoded) ||
            decoded != target_size)
            return false;
    }
    return seen_fields == (FIELD_KEY_ID | FIELD_ALGORITHM | FIELD_SIGNATURE);
}

char *genesis_signature_envelope_format(
    const uint8_t key_id[GENESIS_KEY_ID_BYTES],
    const uint8_t signature[GENESIS_SIGNATURE_BYTES]) {
    if (!key_id || !signature)
        return NULL;
    char key_id_text[32];
    char signature_text[128];
    if (!genesis_signature_base64url_encode(
            key_id, GENESIS_KEY_ID_BYTES, key_id_text,
            sizeof(key_id_text)) ||
        !genesis_signature_base64url_encode(
            signature, GENESIS_SIGNATURE_BYTES, signature_text,
            sizeof(signature_text)))
        return NULL;
    const char *template_text =
        "GENESIS-SIG-1\nkeyId: %s\nalg: %s\nsig: %s\n";
    size_t needed = strlen(template_text) + strlen(key_id_text) +
                    strlen(GENESIS_SIGNATURE_ALGORITHM) +
                    strlen(signature_text) + 1;
    char *envelope = (char *)malloc(needed);
    if (!envelope)
        return NULL;
    GENESIS_SNPRINTF(envelope, needed, template_text, key_id_text,
                     GENESIS_SIGNATURE_ALGORITHM, signature_text);
    return envelope;
}
