/* gpm: standalone package manager for Genesis source packages.
   Slices 1-3 provide init, check, remove, add, install, and update per
   docs/package-manager.md. Runtime @pkg/ specifier support arrives later. */

#include "json.h"
#include "manifest.h"
#include "registry.h"
#include "resolver.h"
#include "signing.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define gpm_mkdir(path) _mkdir(path)
#define gpm_strdup(text) _strdup(text)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define gpm_mkdir(path) mkdir(path, 0755)
#define gpm_strdup(text) strdup(text)
#endif

#define PKG_ERROR_SIZE 512
#define GIT_SHA_TEXT_LENGTH 41
#define SHA256_TEXT_LENGTH 65

static void pkg_error(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    fputs("gpm: ", stderr);
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
}

static bool pkg_path_join(char *out, size_t capacity, const char *directory,
                          const char *name) {
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);
    bool needs_separator = directory_length && directory[directory_length - 1] != '/' &&
                           directory[directory_length - 1] != '\\';
    if (directory_length + (needs_separator ? 1 : 0) + name_length + 1 >
        capacity)
        return false;
    memcpy(out, directory, directory_length);
    size_t offset = directory_length;
    if (needs_separator)
        out[offset++] = '/';
    memcpy(out + offset, name, name_length + 1);
    return true;
}

static bool pkg_file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    fclose(file);
    return true;
}

/* Creates a directory and any missing parents. */
static bool pkg_ensure_dir(const char *path) {
    char partial[4096];
    size_t length = strlen(path);
    if (length >= sizeof(partial))
        return false;
    memcpy(partial, path, length + 1);
    for (char *cursor = partial + 1; *cursor; cursor++) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (gpm_mkdir(partial) != 0 && errno != EEXIST)
            return false;
        *cursor = '/';
    }
    return gpm_mkdir(partial) == 0 || errno == EEXIST;
}

static bool pkg_is_dir(const char *path) {
#if defined(_WIN32)
    struct _stat status;
    return _stat(path, &status) == 0 && (status.st_mode & _S_IFDIR) != 0;
#else
    struct stat status;
    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
#endif
}

#if defined(_WIN32)
static bool pkg_remove_tree(const char *path) {
    char pattern[4096];
    if (!pkg_path_join(pattern, sizeof(pattern), path, "*"))
        return false;
    WIN32_FIND_DATAA entry;
    HANDLE handle = FindFirstFileA(pattern, &entry);
    if (handle == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ||
               GetLastError() == ERROR_PATH_NOT_FOUND;
    bool ok = true;
    do {
        if (!strcmp(entry.cFileName, ".") || !strcmp(entry.cFileName, ".."))
            continue;
        char child[4096];
        if (!pkg_path_join(child, sizeof(child), path, entry.cFileName)) {
            ok = false;
            break;
        }
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!pkg_remove_tree(child)) {
                ok = false;
                break;
            }
        } else if (!DeleteFileA(child)) {
            ok = false;
            break;
        }
    } while (FindNextFileA(handle, &entry));
    FindClose(handle);
    if (ok && !RemoveDirectoryA(path))
        ok = false;
    return ok;
}
#else
static bool pkg_remove_tree(const char *path) {
    struct stat status;
    if (lstat(path, &status) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(status.st_mode))
        return unlink(path) == 0;
    DIR *directory = opendir(path);
    if (!directory)
        return false;
    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char child[4096];
        if (!pkg_path_join(child, sizeof(child), path, entry->d_name) ||
            !pkg_remove_tree(child)) {
            ok = false;
            break;
        }
    }
    closedir(directory);
    if (ok && rmdir(path) != 0)
        ok = false;
    return ok;
}
#endif

static int command_init(void) {
    char error[PKG_ERROR_SIZE];
    if (pkg_file_exists(GPM_MANIFEST_FILE)) {
        pkg_error("A %s already exists in this directory.",
                  GPM_MANIFEST_FILE);
        return 1;
    }
    GpmManifest *manifest = gpm_manifest_new();
    if (!manifest) {
        pkg_error("Out of memory.", NULL);
        return 1;
    }
    manifest->name = gpm_strdup("@my-org/my-project");
    manifest->version = gpm_strdup("0.1.0");
    manifest->entry = gpm_strdup("index.genesis");
    if (!manifest->name || !manifest->version || !manifest->entry ||
        !gpm_manifest_save(manifest, GPM_MANIFEST_FILE, error,
                                   sizeof(error))) {
        pkg_error("%s", error);
        gpm_manifest_free(manifest);
        return 1;
    }
    gpm_manifest_free(manifest);
    printf("Created %s\n", GPM_MANIFEST_FILE);
    return 0;
}

/* Loads the lockfile, treating a missing file as an empty lockfile. Returns
   NULL only when the file exists but cannot be parsed. */
static GpmLockfile *load_lockfile_lenient(const char *path) {
    if (!pkg_file_exists(path))
        return gpm_lockfile_new();
    char error[PKG_ERROR_SIZE];
    GpmLockfile *lockfile =
        gpm_lockfile_load(path, error, sizeof(error));
    if (!lockfile)
        pkg_error("%s", error[0] ? error : "Cannot read lockfile.");
    return lockfile;
}

static void remove_dependency_at(GpmManifest *manifest, size_t index) {
    free(manifest->dependencies[index].alias);
    free(manifest->dependencies[index].source);
    memmove(&manifest->dependencies[index],
            &manifest->dependencies[index + 1],
            (manifest->dependency_count - index - 1) *
                sizeof(*manifest->dependencies));
    manifest->dependency_count--;
    if (!manifest->dependency_count) {
        free(manifest->dependencies);
        manifest->dependencies = NULL;
    }
}

static void remove_package_at(GpmLockfile *lockfile, size_t index) {
    GpmLockedPackage *package = &lockfile->packages[index];
    free(package->alias);
    free(package->source);
    free(package->commit);
    free(package->sha256);
    free(package->version);
    free(package->signature);
    for (size_t dep = 0; dep < package->dependency_count; dep++) {
        free(package->dependencies[dep].alias);
        free(package->dependencies[dep].source);
    }
    free(package->dependencies);
    memmove(&lockfile->packages[index], &lockfile->packages[index + 1],
            (lockfile->package_count - index - 1) *
                sizeof(*lockfile->packages));
    lockfile->package_count--;
    if (!lockfile->package_count) {
        free(lockfile->packages);
        lockfile->packages = NULL;
    }
}

static int command_remove(const char *alias) {
    if (!gpm_alias_valid(alias)) {
        pkg_error("Invalid package alias '%s'.", alias);
        return 1;
    }
    char error[PKG_ERROR_SIZE];
    GpmManifest *manifest =
        gpm_manifest_load(GPM_MANIFEST_FILE, error,
                                  sizeof(error));
    if (!manifest) {
        pkg_error("%s", error);
        return 1;
    }
    GpmDependency *dependency =
        gpm_manifest_find_dependency(manifest, alias);
    if (!dependency) {
        pkg_error("No dependency named '%s' in %s.", alias,
                  GPM_MANIFEST_FILE);
        gpm_manifest_free(manifest);
        return 1;
    }
    /* Dependency arrays are small; locate the index for removal. */
    size_t dependency_index = 0;
    while (&manifest->dependencies[dependency_index] != dependency)
        dependency_index++;
    remove_dependency_at(manifest, dependency_index);
    if (!gpm_manifest_validate(manifest, error, sizeof(error)) ||
        !gpm_manifest_save(manifest, GPM_MANIFEST_FILE, error,
                                   sizeof(error))) {
        pkg_error("%s", error);
        gpm_manifest_free(manifest);
        return 1;
    }
    gpm_manifest_free(manifest);

    bool lockfile_existed = pkg_file_exists(GPM_LOCKFILE_FILE);
    GpmLockfile *lockfile =
        load_lockfile_lenient(GPM_LOCKFILE_FILE);
    if (!lockfile) {
        pkg_error("Cannot update %s.", GPM_LOCKFILE_FILE);
        return 1;
    }
    if (lockfile_existed) {
        for (size_t index = 0; index < lockfile->package_count; index++)
            if (!strcmp(lockfile->packages[index].alias, alias)) {
                remove_package_at(lockfile, index);
                break;
            }
        if (!gpm_lockfile_save(lockfile, GPM_LOCKFILE_FILE,
                                       error, sizeof(error))) {
            pkg_error("%s", error);
            gpm_lockfile_free(lockfile);
            return 1;
        }
    }
    gpm_lockfile_free(lockfile);

    char package_path[4096];
    if (pkg_path_join(package_path, sizeof(package_path),
                     GPM_PACKAGES_DIR, alias) &&
        (pkg_file_exists(package_path) || pkg_is_dir(package_path)) &&
        !pkg_remove_tree(package_path)) {
        pkg_error("Removed '%s' from %s and %s, but could not delete '%s'.",
                  alias, GPM_MANIFEST_FILE, GPM_LOCKFILE_FILE,
                  package_path);
        return 1;
    }
    printf("Removed %s\n", alias);
    return 0;
}

