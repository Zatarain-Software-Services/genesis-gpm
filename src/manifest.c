#include "manifest.h"

#include "json.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define MANIFEST_ERROR_SIZE 256

static char *manifest_strdup(const char *text) {
    if (!text)
        return NULL;
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1);
    if (!copy)
        return NULL;
    memcpy(copy, text, length + 1);
    return copy;
}

static void manifest_set_error(char *error_buffer, size_t error_capacity,
                               const char *format, ...) {
    if (!error_buffer || !error_capacity)
        return;
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(error_buffer, error_capacity, format, arguments);
    va_end(arguments);
}

bool gpm_alias_valid(const char *alias) {
    if (!alias || !alias[0] || !strcmp(alias, "std"))
        return false;
    if (!islower((unsigned char)alias[0]))
        return false;
    for (const unsigned char *cursor = (const unsigned char *)alias; *cursor;
         cursor++) {
        if (!islower(*cursor) && !isdigit(*cursor) && *cursor != '-')
            return false;
    }
    return true;
}

/* A scoped-name segment follows GitHub naming rules: lowercase letters and
   digits with interior hyphens; it may not start or end with '-'. */
static bool pkg_name_segment_valid(const char *start, size_t length) {
    if (!length)
        return false;
    unsigned char first = (unsigned char)start[0];
    unsigned char last = (unsigned char)start[length - 1];
    if ((!islower(first) && !isdigit(first)) ||
        (!islower(last) && !isdigit(last)))
        return false;
    for (size_t index = 0; index < length; index++) {
        char character = start[index];
        if (!islower((unsigned char)character) &&
            !isdigit((unsigned char)character) && character != '-')
            return false;
    }
    return true;
}

/* Package names are scoped GitHub-style identifiers: "@author/name". The
   author is the publishing GitHub user or organization; the name segment
   identifies the package within that scope. */
bool gpm_name_valid(const char *name) {
    if (!name || name[0] != '@')
        return false;
    const char *rest = name + 1;
    const char *slash = strchr(rest, '/');
    if (!slash || slash == rest || !slash[1] || strchr(slash + 1, '/'))
        return false;
    return pkg_name_segment_valid(rest, (size_t)(slash - rest)) &&
           pkg_name_segment_valid(slash + 1, strlen(slash + 1));
}

bool gpm_version_valid(const char *version) {
    if (!version || !version[0])
        return false;
    int components = 0;
    const char *cursor = version;
    while (*cursor && components < 3) {
        if (!isdigit((unsigned char)*cursor))
            return false;
        while (isdigit((unsigned char)*cursor))
            cursor++;
        components++;
        if (*cursor == '.') {
            cursor++;
            continue;
        }
        break;
    }
    return components == 3 && *cursor == '\0';
}

bool gpm_commit_valid(const char *commit) {
    if (!commit || strlen(commit) != 40)
        return false;
    for (const char *cursor = commit; *cursor; cursor++)
        if (!isxdigit((unsigned char)*cursor))
            return false;
    return true;
}

bool gpm_sha256_valid(const char *sha256) {
    if (!sha256 || strlen(sha256) != 64)
        return false;
    for (const char *cursor = sha256; *cursor; cursor++)
        if (!isxdigit((unsigned char)*cursor))
            return false;
    return true;
}

/* Validates "github:<owner>/<repo>[@<ref>]". Owner and repo use the same
   lowercase segment rules as aliases but may contain multiple
   slash-separated... no: exactly owner/repo. The optional ref is non-empty
   and contains no whitespace. */
