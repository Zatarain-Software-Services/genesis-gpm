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
#include <sys/stat.h>

#if defined(_WIN32)
#include <windows.h>
#define resolver_strcasecmp _stricmp
#else
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define resolver_strcasecmp strcasecmp
#endif

#include "signing.h"

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

#if !defined(_WIN32)

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

#else /* _WIN32 */

/* Appends one CommandLineToArgvW-compatible quoted argument to buffer,
   which must have room for the worst case (every byte escaped). Backslashes
   are literal unless they precede a quote or the closing quote. */
static void resolver_append_arg(char *buffer, size_t *used,
                                const char *arg) {
    size_t length = strlen(arg);
    buffer[(*used)++] = '"';
    for (size_t index = 0; index < length; index++) {
        if (arg[index] == '"') {
            buffer[(*used)++] = '\\';
            buffer[(*used)++] = '"';
        } else if (arg[index] == '\\') {
            size_t run = 1;
            while (index + run < length && arg[index + run] == '\\')
                run++;
            bool doubled = index + run == length || arg[index + run] == '"';
            for (size_t mark = 0; mark < run * (doubled ? 2 : 1); mark++)
                buffer[(*used)++] = '\\';
            index += run - 1;
        } else {
            buffer[(*used)++] = arg[index];
        }
    }
    buffer[(*used)++] = '"';
}

/* Joins argv into a single CreateProcess command line. */
static char *resolver_join_argv(char *const argv[]) {
    size_t needed = 1;
    for (size_t index = 0; argv[index]; index++)
        needed += strlen(argv[index]) * 2 + 4;
    char *line = (char *)malloc(needed);
    if (!line)
        return NULL;
    size_t used = 0;
    for (size_t index = 0; argv[index]; index++) {
        if (index)
            line[used++] = ' ';
        resolver_append_arg(line, &used, argv[index]);
    }
    line[used] = '\0';
    return line;
}

/* Runs argv[0] with arguments on Windows, capturing stdout into a malloc'd
   NUL-terminated buffer (*out_buf, *out_len). Returns false only when the
   process could not be started or its output could not be read; the exit
   code is reported through exit_code_out either way. */
static bool resolver_spawn_capture(char *const argv[], char **out_buf,
                                   size_t *out_len, DWORD *exit_code_out,
                                   char *error_buffer, size_t error_capacity) {
    *out_buf = NULL;
    *out_len = 0;
    *exit_code_out = 1;

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = NULL;
    HANDLE write_pipe = NULL;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        resolver_error(error_buffer, error_capacity,
                       "Could not create a pipe for '%s'.", argv[0]);
        return false;
    }
    /* The read end must not leak into the child. */
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    char *command_line = resolver_join_argv(argv);
    if (!command_line) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        resolver_error(error_buffer, error_capacity, "Out of memory.");
        return false;
    }
    int wide_length = MultiByteToWideChar(CP_UTF8, 0, command_line, -1, NULL,
                                        0);
    wchar_t *wide_command = wide_length > 0
                                ? (wchar_t *)malloc((size_t)wide_length *
                                                    sizeof(wchar_t))
                                : NULL;
    if (!wide_command ||
        MultiByteToWideChar(CP_UTF8, 0, command_line, -1, wide_command,
                            wide_length) == 0) {
        free(wide_command);
        free(command_line);
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        resolver_error(error_buffer, error_capacity, "Out of memory.");
        return false;
    }
    free(command_line);

    STARTUPINFOW startup;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_pipe;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION process;
    memset(&process, 0, sizeof(process));
    BOOL created = CreateProcessW(NULL, wide_command, NULL, NULL, TRUE, 0,
                                  NULL, NULL, &startup, &process);
    free(wide_command);
    CloseHandle(write_pipe);
    if (!created) {
        CloseHandle(read_pipe);
        resolver_error(error_buffer, error_capacity,
                       "Could not start '%s'.", argv[0]);
        return false;
    }

    size_t capacity = 8192;
    size_t used = 0;
    char *body = (char *)malloc(capacity + 1);
    if (!body) {
        CloseHandle(read_pipe);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        resolver_error(error_buffer, error_capacity, "Out of memory.");
        return false;
    }
    char chunk[8192];
    DWORD received = 0;
    for (;;) {
        if (!ReadFile(read_pipe, chunk, sizeof(chunk), &received, NULL) ||
            received == 0)
            break;
        if (used + received + 1 > capacity) {
            while (used + received + 1 > capacity)
                capacity *= 2;
            char *grown = (char *)realloc(body, capacity + 1);
            if (!grown) {
                free(body);
                CloseHandle(read_pipe);
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
                resolver_error(error_buffer, error_capacity,
                               "Out of memory.");
                return false;
            }
            body = grown;
        }
        memcpy(body + used, chunk, received);
        used += received;
    }
    CloseHandle(read_pipe);
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, exit_code_out);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    body[used] = '\0';
    *out_buf = body;
    *out_len = used;
    return true;
}