static int command_check(void) {
    char error[PKG_ERROR_SIZE];
    GpmManifest *manifest =
        gpm_manifest_load(GPM_MANIFEST_FILE, error,
                                  sizeof(error));
    if (!manifest) {
        pkg_error("%s", error);
        return 1;
    }
    if (!gpm_manifest_validate(manifest, error, sizeof(error))) {
        pkg_error("%s", error);
        gpm_manifest_free(manifest);
        return 1;
    }
    gpm_manifest_free(manifest);

    GpmLockfile *lockfile =
        load_lockfile_lenient(GPM_LOCKFILE_FILE);
    if (!lockfile) {
        pkg_error("Cannot read %s.", GPM_LOCKFILE_FILE);
        return 1;
    }
    for (size_t index = 0; index < lockfile->package_count; index++) {
        GpmLockedPackage *package = &lockfile->packages[index];
        char reason[128];
        bool ok = gpm_alias_valid(package->alias) &&
                  gpm_commit_valid(package->commit) &&
                  gpm_sha256_valid(package->sha256) &&
                  gpm_source_valid(package->source, reason,
                                           sizeof(reason));
        if (!ok) {
            pkg_error("Lockfile package '%s' is invalid: %s", package->alias,
                      reason);
            gpm_lockfile_free(lockfile);
            return 1;
        }
        for (size_t dep = 0; dep < package->dependency_count; dep++) {
            if (!gpm_alias_valid(package->dependencies[dep].alias) ||
                !gpm_source_valid(package->dependencies[dep].source,
                                          reason, sizeof(reason))) {
                pkg_error("Lockfile dependency '%s' in package '%s' is "
                          "invalid: %s",
                          package->dependencies[dep].alias, package->alias,
                          reason);
                gpm_lockfile_free(lockfile);
                return 1;
            }
        }
    }
    gpm_lockfile_free(lockfile);
    printf("%s is valid\n", GPM_MANIFEST_FILE);
    return 0;
}

/* --- resolution and installation ---------------------------------------- */

#define GPM_MAX_DEPTH 32
#define GPM_MARKER_FILE ".gpm-verified"

static void gpm_dependency_free_fields(GpmDependency *dependency) {
    free(dependency->alias);
    free(dependency->source);
    dependency->alias = NULL;
    dependency->source = NULL;
}

static void gpm_locked_package_clear(GpmLockedPackage *package) {
    free(package->alias);
    free(package->source);
    free(package->commit);
    free(package->sha256);
    free(package->version);
    free(package->signature);
    for (size_t index = 0; index < package->dependency_count; index++)
        gpm_dependency_free_fields(&package->dependencies[index]);
    free(package->dependencies);
    memset(package, 0, sizeof(*package));
}

static bool gpm_dependencies_copy(const GpmDependency *source,
                                  size_t count, GpmDependency **out) {
    *out = NULL;
    if (!count)
        return true;
    GpmDependency *copy =
        (GpmDependency *)calloc(count, sizeof(*copy));
    if (!copy)
        return false;
    for (size_t index = 0; index < count; index++) {
        copy[index].alias = gpm_strdup(source[index].alias);
        copy[index].source = gpm_strdup(source[index].source);
        if (!copy[index].alias || !copy[index].source) {
            for (size_t freed = 0; freed <= index; freed++)
                gpm_dependency_free_fields(&copy[freed]);
            free(copy);
            return false;
        }
    }
    *out = copy;
    return true;
}

static bool gpm_manifest_add_dependency(GpmManifest *manifest,
                                        const char *alias,
                                        const char *source) {
    GpmDependency *grown = (GpmDependency *)realloc(
        manifest->dependencies,
        (manifest->dependency_count + 1) * sizeof(*grown));
    if (!grown)
        return false;
    manifest->dependencies = grown;
    GpmDependency *dependency =
        &manifest->dependencies[manifest->dependency_count];
    dependency->alias = gpm_strdup(alias);
    dependency->source = gpm_strdup(source);
    if (!dependency->alias || !dependency->source) {
        gpm_dependency_free_fields(dependency);
        return false;
    }
    manifest->dependency_count++;
    return true;
}

/* Replaces the lockfile entry for alias, or appends a new one. */
static bool gpm_lockfile_upsert(GpmLockfile *lockfile, const char *alias,
                                const char *source, const char *commit,
                                const char *sha256, const char *version,
                                const char *signature,
                                const GpmDependency *dependencies,
                                size_t dependency_count) {
    GpmLockedPackage *package = gpm_lockfile_find_package(lockfile, alias);
    if (package) {
        gpm_locked_package_clear(package);
    } else {
        GpmLockedPackage *grown = (GpmLockedPackage *)realloc(
            lockfile->packages,
            (lockfile->package_count + 1) * sizeof(*grown));
        if (!grown)
            return false;
        lockfile->packages = grown;
        package = &lockfile->packages[lockfile->package_count++];
        memset(package, 0, sizeof(*package));
    }
    package->alias = gpm_strdup(alias);
    package->source = gpm_strdup(source);
    package->commit = gpm_strdup(commit);
    package->sha256 = gpm_strdup(sha256);
    package->version = version ? gpm_strdup(version) : NULL;
    package->signature = signature ? gpm_strdup(signature) : NULL;
    if (!package->alias || !package->source || !package->commit ||
        !package->sha256 || (version && !package->version) ||
        (signature && !package->signature) ||
        !gpm_dependencies_copy(dependencies, dependency_count,
                               &package->dependencies)) {
        /* Callers abort on failure and free the lockfile; a cleared entry
           frees cleanly. */
        gpm_locked_package_clear(package);
        return false;
    }
    package->dependency_count = dependency_count;
    return true;
}

static bool pkg_write_marker(const char *package_dir, const char *commit,
                             const char *sha256) {
    char marker_path[4096];
    if (!pkg_path_join(marker_path, sizeof(marker_path), package_dir,
                       GPM_MARKER_FILE))
        return false;
    FILE *file = fopen(marker_path, "w");
    if (!file)
        return false;
    bool ok = fprintf(file, "%s %s\n", commit, sha256) > 0;
    if (fclose(file) != 0)
        ok = false;
    return ok;
}

static bool pkg_package_dir_path(const char *alias, char *out,
                                 size_t capacity) {
    return pkg_path_join(out, capacity, GPM_PACKAGES_DIR, alias);
}

static bool pkg_marker_matches(const char *package_dir, const char *commit,
                               const char *sha256) {
    char marker_path[4096];
    if (!pkg_path_join(marker_path, sizeof(marker_path), package_dir,
                       GPM_MARKER_FILE))
        return false;
    FILE *file = fopen(marker_path, "r");
    if (!file)
        return false;
    char recorded_commit[64];
    char recorded_sha256[80];
    bool ok = fscanf(file, "%63s %79s", recorded_commit,
                     recorded_sha256) == 2 &&
              !strcmp(recorded_commit, commit) &&
              !strcmp(recorded_sha256, sha256);
    fclose(file);
    return ok;
}

struct ResolveContext {
    GpmLockfile *lockfile;
    /* Effective package trust for signature verification: the project
       manifest's trustedPackageKeys/signedPackagesRequired when present,
       otherwise the lockfile's pinned copy. */
    const GpmPackageTrust *package_trust;
    char error[PKG_ERROR_SIZE];
};

/* Picks the trust configuration that actually constrains installs:
   genesis.json is the project trust root; the lockfile carries a pinned
   copy for frozen installs. A configured primary wins over fallback. */
static const GpmPackageTrust *pkg_effective_trust(
    const GpmPackageTrust *primary, const GpmPackageTrust *fallback) {
    if (primary && (primary->key_count || primary->signatures_required))
        return primary;
    return fallback;
}

/* Copies the manifest's trust configuration into the lockfile so frozen
   installs verify against the keys that resolved it. Only overwrites
   when the manifest actually configures trust. */
