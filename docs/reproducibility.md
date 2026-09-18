# saltOS Reproducibility (declarative system config + lockfile)

This document describes the reproducibility model implemented by `salt lock`,
`salt lock apply` / `salt install --locked`, `salt lock diff` and
`salt config {apply,diff,history,rollback,gc}`. It spans **both** package planes: the native plane (recipes that build `.grain`
packages) and the stratum plane (foreign packages installed by `pacman`, `apt`,
`xbps`, `dnf`, `zypper`, `apk`). The goal the repo owner asked for: a single
declarative system config plus a lockfile that together reproduce an identical
system on another machine — pinning exact versions and content hashes per plane.

It builds on what already exists rather than replacing it:

- The native package manager, transactions, snapshots, and rollback in `halite`
  (`src/halite/include/salt/txn.h`, `db.h`, `repo.h`) and the `salt` CLI
  (`src/salt/cmd_install.cpp`, `cmd_query.cpp`).
- The stratum plane (`src/salt/cmd_stratum.cpp`, `cmd_strata_sugar.cpp`,
  `cmd_run.cpp`) and its per-package-manager dispatch in
  `src/halite/src/run.c` (`salt_pkg_kind` / `salt_stratum_pkg`).
- The Btrfs subvolume + snapshot rollback model in [rollback.md](rollback.md).
- The signed native repository and trust model in [repository.md](repository.md)
  and [trust-model.md](trust-model.md).

See [examples/system.toml](examples/system.toml) and
[examples/system.lock.toml](examples/system.lock.toml) for the concrete schema
this document describes.

The design separates two artifacts, exactly as Nix separates a flake/config
from a flake.lock, and Cargo separates `Cargo.toml` from `Cargo.lock`:

- the **config** (`/etc/salt/system.toml`) — human-authored intent: which
  native packages, which strata, which foreign packages, and policy.
- the **lockfile** (`/etc/salt/system.lock.toml`) — machine-generated, fully
  pinned resolution of that intent: exact versions, exact content hashes, and
  the repository snapshot each package came from.

`salt lock` writes the lockfile from the live, verified system state.
`salt lock apply` / `salt config apply` bring a machine to the state pinned by
the lock. `salt lock diff` / `salt config diff` show the difference between the
lock and reality. Section 9 states precisely which parts of the model below are
enforced today and which are recorded but not yet enforced.

## 1. Honesty about what is achievable per plane

Reproducibility is not binary; it is a spectrum, and saltOS spans package
managers with very different guarantees. This design is explicit about the
ceiling of each plane so the model is not oversold.

| Plane / PM | Pin identity | Pin content hash | Pin exact repo snapshot | Bit-reproducible rebuild |
| --- | --- | --- | --- | --- |
| native (`.grain`) | yes (name+ver+release) | yes (`.grain` sha256 + source sha256) | yes (signed `index.toml`) | aspirational, recipe-dependent |
| arch / `pacman` | yes | yes (pkg sha256) | partial — needs ALA snapshot URL | reinstall-identical only |
| void / `xbps` | yes | yes (pkg sha256) | partial — repodata revision | reinstall-identical only |
| alpine / `apk` | yes | yes (`.apk` sha256) | yes — pin repo + `/etc/apk/world` versions | reinstall-identical only |
| debian / `apt` | yes | yes (`.deb` sha256) | yes — snapshot.debian.org + `Release` hash | reinstall-identical only |
| fedora / `dnf` | yes | yes (rpm sha256) | partial — needs frozen mirror / koji | reinstall-identical only |
| opensuse / `zypper` | yes | yes (rpm sha256) | partial — repo `repomd.xml` revision | reinstall-identical only |

Two distinct senses of "reproducible" matter here:

1. **Reinstall-identical (achievable now, all planes).** Given the lockfile, the
   same exact package *artifacts* (by content hash) are reinstalled. If a
   foreign mirror still serves that exact version, `salt config apply` produces a
   byte-identical set of installed package files. This is what `salt lock`
   captures and `salt config apply` enforces for the native plane; see §9 for
   the current stratum-plane behavior.

