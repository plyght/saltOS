# saltOS Reproducibility (declarative system config + lockfile)

This document designs a Nix-like reproducibility model for saltOS that spans
**both** package planes: the native plane (recipes that build `.grain`
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

`salt config apply` brings a machine to the state described by the config and
lock. `salt lock` regenerates the lockfile from the current resolved state.
`salt config diff` shows the difference between the config/lock and reality.

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
   captures and `salt config apply` enforces for the stratum plane.

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
  roots, like a Nix `systemPackages` list.
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

Machine-generated by `salt lock`. Never hand-edited. Pins everything needed to
reproduce the resolved system. Conceptually the analogue of `flake.lock`.

```toml
schema = 1
generated = "2026-06-25T00:00:00Z"
config_hash = "sha256:..."     # hash of the system.toml that produced this lock

[[native]]
name = "git"
version = "2.47.0"
release = 1
arch = "x86_64"
grain_sha256 = "sha256:1ce1...bc4e"
source_url = "https://www.kernel.org/pub/software/scm/git/git-2.47.0.tar.xz"
source_sha256 = "sha256:1ce114da88704271b43e027c51e04d9399f8c88e9ef7542dae7aebae7d87bc4e"
repo = "current"
repo_index_sha256 = "sha256:..."     # the signed index.toml this came from
reproducibility = "verified"
build_deps = ["gcc", "make", "curl", "openssl", "zlib", "expat", "perl", "gettext", "pcre2"]

[[stratum]]
name = "arch"
family = "arch"
package_manager = "pacman"
bootstrap_url = "https://geo.mirror.pkgbuild.com/iso/latest/archlinux-bootstrap-x86_64.tar.zst"
bootstrap_sha256 = "sha256:..."
repo_snapshot = "https://archive.archlinux.org/repos/2026/06/25/"

  [[stratum.package]]
  name = "ripgrep"
  version = "14.1.1-1"
  arch = "x86_64"
  pkg_sha256 = "sha256:..."
  pkg_url = "https://archive.archlinux.org/packages/r/ripgrep/ripgrep-14.1.1-1-x86_64.pkg.tar.zst"
  origin_repo = "extra"

[[stratum]]
name = "alpine"
family = "alpine"
package_manager = "apk"
bootstrap_url = "https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.0-x86_64.tar.gz"
bootstrap_sha256 = "sha256:..."
repo_snapshot = "https://dl-cdn.alpinelinux.org/alpine/v3.20/main"

  [[stratum.package]]
  name = "nano"
  version = "7.2-r0"
  arch = "x86_64"
  pkg_sha256 = "sha256:..."
  origin_repo = "main"

[[expose]]
ref = "arch/rg"
alias = "rg"
kind = "cli"
```

What each section pins:

- **`[[native]]`** — for each resolved native package: `name`, `version`,
  `release`, `arch`, the `.grain` content hash (`grain_sha256`), the upstream
  `source_url` + `source_sha256` (copied from the recipe's `[source]`), the
  `repo` name and the `repo_index_sha256` of the signed `index.toml` it was
  resolved against (so a later sync to a different index is detectable), and the
  recipe's `[reproducibility] status`. `build_deps` records the build closure
  for the aspirational rebuild path.
- **`[[stratum]]`** — for each stratum: identity (`name`, `family`,
  `package_manager`), the bootstrap rootfs `url` + `sha256` (so the base
  userspace itself is pinned, not just packages on top), and `repo_snapshot`: a
  frozen archive URL where this is available (`archive.archlinux.org`,
  `snapshot.debian.org`, a pinned Alpine release branch) or the live repo plus a
  revision id where it is not.
- **`[[stratum.package]]`** — for each foreign package: `name`, exact `version`
  (in the package manager's own version string format), `arch`, the package file
  content hash (`pkg_sha256`), the resolvable `pkg_url` where the manager exposes
  one, and `origin_repo` (which configured repo provided it). This is the core
  cross-package-manager pin.
- **`[[expose]]`** — the resolved exposure set, so shims are part of the lock.

## 4. How `salt lock` captures each package manager

`salt lock` walks the live system and emits the lockfile. The native plane is
captured from the local SQLite database and the signed index. The stratum plane
is captured by running each manager's *query* commands **inside** the stratum
via the existing `salt_stratum_run` mechanism (private mount namespace + chroot)
and parsing their output. These are all read-only queries — none mutate the
stratum — so they need no snapshot.

### 4.1 Native plane

The installed set, versions, releases, and per-file sha256 are already in
`db.sqlite` (`salt_db_list_installed`, `salt_db_pkg_manifest`). The `.grain`
content hash and the source URL/hash come from the signed `index.toml`
(`salt_repo_index_load` / `salt_repo_index_find`, which already exposes
`sha256`, `version`, `release`, and the recipe carries `[source]`). No new
mechanism is needed; `salt lock` reads what `salt install` already verified at
install time.

### 4.2 Stratum plane — per-manager capture commands

Each row below is a **read-only** query run with `salt run <stratum> -- <cmd>`.
The capture layer parses stdout into `(name, version, arch, pkg_sha256,
origin_repo, pkg_url)` tuples.

- **pacman (Arch).**
  - Installed name+version: `pacman -Q` (`name version` per line).
  - Origin repo: `pacman -Qi <pkg>` → `Repository` field, or
    `pacman -Sl` to map package→repo.
  - Content hash: the cached package file under
    `/var/cache/pacman/pkg/<name>-<version>-<arch>.pkg.tar.zst` is sha256'd
    directly; if not cached, the download URL is reconstructed against
    `archive.archlinux.org/packages/<x>/<name>/` and fetched to hash.
  - Snapshot pin: `archive.archlinux.org/repos/<YYYY>/<MM>/<DD>/` frozen URL.

- **xbps (Void).**
  - Installed: `xbps-query -l` (`ii <name>-<version> <desc>`).
  - Per-package detail incl. repo + filename:
    `xbps-query -p pkgver,repository,filename-sha256 <pkg>` — xbps already stores
    `filename-sha256` in its metadata, so the content hash is read directly with
    no re-download.
  - Snapshot pin: the configured `repository` URL plus its `<repodata>` index
    revision.

- **apk (Alpine).**
  - Installed with versions: `apk info -v` (`<name>-<version>` per line).
  - Per-package detail: `apk info -a <pkg>` and the world file
    `/etc/apk/world` for the explicitly-requested set.
  - Content hash: `.apk` files carry a Q1-prefixed base64 SHA1 in the `APKINDEX`
    (`apk index`/`apk fetch`); `salt lock` records both the APKINDEX checksum and
    a recomputed sha256 of the fetched `.apk` for a stronger pin.
  - Snapshot pin: a pinned release branch URL (e.g. `.../alpine/v3.20/main`),
    which Alpine keeps stable per release.

- **apt/dpkg (Debian).**
  - Installed name+version: `dpkg-query -W -f='${Package} ${Version} ${Architecture}\n'`.
  - Origin + hash: `apt-get install --reinstall --download-only --print-uris <pkg>`
    prints the `.deb` URL **and** the expected hash (`SHA256:...`) without
    installing; alternatively the `.deb` in `/var/cache/apt/archives` is sha256'd.
  - Snapshot pin: `snapshot.debian.org/archive/debian/<timestamp>/` plus the
    `Release` file sha256 — Debian's snapshot service makes apt the strongest
    foreign reproducibility story.

- **dnf (Fedora).**
  - Installed: `rpm -qa --qf '%{NAME} %{VERSION}-%{RELEASE} %{ARCH}\n'`.
  - Origin repo: `dnf repoquery --installed --qf '%{name} %{repoid}'`.
  - URL + hash: `dnf download --url <pkg>` for the URL;
    `rpm -q --qf '%{SHA256HEADER}'` or sha256 of the cached rpm under
    `/var/cache/dnf/.../packages/` for content.
  - Snapshot pin: partial — record the repo `repomd.xml` `revision`; a frozen
    mirror or koji build id is required for true refetchability and is recorded
    when configured.

- **zypper (openSUSE).**
  - Installed: same `rpm -qa` query as dnf.
  - Origin: `zypper --no-refresh search -i -s <pkg>` (repo column).
  - URL + hash: cached rpm under `/var/cache/zypp/packages/<repo>/` is sha256'd;
    `repomd.xml` revision recorded for the snapshot pin.

The capture layer lives in `halite` as `salt_repro_*` functions so the parsing
is unit-testable independently of the CLI, mirroring how `salt_stratum_pkg`
centralizes the per-manager command tables in `run.c`.

## 5. How `salt config apply` rebuilds/reinstalls to match the lock

`apply` is a single transaction-aware operation that drives the machine toward
the locked state. It reuses the existing transaction + snapshot machinery so a
failed `apply` rolls back exactly like any other `salt` transaction.

Order of operations:

1. **Validate.** Recompute `config_hash`; if `system.toml` changed since the lock
   was generated, refuse unless `--relock` is passed (mirrors `cargo build`
   refusing a stale lock). Verify the native `repo_index_sha256` matches the
   synced index; if not, `salt sync` then re-check.
2. **Snapshot.** Open a host transaction (`salt_db_txn_new`) and snapshot `@`
   (`salt_snapshot_create`) before any mutation — identical to `cmd_install`.
3. **Native plane.** For each `[[native]]` entry, install the exact
   `name`-`version`-`release` from the locked repo, verifying `grain_sha256`
   against the downloaded `.grain` (the hash check in `install_one` already does
   this). Remove native packages present on the system but absent from the lock
   (convergent, like Nix `switch`). Honor `[policy] require_signed_native`.
4. **Strata.** For each `[[stratum]]`:
   - Ensure the stratum exists, bootstrapping from the pinned `bootstrap_url` +
     `bootstrap_sha256` if absent (reuses `ensure_stratum` /
     `salt_stratum_bootstrap`). Take a per-stratum snapshot before mutation
     (reuses `salt_stratum_snapshot_create`, exactly as `salt pkg` does).
   - Point the stratum's package manager at the locked `repo_snapshot` (write the
     frozen mirror into the manager's config inside the chroot: `mirrorlist` for
     pacman, `sources.list` for apt, `/etc/apk/repositories` for apk, repo files
     for dnf/zypper, `xbps.d` for xbps).
   - Install each `[[stratum.package]]` at its exact locked version using the
     manager's version-pinning syntax (`pacman -S name=ver` against the archive,
     `apt-get install name=ver`, `apk add name=ver`, `dnf install name-ver`,
     `zypper install name=ver`, `xbps-install name-ver`), then verify the fetched
     artifact's sha256 against `pkg_sha256`. On mismatch or unfetchable artifact,
     obey `[policy] on_missing_artifact` (`fail` aborts the transaction and rolls
     back; `warn` records drift).
5. **Expose.** Reconcile host shims to match `[[expose]]` (add missing via
   `salt_expose_add` / `salt_expose_desktop`, remove shims not in the lock).
6. **Finish.** Commit the transaction on success; on any failure restore the
   host snapshot and per-stratum snapshots, leaving the machine untouched.

`apply` is **convergent and idempotent**: running it twice is a no-op the second
time, and it both installs what is missing and removes what is extra, so the
config+lock is the single source of truth for system state.

## 6. Garbage collection and rollback

This reuses the existing Btrfs subvolume + snapshot design ([rollback.md](rollback.md))
rather than inventing a parallel store.

- **Generations.** Each `salt config apply` records a deployment row
  (`salt_db_txn_new` + `salt_db_txn_set_snapshot`) and stores a copy of the
  `system.lock.toml` it applied alongside the snapshot. A *generation* is the
  pair (host `@` snapshot, applied lock). `salt config history` lists them,
  reusing `salt_deployments_list`.
- **Rollback.** `salt config rollback` restores the previous generation's `@`
  snapshot (`salt_rollback_last`) **and** the per-stratum snapshots taken during
  that `apply`, then re-points exposed shims to the restored lock. Because each
  stratum is independently snapshot-capable, a single bad foreign update can be
  rolled back per stratum (`salt stratum rollback <name>`) without reverting the
  whole generation — the finer-grained control already exists.
- **Garbage collection.** `salt config gc` prunes snapshots/generations older
  than a retention policy and deletes cached `.grain`/foreign package artifacts
  under `var/lib/salt/cache/` and the per-stratum caches that no live generation
  references — the analogue of `nix-collect-garbage`, expressed in terms of the
  snapshot retention saltOS already has. GC never touches `@home`.

Unlike Nix's content-addressed `/nix/store`, saltOS is a mutable-root system with
snapshots, so "generations" are snapshots of `@` plus their locks rather than
distinct store paths. This is a deliberate fit to the existing Btrfs model: it
gives generation/rollback/GC semantics without a store rewrite.

## 7. Mapping onto the existing salt CLI

New subcommands, following the exact dispatch pattern in `src/salt/cli.cpp`
(`dispatch()` string compare → `cmd_*` function declared in `cli.hpp` →
implemented in a `cmd_*.cpp` listed in `src/salt/CMakeLists.txt`):

```
salt config show              print the resolved config
salt config apply [--relock]  converge the system to config + lock
salt config diff              show config/lock vs. live system
salt config history           list generations (reuses deployments)
salt config rollback [id]     restore a previous generation
salt config gc [--keep N]     prune old generations + unreferenced artifacts
salt lock [--update]          (re)generate system.lock.toml from resolved state
```

`salt config` is one new dispatcher command (`cmd_config`) that switches on its
first argument, exactly like `cmd_stratum` / `cmd_trust` already do. `salt lock`
is a second top-level command (`cmd_lock`). Both are additive — no existing
command changes behavior.

### 7.1 Proposed CLI dispatch wiring

The wiring is intentionally documented here rather than committed, so the owner
can review before the build changes. It is exactly the existing pattern.

In `src/salt/cli.hpp`, alongside the other `cmd_*` declarations:

```cpp
int cmd_config(const Options &o, const std::vector<std::string> &args);
int cmd_lock(const Options &o, const std::vector<std::string> &args);
```

In `src/salt/cli.cpp`, inside `dispatch()`:

```cpp
  if (cmd == "config") return cmd_config(o, args);
  if (cmd == "lock") return cmd_lock(o, args);
```

In `src/salt/CMakeLists.txt`, add the two new translation units to the
`add_executable(salt ...)` list:

```cmake
  cmd_config.cpp
  cmd_lock.cpp
```

### 7.2 Proposed `cmd_config.cpp` skeleton

A safe, compile-ready skeleton that wires the subcommand surface and the
config-hash validation without yet performing mutation. It follows the file
conventions of the existing `cmd_*.cpp` (extern "C" halite headers, `Options`,
`confirm`, no comments).

```cpp
#include "cli.hpp"

extern "C" {
#include "salt/util.h"
#include "salt/toml.h"
#include "salt/hash.h"
}

#include <cstdio>
#include <string>
#include <vector>

static std::string system_config_path(const Options &o) {
  return path_join(o.root, "etc/salt/system.toml");
}

static std::string system_lock_path(const Options &o) {
  return path_join(o.root, "etc/salt/system.lock.toml");
}

static void config_usage() {
  fprintf(stderr,
          "usage: salt config <subcommand>\n"
          "  show              print the resolved declarative config\n"
          "  apply [--relock]  converge the system to config + lock\n"
          "  diff              show config/lock vs. the live system\n"
          "  history           list generations\n"
          "  rollback [id]     restore a previous generation\n"
          "  gc [--keep N]     prune old generations and unreferenced artifacts\n");
}

static int config_show(const Options &o) {
  std::string p = system_config_path(o);
  salt_buf b;
  salt_buf_init(&b);
  if (salt_read_file(p.c_str(), &b) != SALT_OK) {
    fprintf(stderr, "salt: no system config at %s\n", p.c_str());
    salt_buf_free(&b);
    return 1;
  }
  if (b.data && b.len) fwrite(b.data, 1, b.len, stdout);
  salt_buf_free(&b);
  return 0;
}

int cmd_config(const Options &o, const std::vector<std::string> &args) {
  if (args.empty()) {
    config_usage();
    return 2;
  }
  const std::string &sub = args[0];
  if (sub == "show") return config_show(o);
  if (sub == "apply" || sub == "diff" || sub == "history" || sub == "rollback" ||
      sub == "gc") {
    fprintf(stderr, "salt config %s: not yet implemented\n", sub.c_str());
    return 3;
  }
  config_usage();
  return 2;
}
```

### 7.3 Proposed `cmd_lock.cpp` skeleton

```cpp
#include "cli.hpp"

extern "C" {
#include "salt/util.h"
}

#include <cstdio>
#include <string>
#include <vector>

int cmd_lock(const Options &o, const std::vector<std::string> &args) {
  (void)o;
  bool update = false;
  for (const auto &a : args)
    if (a == "--update") update = true;
  fprintf(stderr, "salt lock%s: not yet implemented\n", update ? " --update" : "");
  return 3;
}
```

These skeletons compile against the current headers and change no existing
behavior; they are documented rather than committed so the owner reviews the new
command surface before it enters the build (per the project's review-first rule).

## 8. Recommended first PR-sized step

1. Land the two schema example files (`docs/examples/system.toml`,
   `docs/examples/system.lock.toml`) and this document. (Done — see below.)
2. Add `cmd_config.cpp` + `cmd_lock.cpp` from §7.2/§7.3 and wire the dispatch
   (§7.1). This is purely additive: `salt config show` works, every other
   subcommand prints "not yet implemented". No existing flow changes.
3. Implement `salt lock` for the **native plane only** first — it reads
   `db.sqlite` + the signed index, which already expose everything needed, so it
   carries no foreign-PM parsing risk and produces a verifiable artifact.
4. Implement `salt config apply` for the native plane (install/remove to match
   the lock inside one transaction), reusing `cmd_install`'s verified install
   path.
5. Only then add per-stratum capture (`salt_repro_*` in `halite`) one package
   manager at a time, gated by tests against each stratum the CI matrix already
   bootstraps.

This sequences the highest-certainty, lowest-risk work first and defers the
genuinely hard foreign-snapshot pinning until the native plane proves the model
end to end.