static void pkg_pin_trust_to_lockfile(GpmManifest *manifest,
                                      GpmLockfile *lockfile) {
    if (!manifest)
        return;
    if (!manifest->package_trust.key_count &&
        !manifest->package_trust.signatures_required)
        return;
    gpm_package_trust_free(&lockfile->package_trust);
    (void)gpm_package_trust_copy(&manifest->package_trust,
                                 &lockfile->package_trust);
}

/* Installs the verified staging tree at packages/<alias>, replacing any
   previous installation only after staging succeeded. */
static bool pkg_install_staged(const char *alias, const char *staging_dir,
                               const char *commit, const char *sha256) {
    char destination[4096];
    if (!pkg_path_join(destination, sizeof(destination), GPM_PACKAGES_DIR,
                       alias) ||
        !pkg_ensure_dir(GPM_PACKAGES_DIR))
        return false;
    if (pkg_is_dir(destination) && !pkg_remove_tree(destination))
        return false;
    if (rename(staging_dir, destination) != 0)
        return false;
    return pkg_write_marker(destination, commit, sha256);
}

/* Resolves one alias->source pair: pins the commit, downloads and verifies
   the archive, installs it under .genesis/packages/<alias>, records the
   flattened lockfile entry, then recurses into the package's own
   dependencies. With refresh set, an existing lockfile entry is re-resolved
   instead of reused. Mutates ctx->lockfile only; callers save on success. */
static bool gpm_resolve_and_install(const char *alias, const char *source_spec,
                                    struct ResolveContext *ctx, bool refresh,
                                    int depth,
                                    const char *provided_signature) {
    if (depth > GPM_MAX_DEPTH) {
        snprintf(ctx->error, sizeof(ctx->error),
                 "Dependency chain for '%s' exceeds %d levels; possible "
                 "cycle.",
                 alias, GPM_MAX_DEPTH);
        return false;
    }
    GpmLockedPackage *existing =
        gpm_lockfile_find_package(ctx->lockfile, alias);
    if (existing && !refresh) {
        /* Already pinned: reuse the lockfile entry when the source agrees. */
        GpmSource pinned;
        char parse_error[PKG_ERROR_SIZE];
        if (!gpm_source_parse(existing->source, &pinned, parse_error,
                              sizeof(parse_error))) {
            snprintf(ctx->error, sizeof(ctx->error),
                     "Lockfile source for '%.200s' is invalid: %.250s",
                     alias, parse_error);
            return false;
        }
        GpmSource requested;
        bool same = gpm_source_parse(source_spec, &requested, parse_error,
                                     sizeof(parse_error)) &&
                    !strcmp(pinned.owner, requested.owner) &&
                    !strcmp(pinned.repo, requested.repo) &&
                    ((pinned.ref == NULL) == (requested.ref == NULL)) &&
                    (!pinned.ref || !strcmp(pinned.ref, requested.ref));
        gpm_source_free(&pinned);
        gpm_source_free(&requested);
        if (!same) {
            snprintf(ctx->error, sizeof(ctx->error),
                     "Alias '%s' is already locked to a different source.",
                     alias);
            return false;
        }
        return true;
    }

    /* Preserve the previous pin while re-resolving: an unchanged ref with a
       verified installation needs no download at all. */
    char previous_commit[GIT_SHA_TEXT_LENGTH];
    char previous_sha256[SHA256_TEXT_LENGTH];
    char *previous_signature = NULL;
    bool had_previous =
        existing && existing->commit && existing->sha256 &&
        strlen(existing->commit) == GIT_SHA_TEXT_LENGTH - 1 &&
        strlen(existing->sha256) == SHA256_TEXT_LENGTH - 1;
    if (had_previous) {
        snprintf(previous_commit, sizeof(previous_commit), "%s",
                 existing->commit);
        snprintf(previous_sha256, sizeof(previous_sha256), "%s",
                 existing->sha256);
        if (existing->signature)
            previous_signature = gpm_strdup(existing->signature);
    }

    GpmSource source;
    if (!gpm_source_parse(source_spec, &source, ctx->error,
                          sizeof(ctx->error))) {
        free(previous_signature);
        return false;
    }
    char commit[GIT_SHA_TEXT_LENGTH];
    if (!gpm_resolve_commit(&source, commit, ctx->error,
                            sizeof(ctx->error))) {
        gpm_source_free(&source);
        free(previous_signature);
        return false;
    }
    if (had_previous && !strcmp(commit, previous_commit)) {
        char package_dir[4096];
        if (pkg_package_dir_path(alias, package_dir, sizeof(package_dir)) &&
            pkg_marker_matches(package_dir, commit, previous_sha256)) {
            gpm_source_free(&source);
            free(previous_signature);
            return true;
        }
    }
    if (existing)
        remove_package_at(ctx->lockfile,
                          (size_t)(existing - ctx->lockfile->packages));
    char staging_dir[4096];
    snprintf(staging_dir, sizeof(staging_dir), "%s/.staging-%s",
             GPM_PACKAGES_DIR, alias);
    pkg_remove_tree(staging_dir);
    if (!pkg_ensure_dir(staging_dir)) {
        snprintf(ctx->error, sizeof(ctx->error),
                 "Could not create staging directory '%.440s'.",
                 staging_dir);
        gpm_source_free(&source);
        free(previous_signature);
        return false;
    }
    char sha256[SHA256_TEXT_LENGTH];
    /* When the ref still resolves to the previously pinned commit the old
       digest stays authoritative; otherwise the fresh archive hash is
       recorded into the lockfile for later verification. */
    const char *expected_sha256 =
        had_previous && !strcmp(commit, previous_commit)
            ? previous_sha256
            : NULL;
    if (!gpm_fetch_package(&source, commit, expected_sha256, staging_dir,
                           sha256, ctx->error, sizeof(ctx->error))) {
        pkg_remove_tree(staging_dir);
        gpm_source_free(&source);
        free(previous_signature);
        return false;
    }
    /* The detached signature travels as a "<archive-url>.sig" sidecar;
       a previously pinned lockfile signature is the fallback when the
       host does not publish one. */
    char *fetched_signature = NULL;
    if (!gpm_fetch_package_signature(&source, commit, staging_dir,
                                     &fetched_signature, ctx->error,
                                     sizeof(ctx->error))) {
        pkg_remove_tree(staging_dir);
        gpm_source_free(&source);
        free(previous_signature);
        return false;
    }
    /* A signature supplied by the registry document sits between a live
       sidecar fetch and the previously pinned lockfile signature: it is
       fresher than the lockfile but the sidecar remains authoritative
       when the host publishes one. */
    const char *signature_text =
        fetched_signature ? fetched_signature
                          : provided_signature ? provided_signature
                                               : previous_signature;
    /* Load the fetched manifest for version and dependency flattening. */
    char manifest_path[8192];
    snprintf(manifest_path, sizeof(manifest_path), "%s/%s", staging_dir,
             GPM_MANIFEST_FILE);
    GpmManifest *package_manifest =
        gpm_manifest_load(manifest_path, ctx->error, sizeof(ctx->error));
    if (!package_manifest ||
        !gpm_manifest_validate(package_manifest, ctx->error,
                               sizeof(ctx->error))) {
        if (!ctx->error[0])
            snprintf(ctx->error, sizeof(ctx->error),
                     "Package manifest for '%s' is invalid.", alias);
        if (package_manifest)
            gpm_manifest_free(package_manifest);
        pkg_remove_tree(staging_dir);
        gpm_source_free(&source);
        free(fetched_signature);
        free(previous_signature);
        return false;
    }
    /* Verify the package signature against the canonical
       gpm-package-v1 payload before anything is installed. The
       version-or-commit binding prefers the package's declared version
       and falls back to the pinned commit. */
    const char *version_or_commit =
        package_manifest->version && *package_manifest->version
            ? package_manifest->version
            : commit;
    if (signature_text) {
        if (!gpm_verify_package_signature(
                alias, version_or_commit, sha256, signature_text,
                ctx->package_trust, ctx->error, sizeof(ctx->error))) {
            if (package_manifest)
                gpm_manifest_free(package_manifest);
            pkg_remove_tree(staging_dir);
            gpm_source_free(&source);
            free(fetched_signature);
            free(previous_signature);
            return false;
        }
    } else if (ctx->package_trust &&
               ctx->package_trust->signatures_required) {
        snprintf(ctx->error, sizeof(ctx->error),
                 "Package '%s' requires a detached signature "
                 "(signedPackagesRequired), but no signature was "
                 "published with the archive.", alias);
        gpm_manifest_free(package_manifest);
        pkg_remove_tree(staging_dir);
        gpm_source_free(&source);
        free(fetched_signature);
        free(previous_signature);
        return false;
    }
    if (!pkg_install_staged(alias, staging_dir, commit, sha256)) {
        snprintf(ctx->error, sizeof(ctx->error),
                 "Could not install '%s' into %s.", alias,
                 GPM_PACKAGES_DIR);
        gpm_manifest_free(package_manifest);
        pkg_remove_tree(staging_dir);
        gpm_source_free(&source);
        free(fetched_signature);
        free(previous_signature);
        return false;
    }
    bool recorded = gpm_lockfile_upsert(
        ctx->lockfile, alias, source_spec, commit, sha256,
        package_manifest->version, signature_text,
        package_manifest->dependencies,
        package_manifest->dependency_count);
    gpm_manifest_free(package_manifest);
    gpm_source_free(&source);
    free(fetched_signature);
    free(previous_signature);
    if (!recorded) {
        snprintf(ctx->error, sizeof(ctx->error), "Out of memory.");
        return false;
    }
    /* Recurse into the package's declared dependencies (flattened into the
       root lockfile). Copy them first: recursive calls may grow the
       lockfile package array and invalidate `entry`. */
    GpmLockedPackage *entry = gpm_lockfile_find_package(ctx->lockfile, alias);
    GpmDependency *dependencies = NULL;
    size_t dependency_count = entry ? entry->dependency_count : 0;
    if (entry &&
        !gpm_dependencies_copy(entry->dependencies, dependency_count,
                               &dependencies)) {
        snprintf(ctx->error, sizeof(ctx->error), "Out of memory.");
        return false;
    }
    for (size_t index = 0; index < dependency_count; index++) {
        if (!gpm_resolve_and_install(dependencies[index].alias,
                                     dependencies[index].source, ctx,
                                     false, depth + 1, NULL)) {
            for (size_t freed = 0; freed < dependency_count; freed++)
                gpm_dependency_free_fields(&dependencies[freed]);
            free(dependencies);
            return false;
        }
    }
    for (size_t freed = 0; freed < dependency_count; freed++)
        gpm_dependency_free_fields(&dependencies[freed]);
    free(dependencies);
    return true;
}

