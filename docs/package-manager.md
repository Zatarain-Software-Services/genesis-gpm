# Genesis package manager design

Status: approved design, all slices implemented. This document defines the
standalone package-manager executable for shareable Genesis source packages.
The `gpm` tool — manifest/lockfile model, `init`, `check`, `remove`, and the
git-based `add`/`install`/`update` resolver — lives in `tools/gpm/` and
builds as a standalone executable, and the runtime resolves `@pkg/`
specifiers against `.genesis/packages/`. Slice 5 added the focused
coverage: alias conflicts (manifest-level, lockfile-level, and transitive),
archive hash-mismatch rejection, lockfile rebuild on plain `install`,
frozen-mode staleness detection, deduplication of already-locked
transitive dependencies, and `@pkg/` edge cases (nested packages-root
precedence and shadowing, alias-is-a-file, subpath case diagnostics,
explicit `.genesis` extensions). All package tests run under
`GENESIS_MINIMAL_BUILD=ON`.

## Scope and boundaries

- The package manager is a **separate executable** (`gpm`, the Genesis
  Package Manager), never
  linked into `genesis_runtime` or the `genesis` CLI. Production runtime
  builds contain no package-manager, network, or git code.
- The runtime **consumes already-resolved source packages**. It performs no
  network access, no git operations, and does not read manifests or the
  lockfile for content. Its only package-related rule is mapping the reserved
  `@pkg/` specifier prefix onto an installed packages directory (see
  [Import resolution](#import-resolution)).
- Standard modules (`@std/`) are unchanged. Optional native extensions are
  a third, distinct layer declared per project — see
  [Native extensions](#native-extensions) below. Packages are a fourth
  layer: shareable Genesis source only. **A package cannot ship native
  code, and `gpm` will never fetch, install, or manage native libraries —
  this is a deliberate, permanent security boundary.**
- Implementation language is C11, matching the repository rule. The manager
  may shell out to the `git` executable; it must not require libcurl or any
  new runtime dependency.

## Package identity and manifest

A package is a directory tree of `.genesis` source files. Its manifest is
`genesis.json` at the package root:

```json
{
  "name": "@example/http-kit",
  "version": "1.4.0",
  "entry": "index.genesis",
  "dependencies": {
    "json": "github:example/json-tools@v2.1.0"
  }
}
```

Field rules:

- `name` (required): canonical package identity in scoped form
  `@author/name`. `author` is the publishing GitHub user or organization —
  every Genesis package source is a GitHub repository, so the scope is
  always known. Both segments are lowercase `[a-z0-9]` with interior `-`
  and may not start or end with `-`. It is metadata and diagnostic text;
  it is **not** the import key (see aliases below).
- `version` (required): `major.minor.patch` numeric string. Informational
  in v1; the lockfile, not this field, determines what is installed.
- `entry` (optional): module specifier used when the package is imported
  without a subpath. Defaults to `index.genesis`. Must resolve inside the
  package by the same rules as any module path.
- `dependencies` (optional): map of **alias → source specifier**. This is
  the manifest-alias mechanism: the alias is what importers write after
  `@pkg/`, not the upstream repository name.
- `nativeExtensions` (optional): array of strings naming native extension
  library files to load for this project. See
  [Native extensions](#native-extensions).
- `registry` (optional): base URL of a static-JSON package registry used
  by `gpm add <alias>`, `gpm info`, and `gpm search`. See
  [the registry design](registry.md). `GPM_REGISTRY_URL` and the
  `--registry` flag override it; there is no built-in default.

Source specifiers in v1 have exactly one form:

```
github:<owner>/<repo>[@<ref>]
```

- `<ref>` may be a tag, branch, or full/short commit SHA. When omitted, the
  repository's default branch HEAD is used **only** to compute the locked
  commit; the lockfile always records the resolved immutable SHA.
- Other hosts, local paths, and hosted registry *services* are out of
  scope. A dependency specifier in `genesis.json` is always a resolved
  `github:` source — registry aliases are resolved by `gpm add` before
  anything is recorded (see [the registry design](registry.md)). The
  specifier grammar is parsed, not evaluated, so additional schemes can
  be added later without changing manifests.

## Native extensions

`genesis.json` may also declare `"nativeExtensions"`: an array of paths to
compiled extension libraries (`.so`, `.dylib`, or `.dll`) that the project
loads explicitly:

```json
{
  "name": "my-app",
  "version": "0.1.0",
  "nativeExtensions": [
    "./build/extensions/libmath_extension.so",
    "./vendor/libacme_db.so"
  ]
}
```

Rules:

- Entries are paths relative to the manifest's directory, or absolute.
  The nearest `genesis.json` at or above the entry `.genesis` file's
  directory is used; the CLI (`genesis run`/`check`) and `genesis-lsp`
  share this resolution, so editor diagnostics match command-line results.
- The manifest lists **files, not directories** — nothing is globbed or
  scanned implicitly. Each listed file is `dlopen`ed/`LoadLibrary`ed and
  its `genesis_register` entry point supplies the authoritative type
  signatures; the manifest never duplicates type information.
- Loading native code executes it with the process's privileges. Entries
  are added **manually by the project author**; `gpm` neither fetches nor
  installs native libraries, and packages cannot ship them. Opening a
  project in an editor therefore runs whatever binaries its manifest
  names — only open projects you trust.
- A missing or unloadable declared file is an error for the CLI and a
  logged warning for the language server (the editor stays usable while
  extensions are not yet built).
- The runtime deduplicates libraries by canonical path, so a file listed
  in the manifest and also supplied through other loaders is registered
  once.

## Package signing

Package archives can carry detached Ed25519 signatures per
`docs/signing.md`. Signing is opt-in: unsigned packages keep working
unless the project sets `signedPackagesRequired`.

`genesis.json` and `genesis.lock.json` accept two optional members:

```json
{
  "trustedPackageKeys": {
    "<keyId>": "<base64url 32-byte Ed25519 public key>"
  },
  "signedPackagesRequired": true
}
```

- `trustedPackageKeys` maps a keyId (the first 16 bytes of
  SHA-512(public key), base64url) to its base64url public key. Entries
  whose keyId does not match the key's fingerprint are rejected as
  malformed. A signature from a key not listed here is never accepted.
- `signedPackagesRequired: true` makes a missing signature a hard error;
  absent or false keeps unsigned packages installable. A *present*
  signature must always verify against a listed key, required or not.
  The requirement applies to transitive dependencies too — an unsigned
  transitive fails closed like any other package.
- `gpm add`/`install`/`update` look for the signature as a
  `<archive-url>.sig` sidecar downloaded alongside the archive (the same
  mechanism mirrors and registries can serve). For `add <alias>` through
  a registry, a `signature` embedded in the registry's version entry is
  used when no sidecar exists; either way a `signature` member already
  pinned in `genesis.lock.json` is the final fallback. The verified
  GENESIS-SIG-1 envelope is recorded in the lockfile entry so `install`
  re-verifies the pinned signature before fetching.
- The signed payload is canonical:
  `gpm-package-v1\n<alias>\n<version-or-commit>\n<sha256-of-archive>`
  (LF separators, no trailing newline), binding package identity and
  content hash. The version-or-commit field is the package manifest's
  `version` when present, otherwise the pinned commit.
- `gpm` ships signing subcommands: `gpm keygen` generates an Ed25519
  keypair into `~/.config/genesis/keys/<keyId>.key` (mode 0600; override
  the directory with `GPM_KEYS_DIR`) and prints the keyId/publicKey pair
  to pin; `gpm sign --key <keyfile> --alias <a> (--version <v> |
  --commit <c>) <archive>` writes `<archive>.sig`; `gpm verify --key
  <keyfile>` or `--pubkey <base64url>` checks a `.sig` against an
  archive. Private keys never enter `genesis.json`.
- `gpm sign-file --key <keyfile> <file> [sig]` and `gpm verify-file
  --key <keyfile> | --pubkey <base64url> <file> [sig]` sign and verify
  **raw file bytes** — the same GENESIS-SIG-1 envelope with the file
  contents as the payload. This is the format registry documents
  (`<doc>.sig` sidecars, see [the registry design](registry.md)) and
  native extension binaries use.

## Lockfile

Each project root keeps `genesis.lock.json`, generated and rewritten only by
`gpm`. It is committed to version control.

```json
{
  "version": 1,
  "packages": {
    "http-kit": {
      "source": "github:zatarain/http-kit@v1.4.0",
      "commit": "9f2c1ab4d5e6...",
      "sha256": "…64 hex chars…",
      "version": "1.4.0",
      "signature": "GENESIS-SIG-1\nkeyId: …\nalg: ed25519\nsig: …",
      "dependencies": {
        "json": "github:example/json-tools@v2.1.0"
      }
    }
  },
  "trustedPackageKeys": {
    "<keyId>": "<base64url public key>"
  },
  "signedPackagesRequired": false
}
```

- Keys are **aliases** (the `@pkg/` path segment), matching the dependency
  alias in the dependent's manifest.
- `commit` is the immutable 40-hex SHA the source specifier resolved to.
  `sha256` is the SHA-256 of the downloaded source archive, checked before
  unpacking on every install.
- Transitive dependencies are recorded flattened under their own aliases; a
  dependency conflict (two packages requiring different aliases is fine —
  aliases are per-importer — but the **same alias resolving to different
  commits** in one lockfile) is a hard error reported by `install`/`add`.

## Import resolution

- Reserved specifier prefix `@pkg/<alias>` and `@pkg/<alias>/<subpath>`.
  `@pkg/` joins `@std/` as a resolver-handled prefix; it is never treated as
  a filesystem path.
- The runtime resolves `@pkg/<alias>` to
  `<packages-root>/<alias>` and `@pkg/<alias>/<subpath>` to
  `<packages-root>/<alias>/<subpath>`, then applies the existing module rules
  unchanged: `.genesis` may be omitted, a directory resolves to
  `index.genesis`, exact-case checks apply, cycles are rejected, and modules
  execute once per runtime.
- `<packages-root>` discovery: the runtime walks upward from the **importing
  file's directory** looking for a directory named `.genesis/packages`. The
  first match wins. If none exists, `@pkg/` imports fail with a diagnostic
  telling the user to run `gpm install`. No lockfile or manifest is
  read — the on-disk layout is the whole contract.
- `gpm install` materializes packages at
  `<project>/.genesis/packages/<alias>/`, so discovery works for any entry
  point inside the project tree.
- `@pkg/<alias>` with no subpath resolves through the package's installed
  `index.genesis`; a package whose manifest declares a different `entry`
  still uses `index.genesis` for bare imports in v1 (the `entry` field is
  reserved for future subpath remapping and currently only validated).

## Commands (v1 surface)

`gpm` is a small CLI; exact flags may be refined during
implementation, but the operations are fixed:

- `init` — write a starter `genesis.json` in the current directory. Fails if
  a manifest already exists.
- `check` — validate `genesis.json` (required fields, alias and source
  rules) and, when present, `genesis.lock.json` (commit/SHA-256 shapes and
  specifier syntax). A missing lockfile is valid; a missing or malformed
  manifest is an error.
- `add <source-specifier> [--alias <name>] [--version <v>] [--registry <u>]`
  — resolve the specifier to a commit, download and verify the archive,
  update `genesis.json` dependencies and `genesis.lock.json`, and install
  into `.genesis/packages/`. A specifier that does not start with
  `github:` is treated as a registry alias: the configured registry's
  `packages/<alias>.json` selects the version (`--version`, or the
  document's `latest` field) and yields the `github:` source actually
  recorded. An embedded `signature` field is verified exactly like a
  lockfile-pinned signature.
- `info <alias> [--registry <u>]` — print a registry package document:
  name, description, license, homepage, latest, and the version list.
- `search <query> [--registry <u>]` — substring-match (case-insensitive)
  `index.json` entries on alias, name, and description.
- `remove <alias>` — drop the dependency from `genesis.json`, remove its
  lockfile entry when a lockfile exists, and delete
  `.genesis/packages/<alias>/` when present.
- `install` — read `genesis.lock.json` and materialize
  `.genesis/packages/` without network access when a verified cache entry
  exists; otherwise fetch each pinned commit. `--frozen` forbids any
  resolution change and fails if the lockfile is stale relative to
  `genesis.json`. A package counts as a verified cache entry when its
  `.gpm-verified` marker records the lockfile's commit and archive SHA-256;
  anything else is re-fetched and re-verified.
- `update [alias]` — re-resolve specifiers to current refs and rewrite the
  lockfile (explicit opt-in; `install` never moves pins). Without an alias
  every manifest dependency is re-resolved; transitive entries keep their
  pins unless their parent package changed. A ref that still resolves to
  the pinned commit skips the archive download entirely when the installed
  `.gpm-verified` marker matches.
- `tree` — print the dependency tree: the project manifest's direct
  dependencies at the root, each locked package's recorded dependencies
  nested underneath. Repeated subtrees are marked `(deduped)`, manifest
  dependencies missing from the lockfile show `(not installed)`, and
  lockfile packages unreachable from the manifest are listed as extraneous.
- `keygen` / `sign` / `verify` — package signing key generation, archive
  signing, and signature verification; see "Package signing" above.

## GitHub resolution and integrity

- Resolution uses the `git` CLI: `git ls-remote` maps a ref to a commit SHA
  without credentials for public repositories; the codeload tarball URL
  (`https://codeload.github.com/<owner>/<repo>/tar.gz/<commit>`) fetches the
  pinned commit via `curl`, and `tar` unpacks it. Private repositories are
  out of scope for v1 (no credential handling is defined).
- Registry access adds one more operator-facing override:
  `GPM_REGISTRY_URL` replaces the manifest's `registry` field (and is
  itself overridden by `--registry`). Registry documents are fetched
  with `curl`, so `file://` URLs work for tests and mirrors. When a
  registry publishes `<doc>.sig` sidecars (raw-bytes signatures per
  [registry.md](registry.md)), `add <alias>`/`info`/`search` verify them
  against `trustedPackageKeys`: pinned signatures must verify, while
  absent sidecars and unpinned signers are ignored because registry
  metadata is advisory.
- Two environment overrides exist for mirrors and offline testing:
  `GPM_GIT_BASE_URL` replaces `https://github.com` as the repository base
  (`<base>/<owner>/<repo>` is passed to `git ls-remote`), and
  `GPM_ARCHIVE_BASE_URL` replaces `https://codeload.github.com` as the
  archive base. They are operator-facing escape hatches, not part of the
  package format contract.
- Every download is verified twice: the resolved commit SHA must match the
  lockfile `commit`, and the archive SHA-256 must match `sha256` before any
  file is written into `.genesis/packages/`. A mismatch fails the command
  and leaves the previous installation untouched.
- Package contents are trusted source: the manager validates structure
  (manifest parses, entry resolves) but does not sandbox or audit Genesis
  code. This matches the educational-language posture; packages are code,
  not data.

## Compatibility rules

- v1 has **no version-range resolution**. A dependency specifier names a
  repository plus an exact ref; compatibility is whatever the author
  published under that ref. `version` fields are informational.
- There is no `engines`/language-version gate in v1. If Genesis later needs
  one, it is a manifest field plus an `install` check — no resolver change.
- Alias rules: lowercase `[a-z0-9][a-z0-9-]*`, unique within a manifest,
  and may not equal `std` or collide with another alias after
  normalization. The alias, not the upstream repo name, is the `@pkg/`
  segment, so two packages may vendor the same upstream repository under
  different aliases.
- Packages may themselves declare dependencies; the manager flattens them
  into the root lockfile (no nested `node_modules`-style trees).

## Implementation slices

1. [x] `gpm` skeleton: argument parsing, `genesis.json`/`genesis.lock.json`
   read/write and validation, alias/specifier rules, `init`, `check`, and
   `remove` (manifest, lockfile, and installed-directory bookkeeping only —
   no network). Implemented in `tools/gpm/` with focused CTest
   scenarios under `tests/gpm_fixtures/`.
2. [x] Resolver: `git ls-remote` ref→SHA (branch, peeled annotated tag, tag,
   then exact refname; remote `HEAD` when no ref is given), codeload tarball
   download, SHA-256 verification before unpacking, `tar --strip-components`
   extraction, and package-structure validation (parseable `genesis.json`,
   resolvable `entry`). Implemented in `tools/gpm/resolver.c`; fetches stage
   into `.genesis/packages/.staging-<alias>` and only replace the installed
   directory after verification succeeds. POSIX-only for now — Windows
   builds compile but report resolution as unsupported.
3. [x] `add`/`install`/`update` commands and `--frozen` semantics. `add`
   resolves, verifies, installs, records manifest + flattened lockfile
   entries, and recurses into the package's own dependencies; alias
   collisions against differently-sourced packages are hard errors.
   `install` reuses `.gpm-verified` markers to skip verified packages,
   re-fetches pinned commits otherwise, resolves manifest dependencies
   missing from the lockfile (unless `--frozen`), and `--frozen` fails when
   the lockfile does not cover the manifest. `update [alias]` re-resolves
   and re-pins. Covered end-to-end by `tests/expect_gpm_resolver.cmake`,
   which stands up a local git remote and `file://` archive base.
4. [x] Runtime `@pkg/` specifier support and `.genesis/packages` discovery —
   the only runtime change, kept deliberately tiny. `module_resolve_path`
   intercepts the `@pkg/` prefix before filesystem resolution, walks upward
   from the importing file for the nearest `.genesis/packages` directory,
   validates the alias (`[a-z0-9-]`) and rejects `.`/`..` subpath segments,
   then applies the ordinary module rules (extensionless paths, directory →
   `index.genesis`, exact-case checks, cycle rejection). Failures report
   whether no packages root exists, the alias is not installed, or the
   specifier is invalid — each directing the user to `gpm install` where
   relevant. No manifest or lockfile is read. Covered by
   `tests/pkg_project/` fixtures (`genesis_pkg_*` CTest cases), including a
   package importing another package through `@pkg/`.
5. [x] Focused tests: alias conflicts (manifest redeclaration, lockfile
   alias pinned to a different source, and transitive conflicts), archive
   SHA-256 mismatch rejection, lockfile rebuild via plain `install`,
   `--frozen` staleness when manifest sources disagree with pins,
   deduplication of already-locked dependencies, `update` of an unknown
   alias, `@pkg/` edge cases (nested `.genesis/packages` precedence and
   shadowing, alias path that is a file, subpath case-mismatch
   diagnostics, explicit `.genesis` extension imports), and
   minimal-build unaffectedness (`GENESIS_MINIMAL_BUILD=ON` runs all
   `genesis_pkg_*` and `gpm_*` tests).

## Explicit non-goals

- No hosted registry *service* (static-JSON indexes per
  [registry.md](registry.md) are supported), no semver ranges, no
  private-repository
  credentials, no native/binary packages, no global package cache sharing
  semantics beyond the per-project `.genesis/` directory, no runtime
  network access, and no browser-console-style `table` or other unrelated
  standard-library work.
