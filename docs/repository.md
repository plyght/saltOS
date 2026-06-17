# saltOS Repository Model

saltOS ships software through a single curated official binary repository. The
repository is intentionally small, reviewed, signed, and maintained by trusted
people. There is deliberately **no AUR-equivalent**: saltOS does not provide a
default untrusted user package repository, and packages do not enter the
official repository by drive-by submission. Everything users install by default
comes from metadata that has been signed and from packages whose hashes are
recorded in that signed metadata.

This document describes how the repository is laid out, how an index is built
and signed, how `salt` consumes it, and how the repository fits into the saltOS
release model.

See also: [package-manager.md](package-manager.md),
[trust-model.md](trust-model.md), [recipes.md](recipes.md).

## Layout

The repository has **one tree per architecture**. saltOS targets `x86_64` and
`aarch64`, so the official repository contains both:

```
repo/
  x86_64/
    index.toml
    index.toml.sig
    packages/
      zlib-1.3.1-1-x86_64.saltpkg
      helium-0.13.4-1-x86_64.saltpkg
  aarch64/
    index.toml
    index.toml.sig
    packages/
      zlib-1.3.1-1-aarch64.saltpkg
      helium-0.13.4-1-aarch64.saltpkg
```

Each architecture tree contains exactly three things:

- `index.toml` — the signed list of packages available for that arch. Each
  entry records `name`, `version`, `release`, `arch`, `filename`, `sha256`,
  `size`, and `deps`.
- `index.toml.sig` — an ed25519 signature of `index.toml`, stored as hex.
- `packages/` — the `.saltpkg` files, named
  `<name>-<version>-<release>-<arch>.saltpkg`.

Packages are **hash-addressed by the index**: the authoritative record of what a
package is and what it contains is the `sha256` in the signed index, not the
filename. The filename is a convenience; the hash is the identity.

### Index entries

Each entry in `index.toml` corresponds to a `salt_repo_entry` (see
`include/salt/repo.h`):

```c
typedef struct {
  char *name;
  char *version;
  int release;
  char *arch;
  char *filename;
  char *sha256;
  uint64_t size;
  salt_strlist deps;
} salt_repo_entry;
```

The whole index is a `salt_repo_index`, which also carries the repository name
and the arch the tree is built for.

## Trust order

`salt` follows a strict, boring verification order before it will install
anything. This is the heart of the repository's security model:

1. **Verify the index signature.** Load `index.toml.sig` and verify it against
   the **trusted public key** (configured in `/etc/salt/repo.conf` on an
   installed system, or passed with `--key`). This uses `salt_verify_file` /
   `salt_verify_buf` from `include/salt/sign.h`. If the signature does not
   verify against the trusted key, the index is rejected and nothing proceeds.
2. **Trust the signed index.** Once the index signature is verified, the
   contents of `index.toml` are trusted — including every package's recorded
   `sha256` and `size`.
3. **Verify each package hash.** Before installing a `.saltpkg`, `salt`
   recomputes its sha256 and checks it against the hash recorded in the signed
   index. A package whose contents do not match the signed index is rejected.

In short: **verify the signed metadata first, then verify package hashes from
the signed index.** A package is never trusted on its own; it is trusted only
because the signed index vouches for its hash.

## Building and signing an index

The repository tooling lives in `include/salt/repo.h`:

- `salt_repo_build_index(packages_dir, repo_name, arch, out)` scans a directory
  of `.saltpkg` files, reads each package's identity and dependencies, computes
  each package's `sha256` and `size`, and assembles a `salt_repo_index`.
- `salt_repo_index_to_toml(idx, out)` serializes that index to `index.toml`.
- `salt_repo_publish(out_dir, repo_name, arch, sec_key_hex)` performs the full
  publish step: it builds the index for the packages in `out_dir`, writes
  `index.toml`, signs it with the supplied secret key, and writes
  `index.toml.sig`.
- `salt_repo_index_load(path, out)` reads an `index.toml` back into a
  `salt_repo_index`, and `salt_repo_index_find(idx, name)` looks up a single
  package entry by name.

The maintainer-facing workflow uses the `salt` CLI:

```sh
salt build recipes/zlib                          # produce out/zlib-1.3.1-1-<arch>.saltpkg
salt sign out/zlib-1.3.1-1-x86_64.saltpkg        # sign an individual artifact
salt repo publish out/                           # build + sign the repository index
```

`salt repo publish out/` is the normal path: it builds the index over the
packages in `out/`, signs `index.toml`, and writes `index.toml.sig` alongside
it, ready to be served as a `repo/<arch>/` tree.

### Keys

Signing uses ed25519 keypairs (see `include/salt/sign.h`):
`salt_keypair_generate` produces a public key (`SALT_PUBKEY_HEXLEN` = 64 hex
chars) and a secret key (`SALT_SECKEY_HEXLEN` = 128 hex chars); signatures are
`SALT_SIG_HEXLEN` = 128 hex chars.

In the repository:

- `keys/` holds the **public** signing key, which is committed to the tree so it
  can travel with the OS.
- The **secret** signing key is **git-ignored** and never committed. It is held
  only by the signer who publishes releases.

On an installed system, `/etc/salt/repo.conf` records the repository source URL
and the **trusted public key** that signed indexes must verify against.

## How `salt sync` consumes the index

`salt sync` refreshes the local copy of the repository index from the configured
source (`--repo`, or the source in `/etc/salt/repo.conf`). It:

1. fetches `index.toml` and `index.toml.sig` for the host arch (the arch is
   detected at runtime: `arm64` → `aarch64`, `amd64` → `x86_64`);
2. verifies `index.toml.sig` against the trusted public key;
3. loads the verified index for later use by `salt search`, `salt install`, and
   `salt update`.

From then on, `salt install <pkg>` and `salt update` resolve packages against
the verified index, fetch the corresponding `.saltpkg` from `packages/`, and
re-verify each package's sha256 against the signed index before it is installed.

## Release model

saltOS uses a **rolling curated model with snapshot releases** (DISTRO §14). The
repository is organized around three branches:

- **`unstable`** — active package updates; breakage is allowed here. This is
  where new versions and new recipes land first.
- **`current`** — the default user track. Packages reach `current` after basic
  testing on `unstable`. This is what a normal saltOS user follows.
- **`stable-snapshot`** — a periodic known-good snapshot of the image and
  repository. It is cut from `current` at points considered solid.

The intent is that saltOS feels current without being reckless: software is
recent, but updates flow through review and testing and are grouped into tested
snapshots where possible, rather than being pulled blindly from arbitrary user
recipes.

Each branch is still a curated, signed repository: regardless of branch, the
index is signed and packages are hash-addressed by that signed index, and the
same trust order applies on the client.