static int command_add(int argument_count, char **argument_values) {
    const char *requested = argument_values[2];
    const char *alias = NULL;
    const char *version = NULL;
    const char *registry_override = NULL;
    for (int index = 3; index < argument_count; index++) {
        if (!strcmp(argument_values[index], "--alias") && index + 1 <
                                                       argument_count)
            alias = argument_values[++index];
        else if (!strcmp(argument_values[index], "--version") &&
                 index + 1 < argument_count)
            version = argument_values[++index];
        else if (!strcmp(argument_values[index], "--registry") &&
                 index + 1 < argument_count)
            registry_override = argument_values[++index];
        else {
            pkg_error("Unknown argument '%s'.", argument_values[index]);
            return 1;
        }
    }
    char error[PKG_ERROR_SIZE];
    GpmManifest *manifest =
        gpm_manifest_load(GPM_MANIFEST_FILE, error, sizeof(error));
    if (!manifest) {
        pkg_error("%s", error[0] ? error : "Cannot read genesis.json.");
        return 1;
    }
    if (!gpm_manifest_validate(manifest, error, sizeof(error))) {
        pkg_error("%s", error);
        gpm_manifest_free(manifest);
        return 1;
    }

    /* A specifier starting with 'github:' is a direct source; anything
       else is a registry alias resolved through the configured registry
       (docs/registry.md). The manifest records the resolved github:
       source either way — the registry is discovery-only. */
    bool registry_lookup = strncmp(requested, "github:", 7) != 0;
    const char *source_spec = requested;
    char *resolved_source = NULL;
    char *resolved_signature = NULL;
    if (registry_lookup) {
        if (version && !*version) {
            pkg_error("--version requires a version string.");
            gpm_manifest_free(manifest);
            return 1;
        }
        if (!alias)
            alias = requested;
        char registry_base[2048];
        if (!gpm_registry_base_url(registry_override, manifest->registry,
                                   registry_base, sizeof(registry_base))) {
            pkg_error("No registry configured for '%s'; pass --registry, "
                      "set GPM_REGISTRY_URL, or add \"registry\" to %s.",
                      requested, GPM_MANIFEST_FILE);
            gpm_manifest_free(manifest);
            return 1;
        }
        if (!gpm_registry_resolve(registry_base, requested, version,
                                  &resolved_source, &resolved_signature,
                                  error, sizeof(error))) {
            pkg_error("%s", error[0] ? error : "Registry resolution failed.");
            gpm_manifest_free(manifest);
            return 1;
        }
        source_spec = resolved_source;
    } else if (version) {
        pkg_error("--version only applies to registry aliases, not "
                  "'github:' sources.");
        gpm_manifest_free(manifest);
        return 1;
    }

    /* Default alias: the repository name when it already satisfies the
       alias rules; otherwise the user must pass --alias. Registry
       lookups already carry an explicit alias. */
    GpmSource parsed = {0};
    if (!registry_lookup &&
        !gpm_source_parse(source_spec, &parsed, error, sizeof(error))) {
        pkg_error("%s", error);
        free(resolved_source);
        free(resolved_signature);
        gpm_manifest_free(manifest);
        return 1;
    }
    char derived[256];
    if (!alias) {
        size_t length = strlen(parsed.repo);
        if (!length || length >= sizeof(derived)) {
            pkg_error("Cannot derive an alias from '%s'; pass --alias.",
                      parsed.repo);
            gpm_source_free(&parsed);
            free(resolved_source);
            free(resolved_signature);
            gpm_manifest_free(manifest);
            return 1;
        }
        memcpy(derived, parsed.repo, length + 1);
        for (char *cursor = derived; *cursor; cursor++)
            if (!((*cursor >= 'a' && *cursor <= 'z') ||
                  (*cursor >= '0' && *cursor <= '9') || *cursor == '-'))
                *cursor = '-';
        while (*derived == '-')
            memmove(derived, derived + 1, strlen(derived));
        size_t last = strlen(derived);
        while (last && derived[last - 1] == '-')
            derived[--last] = '\0';
        alias = derived;
    }
    if (!registry_lookup)
        gpm_source_free(&parsed);
    if (!gpm_alias_valid(alias)) {
        pkg_error("Invalid package alias '%s'.", alias);
        free(resolved_source);
        free(resolved_signature);
        gpm_manifest_free(manifest);
        return 1;
    }
    if (gpm_manifest_find_dependency(manifest, alias)) {
        pkg_error("%s already declares dependency '%s'.", GPM_MANIFEST_FILE,
                  alias);
        free(resolved_source);
        free(resolved_signature);
        gpm_manifest_free(manifest);
        return 1;
    }
    GpmLockfile *lockfile = load_lockfile_lenient(GPM_LOCKFILE_FILE);
    if (!lockfile) {
        free(resolved_source);
        free(resolved_signature);
        gpm_manifest_free(manifest);
        return 1;
    }
    struct ResolveContext context;
    memset(&context, 0, sizeof(context));
    context.lockfile = lockfile;
    context.package_trust = pkg_effective_trust(
        &manifest->package_trust, &lockfile->package_trust);
    if (!gpm_resolve_and_install(alias, source_spec, &context, false, 0,
                                 resolved_signature)) {
        pkg_error("%s", context.error[0] ? context.error
                                          : "Resolution failed.");
        gpm_lockfile_free(lockfile);
        free(resolved_source);
        free(resolved_signature);
        gpm_manifest_free(manifest);
        return 1;
    }
    pkg_pin_trust_to_lockfile(manifest, lockfile);
    if (!gpm_manifest_add_dependency(manifest, alias, source_spec) ||
        !gpm_manifest_save(manifest, GPM_MANIFEST_FILE, error,
                           sizeof(error)) ||
        !gpm_lockfile_save(lockfile, GPM_LOCKFILE_FILE, error,
                           sizeof(error))) {
        pkg_error("%s", error[0] ? error : "Out of memory.");
        gpm_lockfile_free(lockfile);
        free(resolved_source);
        free(resolved_signature);
        gpm_manifest_free(manifest);
        return 1;
    }
    gpm_lockfile_free(lockfile);
    gpm_manifest_free(manifest);
    printf("Added %s (%s)\n", alias, source_spec);
    free(resolved_source);
    free(resolved_signature);
    return 0;
}

/* Loads genesis.json leniently for registry commands: a missing or
   invalid manifest only means no "registry" field, not a hard error —
   --registry and GPM_REGISTRY_URL still apply. */
