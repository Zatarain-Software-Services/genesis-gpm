#ifndef GPM_MANIFEST_H
#define GPM_MANIFEST_H

#include "signature.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

/* A manifest dependency alias maps to a source specifier such as
   "github:owner/repo@v1.0.0". */
typedef struct {
    char *alias;
    char *source;
} GpmDependency;

/* A trusted package signing key: key_id is the 16-byte SHA-512(public
   key) fingerprint and public_key the raw 32-byte Ed25519 key, matching
   the GENESIS-SIG-1 envelope and docs/signing.md. */
typedef struct {
    uint8_t key_id[GENESIS_KEY_ID_BYTES];
    uint8_t public_key[GENESIS_PUBLIC_KEY_BYTES];
} GpmTrustedKey;

/* Package signature trust configuration shared by genesis.json and
   genesis.lock.json: a keyId -> public-key map plus the opt-in
   requirement flag. */
typedef struct {
    GpmTrustedKey *keys;
    size_t key_count;
    bool signatures_required;
} GpmPackageTrust;

typedef struct {
    char *name;
    char *version;
    char *entry;
    GpmDependency *dependencies;
    size_t dependency_count;
    /* Optional native extension library paths, relative to the manifest
       directory (or absolute). gpm never fetches or installs these; the
       project author maintains them manually. The runtime/CLI/LSP load
       the listed files so their registered type signatures are visible
       to the checker. */
    char **native_extensions;
    size_t native_extension_count;
    /* Optional package signing trust: trustedPackageKeys maps a keyId to
       a base64url public key; signedPackagesRequired makes a missing or
       invalid package signature a hard error. Both default to absent. */
    GpmPackageTrust package_trust;
} GpmManifest;

typedef struct {
    char *alias;
    char *source;
    char *commit;
    char *sha256;
    char *version;
    /* Detached GENESIS-SIG-1 envelope text for this package's archive,
       when one was supplied by the package host or pinned by hand.
       Signs the gpm-package-v1 canonical payload. */
    char *signature;
    GpmDependency *dependencies;
    size_t dependency_count;
} GpmLockedPackage;

typedef struct {
    int version;
    GpmLockedPackage *packages;
    size_t package_count;
    /* Pinned copy of the project's package trust configuration, written
       by gpm add/update so `gpm install` verifies against the same keys
       that resolved the lockfile. */
    GpmPackageTrust package_trust;
} GpmLockfile;

#define GPM_MANIFEST_FILE "genesis.json"
#define GPM_LOCKFILE_FILE "genesis.lock.json"
#define GPM_PACKAGES_DIR ".genesis/packages"

bool gpm_alias_valid(const char *alias);
bool gpm_name_valid(const char *name);
bool gpm_source_valid(const char *source, char *reason,
                              size_t reason_capacity);
bool gpm_commit_valid(const char *commit);
bool gpm_sha256_valid(const char *sha256);
bool gpm_version_valid(const char *version);

GpmManifest *gpm_manifest_new(void);
void gpm_manifest_free(GpmManifest *manifest);
GpmManifest *gpm_manifest_load(const char *path,
                                              char *error_buffer,
                                              size_t error_capacity);
bool gpm_manifest_save(const GpmManifest *manifest,
                               const char *path, char *error_buffer,
                               size_t error_capacity);
/* Validates every field and dependency. Returns false and describes the
   first problem in error_buffer. */
bool gpm_manifest_validate(const GpmManifest *manifest,
                                   char *error_buffer, size_t error_capacity);
GpmDependency *gpm_manifest_find_dependency(
    GpmManifest *manifest, const char *alias);

GpmLockfile *gpm_lockfile_new(void);
void gpm_lockfile_free(GpmLockfile *lockfile);
GpmLockfile *gpm_lockfile_load(const char *path,
                                              char *error_buffer,
                                              size_t error_capacity);
bool gpm_lockfile_save(const GpmLockfile *lockfile,
                               const char *path, char *error_buffer,
                               size_t error_capacity);
GpmLockedPackage *gpm_lockfile_find_package(
    GpmLockfile *lockfile, const char *alias);

void gpm_package_trust_free(GpmPackageTrust *trust);
bool gpm_package_trust_copy(const GpmPackageTrust *source,
                            GpmPackageTrust *out);

#endif