bool gpm_source_valid(const char *source, char *reason,
                              size_t reason_capacity) {
    if (!source || strncmp(source, "github:", 7) != 0) {
        manifest_set_error(reason, reason_capacity,
                           "Source specifier must start with 'github:'.");
        return false;
    }
    const char *rest = source + 7;
    const char *at = strrchr(rest, '@');
    size_t path_length = at ? (size_t)(at - rest) : strlen(rest);
    if (path_length == 0) {
        manifest_set_error(reason, reason_capacity,
                           "GitHub source is missing owner/repository.");
        return false;
    }
    /* Exactly one '/' separating owner and repo, no leading/trailing slash. */
    const char *slash = memchr(rest, '/', path_length);
    if (!slash || slash == rest || slash + 1 == rest + path_length ||
        memchr(slash + 1, '/', (size_t)(rest + path_length - slash - 1))) {
        manifest_set_error(reason, reason_capacity,
                           "GitHub source must be 'github:owner/repo'.");
        return false;
    }
    for (size_t index = 0; index < path_length; index++) {
        char character = rest[index];
        if (character == '/')
            continue;
        /* GitHub owner/repository names are case-insensitive and permit
           uppercase; git and codeload resolve them as given. */
        if (!isalnum((unsigned char)character) && character != '-' &&
            character != '_' && character != '.') {
            manifest_set_error(reason, reason_capacity,
                               "GitHub owner/repository names may only contain "
                               "letters, digits, '-', '_', and '.'.");
            return false;
        }
    }
    if (at) {
        const char *ref = at + 1;
        if (!*ref) {
            manifest_set_error(reason, reason_capacity,
                               "GitHub source ref after '@' is empty.");
            return false;
        }
        for (const char *cursor = ref; *cursor; cursor++)
            if (isspace((unsigned char)*cursor)) {
                manifest_set_error(reason, reason_capacity,
                                   "GitHub source ref may not contain "
                                   "whitespace.");
                return false;
            }
    }
    return true;
}

static JsonValue *json_dependencies_object(JsonValue *object, const char *key) {
    const JsonValue *member = json_object_member(object, key);
    if (!member)
        return NULL;
    if (member->type != JSON_VALUE_OBJECT)
        return (JsonValue *)member; /* caller reports the type error */
    return (JsonValue *)member;
}

static bool manifest_parse_dependencies(JsonValue *dependencies_object,
                                        GpmDependency **out,
                                        size_t *out_count,
                                        char *error_buffer,
                                        size_t error_capacity) {
    *out = NULL;
    *out_count = 0;
    if (!dependencies_object)
        return true;
    if (dependencies_object->type != JSON_VALUE_OBJECT) {
        manifest_set_error(error_buffer, error_capacity,
                           "'%s' must be an object mapping aliases to source "
                           "specifiers.", "dependencies");
        return false;
    }
    size_t count = dependencies_object->member_count;
    GpmDependency *dependencies = (GpmDependency *)calloc(
        count ? count : 1, sizeof(*dependencies));
    if (!dependencies) {
        manifest_set_error(error_buffer, error_capacity,
                           "Out of memory reading dependencies.");
        return false;
    }
    for (size_t index = 0; index < count; index++) {
        JsonMember *member = &dependencies_object->members[index];
        if (member->value->type != JSON_VALUE_STRING) {
            free(dependencies);
            manifest_set_error(error_buffer, error_capacity,
                               "Dependency '%s' must map to a source "
                               "specifier string.", member->key);
            return false;
        }
        dependencies[index].alias = manifest_strdup(member->key);
        dependencies[index].source = manifest_strdup(member->value->string);
        if (!dependencies[index].alias || !dependencies[index].source) {
            for (size_t freed = 0; freed <= index; freed++) {
                free(dependencies[freed].alias);
                free(dependencies[freed].source);
            }
            free(dependencies);
            manifest_set_error(error_buffer, error_capacity,
                               "Out of memory reading dependencies.");
            return false;
        }
    }
    *out = dependencies;
    *out_count = count;
    return true;
}

static void manifest_free_dependencies(GpmDependency *dependencies,
                                       size_t count) {
    if (!dependencies)
        return;
    for (size_t index = 0; index < count; index++) {
        free(dependencies[index].alias);
        free(dependencies[index].source);
    }
    free(dependencies);
}

void gpm_package_trust_free(GpmPackageTrust *trust) {
    if (!trust)
        return;
    free(trust->keys);
    trust->keys = NULL;
    trust->key_count = 0;
    trust->signatures_required = false;
}

