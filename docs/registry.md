# Genesis package registry — design + implementation

Status: **implemented** (2026-11-23). Extends `gpm` beyond direct
`github:` source specifiers with a discovery layer. Signing semantics are
unchanged — see [signing.md](signing.md); this document only adds *where
gpm learns what packages exist*.

## Problem

Today `gpm add` requires knowing the exact `github:owner/repo[@ref]`
source. There is no way to:

- discover what packages exist ("is there a JSON toolkit?")
- resolve a friendly alias to a source
- see available versions or package metadata (description, license,
  homepage) before installing
- let a project say "use *the* registry" rather than pinning every
  dependency to a repo path

A registry is a **discovery and metadata layer only**. It does not change
what gets installed: resolution still ends at a `github:` source specifier,
an immutable commit, a verified archive sha256, and optionally a verified
signature. The lockfile format does not change.

## Registry shape: static JSON over HTTPS

A registry is a directory of static JSON files served over HTTPS — a
GitHub repository rendered by GitHub Pages, any static host, or even a
local `file://`/directory for tests. No API server, no database, no
runtime service. This matches gpm's existing dependency profile (it
already shells out to `curl` for archives and `git` for resolution).

```
<registry>/
  index.json                     # package directory (see below)
  packages/<alias>.json          # per-package metadata
  packages/<alias>.json.sig      # optional detached signature
```

`index.json` is deliberately small: alias → summary line for `search`
and `info` listings. Per-package detail lives in `packages/<alias>.json`
so gpm fetches only what it needs.

### `index.json`

```json
{
  "registry": "genesis-community",
  "packages": [
    { "alias": "json-tools", "name": "@zatarain/json-tools", "description": "JSON helpers", "latest": "0.1.0" },
    { "alias": "http-kit",   "name": "@acme/http-kit",    "description": "HTTP utilities",  "latest": "1.4.0" }
  ]
}
```

### `packages/<alias>.json`

```json
{
  "alias": "json-tools",
  "name": "@zatarain/json-tools",
  "description": "JSON parse/serialize helpers for Genesis",
  "license": "GPL-3.0",
  "homepage": "https://github.com/Zatarain-Software-Services/genesis-json-tools",
  "latest": "0.1.0",
  "versions": [
    {
      "version": "0.1.0",
      "source": "github:Zatarain-Software-Services/genesis-json-tools@v0.1.0",
      "commit": "<40-hex sha, optional but recommended>",
      "sha256": "<archive sha256, optional>",
      "signature": "GENESIS-SIG-1\nkeyId: ...\nalg: ed25519\nsig: ..."
    }
  ]
}
```

Rules:

- `alias` is the **dependency alias** — the bare name importers write
  after `@pkg/` and the key in a project's `dependencies` map. It
  follows `gpm_alias_valid` rules: lowercase letters, digits, and
  `-`, not `std`. It is *not* the scoped package `name`.
- `name` is the scoped package name from the package's own
  `genesis.json` (`@author/name` shape). Display/attribution only;
  two registries could serve the same alias pointing at different
  `name`s, which is exactly the squatting case the trust model
  covers.
- `latest` is an explicit maintainer-controlled version string
  (open question 3 — recommended over computing highest semver so
  maintainers can yank a release). `gpm add <alias>` uses it unless
  `--version` overrides.
- `source` is a normal gpm source specifier — today always
  `github:owner/repo[@ref]`. The registry never invents a new fetch
  mechanism; anything `gpm add github:...` can do, a registry entry can
  point at.
- `commit`/`sha256`/`signature` are **hints, not authority**. gpm still
  resolves the ref to a commit and verifies the downloaded archive. A
  registry entry that lies about `sha256` produces a normal integrity
  failure; one that omits them is simply less convenient. Pinning them
  in the registry lets `gpm info` show expected values and lets
  `--frozen`-style strictness catch registry drift early.
- `signature` may embed the verified `GENESIS-SIG-1` envelope so
  `signedPackagesRequired` projects get verification without needing
  the archive-sidecar URL to exist. When present it is treated exactly
  like a lockfile-pinned signature.
- `packages/<alias>.json.sig` signs the **canonical package document**
  (UTF-8 JSON with sorted keys and no insignificant whitespace, or the
  raw file bytes — pick one and freeze it; recommendation: sign the raw
  file bytes, simplest and unambiguous). This authenticates *registry
  metadata* (who published this alias→source mapping), which is a
  different claim than the package signature (who built the archive).

## Trust model

Three distinct claims, three distinct keys possible:

1. **Package authenticity** — `gpm-package-v1` signature over the
   archive. Unchanged. `trustedPackageKeys` in `genesis.json` decides
   which package signers a project accepts.
2. **Registry metadata authenticity** — `.sig` over
   `packages/<alias>.json`. Uses the *same* `trustedPackageKeys`
   mechanism: a registry operator is just another signer whose keyId
   the project pins. If the project doesn't pin the registry's key,
   metadata signatures are ignored (metadata is advisory anyway).
