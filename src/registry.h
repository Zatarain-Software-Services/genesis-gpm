#ifndef GPM_REGISTRY_H
#define GPM_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

#include "manifest.h"

/* Resolves the effective registry base URL. Precedence: override_url
   (--registry flag), then the GPM_REGISTRY_URL environment variable,
   then manifest_registry (the "registry" field in genesis.json). The
   result is normalized to have no trailing '/'. Returns false when no
   source is configured. out must hold at least capacity bytes. */
bool gpm_registry_base_url(const char *override_url,
                           const char *manifest_registry, char *out,
                           size_t capacity);

/* Fetches "<base_url>/<path>" (e.g. "index.json" or
   "packages/<alias>.json") as text. On success *out_text receives a
   malloc'd NUL-terminated body (caller frees). */
bool gpm_registry_fetch_document(const char *base_url, const char *path,
                                 char **out_text, char *error_buffer,
                                 size_t error_capacity);

/* Verifies a registry document's detached signature when the project
   pins the signer's key. doc_text is the raw document bytes exactly as
   fetched; path is the document's registry-relative path (e.g.
   "packages/<alias>.json"). The sidecar "<path>.sig" signs the raw file
   bytes per docs/registry.md. Absent or unfetchable sidecars and
   signatures by unpinned keys are ignored — registry metadata is
   advisory. Returns false only when a pinned signature is malformed or
   fails verification. */
bool gpm_registry_verify_document(const char *base_url, const char *path,
                                  const char *doc_text,
                                  const GpmPackageTrust *trust,
                                  char *error_buffer,
                                  size_t error_capacity);

/* Resolves a registry alias to a gpm source specifier. When version is
   non-NULL the package document's versions[] entry with that exact
   version is selected; otherwise the document's "latest" field
   selects it. trust, when non-NULL, enables .sig verification of the
   fetched document per gpm_registry_verify_document. On success
   *source_out receives a malloc'd "github:..." specifier and
   *signature_out a malloc'd GENESIS-SIG-1 envelope when the entry
   embeds one (NULL otherwise); both caller-freed. */
bool gpm_registry_resolve(const char *base_url, const char *alias,
                          const char *version,
                          const GpmPackageTrust *trust, char **source_out,
                          char **signature_out, char *error_buffer,
                          size_t error_capacity);

#endif