bool gpm_package_trust_copy(const GpmPackageTrust *source,
                            GpmPackageTrust *out) {
    memset(out, 0, sizeof(*out));
    if (!source)
        return true;
    out->signatures_required = source->signatures_required;
    if (!source->key_count)
        return true;
    out->keys = (GpmTrustedKey *)calloc(source->key_count,
                                      sizeof(*out->keys));
    if (!out->keys)
        return false;
    memcpy(out->keys, source->keys,
           source->key_count * sizeof(*out->keys));
    out->key_count = source->key_count;
    return true;
}

/* Parses "trustedPackageKeys" (keyId -> base64url public key) and
   "signedPackagesRequired" from a genesis.json or genesis.lock.json
   object. Mirrors the trustedExtensionKeys rules in
   src/native_extensions.c: every keyId must be the SHA-512 fingerprint
   of its public key. */
static bool manifest_parse_package_trust(
    JsonValue *root, GpmPackageTrust *trust, char *error_buffer,
    size_t error_capacity, const char *file_label) {
    memset(trust, 0, sizeof(*trust));
    const JsonValue *keys_member =
        json_object_member(root, "trustedPackageKeys");
    if (keys_member) {
        if (keys_member->type != JSON_VALUE_OBJECT) {
            manifest_set_error(error_buffer, error_capacity,
                               "'trustedPackageKeys' in %s must be an "
                               "object mapping keyId to a base64url public "
                               "key.", file_label);
            return false;
        }
        trust->keys = (GpmTrustedKey *)calloc(
            keys_member->member_count ? keys_member->member_count : 1,
            sizeof(*trust->keys));
        if (!trust->keys && keys_member->member_count) {
            manifest_set_error(error_buffer, error_capacity,
                               "Out of memory reading trustedPackageKeys.");
            return false;
        }
        for (size_t index = 0; index < keys_member->member_count; index++) {
            const JsonMember *entry = &keys_member->members[index];
            const JsonValue *public_key_value = entry->value;
            GpmTrustedKey *key = &trust->keys[index];
            if (public_key_value->type != JSON_VALUE_STRING) {
                manifest_set_error(error_buffer, error_capacity,
                                   "'trustedPackageKeys' entry in %s must "
                                   "map a keyId to a base64url public key "
                                   "string.", file_label);
                gpm_package_trust_free(trust);
                return false;
            }
            size_t decoded_key_id = 0;
            size_t decoded_public_key = 0;
            if (!genesis_signature_base64url_decode(
                    entry->key, strlen(entry->key), key->key_id,
                    GENESIS_KEY_ID_BYTES, &decoded_key_id) ||
                decoded_key_id != GENESIS_KEY_ID_BYTES ||
                !genesis_signature_base64url_decode(
                    public_key_value->string,
                    strlen(public_key_value->string), key->public_key,
                    GENESIS_PUBLIC_KEY_BYTES, &decoded_public_key) ||
                decoded_public_key != GENESIS_PUBLIC_KEY_BYTES) {
                manifest_set_error(error_buffer, error_capacity,
                                   "'trustedPackageKeys' entry in %s is "
                                   "not valid base64url key material.",
                                   file_label);
                gpm_package_trust_free(trust);
                return false;
            }
            uint8_t derived_key_id[GENESIS_KEY_ID_BYTES];
            genesis_signature_key_id(key->public_key, derived_key_id);
            if (memcmp(derived_key_id, key->key_id,
                       GENESIS_KEY_ID_BYTES) != 0) {
                manifest_set_error(error_buffer, error_capacity,
                                   "'trustedPackageKeys' keyId does not "
                                   "match its public key in %s.",
                                   file_label);
                gpm_package_trust_free(trust);
                return false;
            }
            trust->key_count++;
        }
    }
    const JsonValue *required_member =
        json_object_member(root, "signedPackagesRequired");
    trust->signatures_required =
        required_member && required_member->type == JSON_VALUE_TRUE;
    return true;
}