3. **Transport** — HTTPS via curl, same as archive downloads today.
   Signatures make content trustworthy regardless of transport.

What a malicious/compromised registry can do:

- **Availability attacks** — hide packages, return errors. Annoying,
  not dangerous; `github:` specifiers still work directly.
- **Alias squatting on unsigned installs** — point `@foo/bar` at an
  attacker's repo. Only matters when `signedPackagesRequired` is false,
  and is equivalent to a user typing the wrong `github:` path today.
  Mitigation: sign registry metadata and pin the registry keyId;
  `signedPackagesRequired` projects are fully immune (the attacker's
  package signature won't verify).
- **Downgrade/mislabel** — claim an old version is latest, wrong
  descriptions. Caught by commit/sha256 hints and by package
  signatures; cosmetic otherwise.

What it cannot do: forge package signatures, bypass
`signedPackagesRequired`, or inject code that wasn't signed by a
trusted package key.

## gpm UX

New/changed surface (names TBD, lowerCamelCase per convention):

- `genesis.json` accepts optional `"registry": "<https-url>"`.
  `GPM_REGISTRY_URL` env var overrides; `--registry <url>` flag
  overrides both for one invocation. No default registry in v1 —
  absence means `add <alias>` (bare alias) errors with "no registry
  configured" while `add github:...` works as today.
- `gpm add <alias>` — bare alias (no `github:` prefix) resolves via
  the configured registry: fetch `packages/<alias>.json`, pick `latest`
  (or `--version X.Y.Z`), extract `source`, then run the existing
  resolve→download→verify→install pipeline unchanged. The lockfile
  records the resolved `github:` source exactly as today.
- `gpm info <alias>` — print registry metadata + available versions.
- `gpm search <query>` — fetch `index.json`, substring/filter match on
  alias+description. Local filtering; no server-side search.
- `gpm remove`, `gpm update`, `gpm check`, `gpm tree` unchanged —
  they operate on the lockfile, which is registry-agnostic.

Version selection in v1: `latest` from the package doc, or explicit
`--version`. No semver ranges (`^`, `~`) — consistent with the current
"no semver ranges" scope line; ranges can be added later without
breaking the index format.

## Reference implementation

A `genesis-registry` GitHub repository containing `index.json` +
`packages/*.json`, published via GitHub Pages (or raw URLs — gpm can
fetch `raw.githubusercontent.com/.../packages/<alias>.json` directly,
no Pages needed). The official Genesis registry starts nearly empty —
a handful of first-party/example packages — and grows as community
packages adopt the format. Anyone can self-host the same layout;
`"registry"` in `genesis.json` points wherever the project wants.

Seeding: package authors submit a PR adding `packages/<their-alias>.json`
to the registry repo. The `signature` field can carry their
`gpm-package-v1` envelope so consumers verify without sidecar hunting.

## Implementation slices

1. **This document** — format + trust model approved.
2. [x] `tools/gpm/registry.c` + `gpm.c` commands: `add <alias>`,
   `info <alias>`, `search <query>`; `"registry"` manifest field with
   `GPM_REGISTRY_URL`/`--registry` overrides; alias = bare dependency
   alias, `name` = scoped package name; explicit `latest` field;
   registry-embedded signatures verified like lockfile-pinned ones
   (precedence: live sidecar > registry doc > previous lockfile pin).
   Verified end-to-end against the live `genesis-registry` repo and by
   `gpm_resolver_end_to_end` (file:// registry fixture).
3. Registry repo + docs (`docs/package-manager.md` registry section,
   authoring guide for `packages/<alias>.json`).
4. First-party registry key once the release key exists — sign
   `index.json`/package docs, document pinning.
5. Later (only if needed): hosted registry service, semver ranges,
   namespacing beyond `@author/`, federation between registries.

## Open questions

1. **Default registry**: ~~built-in default vs explicit opt-in~~ —
   **decided: opt-in**, implemented. `"registry"` in `genesis.json`,
   `GPM_REGISTRY_URL`, or `--registry`; no built-in default so `add
   <alias>` with zero config fails with a clear error while
   `add github:...` always works.
2. **Package-doc signature scope**: ~~raw file bytes vs canonical
   JSON~~ — **decided: raw file bytes** when `.sig` files land (slice
   4, pending the first-party key).
3. **`latest` semantics**: ~~highest semver in `versions`, or an
   explicit `"latest": "X.Y.Z"` field~~ — **decided: explicit field**,
   already reflected in the format above. Allows yanking and matches
   what `index.json` sketches.
4. **Registry key vs package keys**: ~~reuse `trustedPackageKeys` vs a
   separate `trustedRegistryKeys`~~ — **decided: reuse**. Registry
   metadata signatures (when they land) verify against the same key
   list; a signer is a signer.