2. **Bit-reproducible rebuild (aspirational, native plane only).** Rebuilding a
   `.grain` from its recipe and pinned source yields a byte-identical package.
   This depends on the upstream build being deterministic (`SOURCE_DATE_EPOCH`,
   no embedded timestamps/paths). saltOS recipes already carry a
   `[reproducibility] status` field (`verified` / `unverified` — see
   `recipes/git/recipe.toml`); the lock records that status per package and does
   not claim more than the recipe proves.

The foreign package managers fundamentally do not rebuild from source under our
control, so for the stratum plane the honest guarantee is **content-pinned
reinstall**, not source-level bit reproducibility. Where a snapshotting archive
exists (Debian's snapshot.debian.org, Arch's archive.archlinux.org), the lock
can additionally pin a frozen repository URL so the exact version remains
fetchable even after the live mirror moves on; where it does not (Fedora,
openSUSE rolling repos), the lock records the version+hash and `apply` fails
loudly if the mirror no longer serves that exact artifact, rather than silently
installing a newer one.

## 2. The declarative config: `/etc/salt/system.toml`

Authored by the operator. Declares intent, not resolution. Fits the existing
TOML conventions used by `recipe.toml` and `strata/*.toml`.

```toml
schema = 1

[system]
hostname = "saltbox"
arch = "x86_64"

[native]
repo = "current"
packages = [
  "glibc",
  "linux",
  "runit",
  "salt",
  "git",
  "curl",
]

[native.pin]
git = "2.47.0"

[[strata]]
name = "arch"
recipe = "arch"
packages = ["ripgrep", "fd", "firefox"]

[[strata]]
name = "alpine"
recipe = "alpine"
packages = ["nano"]

[expose]
"arch/rg" = "rg"
"arch/firefox" = { desktop = true }

[policy]
require_signed_native = true
allow_unverified_repro = true
on_missing_artifact = "fail"
```

Key points:

- `[native].packages` is the curated base plus chosen native packages. Resolution
  (dependencies, exact versions) is left to the lock — the config only names
  roots, like a Nix `systemPackages` list. Today `salt` records the config's
  hash in the lock (`config_hash`) and refuses `salt config apply` when the
  config changed since the lock was written (unless `--relock`); the resolved
  set itself comes from the live system at `salt lock` time.
- `[native.pin]` optionally constrains a package to an exact version; otherwise
  the latest in the named `repo` snapshot is locked.
- Each `[[strata]]` block names a stratum, its bootstrap recipe (resolved exactly
  like `salt stratum add` does via `resolve_stratum_recipe`), and the foreign
  packages to install in it. The package manager is implied by the stratum
  recipe (`package_manager` field).
- `[expose]` declares which stratum commands/apps become host shims — this is the
  declarative form of `salt expose` / `salt expose-desktop`, so exposure is part
  of the reproduced system rather than an imperative afterthought.
- `[policy]` controls strictness: whether unsigned native indexes are rejected,
  whether unverified-reproducibility native packages are allowed, and what to do
  when a locked foreign artifact is no longer fetchable (`fail` vs `warn`).

## 3. The lockfile: `/etc/salt/system.lock.toml`

Machine-generated by `salt lock`. Never hand-edited. It pins every installed
native package exactly and records the identity and provenance of every
stratum. `schema = 2` is what `salt lock` writes and what `lock apply` /
`lock diff` read.

```toml
schema = 2
generated = "2026-06-25T00:00:00Z"
arch = "x86_64"
config_hash = "sha256:..."            # sha256 of etc/salt/system.toml, if present
repo_index_sha256 = "sha256:..."      # sha256 of the synced index.toml

[[native]]
name = "git"
version = "2.47.0"
release = 1
arch = "x86_64"
grain_sha256 = "sha256:1ce1...bc4e"   # sha256 of the .grain artifact
filename = "git-2.47.0-1.x86_64.grain"
repo = "current"
deps = ["zlib", "openssl"]

[[stratum]]
name = "alpine"
family = "alpine"
package_manager = "apk"
bootstrap_url = "https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.0-x86_64.tar.gz"
bootstrap_sha256 = "sha256:..."
repo_snapshot = "https://dl-cdn.alpinelinux.org/alpine/v3.20/main"
```