static JsonValue *json_build_trusted_keys(const GpmPackageTrust *trust) {
    JsonValue *object = json_value_new(JSON_VALUE_OBJECT);
    if (!object)
        return NULL;
    for (size_t index = 0; index < trust->key_count; index++) {
        const GpmTrustedKey *key = &trust->keys[index];
        char key_id_text[GENESIS_KEY_ID_BYTES * 2];
        char public_key_text[GENESIS_PUBLIC_KEY_BYTES * 2];
        if (!genesis_signature_base64url_encode(
                key->key_id, GENESIS_KEY_ID_BYTES, key_id_text,
                sizeof(key_id_text)) ||
            !genesis_signature_base64url_encode(
                key->public_key, GENESIS_PUBLIC_KEY_BYTES,
                public_key_text, sizeof(public_key_text)) ||
            !json_object_add(object, key_id_text,
                             json_string_new(public_key_text))) {
            json_value_free(object);
            return NULL;
        }
    }
    return object;
}

static bool manifest_write_trust_fields(JsonValue *root,
                                        const GpmPackageTrust *trust,
                                        char *error_buffer,
                                        size_t error_capacity) {
    if (trust->key_count) {
        JsonValue *keys = json_build_trusted_keys(trust);
        if (!keys || !json_object_add(root, "trustedPackageKeys", keys)) {
            json_value_free(keys);
            manifest_set_error(error_buffer, error_capacity,
                               "Out of memory writing trustedPackageKeys.");
            return false;
        }
    }
    if (trust->signatures_required &&
        !json_object_add(root, "signedPackagesRequired",
                         json_value_new(JSON_VALUE_TRUE))) {
        manifest_set_error(error_buffer, error_capacity,
                           "Out of memory writing signedPackagesRequired.");
        return false;
    }
    return true;
}

static void manifest_free_native_extensions(char **extensions,
                                            size_t count);

static bool manifest_parse_native_extensions(JsonValue *root,
                                              char ***out,
                                              size_t *out_count,
                                              char *error_buffer,
                                              size_t error_capacity) {
    *out = NULL;
    *out_count = 0;
    const JsonValue *member = json_object_member(root, "nativeExtensions");
    if (!member)
        return true;
    if (member->type != JSON_VALUE_ARRAY) {
        manifest_set_error(error_buffer, error_capacity,
                           "'nativeExtensions' must be an array of path "
                           "strings.");
        return false;
    }
    size_t count = member->item_count;
    char **extensions = (char **)calloc(count ? count : 1, sizeof(*extensions));
    if (!extensions) {
        manifest_set_error(error_buffer, error_capacity,
                           "Out of memory reading nativeExtensions.");
        return false;
    }
    for (size_t index = 0; index < count; index++) {
        const JsonValue *item = member->items[index];
        if (item->type != JSON_VALUE_STRING || !item->string[0]) {
            manifest_free_native_extensions(extensions, index);
            manifest_set_error(error_buffer, error_capacity,
                               "'nativeExtensions' entries must be non-empty "
                               "path strings.");
            return false;
        }
        extensions[index] = manifest_strdup(item->string);
        if (!extensions[index]) {
            manifest_free_native_extensions(extensions, index);
            manifest_set_error(error_buffer, error_capacity,
                           "Out of memory reading nativeExtensions.");
            return false;
        }
    }
    *out = extensions;
    *out_count = count;
    return true;
}

GpmManifest *gpm_manifest_new(void) {
    return (GpmManifest *)calloc(1, sizeof(GpmManifest));
}

static void manifest_free_native_extensions(char **extensions,
                                            size_t count) {
    if (!extensions)
        return;
    for (size_t index = 0; index < count; index++)
        free(extensions[index]);
    free(extensions);
}

void gpm_manifest_free(GpmManifest *manifest) {
    if (!manifest)
        return;
    free(manifest->name);
    free(manifest->version);
    free(manifest->entry);
    free(manifest->registry);
    manifest_free_dependencies(manifest->dependencies,
                             manifest->dependency_count);
    manifest_free_native_extensions(manifest->native_extensions,
                                    manifest->native_extension_count);
    gpm_package_trust_free(&manifest->package_trust);
    free(manifest);
}