static GpmManifest *pkg_manifest_for_registry(void) {
    char error[PKG_ERROR_SIZE];
    GpmManifest *manifest =
        gpm_manifest_load(GPM_MANIFEST_FILE, error, sizeof(error));
    if (manifest &&
        !gpm_manifest_validate(manifest, error, sizeof(error))) {
        gpm_manifest_free(manifest);
        return NULL;
    }
    return manifest;
}

static bool pkg_registry_base_or_error(const char *override,
                                       GpmManifest *manifest, char *out,
                                       size_t capacity) {
    if (gpm_registry_base_url(override,
                              manifest ? manifest->registry : NULL, out,
                              capacity))
        return true;
    pkg_error("No registry configured; pass --registry, set "
              "GPM_REGISTRY_URL, or add \"registry\" to %s.",
              GPM_MANIFEST_FILE);
    return false;
}

static const char *pkg_json_string(const JsonValue *object,
                                   const char *key) {
    const JsonValue *member = json_object_member(object, key);
    if (!member || member->type != JSON_VALUE_STRING || !member->string)
        return NULL;
    return member->string;
}

static bool pkg_json_fetch(const char *base, const char *path,
                           JsonValue **document_out, char *error,
                           size_t error_capacity) {
    char *text = NULL;
    if (!gpm_registry_fetch_document(base, path, &text, error,
                                     error_capacity))
        return false;
    char json_error[JSON_ERROR_SIZE];
    JsonValue *document = json_parse(text, json_error, sizeof(json_error));
    free(text);
    if (!document) {
        snprintf(error, error_capacity,
                 "Registry document '%s' is not valid JSON: %s", path,
                 json_error);
        return false;
    }
    if (document->type != JSON_VALUE_OBJECT) {
        json_value_free(document);
        snprintf(error, error_capacity,
                 "Registry document '%s' must be a JSON object.", path);
        return false;
    }
    *document_out = document;
    return true;
}

static int command_info(int argument_count, char **argument_values) {
    const char *alias = argument_values[2];
    const char *registry_override = NULL;
    for (int index = 3; index < argument_count; index++) {
        if (!strcmp(argument_values[index], "--registry") && index + 1 <
                                                       argument_count)
            registry_override = argument_values[++index];
        else {
            pkg_error("Unknown argument '%s'.", argument_values[index]);
            return 1;
        }
    }
    if (!gpm_alias_valid(alias)) {
        pkg_error("Invalid package alias '%s'.", alias);
        return 1;
    }
    GpmManifest *manifest = pkg_manifest_for_registry();
    char base[2048];
    if (!pkg_registry_base_or_error(registry_override, manifest, base,
                                    sizeof(base))) {
        if (manifest)
            gpm_manifest_free(manifest);
        return 1;
    }
    char path[512];
    snprintf(path, sizeof(path), "packages/%s.json", alias);
    char error[PKG_ERROR_SIZE];
    JsonValue *document = NULL;
    if (!pkg_json_fetch(base, path, &document, error, sizeof(error))) {
        pkg_error("%s", error[0] ? error : "Registry lookup failed.");
        if (manifest)
            gpm_manifest_free(manifest);
        return 1;
    }
    const char *name = pkg_json_string(document, "name");
    const char *description = pkg_json_string(document, "description");
    const char *license = pkg_json_string(document, "license");
    const char *homepage = pkg_json_string(document, "homepage");
    const char *latest = pkg_json_string(document, "latest");
    printf("%s\n", name ? name : alias);
    if (description)
        printf("  %s\n", description);
    if (license)
        printf("  license: %s\n", license);
    if (homepage)
        printf("  homepage: %s\n", homepage);
    if (latest)
        printf("  latest: %s\n", latest);
    const JsonValue *versions = json_object_member(document, "versions");
    if (versions && versions->type == JSON_VALUE_ARRAY) {
        printf("  versions:\n");
        for (size_t index = 0; index < versions->item_count; index++) {
            const JsonValue *entry = versions->items[index];
            const char *version =
                entry->type == JSON_VALUE_OBJECT
                    ? pkg_json_string(entry, "version")
                    : NULL;
            const char *source =
                entry->type == JSON_VALUE_OBJECT
                    ? pkg_json_string(entry, "source")
                    : NULL;
            if (version)
                printf("    %s%s%s\n", version, source ? "  " : "",
                       source ? source : "");
        }
    }
    json_value_free(document);
    if (manifest)
        gpm_manifest_free(manifest);
    return 0;
}

static bool pkg_contains_fold(const char *haystack, const char *needle) {
    if (!haystack || !needle)
        return false;
    size_t needle_length = strlen(needle);
    if (!needle_length)
        return true;
    for (const char *cursor = haystack; *cursor; cursor++) {
        size_t index = 0;
        while (index < needle_length && cursor[index] &&
               tolower((unsigned char)cursor[index]) ==
                   tolower((unsigned char)needle[index]))
            index++;
        if (index == needle_length)
            return true;
    }
    return false;
}

static int command_search(int argument_count, char **argument_values) {
    if (argument_count < 3) {
        pkg_error("Usage: gpm search <query> [--registry <url>]");
        return 1;
    }
    const char *query = argument_values[2];
    const char *registry_override = NULL;
    for (int index = 3; index < argument_count; index++) {
        if (!strcmp(argument_values[index], "--registry") && index + 1 <
                                                       argument_count)
            registry_override = argument_values[++index];
        else {
            pkg_error("Unknown argument '%s'.", argument_values[index]);
            return 1;
        }
    }
    GpmManifest *manifest = pkg_manifest_for_registry();
    char base[2048];
    if (!pkg_registry_base_or_error(registry_override, manifest, base,
                                    sizeof(base))) {
        if (manifest)
            gpm_manifest_free(manifest);
        return 1;
    }
    char error[PKG_ERROR_SIZE];
    JsonValue *document = NULL;
    if (!pkg_json_fetch(base, "index.json", &document, error,
                        sizeof(error))) {
        pkg_error("%s", error[0] ? error : "Registry lookup failed.");
        if (manifest)
            gpm_manifest_free(manifest);
        return 1;
    }
    const JsonValue *packages = json_object_member(document, "packages");
    size_t matches = 0;
    if (packages && packages->type == JSON_VALUE_ARRAY) {
        for (size_t index = 0; index < packages->item_count; index++) {
            const JsonValue *entry = packages->items[index];
            if (entry->type != JSON_VALUE_OBJECT)
                continue;
            const char *alias = pkg_json_string(entry, "alias");
            const char *name = pkg_json_string(entry, "name");
            const char *description =
                pkg_json_string(entry, "description");
            const char *latest = pkg_json_string(entry, "latest");
            if (!pkg_contains_fold(alias, query) &&
                !pkg_contains_fold(name, query) &&
                !pkg_contains_fold(description, query))
                continue;
            matches++;
            printf("%s", alias ? alias : "(unnamed)");
            if (name && strcmp(name, alias ? alias : ""))
                printf(" (%s)", name);
            if (description)
                printf(" — %s", description);
            if (latest)
                printf(" [latest %s]", latest);
            putchar('\n');
        }
    }
    json_value_free(document);
    if (manifest)
        gpm_manifest_free(manifest);
    if (!matches)
        printf("No packages matching '%s'.\n", query);
    return 0;
}

/* The lockfile may hold flattened transitive entries beyond the manifest's
   direct dependencies, so freshness means every manifest dependency has a
   lockfile entry pinned to the same source. */
static bool gpm_lockfile_covers_manifest(const GpmManifest *manifest,
                                         GpmLockfile *lockfile) {
    for (size_t index = 0; index < manifest->dependency_count; index++) {
        const GpmLockedPackage *package =
            gpm_lockfile_find_package(lockfile,
                                      manifest->dependencies[index].alias);
        if (!package ||
            strcmp(package->source, manifest->dependencies[index].source))
            return false;
    }
    return true;
}

