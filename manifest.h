#ifndef GPM_MANIFEST_H
#define GPM_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* A manifest dependency alias maps to a source specifier such as
   "github:owner/repo@v1.0.0". */
typedef struct {
    char *alias;
    char *source;
} GpmDependency;

typedef struct {
    char *name;
    char *version;
    char *entry;
    GpmDependency *dependencies;
    size_t dependency_count;
} GpmManifest;

typedef struct {
    char *alias;
    char *source;
    char *commit;
    char *sha256;
    char *version;
    GpmDependency *dependencies;
    size_t dependency_count;
} GpmLockedPackage;

typedef struct {
    int version;
    GpmLockedPackage *packages;
    size_t package_count;
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

#endif