static bool manifest_member_string(JsonValue *object, const char *key,
                                   char **out, bool required,
                                   char *error_buffer, size_t error_capacity) {
    const JsonValue *member = json_object_member(object, key);
    if (!member) {
        if (required) {
            manifest_set_error(error_buffer, error_capacity,
                               "Manifest is missing required field '%s'.", key);
            return false;
        }
        *out = NULL;
        return true;
    }
    if (member->type != JSON_VALUE_STRING) {
        manifest_set_error(error_buffer, error_capacity,
                           "Manifest field '%s' must be a string.", key);
        return false;
    }
    *out = manifest_strdup(member->string);
    if (!*out) {
        manifest_set_error(error_buffer, error_capacity,
                           "Out of memory reading manifest field '%s'.", key);
        return false;
    }
    return true;
}

GpmManifest *gpm_manifest_load(const char *path,
                                              char *error_buffer,
                                              size_t error_capacity) {
    char json_error[JSON_ERROR_SIZE];
    JsonValue *root = json_parse_file(path, json_error, sizeof(json_error));
    if (!root) {
        manifest_set_error(error_buffer, error_capacity, "%s", json_error);
        return NULL;
    }
    if (root->type != JSON_VALUE_OBJECT) {
        json_value_free(root);
        manifest_set_error(error_buffer, error_capacity,
                           "Manifest must be a JSON object.");
        return NULL;
    }
    GpmManifest *manifest = gpm_manifest_new();
    if (!manifest) {
        json_value_free(root);
        manifest_set_error(error_buffer, error_capacity,
                           "Out of memory reading manifest.");
        return NULL;
    }
    bool ok = manifest_member_string(root, "name", &manifest->name, true,
                                     error_buffer, error_capacity) &&
              manifest_member_string(root, "version", &manifest->version, true,
                                     error_buffer, error_capacity) &&
              manifest_member_string(root, "entry", &manifest->entry, false,
                                     error_buffer, error_capacity) &&
              manifest_member_string(root, "registry", &manifest->registry,
                                     false, error_buffer, error_capacity) &&
              manifest_parse_dependencies(
                  json_dependencies_object(root, "dependencies"),
                  &manifest->dependencies, &manifest->dependency_count,
                  error_buffer, error_capacity) &&
              manifest_parse_native_extensions(root, &manifest->native_extensions,
                                               &manifest->native_extension_count,
                                               error_buffer, error_capacity) &&
              manifest_parse_package_trust(root, &manifest->package_trust,
                                           error_buffer, error_capacity,
                                           GPM_MANIFEST_FILE);
    json_value_free(root);
    if (!ok) {
        gpm_manifest_free(manifest);
        return NULL;
    }
    return manifest;
}

static JsonValue *json_build_dependencies(
    const GpmDependency *dependencies, size_t count) {
    JsonValue *object = json_value_new(JSON_VALUE_OBJECT);
    if (!object)
        return NULL;
    for (size_t index = 0; index < count; index++) {
        JsonValue *source = json_string_new(dependencies[index].source);
        if (!source || !json_object_add(object, dependencies[index].alias,
                                        source)) {
            json_value_free(source);
            json_value_free(object);
            return NULL;
        }
    }
    return object;
}

static bool json_write_file(const JsonValue *value, const char *path,
                            char *error_buffer, size_t error_capacity) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        manifest_set_error(error_buffer, error_capacity,
                           "Cannot open '%s' for writing.", path);
        return false;
    }
    bool ok = json_write(value, file, true);
    if (fclose(file) != 0)
        ok = false;
    if (!ok)
        manifest_set_error(error_buffer, error_capacity,
                           "Cannot write '%s'.", path);
    return ok;
}

