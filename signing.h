#ifndef GPM_SIGNING_H
#define GPM_SIGNING_H

#include "manifest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Package signing helpers for gpm keygen/sign/verify and install-time
   verification, per docs/signing.md. The signed payload is the canonical
   string "gpm-package-v1\n<alias>\n<version-or-commit>\n<sha256-of-
   archive>" (UTF-8, LF separators, no trailing newline). */

/* SHA-256 of a file, lowercase hex into hex_out (65 bytes). Portable:
   used by gpm sign/verify on every platform, unlike the resolver's
   sha256sum shell-out. */
bool gpm_sha256_file_hex(const char *path, char hex_out[65]);

/* Builds the canonical gpm-package-v1 payload. version_or_commit is the
   package version when known, otherwise the pinned commit. Returns a
   malloc'd string (caller frees) or NULL on allocation failure. */
char *gpm_package_payload(const char *alias, const char *version_or_commit,
                          const char *sha256_hex);

/* Verifies a GENESIS-SIG-1 envelope (envelope_text) against the canonical
   package payload for alias/version_or_commit/sha256_hex. The envelope's
   keyId must match a key in trust->keys; a signature from an unlisted
   key is rejected. When trust->key_count is zero every signature is
   untrusted. */
bool gpm_verify_package_signature(const char *alias,
                                  const char *version_or_commit,
                                  const char *sha256_hex,
                                  const char *envelope_text,
                                  const GpmPackageTrust *trust,
                                  char *error_buffer,
                                  size_t error_capacity);

/* Key file helpers. Private keys live in files named
   <keyid>.key containing the base64url 64-byte secret key; directories
   are created 0700 and files 0600 where the platform supports it.
   gpm_keys_dir honors the GPM_KEYS_DIR override for tests. */
char *gpm_keys_dir(void);
bool gpm_key_file_write(const char *directory,
                        const uint8_t public_key[GENESIS_PUBLIC_KEY_BYTES],
                        const uint8_t secret_key[GENESIS_SECRET_KEY_BYTES]);
bool gpm_key_file_load(const char *path,
                       uint8_t secret_key[GENESIS_SECRET_KEY_BYTES]);

#endif
