# Working in genesis-gpm

`gpm` is the standalone package manager for Genesis source packages,
extracted from the Genesis monorepo. It never links against
`genesis_runtime` and never distributes native binaries — packages are
Genesis source only, by design.

## Layout

- `src/gpm.c` — CLI commands and orchestration.
- `src/manifest.c`, `src/json.c` — `genesis.json`/`genesis.lock.json`
  model (json.c is vendored from the Genesis runtime).
- `src/registry.c` — static-JSON registry client (`docs/registry.md`).
- `src/resolver.c` — git/curl/tar resolution and verified fetching
  (POSIX-only; Windows stubs report unsupported).
- `src/signing.c`, `src/signature.c`, `src/vendor/tweetnacl/` —
  Ed25519 `GENESIS-SIG-1` package signatures (vendored from Genesis).
- `tests/` — fixture-driven scenarios plus the resolver end-to-end
  script (`file://` git remote + archive + registry stand-ins).
- `docs/` — package-manager design, registry design, authoring guide.

## Rules

- Keep implementation in C11; match the existing compact style.
- The package format is a contract shared with the Genesis runtime's
  `@pkg/` resolver — breaking changes need a matching Genesis release.
- `gpm` must remain source-only forever: no native libraries, no
  registry-hosted binaries.
- Run `cmake -S . -B build && cmake --build build && ctest` before
  committing; the resolver end-to-end test is POSIX-only.
- Commit each completed task locally; do not push unless asked.