bool gpm_manifest_save(const GpmManifest *manifest,
                               const char *path, char *error_buffer,
                               size_t error_capacity) {
    JsonValue *root = json_value_new(JSON_VALUE_OBJECT);
    if (!root) {
        manifest_set_error(error_buffer, error_capacity,
                           "Out of memory writing manifest.");
        return false;
    }
    bool ok = json_object_add(root, "name", json_string_new(manifest->name)) &&
              json_object_add(root, "version",
                              json_string_new(manifest->version));
    if (ok && manifest->entry)
        ok = json_object_add(root, "entry", json_string_new(manifest->entry));
    if (ok && manifest->registry)
        ok = json_object_add(root, "registry",
                             json_string_new(manifest->registry));
    if (ok && manifest->dependency_count) {
        JsonValue *dependencies = json_build_dependencies(
            manifest->dependencies, manifest->dependency_count);
        if (dependencies)
            ok = json_object_add(root, "dependencies", dependencies);
        else
            ok = false;
    }
    if (ok && manifest->native_extension_count) {
        JsonValue *extensions = json_value_new(JSON_VALUE_ARRAY);
        if (extensions) {
            for (size_t index = 0; index < manifest->native_extension_count &&
                                  ok;
                 index++) {
                ok = json_array_add(extensions,
                                    json_string_new(
                                        manifest->native_extensions[index]));
            }
        }
        if (extensions && ok)
            ok = json_object_add(root, "nativeExtensions", extensions);
        else {
            json_value_free(extensions);
            ok = false;
        }
    }
    if (ok)
        ok = manifest_write_trust_fields(root, &manifest->package_trust,
                                         error_buffer, error_capacity);
    if (ok)
        ok = json_write_file(root, path, error_buffer, error_capacity);
    if (!ok && error_buffer && error_capacity && !error_buffer[0])
        manifest_set_error(error_buffer, error_capacity,
                           "Cannot write manifest '%s'.", path);
    json_value_free(root);
    return ok;
}

bool gpm_manifest_validate(const GpmManifest *manifest,
                                   char *error_buffer, size_t error_capacity) {
    if (!manifest) {
        manifest_set_error(error_buffer, error_capacity,
                           "Manifest is empty.");
        return false;
    }
    if (!gpm_name_valid(manifest->name)) {
        manifest_set_error(error_buffer, error_capacity,
                           "Manifest 'name' must be a scoped package name of "
                           "the form '@author/name' using lowercase letters, "
                           "digits, and '-'.");
        return false;
    }
    if (!gpm_version_valid(manifest->version)) {
        manifest_set_error(error_buffer, error_capacity,
                           "Manifest 'version' must be major.minor.patch.");
        return false;
    }
    if (manifest->entry && !manifest->entry[0]) {
        manifest_set_error(error_buffer, error_capacity,
                           "Manifest 'entry' may not be empty.");
        return false;
    }
    if (manifest->registry && !manifest->registry[0]) {
        manifest_set_error(error_buffer, error_capacity,
                           "Manifest 'registry' may not be empty.");
        return false;
    }
    for (size_t index = 0; index < manifest->dependency_count; index++) {
        const GpmDependency *dependency =
            &manifest->dependencies[index];
        if (!gpm_alias_valid(dependency->alias)) {
            manifest_set_error(error_buffer, error_capacity,
                               "Invalid dependency alias '%s'.", dependency->alias);
            return false;
        }
        for (size_t previous = 0; previous < index; previous++)
            if (!strcmp(manifest->dependencies[previous].alias,
                        dependency->alias)) {
                manifest_set_error(error_buffer, error_capacity,
                                   "Duplicate dependency alias '%s'.",
                                   dependency->alias);
                return false;
            }
        char reason[MANIFEST_ERROR_SIZE];
        if (!gpm_source_valid(dependency->source, reason,
                                      sizeof(reason))) {
            if (error_buffer && error_capacity)
                (void)snprintf(error_buffer, error_capacity,
                               "Dependency '%s': %s", dependency->alias, reason);
            return false;
        }
    }
    return true;
}

GpmDependency *gpm_manifest_find_dependency(
    GpmManifest *manifest, const char *alias) {
    if (!manifest || !alias)
        return NULL;
    for (size_t index = 0; index < manifest->dependency_count; index++)
        if (!strcmp(manifest->dependencies[index].alias, alias))
            return &manifest->dependencies[index];
    return NULL;
}

