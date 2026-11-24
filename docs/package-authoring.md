# Publishing a Genesis package

Everything a package author needs: repository layout, versioning,
signing keys, signing releases, and getting listed in the community
registry. Design rationale lives in [package-manager.md](package-manager.md)
and [registry.md](registry.md); trust/ceremony details in
[signing.md](signing.md). This page is the operational checklist.

## 1. Repository layout

A package is plain Genesis source — gpm never distributes binaries:

```
my-package/
  genesis.json        # manifest (required)
  index.genesis       # entry module (default entry)
  lib/                # optional submodules for @pkg/<alias>/lib/...
  README.md
```

Minimal `genesis.json`:

```json
{
  "name": "@you/my-package",
  "version": "0.1.0",
  "entry": "index.genesis",
  "dependencies": {
    "somedep": "github:other/repo@v1.0.0"
  }
}
```

- `name` is `@author/name`, lowercase `[a-z0-9-]` segments. `author`
  should match the GitHub user/org that owns the repo.
- `entry` defaults to `index.genesis`; consumers can also import
  subpaths like `@pkg/my-package/lib/util`.
- `dependencies` map **your chosen alias** → `github:owner/repo@ref`.
  Aliases are what consumers write after `@pkg/` — pick short,
  collision-resistant names.
- Optional `"registry"` configures *your own* project's registry; it is
  not part of the published package contract.

Validate locally before pushing:

```sh
gpm check
```

## 2. Versioning and tags

Consumers pin whatever ref the specifier names; `gpm` always resolves
it to an **immutable commit SHA** in `genesis.lock.json`. Tag releases
so refs stay meaningful:

```sh
git tag v0.1.0 && git push origin v0.1.0
```

Consumers then write `github:you/my-package@v0.1.0` (or the registry
maps an alias to it). There are no semver ranges — a ref is exact.

## 3. Signing keys

Signing is optional but recommended for anything others will depend
on. One Ed25519 keypair per author/org is enough.

### Generate

```sh
gpm keygen
```

Output:

```
  keyId:      <16-byte fingerprint, base64url>
  publicKey:  <32-byte Ed25519 public key, base64url>
  privateKey: ~/.config/genesis/keys/<keyId>.key
```

- The private key file holds the 64-byte secret (seed‖public), mode
  `0600`, under `~/.config/genesis/keys/` — **outside any repository**.
- `GPM_KEYS_DIR` overrides the directory (useful for CI or tests).
- The `keyId` is the first 16 bytes of `SHA-512(publicKey)` — it is
  derived, not chosen, so it can never disagree with the key.

### Custody rules

- **Never commit a `.key` file.** It *is* your publishing identity.
- Back it up offline/encrypted. Losing it means rotating to a new key
  and asking consumers to update `trustedPackageKeys`; leaking it means
  revocation — there is no recovery.
- The public half (`keyId` + `publicKey`) is safe to publish anywhere:
  docs, registry entries, your README.

### .gitignore

Keys belong outside the repo, so a normal project needs nothing. If you
ever keep keys inside a working tree (e.g. a shared build dir), ignore
them explicitly:

```gitignore
*.key
```

`.sig` files are **public** — signatures are meant to be distributed,
so do not ignore them.

## 4. Signing a release

The signed payload binds three things:

```
gpm-package-v1\n<alias>\n<version-or-commit>\n<sha256-of-archive>
```

`alias` is the dependency alias consumers will use, `version-or-commit`
is the package manifest's `version` (or the pinned commit when the
manifest has none), and `sha256` is the digest of the exact archive
bytes consumers will download.

### Steps

```sh
# 1. Fetch the exact archive gpm will download (codeload tarball at the
#    tagged commit):
curl -fsSL -o my-package.tar.gz \
  "https://codeload.github.com/you/my-package/tar.gz/<commit-sha>"

# 2. Sign it. --alias must equal the dependency alias consumers use;
#    --version should equal the package's genesis.json version.
gpm sign \
  --key ~/.config/genesis/keys/<keyId>.key \
  --alias my-package \
  --version 0.1.0 \
  my-package.tar.gz
# writes my-package.tar.gz.sig

# 3. Verify before publishing:
gpm verify \
  --pubkey <base64url public key> \
  --alias my-package \
  --version 0.1.0 \
  my-package.tar.gz my-package.tar.gz.sig
```

Notes:

- Sign the **archive**, not the repo — the signature covers the tarball
  bytes and their sha256, which is what `gpm` verifies.
- `--commit <sha>` is the alternative binding when you don't want to
  rely on the manifest `version`.