/* Runs argv[0] with arguments, capturing stdout into output (may be NULL to
   discard). Returns true only when the process exits with status 0. */
static bool resolver_run_capture(char *const argv[], char *output,
                                 size_t output_capacity, char *error_buffer,
                                 size_t error_capacity) {
    char *body = NULL;
    size_t length = 0;
    DWORD exit_code = 1;
    if (!resolver_spawn_capture(argv, &body, &length, &exit_code,
                                error_buffer, error_capacity)) {
        free(body);
        return false;
    }
    if (output && output_capacity) {
        size_t copy = length < output_capacity ? length : output_capacity - 1;
        memcpy(output, body, copy);
        output[copy] = '\0';
    }
    free(body);
    if (exit_code != 0) {
        resolver_error(error_buffer, error_capacity,
                       "'%s' failed (exit status %lu).", argv[0],
                       (unsigned long)exit_code);
        return false;
    }
    return true;
}

#endif /* !_WIN32 */

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
    /* Portable SHA-256 from signing.c — no sha256sum/shasum dependency. */
    if (!gpm_sha256_file_hex(path, hex_out)) {
        resolver_error(error_buffer, error_capacity,
                       "Could not compute the SHA-256 digest of '%s'.", path);
        return false;
    }
    return true;
}

static bool resolver_file_exists(const char *path) {
#if defined(_WIN32)
    struct _stat status;
    return _stat(path, &status) == 0 && (status.st_mode & _S_IFREG);
#else
    struct stat status;
    return stat(path, &status) == 0 && S_ISREG(status.st_mode);
#endif
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
        remove(archive_path);
        return false;
    }
    if (actual_sha256_out)
        memcpy(actual_sha256_out, actual_sha256, sizeof(actual_sha256));
    if (expected_sha256 &&
        resolver_strcasecmp(actual_sha256, expected_sha256) != 0) {
        resolver_error(error_buffer, error_capacity,
                       "Archive SHA-256 mismatch for %s/%s at %s: expected "
                       "%s, got %s.",
                       source->owner, source->repo, commit, expected_sha256,
                       actual_sha256);
        remove(archive_path);
        return false;
    }
    char *tar_argv[] = {"tar", "-xzf", archive_path, "-C", (char *)staging_dir,
                        "--strip-components=1", NULL};
    bool ok = resolver_run_capture(tar_argv, NULL, 0, error_buffer,
                                   error_capacity);
    remove(archive_path);
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

static char *resolver_read_text_file(const char *path, size_t *length_out) {
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
    char *text = (char *)malloc((size_t)size + 1);
    if (!text) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(text, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        free(text);
        return NULL;
    }
    text[size] = '\0';
    if (length_out)
        *length_out = (size_t)size;
    return text;
}

#if !defined(_WIN32)

/* Fetches url via curl and returns the body as a malloc'd string. The
   capture buffer grows on demand so index/package documents of any
   reasonable size work without a fixed cap. */