GpmLockfile *gpm_lockfile_new(void) {
    GpmLockfile *lockfile = (GpmLockfile *)calloc(
        1, sizeof(*lockfile));
    if (lockfile)
        lockfile->version = 1;
    return lockfile;
}

static void lockfile_free_package(GpmLockedPackage *package) {
    free(package->alias);
    free(package->source);
    free(package->commit);
    free(package->sha256);
    free(package->version);
    free(package->signature);
    manifest_free_dependencies(package->dependencies,
                             package->dependency_count);
}

void gpm_lockfile_free(GpmLockfile *lockfile) {
    if (!lockfile)
        return;
    for (size_t index = 0; index < lockfile->package_count; index++)
        lockfile_free_package(&lockfile->packages[index]);
    free(lockfile->packages);
    gpm_package_trust_free(&lockfile->package_trust);
    free(lockfile);
}

static bool lockfile_parse_dependencies(JsonValue *dependencies_object,
                                        GpmDependency **out,
                                        size_t *out_count,
                                        char *error_buffer,
                                        size_t error_capacity) {
    return manifest_parse_dependencies(dependencies_object, out, out_count,
                                       error_buffer, error_capacity);
}

GpmLockfile *gpm_lockfile_load(const char *path,
                                              char *error_buffer,
                                              size_t error_capacity) {
    char json_error[JSON_ERROR_SIZE];
    JsonValue *root = json_parse_file(path, json_error, sizeof(json_error));
    if (!root) {
        /* A missing lockfile is not an error: callers treat NULL with a
           cleared buffer as "no lockfile yet". */
        manifest_set_error(error_buffer, error_capacity, "%s", json_error);
        return NULL;
    }
    if (root->type != JSON_VALUE_OBJECT) {
        json_value_free(root);
        manifest_set_error(error_buffer, error_capacity,
                           "Lockfile must be a JSON object.");
        return NULL;
    }
    GpmLockfile *lockfile = gpm_lockfile_new();
    if (!lockfile) {
        json_value_free(root);
        manifest_set_error(error_buffer, error_capacity,
                           "Out of memory reading lockfile.");
        return NULL;
    }
    const JsonValue *version_member = json_object_member(root, "version");
    if (version_member && version_member->type == JSON_VALUE_NUMBER)
        lockfile->version = atoi(version_member->string);
    const JsonValue *packages_member = json_object_member(root, "packages");
    bool ok = manifest_parse_package_trust(root, &lockfile->package_trust,
                                           error_buffer, error_capacity,
                                           GPM_LOCKFILE_FILE);
    if (packages_member) {
        if (packages_member->type != JSON_VALUE_OBJECT) {
            manifest_set_error(error_buffer, error_capacity,
                               "Lockfile 'packages' must be an object.");
            ok = false;
        } else {
            size_t count = packages_member->member_count;
            GpmLockedPackage *packages =
                (GpmLockedPackage *)calloc(
                    count ? count : 1, sizeof(*packages));
            if (!packages && count) {
                manifest_set_error(error_buffer, error_capacity,
                                   "Out of memory reading lockfile packages.");
                ok = false;
            } else {
                for (size_t index = 0; index < count && ok; index++) {
                    JsonMember *member = &packages_member->members[index];
                    JsonValue *entry = member->value;
                    if (entry->type != JSON_VALUE_OBJECT) {
                        manifest_set_error(error_buffer, error_capacity,
                                           "Lockfile package '%s' must be an "
                                           "object.", member->key);
                        ok = false;
                        break;
                    }
                    GpmLockedPackage *package = &packages[index];
                    package->alias = manifest_strdup(member->key);
                    ok = manifest_member_string(entry, "source",
                                                &package->source, true,
                                                error_buffer, error_capacity) &&
                         manifest_member_string(entry, "commit",
                                                &package->commit, true,
                                                error_buffer, error_capacity) &&
                         manifest_member_string(entry, "sha256",
                                                &package->sha256, true,
                                                error_buffer, error_capacity) &&
                         manifest_member_string(entry, "version",
                                                &package->version, false,
                                                error_buffer, error_capacity) &&
                         manifest_member_string(entry, "signature",
                                                &package->signature, false,
                                                error_buffer, error_capacity) &&
                         lockfile_parse_dependencies(
                             json_dependencies_object(entry, "dependencies"),
                             &package->dependencies,
                             &package->dependency_count, error_buffer,
                             error_capacity);
                    if (!ok || !package->alias) {
                        if (!package->alias)
                            manifest_set_error(error_buffer, error_capacity,
                                               "Out of memory reading "
                                               "lockfile package '%s'.",
                                               member->key);
                        break;
                    }
                }
                if (ok) {
                    lockfile->packages = packages;
                    lockfile->package_count = count;
                } else {
                    for (size_t freed = 0; freed < count; freed++)
                        lockfile_free_package(&packages[freed]);
                    free(packages);
                }
            }
        }
    }
    json_value_free(root);
    if (!ok) {
        gpm_lockfile_free(lockfile);
        return NULL;
    }
    return lockfile;
}