What each section pins:

- **top level** — `arch` of the machine, `config_hash` of the operator config
  the lock corresponds to, and `repo_index_sha256` of the signed `index.toml`
  the native entries were resolved against, so a later sync to a different
  index is detectable.
- **`[[native]]`** — for each installed native package: `name`, `version`,
  `release`, `arch`, the `.grain` content hash (`grain_sha256`), the repository
  `filename`, the `repo` it was installed from, and its runtime `deps`. The
  hash is the one `salt install` verified at install time and stored in the
  database; when an older database row predates hash provenance the hash is
  taken from the index entry for that exact version/release. If neither source
  can supply a verified hash, `salt lock` fails and writes nothing rather than
  emitting an unpinned entry.
- **`[[stratum]]`** — for each stratum: identity (`name`, `family`,
  `package_manager`) and, when the stratum recipe is resolvable, the bootstrap
  rootfs `bootstrap_url` + `bootstrap_sha256` and each configured repository as
  `repo_snapshot`.

`lock apply` accepts an optional `[[stratum.package]]` array (`name` per entry)
under a stratum and installs those packages by name through the stratum's
package manager; `salt lock` does not generate it today (see §9).

### 3.1 Validation on load

`lock apply`, `lock diff` and `install --locked` refuse a lockfile that:

- is not parseable TOML or has no `[[native]]` array table;
- has a native entry without `name`, `version`, a positive integer `release`,
  or a `grain_sha256` that is not a lowercase 64-hex digest (with or without the
  `sha256:` prefix); a placeholder such as `TODO-sha256` is rejected;
- names the same package twice.

## 4. How `salt lock` captures each plane

### 4.1 Native plane

The installed set, versions, releases, repository, and the verified artifact
hash/filename are read from `db.sqlite` (`salt_db_list_installed`,
`salt_db_pkg_deps`). Exact-version index lookups use
`salt_repo_index_find_exact`, and hash validity is `salt_sha256_hex_valid`. No
network access is involved; `salt lock` only reads what `salt install` already
verified.

`salt lock [--output FILE] [--update]` writes `etc/salt/system.lock.toml` under
the active root (or `FILE`). `--update` requires an existing lockfile and
rewrites it in place.

### 4.2 Stratum plane

`salt lock` walks the strata database and, for each stratum, resolves its
recipe (`resolve_stratum_recipe`) to record the bootstrap rootfs URL/hash and
configured repositories. It does not run foreign package-manager queries
inside the stratum; per-package foreign pins are not captured (see §9).

## 5. How `salt lock apply` / `salt install --locked` converge to the lock

Both commands take the same path (`lock_apply`) and the same transaction flags
as `salt install` (`--dry-run`, `--download-only`, `--allow-unverified` is
ignored because a lock is always verified).

1. **Load and validate** the lockfile (§3.1). Load the synced index; without one
   the command fails with "run 'salt sync' first".
2. **Compare** the lock with the database: entries missing from the system,
   entries whose installed version/release/hash differ, installed packages
   absent from the lock (`extra`), and matched entries.
3. **Resolve exactly.** Every missing/changed entry must be present in the index
   at that exact `name`/`version`/`release`, and the index hash must be a valid
   digest equal to the lock's `grain_sha256`. Any deviation prints
   `HASH MISMATCH` (or "the repository index does not offer it") and the whole
   apply is refused before anything is touched.
4. **Plan** removals of `extra` packages and dependency-ordered installs of the
   missing/changed ones. `--dry-run` prints the plan and stops.
5. **Execute** through the normal transaction (`run_native_plan`): snapshot,
   remove, download-or-reuse cached artifacts, verify each artifact's sha256
   against the lock (a mismatching file in the cache is refused, never
   installed), extract, record. Failure rolls back files and database.
6. **Strata.** For each `[[stratum]]` the stratum is bootstrapped if absent and
   any `[[stratum.package]]` names are installed via the stratum's package
   manager.

