/* resolver.c: git-based source resolution and verified package fetching for
   gpm. Uses the git, curl, tar, and sha256sum/shasum CLIs per the approved
   design in docs/package-manager.md; no network code lives in the Genesis
   runtime. */

#include "resolver.h"

#include "manifest.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if !defined(_WIN32)

#define RESOLVER_ERROR_SIZE 512
#define GIT_SHA_LENGTH 40
#define SHA256_LENGTH 64

static void resolver_error(char *buffer, size_t capacity, const char *format,
                           ...) {
    if (!buffer || !capacity)
        return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(buffer, capacity, format, arguments);
    va_end(arguments);
}

static char *resolver_strdup(const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = (char *)malloc(length);
    if (copy)
        memcpy(copy, text, length);
    return copy;
}

/* Runs argv[0] with arguments, capturing stdout into output (may be NULL to
   discard). Returns true only when the process exits with status 0. */
static bool resolver_run_capture(char *const argv[], char *output,
                                 size_t output_capacity, char *error_buffer,
                                 size_t error_capacity) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        resolver_error(error_buffer, error_capacity,
                       "Could not create a pipe for '%s'.", argv[0]);
        return false;
    }
    pid_t child = fork();
    if (child < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        resolver_error(error_buffer, error_capacity,
                       "Could not start '%s'.", argv[0]);
        return false;
    }
    if (child == 0) {
        dup2(pipe_fds[1], STDOUT_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipe_fds[1]);
    size_t used = 0;
    char chunk[8192];
    for (;;) {
        ssize_t received = read(pipe_fds[0], chunk, sizeof(chunk));
        if (received <= 0)
            break;
        if (output && used < output_capacity) {
            size_t room = output_capacity - used;
            size_t take = (size_t)received < room ? (size_t)received : room;
            memcpy(output + used, chunk, take);
            used += take;
        }
    }
    close(pipe_fds[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        ;
    if (output)
        output[used < output_capacity ? used : output_capacity - 1] = '\0';
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        resolver_error(error_buffer, error_capacity,
                       "'%s' failed (exit status %d).", argv[0],
                       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return false;
    }
    return true;
}

/* --- source specifier parsing ------------------------------------------- */

static bool resolver_copy_token(const char *start, size_t length,
                                char **out, char *error_buffer,
                                size_t error_capacity) {
    char *copy = (char *)malloc(length + 1);
    if (!copy) {
        resolver_error(error_buffer, error_capacity, "Out of memory.");
        return false;
    }
    memcpy(copy, start, length);
    copy[length] = '\0';
    *out = copy;
    return true;
}

bool gpm_source_parse(const char *source, GpmSource *out,
                      char *error_buffer, size_t error_capacity) {
    memset(out, 0, sizeof(*out));
    if (!source || strncmp(source, "github:", 7) != 0) {
        resolver_error(error_buffer, error_capacity,
                       "Source specifier must start with 'github:'.");
        return false;
    }
    const char *rest = source + 7;
    const char *at = strrchr(rest, '@');
    size_t path_length = at ? (size_t)(at - rest) : strlen(rest);
    const char *slash = memchr(rest, '/', path_length);
    if (!slash || slash == rest || slash + 1 == rest + path_length ||
        memchr(slash + 1, '/', (size_t)(rest + path_length - slash - 1))) {
        resolver_error(error_buffer, error_capacity,
                       "GitHub source must be 'github:owner/repo[@ref]'.");
        return false;
    }
    if (!resolver_copy_token(rest, (size_t)(slash - rest), &out->owner,
                             error_buffer, error_capacity) ||
        !resolver_copy_token(slash + 1,
                             (size_t)(rest + path_length - slash - 1),
                             &out->repo, error_buffer, error_capacity))
        return false;
    if (at) {
        const char *ref = at + 1;
        if (!*ref) {
            resolver_error(error_buffer, error_capacity,
                           "GitHub source ref after '@' is empty.");
            gpm_source_free(out);
            return false;
        }
        /* The ref reaches argv only through URL construction, but keep it to
           a conservative charset anyway so specifiers stay shell-safe. */
        for (const char *cursor = ref; *cursor; cursor++) {
            unsigned char c = (unsigned char)*cursor;
            if (!isalnum(c) && c != '.' && c != '_' && c != '-' &&
                c != '/') {
                resolver_error(error_buffer, error_capacity,
                               "GitHub source ref may only contain letters, "
                               "digits, '.', '_', '-', and '/'.");
                gpm_source_free(out);
                return false;
            }
        }
        out->ref = resolver_strdup(ref);
        if (!out->ref) {
            resolver_error(error_buffer, error_capacity, "Out of memory.");
            gpm_source_free(out);
            return false;
        }
    }
    return true;
}

void gpm_source_free(GpmSource *source) {
    if (!source)
        return;
    free(source->owner);
    free(source->repo);
    free(source->ref);
    memset(source, 0, sizeof(*source));
}

/* --- remote resolution --------------------------------------------------- */

static const char *resolver_git_base(void) {
    const char *base = getenv("GPM_GIT_BASE_URL");
    return base && *base ? base : "https://github.com";
}

static const char *resolver_archive_base(void) {
    const char *base = getenv("GPM_ARCHIVE_BASE_URL");
    return base && *base ? base : "https://codeload.github.com";
}

static bool resolver_is_hex(const char *text, size_t length) {
    for (size_t index = 0; index < length; index++)
        if (!isxdigit((unsigned char)text[index]))
            return false;
    return true;
}

bool gpm_resolve_commit(const GpmSource *source, char *sha_out,
                        char *error_buffer, size_t error_capacity) {
    if (!source || !source->owner || !source->repo || !sha_out) {
        resolver_error(error_buffer, error_capacity, "Invalid arguments.");
        return false;
    }
    char url[1024];
    int written = snprintf(url, sizeof(url), "%s/%s/%s",
                           resolver_git_base(), source->owner,
                           source->repo);
    if (written < 0 || (size_t)written >= sizeof(url)) {
        resolver_error(error_buffer, error_capacity,
                       "Repository URL is too long.");
        return false;
    }
    char *argv[] = {"git", "ls-remote", url, NULL};
    char *output = (char *)malloc(1u << 20);
    if (!output) {
        resolver_error(error_buffer, error_capacity, "Out of memory.");
        return false;
    }
    bool ok = resolver_run_capture(argv, output, 1u << 20, error_buffer,
                                   error_capacity);
    if (!ok) {
        free(output);
        return false;
    }
    /* Lines look like "<40-hex>\t<refname>". For an explicit ref prefer the
       branch, then the peeled annotated tag, then the tag itself, then an
       exact refname match. Without a ref, the remote HEAD wins. */
    const char *branch_ref = NULL;
    const char *peeled_tag_ref = NULL;
    const char *tag_ref = NULL;
    const char *exact_ref = NULL;
    const char *head_ref = NULL;
    char branch_name[512];
    char peeled_name[512];
    char tag_name[512];
    if (source->ref) {
        snprintf(branch_name, sizeof(branch_name), "refs/heads/%s",
                 source->ref);
        snprintf(peeled_name, sizeof(peeled_name), "refs/tags/%s^{}",
                 source->ref);
        snprintf(tag_name, sizeof(tag_name), "refs/tags/%s", source->ref);
    }
    for (char *line = strtok(output, "\n"); line; line = strtok(NULL, "\n")) {
        char *tab = strchr(line, '\t');
        if (!tab || tab - line != GIT_SHA_LENGTH ||
            !resolver_is_hex(line, GIT_SHA_LENGTH))
            continue;
        const char *refname = tab + 1;
        if (!source->ref) {
            if (!strcmp(refname, "HEAD"))
                head_ref = line;
            continue;
        }
        if (!strcmp(refname, branch_name))
            branch_ref = line;
        else if (!strcmp(refname, peeled_name))
            peeled_tag_ref = line;
        else if (!strcmp(refname, tag_name))
            tag_ref = line;
        else if (!strcmp(refname, source->ref))
            exact_ref = line;
    }
    const char *match = source->ref
                            ? (branch_ref ? branch_ref
                                          : (peeled_tag_ref ? peeled_tag_ref
                                                            : (tag_ref ? tag_ref
                                                                       : exact_ref)))
                            : head_ref;
    if (!match) {
        resolver_error(error_buffer, error_capacity,
                       source->ref
                           ? "Ref '%s' not found in %s/%s."
                           : "Remote %s/%s does not advertise HEAD.",
                       source->ref ? source->ref : "",
                       source->owner, source->repo);
        free(output);
        return false;
    }
    memcpy(sha_out, match, GIT_SHA_LENGTH);
    sha_out[GIT_SHA_LENGTH] = '\0';
    free(output);
    return true;
}

/* --- archive download, verification, and unpacking ----------------------- */

static bool resolver_sha256_file(const char *path, char hex_out[SHA256_LENGTH + 1],
                                char *error_buffer, size_t error_capacity) {
    char *digest_argv[] = {"sha256sum", (char *)path, NULL};
    char *output = (char *)malloc(4096);
    if (!output) {
        resolver_error(error_buffer, error_capacity, "Out of memory.");
        return false;
    }
    bool ok = resolver_run_capture(digest_argv, output, 4096, error_buffer,
                                   error_capacity);
    if (!ok) {
        /* macOS ships shasum instead of sha256sum. */
        char *fallback_argv[] = {"shasum", "-a", "256", (char *)path, NULL};
        ok = resolver_run_capture(fallback_argv, output, 4096, error_buffer,
                                  error_capacity);
    }
    if (!ok) {
        free(output);
        return false;
    }
    if (strlen(output) < SHA256_LENGTH ||
        !resolver_is_hex(output, SHA256_LENGTH)) {
        resolver_error(error_buffer, error_capacity,
                       "Could not parse the SHA-256 digest of '%s'.", path);
        free(output);
        return false;
    }
    memcpy(hex_out, output, SHA256_LENGTH);
    hex_out[SHA256_LENGTH] = '\0';
    for (size_t index = 0; index < SHA256_LENGTH; index++)
        hex_out[index] = (char)tolower((unsigned char)hex_out[index]);
    free(output);
    return true;
}

static bool resolver_file_exists(const char *path) {
    struct stat status;
    return stat(path, &status) == 0 && S_ISREG(status.st_mode);
}

bool gpm_fetch_package(const GpmSource *source, const char *commit,
                       const char *expected_sha256, const char *staging_dir,
                       char *actual_sha256_out, char *error_buffer,
                       size_t error_capacity) {
    if (!source || !commit || !staging_dir ||
        !resolver_is_hex(commit, GIT_SHA_LENGTH) ||
        (expected_sha256 &&
         (strlen(expected_sha256) != SHA256_LENGTH ||
          !resolver_is_hex(expected_sha256, SHA256_LENGTH)))) {
        resolver_error(error_buffer, error_capacity, "Invalid arguments.");
        return false;
    }
    char archive_path[4096];
    int written = snprintf(archive_path, sizeof(archive_path),
                           "%s/.gpm-archive.tar.gz", staging_dir);
    if (written < 0 || (size_t)written >= sizeof(archive_path)) {
        resolver_error(error_buffer, error_capacity, "Path too long.");
        return false;
    }
    char url[1200];
    written = snprintf(url, sizeof(url), "%s/%s/%s/tar.gz/%s",
                       resolver_archive_base(), source->owner,
                       source->repo, commit);
    if (written < 0 || (size_t)written >= sizeof(url)) {
        resolver_error(error_buffer, error_capacity,
                       "Archive URL is too long.");
        return false;
    }
    char *download_argv[] = {"curl", "-fsSL", "-o", archive_path, url, NULL};
    if (!resolver_run_capture(download_argv, NULL, 0, error_buffer,
                              error_capacity)) {
        resolver_error(error_buffer, error_capacity,
                       "Could not download the package archive for %s/%s "
                       "at %s.",
                       source->owner, source->repo, commit);
        return false;
    }
    char actual_sha256[SHA256_LENGTH + 1];
    if (!resolver_sha256_file(archive_path, actual_sha256, error_buffer,
                              error_capacity)) {
        unlink(archive_path);
        return false;
    }
    if (actual_sha256_out)
        memcpy(actual_sha256_out, actual_sha256, sizeof(actual_sha256));
    if (expected_sha256 &&
        strcasecmp(actual_sha256, expected_sha256) != 0) {
        resolver_error(error_buffer, error_capacity,
                       "Archive SHA-256 mismatch for %s/%s at %s: expected "
                       "%s, got %s.",
                       source->owner, source->repo, commit, expected_sha256,
                       actual_sha256);
        unlink(archive_path);
        return false;
    }
    char *tar_argv[] = {"tar", "-xzf", archive_path, "-C", (char *)staging_dir,
                        "--strip-components=1", NULL};
    bool ok = resolver_run_capture(tar_argv, NULL, 0, error_buffer,
                                   error_capacity);
    unlink(archive_path);
    if (!ok) {
        resolver_error(error_buffer, error_capacity,
                       "Could not unpack the package archive for %s/%s.",
                       source->owner, source->repo);
        return false;
    }
    /* Structure check: the package must carry a parseable genesis.json whose
       declared entry (when present) resolves inside the package. */
    char manifest_path[4096];
    written = snprintf(manifest_path, sizeof(manifest_path),
                       "%s/%s", staging_dir, GPM_MANIFEST_FILE);
    if (written < 0 || (size_t)written >= sizeof(manifest_path) ||
        !resolver_file_exists(manifest_path)) {
        resolver_error(error_buffer, error_capacity,
                       "Package %s/%s does not contain a %s manifest.",
                       source->owner, source->repo, GPM_MANIFEST_FILE);
        return false;
    }
    GpmManifest *package_manifest = gpm_manifest_load(
        manifest_path, error_buffer, error_capacity);
    if (!package_manifest)
        return false;
    bool valid = gpm_manifest_validate(package_manifest, error_buffer,
                                       error_capacity);
    if (valid && package_manifest->entry && *package_manifest->entry) {
        char entry_path[4096];
        const char *candidates[3];
        char with_extension[4096];
        char index_path[4096];
        written = snprintf(with_extension, sizeof(with_extension), "%s.genesis",
                           package_manifest->entry);
        written = snprintf(index_path, sizeof(index_path), "%s/index.genesis",
                           package_manifest->entry);
        candidates[0] = package_manifest->entry;
        candidates[1] = with_extension;
        candidates[2] = index_path;
        bool entry_found = false;
        for (size_t index = 0; index < 3 && !entry_found; index++) {
            written = snprintf(entry_path, sizeof(entry_path), "%s/%s",
                               staging_dir, candidates[index]);
            if (written >= 0 && (size_t)written < sizeof(entry_path) &&
                resolver_file_exists(entry_path))
                entry_found = true;
        }
        if (!entry_found) {
            resolver_error(error_buffer, error_capacity,
                           "Package %s/%s declares entry '%s' but no such "
                           "file exists.",
                           source->owner, source->repo,
                           package_manifest->entry);
            valid = false;
        }
    }
    /* Structure check passed; callers reload the manifest from staging_dir
       for version/dependency data — manifests are tiny. */
    gpm_manifest_free(package_manifest);
    return valid;
}

#else /* _WIN32: resolver needs git/curl/tar/sha256sum, all POSIX tools. */

static void resolver_unsupported(char *error_buffer, size_t error_capacity) {
    if (error_buffer && error_capacity)
        snprintf(error_buffer, error_capacity,
                 "Package resolution is not supported on Windows yet.");
}

bool gpm_source_parse(const char *source, GpmSource *out,
                      char *error_buffer, size_t error_capacity) {
    (void)source;
    memset(out, 0, sizeof(*out));
    resolver_unsupported(error_buffer, error_capacity);
    return false;
}

void gpm_source_free(GpmSource *source) {
    if (!source)
        return;
    free(source->owner);
    free(source->repo);
    free(source->ref);
    memset(source, 0, sizeof(*source));
}

bool gpm_resolve_commit(const GpmSource *source, char *sha_out,
                        char *error_buffer, size_t error_capacity) {
    (void)source;
    (void)sha_out;
    resolver_unsupported(error_buffer, error_capacity);
    return false;
}

bool gpm_fetch_package(const GpmSource *source, const char *commit,
                       const char *expected_sha256, const char *staging_dir,
                       char *actual_sha256_out, char *error_buffer,
                       size_t error_capacity) {
    (void)source;
    (void)commit;
    (void)expected_sha256;
    (void)staging_dir;
    (void)actual_sha256_out;
    resolver_unsupported(error_buffer, error_capacity);
    return false;
}

#endif /* !_WIN32 */