bool gpm_fetch_text_url(const char *url, char **out_text,
                        char *error_buffer, size_t error_capacity) {
    *out_text = NULL;
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        resolver_error(error_buffer, error_capacity,
                       "Could not create a pipe for 'curl'.");
        return false;
    }
    pid_t child = fork();
    if (child < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        resolver_error(error_buffer, error_capacity,
                       "Could not start 'curl'.");
        return false;
    }
    if (child == 0) {
        dup2(pipe_fds[1], STDOUT_FILENO);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        char *curl_argv[] = {"curl", "-fsSL", (char *)url, NULL};
        execvp("curl", curl_argv);
        _exit(127);
    }
    close(pipe_fds[1]);
    size_t capacity = 8192;
    size_t used = 0;
    char *body = (char *)malloc(capacity);
    if (!body) {
        close(pipe_fds[0]);
        resolver_error(error_buffer, error_capacity, "Out of memory.");
        return false;
    }
    char chunk[8192];
    for (;;) {
        ssize_t received = read(pipe_fds[0], chunk, sizeof(chunk));
        if (received <= 0)
            break;
        if (used + (size_t)received + 1 > capacity) {
            while (used + (size_t)received + 1 > capacity)
                capacity *= 2;
            char *grown = (char *)realloc(body, capacity);
            if (!grown) {
                free(body);
                close(pipe_fds[0]);
                resolver_error(error_buffer, error_capacity,
                               "Out of memory.");
                return false;
            }
            body = grown;
        }
        memcpy(body + used, chunk, (size_t)received);
        used += (size_t)received;
    }
    close(pipe_fds[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(body);
        resolver_error(error_buffer, error_capacity,
                       "Could not fetch '%s' (curl exit status %d).", url,
                       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return false;
    }
    body[used] = '\0';
    *out_text = body;
    return true;
}

#else /* _WIN32 */

/* Fetches url via curl and returns the body as a malloc'd string; the
   captured buffer grows on demand like the POSIX path. */
bool gpm_fetch_text_url(const char *url, char **out_text,
                        char *error_buffer, size_t error_capacity) {
    *out_text = NULL;
    char *curl_argv[] = {"curl", "-fsSL", (char *)url, NULL};
    char *body = NULL;
    size_t length = 0;
    DWORD exit_code = 1;
    if (!resolver_spawn_capture(curl_argv, &body, &length, &exit_code,
                                error_buffer, error_capacity)) {
        free(body);
        return false;
    }
    if (exit_code != 0) {
        free(body);
        resolver_error(error_buffer, error_capacity,
                       "Could not fetch '%s' (curl exit status %lu).", url,
                       (unsigned long)exit_code);
        return false;
    }
    *out_text = body;
    return true;
}

#endif /* !_WIN32 */

bool gpm_fetch_package_signature(const GpmSource *source,
                                 const char *commit,
                                 const char *staging_dir,
                                 char **envelope_out,
                                 char *error_buffer,
                                 size_t error_capacity) {
    *envelope_out = NULL;
    if (!source || !commit || !staging_dir ||
        !resolver_is_hex(commit, GIT_SHA_LENGTH)) {
        resolver_error(error_buffer, error_capacity, "Invalid arguments.");
        return false;
    }
    char sig_path[4096];
    int written = snprintf(sig_path, sizeof(sig_path),
                           "%s/.gpm-package.sig", staging_dir);
    if (written < 0 || (size_t)written >= sizeof(sig_path)) {
        resolver_error(error_buffer, error_capacity, "Path too long.");
        return false;
    }
    char url[1200];
    written = snprintf(url, sizeof(url), "%s/%s/%s/tar.gz/%s.sig",
                       resolver_archive_base(), source->owner,
                       source->repo, commit);
    if (written < 0 || (size_t)written >= sizeof(url)) {
        resolver_error(error_buffer, error_capacity,
                       "Signature URL is too long.");
        return false;
    }
    /* -s without -S keeps curl fully silent: a missing sidecar is normal
       (unsigned package), not a user-facing error. */
    char *download_argv[] = {"curl", "-fsL", "-o", sig_path, url, NULL};
    /* curl exits non-zero for a missing sidecar; that simply means the
       package is unsigned, so any failure leaves envelope_out NULL. */
    resolver_run_capture(download_argv, NULL, 0, NULL, 0);
    if (!resolver_file_exists(sig_path)) {
        remove(sig_path);
        return true;
    }
    size_t length = 0;
    char *envelope = resolver_read_text_file(sig_path, &length);
    remove(sig_path);
    if (!envelope) {
        resolver_error(error_buffer, error_capacity,
                       "Could not read the package signature sidecar.");
        return false;
    }
    *envelope_out = envelope;
    return true;
}