- Re-sign for every release: the payload includes version and archive
  hash, so a v0.1.0 signature never validates v0.2.0.
- `gpm sign`/`verify` also work under WASM with an imported key file;
  only `keygen` needs native entropy.

## 5. How consumers receive the signature

`gpm` looks for a signature in this order:

1. **`<archive-url>.sig` sidecar** — works for mirrors and `file://`
   hosts where you control the URL space. For `github:` sources the
   codeload URL cannot host sidecars, so this path effectively serves
   non-GitHub registries/mirrors.
2. **Registry `signature` field** — the practical path for GitHub
   packages: embed the `.sig` envelope text in the version entry of
   `packages/<alias>.json` in the registry (see §6). Verified exactly
   like a lockfile pin.
3. **`genesis.lock.json` `signature` member** — pinned automatically
   after the first verified install, so `install --frozen` re-verifies
   without re-consulting the registry.

Consumers opt in via `genesis.json`:

```json
{
  "trustedPackageKeys": {
    "<your keyId>": "<your publicKey>"
  },
  "signedPackagesRequired": true
}
```

With `signedPackagesRequired`, an unsigned or wrongly-signed package —
direct or transitive — fails closed.

## 6. Listing in the registry

The community registry is the `genesis-registry` repository — a static
JSON tree, no server. To list a package, open a PR adding
`packages/<alias>.json`:

```json
{
  "alias": "my-package",
  "name": "@you/my-package",
  "description": "What it does, in one line",
  "license": "GPL-3.0",
  "homepage": "https://github.com/you/my-package",
  "latest": "0.1.0",
  "versions": [
    {
      "version": "0.1.0",
      "source": "github:you/my-package@v0.1.0",
      "commit": "<40-hex commit the tag points at>",
      "sha256": "<sha256 of the codeload tarball>",
      "signature": "GENESIS-SIG-1\nkeyId: …\nalg: ed25519\nsig: …"
    }
  ]
}
```

And add a matching entry to `index.json`'s `packages` array.

Field notes:

- `alias` is the bare dependency alias (`[a-z0-9-]`), **not** the
  scoped name — it is what consumers write as `@pkg/<alias>`.
- `commit`/`sha256` are hints gpm cross-checks; resolution still goes
  through `git ls-remote` + archive verification.
- `signature` is the full `.sig` file contents as a JSON string
  (escape newlines as `\n`). Signing the registry *document itself*
  (`.sig` sidecar over raw file bytes) is a separate, optional layer
  that lands with the first-party registry key.
- `latest` is maintainer-controlled — set it to the version you want
  `gpm add <alias>` (no `--version`) to install; update it per release.

Compute `sha256` exactly as gpm will see it:

```sh
curl -fsSL -o pkg.tar.gz \
  "https://codeload.github.com/you/my-package/tar.gz/<commit>"
shasum -a 256 pkg.tar.gz   # macOS; sha256sum on Linux
```

## 7. Release checklist

1. `gpm check` clean; tag the release (`vX.Y.Z`) and push.
2. Download the codeload tarball at the tagged commit.
3. `gpm sign --key <keyfile> --alias <alias> --version X.Y.Z pkg.tar.gz`.
4. `gpm verify --pubkey <pub> --alias <alias> --version X.Y.Z pkg.tar.gz pkg.tar.gz.sig`.
5. Update `packages/<alias>.json` in `genesis-registry`: new version
   entry (source/commit/sha256/signature) + bump `latest` + `index.json`
   entry if the alias is new.
6. Announce the `keyId`/`publicKey` pair so consumers can pin it.

## 8. Consumer quickstart (for your README)

```json
// genesis.json
{
  "name": "@you/app",
  "version": "0.1.0",
  "registry": "https://raw.githubusercontent.com/Zatarain-Software-Services/genesis-registry/master",
  "trustedPackageKeys": {
    "<your keyId>": "<your publicKey>"
  }
}
```

```sh
gpm add my-package          # resolves via registry, verifies signature
```

### Typed APIs and consumer unwraps

Exported signatures resolve their type names in the *exporter's* scope,
so consumers do not need to re-import types your signatures mention.
Nullable returns still follow normal Genesis semantics, though: a
function returning `JsonValue?` requires the consumer to unwrap with
`as JsonValue`, which in turn requires them to import `JsonValue`
themselves (e.g. `import { JsonValue } from "@std/json"`). When
designing a typed package API, prefer concrete return types or document
which stdlib types consumers must import to unwrap nullable results.
