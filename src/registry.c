/* registry.c: client for the static-JSON package registry described in
   docs/registry.md. The registry is a discovery layer only — resolution
   still ends at a github: source specifier handled by resolver.c. */

#include "registry.h"

#include "json.h"
#include "manifest.h"
#include "resolver.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void registry_error(char *buffer, size_t capacity, const char *format,
                           ...) {
    if (!buffer || !capacity)
        return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(buffer, capacity, format, arguments);
    va_end(arguments);
}

static char *registry_strdup(const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = (char *)malloc(length);
    if (copy)
        memcpy(copy, text, length);
    return copy;
}

bool gpm_registry_base_url(const char *override_url,
                           const char *manifest_registry, char *out,
                           size_t capacity) {
    const char *base = override_url && *override_url ? override_url : NULL;
    if (!base) {
        const char *env = getenv("GPM_REGISTRY_URL");
        if (env && *env)
            base = env;
    }
    if (!base)
        base = manifest_registry;
    if (!base || !*base)
        return false;
    size_t length = strlen(base);
    while (length && base[length - 1] == '/')
        length--;
    if (!length || length >= capacity)
        return false;
    memcpy(out, base, length);
    out[length] = '\0';
    return true;
}

bool gpm_registry_fetch_document(const char *base_url, const char *path,
                                 char **out_text, char *error_buffer,
                                 size_t error_capacity) {
    *out_text = NULL;
    char url[2048];
    int written = snprintf(url, sizeof(url), "%s/%s", base_url, path);
    if (written < 0 || (size_t)written >= sizeof(url)) {
        registry_error(error_buffer, error_capacity,
                       "Registry URL is too long.");
        return false;
    }
    if (!gpm_fetch_text_url(url, out_text, error_buffer, error_capacity)) {
        if (!error_buffer[0])
            registry_error(error_buffer, error_capacity,
                           "Could not fetch registry document '%s'.", path);
        return false;
    }
    return true;
}

static const char *registry_string_member(const JsonValue *object,
                                          const char *key) {
    const JsonValue *member = json_object_member(object, key);
    if (!member || member->type != JSON_VALUE_STRING || !member->string)
        return NULL;
    return member->string;
}

static const JsonValue *registry_version_entry(const JsonValue *document,
                                               const char *wanted) {
    const JsonValue *versions = json_object_member(document, "versions");
    if (!versions || versions->type != JSON_VALUE_ARRAY)
        return NULL;
    for (size_t index = 0; index < versions->item_count; index++) {
        const JsonValue *entry = versions->items[index];
        if (entry->type != JSON_VALUE_OBJECT)
            continue;
        const char *version = registry_string_member(entry, "version");
        if (version && wanted && !strcmp(version, wanted))
            return entry;
    }
    return NULL;
}

static void registry_list_versions(const JsonValue *document,
                                   char *error_buffer,
                                   size_t error_capacity) {
    const JsonValue *versions = json_object_member(document, "versions");
    size_t used = 0;
    if (error_buffer && error_capacity)
        error_buffer[0] = '\0';
    if (!versions || versions->type != JSON_VALUE_ARRAY)
        return;
    for (size_t index = 0; index < versions->item_count; index++) {
        const JsonValue *entry = versions->items[index];
        const char *version =
            entry->type == JSON_VALUE_OBJECT
                ? registry_string_member(entry, "version")
                : NULL;
        if (!version)
            continue;
        int written = snprintf(error_buffer + used,
                               error_capacity - used, "%s%s",
                               used ? ", " : "", version);
        if (written < 0 || (size_t)written >= error_capacity - used)
            break;
        used += (size_t)written;
    }
}

bool gpm_registry_resolve(const char *base_url, const char *alias,
                          const char *version, char **source_out,
                          char **signature_out, char *error_buffer,
                          size_t error_capacity) {
    *source_out = NULL;
    *signature_out = NULL;
    if (error_buffer && error_capacity)
        error_buffer[0] = '\0';
    if (!gpm_alias_valid(alias)) {
        registry_error(error_buffer, error_capacity,
                       "Invalid package alias '%s'.", alias);
        return false;
    }
    char path[512];
    int written = snprintf(path, sizeof(path), "packages/%s.json", alias);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        registry_error(error_buffer, error_capacity,
                       "Registry path is too long.");
        return false;
    }
    char *text = NULL;
    if (!gpm_registry_fetch_document(base_url, path, &text, error_buffer,
                                     error_capacity)) {
        if (!error_buffer[0])
            registry_error(error_buffer, error_capacity,
                           "Registry has no package '%s'.", alias);
        return false;
    }
    char json_error[JSON_ERROR_SIZE];
    JsonValue *document = json_parse(text, json_error, sizeof(json_error));
    free(text);
    if (!document) {
        registry_error(error_buffer, error_capacity,
                       "Registry document for '%s' is not valid JSON: %s",
                       alias, json_error);
        return false;
    }
    if (document->type != JSON_VALUE_OBJECT) {
        json_value_free(document);
        registry_error(error_buffer, error_capacity,
                       "Registry document for '%s' must be a JSON object.",
                       alias);
        return false;
    }
    const char *wanted = version;
    char latest[128];
    if (!wanted) {
        const char *latest_field = registry_string_member(document, "latest");
        if (!latest_field || !*latest_field) {
            json_value_free(document);
            registry_error(error_buffer, error_capacity,
                           "Registry document for '%s' has no 'latest' "
                           "version; pass --version.", alias);
            return false;
        }
        snprintf(latest, sizeof(latest), "%s", latest_field);
        wanted = latest;
    }
    const JsonValue *entry = registry_version_entry(document, wanted);
    if (!entry) {
        char available[512];
        registry_list_versions(document, available, sizeof(available));
        json_value_free(document);
        if (available[0])
            registry_error(error_buffer, error_capacity,
                           "Registry has no version '%s' of '%s' "
                           "(available: %s).", wanted, alias, available);
        else
            registry_error(error_buffer, error_capacity,
                           "Registry has no version '%s' of '%s'.",
                           wanted, alias);
        return false;
    }
    const char *source = registry_string_member(entry, "source");
    if (!source || strncmp(source, "github:", 7) != 0) {
        json_value_free(document);
        registry_error(error_buffer, error_capacity,
                       "Registry version '%s' of '%s' has no usable "
                       "'github:' source.", wanted, alias);
        return false;
    }
    const char *signature = registry_string_member(entry, "signature");
    *source_out = registry_strdup(source);
    if (signature)
        *signature_out = registry_strdup(signature);
    json_value_free(document);
    if (!*source_out || (signature && !*signature_out)) {
        free(*source_out);
        free(*signature_out);
        *source_out = NULL;
        *signature_out = NULL;
        registry_error(error_buffer, error_capacity, "Out of memory.");
        return false;
    }
    return true;
}
