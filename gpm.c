/* gpm: standalone package manager for Genesis source packages.
   Slice 1 provides init, check, and remove. Resolution, add, install, and
   update arrive in later slices; see docs/package-manager.md. */

#include "manifest.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
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

static void usage(void) {
    fprintf(stderr,
            "Usage: gpm <command>\n"
            "\n"
            "Commands:\n"
            "  init              Create a starter genesis.json manifest.\n"
            "  check             Validate genesis.json and genesis.lock.json.\n"
            "  remove <alias>    Remove a dependency and its installed package.\n"
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
    if (!strcmp(command, "remove") && argument_count == 3)
        return command_remove(argument_values[2]);
    usage();
    return 1;
}
