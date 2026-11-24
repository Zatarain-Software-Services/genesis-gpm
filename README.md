# genesis-gpm

The standalone package manager for [Genesis](https://github.com/Zatarain-Software-Services/Genesis) source packages. `gpm` resolves `github:` source specifiers (and registry aliases) to immutable commits, verifies archive integrity and optional Ed25519 signatures, and installs packages under `.genesis/packages/` for `@pkg/` imports.

Extracted from the Genesis monorepo (`tools/gpm`); package format and trust semantics are documented in [docs/package-manager.md](docs/package-manager.md), the registry client in [docs/registry.md](docs/registry.md), and the author workflow in [docs/package-authoring.md](docs/package-authoring.md).

## Build

```sh
cmake -S . -B build
cmake --build build
```

Requires a C11 compiler plus `git`, `curl`, `tar`, and a SHA-256 tool (`sha256sum` or `shasum`) on POSIX hosts for package resolution; manifest/lockfile commands (`init`, `check`, `remove`, `tree`, `keygen`, `sign`, `verify`) work everywhere including Windows, where resolution reports a clear unsupported error.

## Test

```sh
cd build && ctest
```

## Usage

```sh
gpm init                     # create genesis.json
gpm add github:owner/repo@v1 # or: gpm add <alias> with a registry configured
gpm install [--frozen]
gpm update [alias]
gpm tree
gpm keygen / sign / verify   # package signing
gpm sign-file / verify-file  # raw-bytes signatures (registry docs, extension binaries)
```

See `gpm --help` and the docs for details.

## License

GPL-3.0 — see [LICENSE](LICENSE). The signature envelope implementation vendors TweetNaCl (`src/vendor/tweetnacl/`), public domain.