static JsonValue *json_build_locked_package(
    const GpmLockedPackage *package) {
    JsonValue *entry = json_value_new(JSON_VALUE_OBJECT);
    if (!entry)
        return NULL;
    bool ok = json_object_add(entry, "source",
                              json_string_new(package->source)) &&
              json_object_add(entry, "commit",
                              json_string_new(package->commit)) &&
              json_object_add(entry, "sha256",
                              json_string_new(package->sha256));
    if (ok && package->version)
        ok = json_object_add(entry, "version",
                             json_string_new(package->version));
    if (ok && package->signature)
        ok = json_object_add(entry, "signature",
                             json_string_new(package->signature));
    if (ok && package->dependency_count) {
        JsonValue *dependencies = json_build_dependencies(
            package->dependencies, package->dependency_count);
        if (dependencies)
            ok = json_object_add(entry, "dependencies", dependencies);
        else
            ok = false;
    }
    if (!ok) {
        json_value_free(entry);
        return NULL;
    }
    return entry;
}

bool gpm_lockfile_save(const GpmLockfile *lockfile,
                               const char *path, char *error_buffer,
                               size_t error_capacity) {
    JsonValue *root = json_value_new(JSON_VALUE_OBJECT);
    if (!root) {
        manifest_set_error(error_buffer, error_capacity,
                           "Out of memory writing lockfile.");
        return false;
    }
    char version_text[16];
    (void)snprintf(version_text, sizeof(version_text), "%d", lockfile->version);
    JsonValue *version = json_number_new(version_text);
    bool ok = version && json_object_add(root, "version", version);
    JsonValue *packages = json_value_new(JSON_VALUE_OBJECT);
    if (ok && packages) {
        for (size_t index = 0; index < lockfile->package_count && ok;
             index++) {
            JsonValue *entry =
                json_build_locked_package(&lockfile->packages[index]);
            if (!entry ||
                !json_object_add(packages, lockfile->packages[index].alias,
                                 entry)) {
                json_value_free(entry);
                ok = false;
            }
        }
        if (ok)
            ok = json_object_add(root, "packages", packages);
        else
            json_value_free(packages);
    } else {
        json_value_free(packages);
        ok = false;
    }
    if (ok)
        ok = manifest_write_trust_fields(root, &lockfile->package_trust,
                                         error_buffer, error_capacity);
    if (ok)
        ok = json_write_file(root, path, error_buffer, error_capacity);
    json_value_free(root);
    if (!ok && error_buffer && error_capacity && !error_buffer[0])
        manifest_set_error(error_buffer, error_capacity,
                           "Cannot write lockfile '%s'.", path);
    return ok;
}

GpmLockedPackage *gpm_lockfile_find_package(
    GpmLockfile *lockfile, const char *alias) {
    if (!lockfile || !alias)
        return NULL;
    for (size_t index = 0; index < lockfile->package_count; index++)
        if (!strcmp(lockfile->packages[index].alias, alias))
            return &lockfile->packages[index];
    return NULL;
}