`apply` is convergent and idempotent: a second run reports
`native: already matches` and changes nothing. `salt config apply` is the same
operation on the default lock path with one extra guard: if
`etc/salt/system.toml` changed since the lock was generated (its sha256 differs
from `config_hash`) it refuses unless `--relock` is passed, in which case the
lock is regenerated after a successful apply.

## 6. `salt lock diff`

`salt lock diff [FILE] [--quiet]` (alias `salt config diff`) prints one line
per difference — `+ name` (in lock, not installed), `~ name` (installed at a
different version/release/hash) and `- name` (installed, not in lock) — then a
one-line summary; `--quiet` prints only the summary. It exits 0 when the system
matches, 1 when it does not, and 2 when the lockfile or database cannot be
read, so it can be used as a check in scripts.

## 7. Generations, rollback and garbage collection

This reuses the Btrfs subvolume + snapshot design ([rollback.md](rollback.md)).

- **Generations.** Every transaction (`install`, `remove`, `update`,
  `lock apply`) records a deployment row and, on Btrfs, a snapshot of `@`;
  otherwise a file-backup transaction state directory under
  `var/lib/salt/state/txn-<id>`. `salt history` / `salt config history` list
  them.
- **Rollback.** `salt rollback` / `salt config rollback` restore the previous
  generation's files and database rows.
- **Garbage collection.** `salt config gc` (alias `salt gc`) keeps the `N` most
  recent generations (`--keep N`, default from `gc.keep` in
  `etc/salt/salt.conf`, default 3) plus the current, booted and pinned ones
  (`--pin ID` or `gc.pinned = [..]` in `salt.conf`), removes the other
  generations' snapshots/state directories, and deletes cached `.grain`
  artifacts under `var/lib/salt/cache/<arch>` that no kept generation and no
  lockfile references. `--dry-run` prints every action prefixed `would remove`
  and changes nothing. Both modes end with a `freed:` / `would free:` line
  listing generations, artifacts and MiB. GC never touches `@home`.

## 8. CLI summary

```
salt lock [--output FILE] [--update]   write the lockfile from the live system
salt lock apply [FILE] [--dry-run] [--download-only]
                                        converge to a lockfile, fail closed on mismatch
salt lock diff [FILE] [--quiet]         compare the live system with a lockfile
salt install --locked [--lockfile FILE] same as lock apply
salt config show                        print etc/salt/system.toml
salt config apply [--relock] [--dry-run] [--download-only]
salt config diff
salt config history                     alias of salt history
salt config rollback [id]
salt config gc [--keep N] [--pin ID]... [--dry-run]
```

## 9. What is enforced today and what is not

Enforced end to end and covered by `tests/cli_smoke.cmake` and
`tests/test_main.c`:

- native lock generation with exact name/version/release/arch/sha256/repo;
- refusal to write a lock when any installed package lacks a verified hash;
- `lock apply` / `install --locked` installing missing, replacing changed and
  removing extra native packages, refusing on index/lock hash mismatch,
  refusing hashless or placeholder lock entries, and refusing a cached artifact
  whose content hash differs from the lock;
- `lock diff` exit codes;
- `config apply` stale-config refusal and `--relock`;
- generation and cache GC including dry-run and pinning.

Recorded but not enforced:

- **Foreign package pins.** `[[stratum]]` entries record the bootstrap rootfs
  and repository URLs from the stratum recipe. `salt lock` does not query the
  foreign package manager for installed packages, so no `[[stratum.package]]`
  versions or content hashes are written, and `lock apply` installs any
  `[[stratum.package]]` names it finds at whatever version the stratum's
  repositories currently serve. The per-manager capture commands the stratum
  plane would need (`pacman -Q`, `xbps-query -p filename-sha256`,
  `apk info -v`, `dpkg-query -W`, `rpm -qa`) are not wired into `salt lock`.
- **`[policy]`, `[native.pin]`, `[[strata]].packages` and `[expose]`** in
  `system.toml` are not read by `salt`; the config is hashed for staleness
  detection only. `salt install` always requires a signed index and a verified
  artifact unless `--allow-unverified` is given explicitly.
