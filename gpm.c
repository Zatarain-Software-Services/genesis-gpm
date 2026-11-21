/* gpm: standalone package manager for Genesis source packages.
   Slices 1-3 provide init, check, remove, add, install, and update per
   docs/package-manager.md. Runtime @pkg/ specifier support arrives later. */

#include "manifest.h"
#include "resolver.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
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
    (void)path;
    pkg_error("Recursive package removal is not yet supported on Windows.",
              NULL);
    return false;
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
        pkg_file_exists(package_path) && !pkg_remove_tree(package_path)) {
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
    if (!package->alias || !package->source || !package->commit ||
        !package->sha256 || (version && !package->version) ||
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
    char error[PKG_ERROR_SIZE];
};

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
                                    int depth) {
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
    bool had_previous =
        existing && existing->commit && existing->sha256 &&
        strlen(existing->commit) == GIT_SHA_TEXT_LENGTH - 1 &&
        strlen(existing->sha256) == SHA256_TEXT_LENGTH - 1;
    if (had_previous) {
        snprintf(previous_commit, sizeof(previous_commit), "%s",
                 existing->commit);
        snprintf(previous_sha256, sizeof(previous_sha256), "%s",
                 existing->sha256);
    }

    GpmSource source;
    if (!gpm_source_parse(source_spec, &source, ctx->error,
                          sizeof(ctx->error)))
        return false;
    char commit[GIT_SHA_TEXT_LENGTH];
    if (!gpm_resolve_commit(&source, commit, ctx->error,
                            sizeof(ctx->error))) {
        gpm_source_free(&source);
        return false;
    }
    if (had_previous && !strcmp(commit, previous_commit)) {
        char package_dir[4096];
        if (pkg_package_dir_path(alias, package_dir, sizeof(package_dir)) &&
            pkg_marker_matches(package_dir, commit, previous_sha256)) {
            gpm_source_free(&source);
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
        return false;
    }
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
        return false;
    }
    if (!pkg_install_staged(alias, staging_dir, commit, sha256)) {
        snprintf(ctx->error, sizeof(ctx->error),
                 "Could not install '%s' into %s.", alias,
                 GPM_PACKAGES_DIR);
        gpm_manifest_free(package_manifest);
        pkg_remove_tree(staging_dir);
        gpm_source_free(&source);
        return false;
    }
    bool recorded = gpm_lockfile_upsert(
        ctx->lockfile, alias, source_spec, commit, sha256,
        package_manifest->version, package_manifest->dependencies,
        package_manifest->dependency_count);
    gpm_manifest_free(package_manifest);
    gpm_source_free(&source);
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
                                     false, depth + 1)) {
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
    const char *source_spec = argument_values[2];
    const char *alias = NULL;
    for (int index = 3; index < argument_count; index++) {
        if (!strcmp(argument_values[index], "--alias") && index + 1 <
                                                       argument_count)
            alias = argument_values[++index];
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
    /* Default alias: the repository name when it already satisfies the
       alias rules; otherwise the user must pass --alias. */
    GpmSource parsed;
    if (!gpm_source_parse(source_spec, &parsed, error, sizeof(error))) {
        pkg_error("%s", error);
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
    gpm_source_free(&parsed);
    if (!gpm_alias_valid(alias)) {
        pkg_error("Invalid package alias '%s'.", alias);
        gpm_manifest_free(manifest);
        return 1;
    }
    if (gpm_manifest_find_dependency(manifest, alias)) {
        pkg_error("%s already declares dependency '%s'.", GPM_MANIFEST_FILE,
                  alias);
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
    if (!gpm_resolve_and_install(alias, source_spec, &context, false, 0)) {
        pkg_error("%s", context.error[0] ? context.error
                                          : "Resolution failed.");
        gpm_lockfile_free(lockfile);
        gpm_manifest_free(manifest);
        return 1;
    }
    if (!gpm_manifest_add_dependency(manifest, alias, source_spec) ||
        !gpm_manifest_save(manifest, GPM_MANIFEST_FILE, error,
                           sizeof(error)) ||
        !gpm_lockfile_save(lockfile, GPM_LOCKFILE_FILE, error,
                           sizeof(error))) {
        pkg_error("%s", error[0] ? error : "Out of memory.");
        gpm_lockfile_free(lockfile);
        gpm_manifest_free(manifest);
        return 1;
    }
    gpm_lockfile_free(lockfile);
    gpm_manifest_free(manifest);
    printf("Added %s (%s)\n", alias, source_spec);
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
        for (size_t index = 0; index < manifest->dependency_count;
             index++) {
            const GpmDependency *dependency =
                &manifest->dependencies[index];
            if (gpm_lockfile_find_package(lockfile, dependency->alias))
                continue;
            if (!gpm_resolve_and_install(dependency->alias,
                                         dependency->source, &context,
                                         false, 0)) {
                pkg_error("%s", context.error[0] ? context.error
                                                  : "Resolution failed.");
                gpm_lockfile_free(lockfile);
                gpm_manifest_free(manifest);
                return 1;
            }
        }
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
    size_t updated = 0;
    for (size_t index = 0; index < manifest->dependency_count; index++) {
        const GpmDependency *dependency = &manifest->dependencies[index];
        if (alias && strcmp(dependency->alias, alias))
            continue;
        if (!gpm_resolve_and_install(dependency->alias, dependency->source,
                                     &context, true, 0)) {
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

static void usage(void) {
    fprintf(stderr,
            "Usage: gpm <command>\n"
            "\n"
            "Commands:\n"
            "  init                     Create a starter genesis.json manifest.\n"
            "  check                    Validate genesis.json and "
            "genesis.lock.json.\n"
            "  add <source> [--alias a] Resolve, verify, and install a "
            "github:owner/repo[@ref] package.\n"
            "  install [--frozen]      Materialize packages pinned in "
            "genesis.lock.json.\n"
            "  update [alias]          Re-resolve specifiers and rewrite "
            "the lockfile.\n"
            "  remove <alias>           Remove a dependency and its "
            "installed package.\n"
            "  tree                     Print the dependency tree from "
            "genesis.json/genesis.lock.json.\n"
            "\n"
            "See docs/package-manager.md for the package format.\n");
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
    if (!strcmp(command, "install") && argument_count >= 2)
        return command_install(argument_count, argument_values);
    if (!strcmp(command, "update") && argument_count <= 3)
        return command_update(argument_count == 3 ? argument_values[2]
                                                  : NULL);
    usage();
    return 1;
}