static int command_install(int argument_count, char **argument_values) {
    bool frozen = false;
    for (int index = 2; index < argument_count; index++) {
        if (!strcmp(argument_values[index], "--frozen"))
            frozen = true;
        else {
            pkg_error("Unknown argument '%s'.", argument_values[index]);
            return 1;
        }
    }
    char error[PKG_ERROR_SIZE];
    bool lockfile_present = pkg_file_exists(GPM_LOCKFILE_FILE);
    GpmManifest *manifest = NULL;
    if (pkg_file_exists(GPM_MANIFEST_FILE)) {
        manifest = gpm_manifest_load(GPM_MANIFEST_FILE, error,
                                     sizeof(error));
        if (!manifest) {
            pkg_error("%s", error[0] ? error : "Cannot read genesis.json.");
            return 1;
        }
    }
    GpmLockfile *lockfile = load_lockfile_lenient(GPM_LOCKFILE_FILE);
    if (!lockfile) {
        if (manifest)
            gpm_manifest_free(manifest);
        return 1;
    }
    if (frozen) {
        if (!lockfile_present || !manifest ||
            !gpm_lockfile_covers_manifest(manifest, lockfile)) {
            pkg_error("%s is missing or stale relative to %s; run "
                      "'gpm update' first.",
                      GPM_LOCKFILE_FILE, GPM_MANIFEST_FILE);
            gpm_lockfile_free(lockfile);
            if (manifest)
                gpm_manifest_free(manifest);
            return 1;
        }
    }
    /* Non-frozen installs resolve any manifest dependencies missing from the
       lockfile so install behaves like update for new entries. */
    if (manifest && !frozen) {
        struct ResolveContext context;
        memset(&context, 0, sizeof(context));
        context.lockfile = lockfile;
        context.package_trust = pkg_effective_trust(
            &manifest->package_trust, &lockfile->package_trust);
        for (size_t index = 0; index < manifest->dependency_count;
             index++) {
            const GpmDependency *dependency =
                &manifest->dependencies[index];
            if (gpm_lockfile_find_package(lockfile, dependency->alias))
                continue;
            if (!gpm_resolve_and_install(dependency->alias,
                                         dependency->source, &context,
                                         false, 0, NULL)) {
                pkg_error("%s", context.error[0] ? context.error
                                                  : "Resolution failed.");
                gpm_lockfile_free(lockfile);
                gpm_manifest_free(manifest);
                return 1;
            }
        }
        pkg_pin_trust_to_lockfile(manifest, lockfile);
        if (!gpm_lockfile_save(lockfile, GPM_LOCKFILE_FILE, error,
                               sizeof(error))) {
            pkg_error("%s", error[0] ? error : "Cannot write lockfile.");
            gpm_lockfile_free(lockfile);
            gpm_manifest_free(manifest);
            return 1;
        }
    }
    if (!lockfile->package_count) {
        printf("Nothing to install.\n");
        gpm_lockfile_free(lockfile);
        if (manifest)
            gpm_manifest_free(manifest);
        return 0;
    }
    for (size_t index = 0; index < lockfile->package_count; index++) {
        GpmLockedPackage *package = &lockfile->packages[index];
        char package_dir[4096];
        if (pkg_path_join(package_dir, sizeof(package_dir),
                          GPM_PACKAGES_DIR, package->alias) &&
            pkg_is_dir(package_dir) &&
            pkg_marker_matches(package_dir, package->commit,
                               package->sha256)) {
            printf("%s already installed\n", package->alias);
            continue;
        }
        /* Verify the pinned archive signature before downloading: the
           lockfile's sha256 is what the signature binds, so a bad sig
           fails fast without touching the network. */
        const GpmPackageTrust *trust = pkg_effective_trust(
            manifest ? &manifest->package_trust : NULL,
            &lockfile->package_trust);
        const char *version_or_commit =
            package->version && *package->version ? package->version
                                                  : package->commit;
        if (package->signature) {
            if (!gpm_verify_package_signature(
                    package->alias, version_or_commit, package->sha256,
                    package->signature, trust, error, sizeof(error))) {
                pkg_error("%s", error[0] ? error
                                         : "Signature verification failed.");
                gpm_lockfile_free(lockfile);
                if (manifest)
                    gpm_manifest_free(manifest);
                return 1;
            }
        } else if (trust->signatures_required) {
            pkg_error("Package '%s' requires a detached signature "
                      "(signedPackagesRequired), but the lockfile has "
                      "none for it.", package->alias);
            gpm_lockfile_free(lockfile);
            if (manifest)
                gpm_manifest_free(manifest);
            return 1;
        }
        GpmSource source;
        if (!gpm_source_parse(package->source, &source, error,
                              sizeof(error))) {
            pkg_error("Lockfile source for '%s' is invalid: %s",
                      package->alias, error);
            gpm_lockfile_free(lockfile);
            if (manifest)
                gpm_manifest_free(manifest);
            return 1;
        }
        char staging_dir[4096];
        snprintf(staging_dir, sizeof(staging_dir), "%s/.staging-%s",
                 GPM_PACKAGES_DIR, package->alias);
        pkg_remove_tree(staging_dir);
        if (!pkg_ensure_dir(staging_dir) ||
            !gpm_fetch_package(&source, package->commit, package->sha256,
                               staging_dir, NULL, error, sizeof(error)) ||
            !pkg_install_staged(package->alias, staging_dir,
                                package->commit, package->sha256)) {
            pkg_error("Could not install '%s': %s", package->alias,
                      error[0] ? error : "unknown error");
            pkg_remove_tree(staging_dir);
            gpm_source_free(&source);
            gpm_lockfile_free(lockfile);
            if (manifest)
                gpm_manifest_free(manifest);
            return 1;
        }
        pkg_remove_tree(staging_dir);
        gpm_source_free(&source);
        printf("Installed %s\n", package->alias);
    }
    gpm_lockfile_free(lockfile);
    if (manifest)
        gpm_manifest_free(manifest);
    return 0;
}

static int command_update(const char *alias) {
    char error[PKG_ERROR_SIZE];
    GpmManifest *manifest =
        gpm_manifest_load(GPM_MANIFEST_FILE, error, sizeof(error));
    if (!manifest) {
        pkg_error("%s", error[0] ? error : "Cannot read genesis.json.");
        return 1;
    }
    if (!gpm_manifest_validate(manifest, error, sizeof(error))) {
        pkg_error("%s", error);
        gpm_manifest_free(manifest);
        return 1;
    }
    GpmLockfile *lockfile = load_lockfile_lenient(GPM_LOCKFILE_FILE);
    if (!lockfile) {
        gpm_manifest_free(manifest);
        return 1;
    }
    struct ResolveContext context;
    memset(&context, 0, sizeof(context));
    context.lockfile = lockfile;
    context.package_trust = pkg_effective_trust(
        &manifest->package_trust, &lockfile->package_trust);
    size_t updated = 0;
    for (size_t index = 0; index < manifest->dependency_count; index++) {
        const GpmDependency *dependency = &manifest->dependencies[index];
        if (alias && strcmp(dependency->alias, alias))
            continue;
        if (!gpm_resolve_and_install(dependency->alias, dependency->source,
                                     &context, true, 0, NULL)) {
            pkg_error("%s", context.error[0] ? context.error
                                              : "Resolution failed.");
            gpm_lockfile_free(lockfile);
            gpm_manifest_free(manifest);
            return 1;
        }
        updated++;
    }
    if (alias && !updated) {
        pkg_error("No dependency named '%s' in %s.", alias,
                  GPM_MANIFEST_FILE);
        gpm_lockfile_free(lockfile);
        gpm_manifest_free(manifest);
        return 1;
    }
    pkg_pin_trust_to_lockfile(manifest, lockfile);
    if (!gpm_lockfile_save(lockfile, GPM_LOCKFILE_FILE, error,
                           sizeof(error))) {
        pkg_error("%s", error[0] ? error : "Cannot write lockfile.");
        gpm_lockfile_free(lockfile);
        gpm_manifest_free(manifest);
        return 1;
    }
    gpm_lockfile_free(lockfile);
    gpm_manifest_free(manifest);
    printf("Updated %zu package%s\n", updated, updated == 1 ? "" : "s");
    return 0;
}

/* --- gpm tree --------------------------------------------------------- */

/* Renders the flattened lockfile as a tree: manifest dependencies at the
   root, each locked package's recorded dependencies underneath. Aliases
   already expanded are marked "(deduped)" — this also makes cyclic
   dependency edges terminate. */
typedef struct {
    GpmLockfile *lockfile;
    char **expanded;
    size_t expanded_count;
} TreeContext;

static bool tree_was_expanded(TreeContext *context, const char *alias) {
    for (size_t index = 0; index < context->expanded_count; index++)
        if (!strcmp(context->expanded[index], alias))
            return true;
    return false;
}

static bool tree_mark_expanded(TreeContext *context, const char *alias) {
    char **grown = (char **)realloc(
        context->expanded,
        (context->expanded_count + 1) * sizeof(*context->expanded));
    if (!grown)
        return false;
    context->expanded = grown;
    context->expanded[context->expanded_count] = strdup(alias);
    if (!context->expanded[context->expanded_count])
        return false;
    context->expanded_count++;
    return true;
}

