#ifndef GPM_RESOLVER_H
#define GPM_RESOLVER_H

#include <stdbool.h>
#include <stddef.h>

/* A parsed "github:owner/repo[@ref]" source specifier. ref is NULL when the
   specifier names no explicit ref; resolution then uses the remote HEAD. */
typedef struct {
    char *owner;
    char *repo;
    char *ref;
} GpmSource;

/* Splits a manifest source specifier into owner/repo/ref. Returns false and
   describes the problem in error_buffer. */
bool gpm_source_parse(const char *source, GpmSource *out,
                      char *error_buffer, size_t error_capacity);
void gpm_source_free(GpmSource *source);

/* Maps the source ref to an immutable commit SHA via `git ls-remote`.
   sha_out must hold at least 41 bytes; the result is 40 hex chars plus NUL. */
bool gpm_resolve_commit(const GpmSource *source, char *sha_out,
                        char *error_buffer, size_t error_capacity);

/* Downloads the archive for an already-pinned commit, verifies its SHA-256
   before unpacking, extracts it into staging_dir (which must exist and be
   empty) stripping the archive's top-level directory, and validates that a
   genesis.json manifest parses inside. On success actual_sha256_out (when
   non-NULL, at least 65 bytes) receives the lowercase hex digest that was
   verified. expected_sha256 may be NULL to skip the comparison. Any failure
   leaves staging_dir contents undefined; callers must remove it. */
bool gpm_fetch_package(const GpmSource *source, const char *commit,
                       const char *expected_sha256, const char *staging_dir,
                       char *actual_sha256_out, char *error_buffer,
                       size_t error_capacity);

/* Attempts to download the detached package signature published next to
   the archive as "<archive-url>.sig" (docs/signing.md: signatures travel
   as repo/host sidecars). On success envelope_out receives a malloc'd
   GENESIS-SIG-1 envelope text (caller frees); a missing or unreadable
   sidecar is not an error — *envelope_out is simply NULL. Returns false
   only on internal failures such as allocation errors. staging_dir must
   exist; a temporary download file is removed before returning. */
bool gpm_fetch_package_signature(const GpmSource *source,
                                 const char *commit,
                                 const char *staging_dir,
                                 char **envelope_out,
                                 char *error_buffer,
                                 size_t error_capacity);

#endif