static void tree_print_node(TreeContext *context, const GpmDependency *dep,
                            const char *prefix, bool last) {
    printf("%s%s ", prefix, last ? "└──" : "├──");
    GpmLockedPackage *package =
        gpm_lockfile_find_package(context->lockfile, dep->alias);
    if (!package) {
        printf("%s %s (not installed)\n", dep->alias, dep->source);
        return;
    }
    printf("%s@%s %s", package->alias,
           package->version && *package->version ? package->version
                                                 : "unknown",
           package->source);
    if (tree_was_expanded(context, dep->alias)) {
        printf(" (deduped)\n");
        return;
    }
    tree_mark_expanded(context, dep->alias);
    printf("\n");
    char *child_prefix =
        malloc(strlen(prefix) + 8); /* "    " or "│   " (6 bytes) + NUL */
    if (!child_prefix)
        return;
    strcpy(child_prefix, prefix);
    strcat(child_prefix, last ? "    " : "│   ");
    for (size_t index = 0; index < package->dependency_count; index++)
        tree_print_node(context, &package->dependencies[index],
                        child_prefix, index + 1 == package->dependency_count);
    free(child_prefix);
}

static void tree_print_dependencies(TreeContext *context,
                                    const GpmDependency *dependencies,
                                    size_t count, const char *prefix) {
    for (size_t index = 0; index < count; index++)
        tree_print_node(context, &dependencies[index], prefix,
                        index + 1 == count);
}

static int command_tree(void) {
    char error[PKG_ERROR_SIZE];
    GpmManifest *manifest =
        gpm_manifest_load(GPM_MANIFEST_FILE, error, sizeof(error));
    if (!manifest) {
        pkg_error("%s", error);
        return 1;
    }
    GpmLockfile *lockfile = load_lockfile_lenient(GPM_LOCKFILE_FILE);
    if (!lockfile) {
        pkg_error("Cannot read %s.", GPM_LOCKFILE_FILE);
        gpm_manifest_free(manifest);
        return 1;
    }

    printf("%s", manifest->name && *manifest->name ? manifest->name
                                                  : "(unnamed)");
    if (manifest->version && *manifest->version)
        printf("@%s", manifest->version);
    printf("\n");

    TreeContext context = {lockfile, NULL, 0};
    tree_print_dependencies(&context, manifest->dependencies,
                            manifest->dependency_count, "");

    /* Lockfile entries unreachable from the manifest are stale. */
    size_t extraneous = 0;
    for (size_t index = 0; index < lockfile->package_count; index++) {
        GpmLockedPackage *package = &lockfile->packages[index];
        if (tree_was_expanded(&context, package->alias))
            continue;
        if (!extraneous)
            printf("Extraneous lockfile packages:\n");
        printf("  %s@%s %s\n", package->alias,
               package->version && *package->version ? package->version
                                                     : "unknown",
               package->source);
        extraneous++;
    }
    for (size_t index = 0; index < context.expanded_count; index++)
        free(context.expanded[index]);
    free(context.expanded);

    gpm_lockfile_free(lockfile);
    gpm_manifest_free(manifest);
    return 0;
}

/* --- signing commands (docs/signing.md) ---------------------------------- */

static char *pkg_read_file(const char *path, size_t *length_out) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        free(buffer);
        return NULL;
    }
    buffer[size] = '\0';
    if (length_out)
        *length_out = (size_t)size;
    return buffer;
}

static int command_keygen(void) {
    uint8_t public_key[GENESIS_PUBLIC_KEY_BYTES];
    uint8_t secret_key[GENESIS_SECRET_KEY_BYTES];
    if (!genesis_signature_keypair(public_key, secret_key)) {
        pkg_error("Could not generate a keypair: no entropy source is "
                  "available on this platform.");
        return 1;
    }
    char *directory = gpm_keys_dir();
    if (!directory) {
        pkg_error("Out of memory.");
        return 1;
    }
    if (!gpm_key_file_write(directory, public_key, secret_key)) {
        pkg_error("Could not write the private key under '%s'.",
                  directory);
        free(directory);
        return 1;
    }
    uint8_t key_id[GENESIS_KEY_ID_BYTES];
    genesis_signature_key_id(public_key, key_id);
    char key_id_text[GENESIS_KEY_ID_BYTES * 2 + 1];
    char public_key_text[GENESIS_PUBLIC_KEY_BYTES * 2 + 1];
    genesis_signature_base64url_encode(key_id, GENESIS_KEY_ID_BYTES,
                                     key_id_text, sizeof(key_id_text));
    genesis_signature_base64url_encode(public_key,
                                     GENESIS_PUBLIC_KEY_BYTES,
                                     public_key_text,
                                     sizeof(public_key_text));
    printf("Generated Ed25519 package signing key.\n");
    printf("  keyId:      %s\n", key_id_text);
    printf("  publicKey:  %s\n", public_key_text);
    printf("  privateKey: %s/%s.key\n", directory, key_id_text);
    printf("Pin the keyId/publicKey pair in genesis.json "
           "trustedPackageKeys.\n");
    free(directory);
    return 0;
}

/* Shared flag parsing for sign/verify: --key/--pubkey/--alias/--version/
   --commit plus positional archive and optional signature paths. */
struct SigningArgs {
    const char *key_path;
    const char *public_key_text;
    const char *alias;
    const char *version;
    const char *commit;
    const char *archive;
    const char *signature_path;
};

static bool signing_args_parse(int argument_count, char **argument_values,
                               struct SigningArgs *args, bool allow_sig_path) {
    memset(args, 0, sizeof(*args));
    for (int index = 2; index < argument_count; index++) {
        const char *arg = argument_values[index];
        const char **slot = NULL;
        if (!strcmp(arg, "--key") && index + 1 < argument_count)
            slot = &args->key_path;
        else if (!strcmp(arg, "--pubkey") && index + 1 < argument_count)
            slot = &args->public_key_text;
        else if (!strcmp(arg, "--alias") && index + 1 < argument_count)
            slot = &args->alias;
        else if (!strcmp(arg, "--version") && index + 1 < argument_count)
            slot = &args->version;
        else if (!strcmp(arg, "--commit") && index + 1 < argument_count)
            slot = &args->commit;
        else if (arg[0] != '-' && !args->archive)
            args->archive = arg;
        else if (allow_sig_path && arg[0] != '-' && args->archive &&
                 !args->signature_path)
            args->signature_path = arg;
        else {
            pkg_error("Unknown argument '%s'.", arg);
            return false;
        }
        if (slot) {
            if (*slot) {
                pkg_error("Duplicate argument '%s'.", arg);
                return false;
            }
            *slot = argument_values[++index];
        }
    }
    return true;
}

static bool signing_args_validate(const struct SigningArgs *args,
                                  const char *command) {
    if (!args->archive || !args->alias ||
        (!args->version && !args->commit) ||
        (args->version && args->commit)) {
        pkg_error("Usage: gpm %s --key <keyfile> --alias <alias> "
                  "(--version <v> | --commit <sha>) <archive-file> "
                  "[signature.sig]",
                  command);
        return false;
    }
    if (!gpm_alias_valid(args->alias)) {
        pkg_error("Invalid package alias '%s'.", args->alias);
        return false;
    }
    if (args->version && !gpm_version_valid(args->version)) {
        pkg_error("Invalid version '%s' (expected major.minor.patch).",
                  args->version);
        return false;
    }
    if (args->commit && !gpm_commit_valid(args->commit)) {
        pkg_error("Invalid commit '%s' (expected 40 hex characters).",
                  args->commit);
        return false;
    }
    return true;
}

static const char *signing_version_or_commit(const struct SigningArgs *args) {
    return args->version ? args->version : args->commit;
}

static int command_sign(int argument_count, char **argument_values) {
    struct SigningArgs args;
    if (!signing_args_parse(argument_count, argument_values, &args,
                            false) ||
        !args.key_path ||
        !signing_args_validate(&args, "sign"))
        return 1;
    uint8_t secret_key[GENESIS_SECRET_KEY_BYTES];
    if (!gpm_key_file_load(args.key_path, secret_key)) {
        pkg_error("Cannot read secret key '%s'.", args.key_path);
        return 1;
    }
    char sha256[SHA256_TEXT_LENGTH];
    if (!gpm_sha256_file_hex(args.archive, sha256)) {
        pkg_error("Cannot hash archive '%s'.", args.archive);
        return 1;
    }
    char *payload = gpm_package_payload(
        args.alias, signing_version_or_commit(&args), sha256);
    if (!payload) {
        pkg_error("Out of memory.");
        return 1;
    }
    /* TweetNaCl secret keys are seed || public key; the public half is
       the trailing 32 bytes. */
    const uint8_t *public_key = secret_key + 32;
    uint8_t key_id[GENESIS_KEY_ID_BYTES];
    uint8_t signature[GENESIS_SIGNATURE_BYTES];
    genesis_signature_key_id(public_key, key_id);
    genesis_signature_sign((const uint8_t *)payload, strlen(payload),
                           secret_key, signature);
    free(payload);
    char *envelope = genesis_signature_envelope_format(key_id, signature);
    if (!envelope) {
        pkg_error("Out of memory.");
        return 1;
    }
    size_t sig_path_length = strlen(args.archive) + 5;
    char *sig_path = (char *)malloc(sig_path_length);
    if (!sig_path) {
        pkg_error("Out of memory.");
        free(envelope);
        return 1;
    }
    snprintf(sig_path, sig_path_length, "%s.sig", args.archive);
    FILE *file = fopen(sig_path, "wb");
    bool ok = file != NULL;
    if (ok) {
        ok = fputs(envelope, file) >= 0;
        if (fclose(file) != 0)
            ok = false;
    }
    free(envelope);
    if (!ok) {
        pkg_error("Cannot write signature '%s'.", sig_path);
        free(sig_path);
        return 1;
    }
    printf("Wrote %s\n", sig_path);
    free(sig_path);
    return 0;
}

static int command_verify(int argument_count, char **argument_values) {
    struct SigningArgs args;
    if (!signing_args_parse(argument_count, argument_values, &args,
                            true) ||
        !signing_args_validate(&args, "verify"))
        return 1;
    if ((args.key_path != NULL) == (args.public_key_text != NULL)) {
        pkg_error("gpm verify needs exactly one of --key <keyfile> or "
                  "--pubkey <base64url>.");
        return 1;
    }
    uint8_t public_key[GENESIS_PUBLIC_KEY_BYTES];
    if (args.key_path) {
        uint8_t secret_key[GENESIS_SECRET_KEY_BYTES];
        if (!gpm_key_file_load(args.key_path, secret_key)) {
            pkg_error("Cannot read secret key '%s'.", args.key_path);
            return 1;
        }
        memcpy(public_key, secret_key + 32, GENESIS_PUBLIC_KEY_BYTES);
    } else {
        size_t decoded = 0;
        if (!genesis_signature_base64url_decode(
                args.public_key_text, strlen(args.public_key_text),
                public_key, GENESIS_PUBLIC_KEY_BYTES, &decoded) ||
            decoded != GENESIS_PUBLIC_KEY_BYTES) {
            pkg_error("'--pubkey' is not a valid base64url 32-byte "
                      "public key.");
            return 1;
        }
    }
    char *sig_path_storage = NULL;
    const char *sig_path = args.signature_path;
    if (!sig_path) {
        size_t length = strlen(args.archive) + 5;
        sig_path_storage = (char *)malloc(length);
        if (!sig_path_storage) {
            pkg_error("Out of memory.");
            return 1;
        }
        snprintf(sig_path_storage, length, "%s.sig", args.archive);
        sig_path = sig_path_storage;
    }
    size_t envelope_length = 0;
    char *envelope = pkg_read_file(sig_path, &envelope_length);
    if (!envelope) {
        pkg_error("Cannot read signature '%s'.", sig_path);
        free(sig_path_storage);
        return 1;
    }
    uint8_t envelope_key_id[GENESIS_KEY_ID_BYTES];
    uint8_t envelope_signature[GENESIS_SIGNATURE_BYTES];
    bool ok = genesis_signature_envelope_parse(
        envelope, envelope_length, envelope_key_id, envelope_signature);
    free(envelope);
    if (!ok) {
        pkg_error("'%s' is not a valid GENESIS-SIG-1 envelope.", sig_path);
        free(sig_path_storage);
        return 1;
    }
    uint8_t expected_key_id[GENESIS_KEY_ID_BYTES];
    genesis_signature_key_id(public_key, expected_key_id);
    if (memcmp(expected_key_id, envelope_key_id, GENESIS_KEY_ID_BYTES)) {
        pkg_error("Signature keyId does not match the supplied public "
                  "key.");
        free(sig_path_storage);
        return 1;
    }
    char sha256[SHA256_TEXT_LENGTH];
    if (!gpm_sha256_file_hex(args.archive, sha256)) {
        pkg_error("Cannot hash archive '%s'.", args.archive);
        free(sig_path_storage);
        return 1;
    }
    char *payload = gpm_package_payload(
        args.alias, signing_version_or_commit(&args), sha256);
    if (!payload) {
        pkg_error("Out of memory.");
        free(sig_path_storage);
        return 1;
    }
    ok = genesis_signature_verify((const uint8_t *)payload,
                                  strlen(payload), envelope_signature,
                                  public_key);
    free(payload);
    free(sig_path_storage);
    if (!ok) {
        pkg_error("Signature verification failed for '%s'.",
                  args.alias);
        return 1;
    }
    char key_id_text[GENESIS_KEY_ID_BYTES * 2 + 1];
    genesis_signature_base64url_encode(expected_key_id,
                                     GENESIS_KEY_ID_BYTES, key_id_text,
                                     sizeof(key_id_text));
    printf("Verified %s signature (keyId %s)\n", args.alias, key_id_text);
    return 0;
}

static void usage(void) {
    fprintf(stderr,
            "Usage: gpm <command>\n"
            "\n"
            "Commands:\n"
            "  init                     Create a starter genesis.json manifest.\n"
            "  check                    Validate genesis.json and "
            "genesis.lock.json.\n"
            "  add <source> [--alias a] [--version v] [--registry u]\n"
            "                          Resolve, verify, and install a "
            "github:owner/repo[@ref] package, or a registry alias when "
            "a registry is configured.\n"
            "  info <alias> [--registry u]\n"
            "                          Show registry metadata and "
            "versions for a package.\n"
            "  search <query> [--registry u]\n"
            "                          Search the configured registry's "
            "package index.\n"
            "  install [--frozen]      Materialize packages pinned in "
            "genesis.lock.json.\n"
            "  update [alias]          Re-resolve specifiers and rewrite "
            "the lockfile.\n"
            "  remove <alias>           Remove a dependency and its "
            "installed package.\n"
            "  tree                     Print the dependency tree from "
            "genesis.json/genesis.lock.json.\n"
            "  keygen                   Generate an Ed25519 package "
            "signing key under "
            "~/.config/genesis/keys.\n"
            "  sign --key K --alias a (--version v|--commit c) <archive>\n"
            "                          Sign a package archive, writing "
            "<archive>.sig.\n"
            "  verify --key K|--pubkey P --alias a (--version v|--commit c)\n"
            "          <archive> [sig] Verify a package signature.\n"
            "\n"
            "See docs/package-manager.md for the package format and "
            "docs/signing.md for package signing.\n");
}

int main(int argument_count, char **argument_values) {
    if (argument_count < 2) {
        usage();
        return 1;
    }
    const char *command = argument_values[1];
    if (!strcmp(command, "init") && argument_count == 2)
        return command_init();
    if (!strcmp(command, "check") && argument_count == 2)
        return command_check();
    if (!strcmp(command, "tree") && argument_count == 2)
        return command_tree();
    if (!strcmp(command, "remove") && argument_count == 3)
        return command_remove(argument_values[2]);
    if (!strcmp(command, "add") && argument_count >= 3)
        return command_add(argument_count, argument_values);
    if (!strcmp(command, "info") && argument_count >= 3)
        return command_info(argument_count, argument_values);
    if (!strcmp(command, "search") && argument_count >= 3)
        return command_search(argument_count, argument_values);
    if (!strcmp(command, "install") && argument_count >= 2)
        return command_install(argument_count, argument_values);
    if (!strcmp(command, "update") && argument_count <= 3)
        return command_update(argument_count == 3 ? argument_values[2]
                                                  : NULL);
    if (!strcmp(command, "keygen") && argument_count == 2)
        return command_keygen();
    if (!strcmp(command, "sign") && argument_count >= 3)
        return command_sign(argument_count, argument_values);
    if (!strcmp(command, "verify") && argument_count >= 3)
        return command_verify(argument_count, argument_values);
    usage();
    return 1;
}
