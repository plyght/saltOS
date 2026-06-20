# saltOS Architecture

This document describes how saltOS fits together: the layered system stack, the
source layout, architecture support, and the control flow of the two operations
that define the project — installing a package and updating with rollback.

For deeper detail on individual subsystems, see
[package-manager.md](package-manager.md), [repository.md](repository.md),
[trust-model.md](trust-model.md), and [rollback.md](rollback.md).

## 1. Design summary

saltOS is a personal-first, Unix-like Linux distribution. It uses a traditional
Unix filesystem layout (`/usr`, `/etc`, `/var`, `/opt`) rather than a Nix-style
content-addressed store. It ships current software through a curated, signed
binary repository, makes full-system rollback a first-class feature via Btrfs
snapshots, and is built from simple native tools that one maintainer can
understand.

The package manager, `salt`, is a C core library (`halite`) wrapped by a
C++23 CLI. Local state lives in a SQLite database. Every system-mutating
operation runs as a transaction that snapshots the root subvolume first and can
roll back automatically on failure.

## 2. Layered stack

From the metal up:

| Layer | Component | Notes |
| --- | --- | --- |
| Hardware | x86_64, aarch64 | Two target architectures. |
| Bootloader | GRUB | Carries one boot entry per deployment for rollback. |
| Kernel | upstream Linux, latest stable | Minimal patching. |
| C library | glibc | Chosen for binary compatibility and desktop support. |
| Init | runit | Service supervision, fronted by the `svc` wrapper. |
| Filesystem | Btrfs | Subvolumes `@ @home @var @log @snapshots`. |
| Package manager | `salt` (`halite` + C++23 CLI) | Transactional, signature-checking, daemonless. |
| Local state | SQLite | `/var/lib/salt/db.sqlite`: packages, transactions, deployments. |
| Repository | curated, signed | One signed `index.toml` per arch. |
| Desktop | LXQt | Wayland-first where practical, X11 where needed. |
| Browser | Helium | Default browser, treated as a special high-risk package. |

### 2.1 Kernel, libc, init

The kernel is the upstream Linux stable series with minimal patching. The C
library is glibc, chosen for the broadest binary compatibility with browsers,
GPU drivers, and third-party developer tooling.

Init is `runit`: simple supervision, fast boot, easy-to-read service
directories. saltOS smooths the raw runit UX with the `svc` wrapper:

```sh
svc enable network
svc disable bluetooth
svc start helium-helper
svc status
```

Service directories live under `/etc/runit/sv/<name>` (each with a `run` script
and optional `finish`/`check`), and a service is enabled by symlinking it into
`/etc/runit/runsvdir/current/`.

### 2.2 Filesystem

The root filesystem is Btrfs with these subvolumes:

```
@           system root
@home       user data — never rolled back
@var        variable state
@log        logs
@snapshots  pre-transaction snapshots
```

Separating `@home` is what makes rollback safe: restoring a previous system
state never touches user data. Snapshots of `@` are the mechanism behind every
transaction and behind `salt rollback`.

### 2.3 Bootloader

GRUB is used so that previous deployments can be offered as boot menu entries.
Because rollback is central to saltOS, bootloader integration with Btrfs
snapshots matters more than aesthetic preference. Each recorded deployment can
have a corresponding boot entry, so a machine that fails to come up after an
update can still boot the previous known-good deployment.

### 2.4 Package manager

`salt` is split into:

- `halite` — the C core library: archive handling, the SQLite database,
  hashing, signing, TOML parsing, the transaction engine, and the trust/
  supply-chain logic. Headers live in `src/halite/include/salt/`.
- the C++23 CLI — argument parsing and higher-level orchestration of the core.

Key core concepts (see the headers for exact signatures):

- `salt_ctx` — the operating context: the target root, the database path, the
  per-transaction state directory, the snapshot directory, and whether Btrfs is
  in use. Created with `salt_ctx_init(ctx, root)`.
- `salt_db` — a handle to the SQLite database, with SQL-level
  begin/commit/rollback, transaction bookkeeping (`salt_db_txn_new`,
  `salt_db_txn_finish`), and install/remove recording.
- `salt_archive` — an opened `.grain`: its `metadata.toml`, `manifest.toml`,
  the zstd payload, and any scripts. Built from a staging directory, written to
  a file, opened, and extracted into a target tree.
- the transaction layer (`txn.h`) — `salt_snapshot_create` /
  `salt_snapshot_restore`, `salt_install_archive`, `salt_remove_pkg`,
  `salt_deployments_list`, and `salt_rollback_last`.

The CLI is daemonless: nothing runs in the background, and every command is a
direct, auditable operation.

### 2.5 Repository

The official repository is curated, binary, and signed. It is laid out one tree
per architecture:

```
repo/<arch>/
  index.toml          signed list: name/version/release/sha256/size/deps
  index.toml.sig      ed25519 signature (hex) of index.toml
  packages/<name>-<version>-<release>-<arch>.grain
```

There is no untrusted user repository. The trust order is fixed: verify
`index.toml.sig` against the trusted public key, then verify each package's
`sha256` against the signed index before installing. See
[repository.md](repository.md) and [trust-model.md](trust-model.md).

### 2.6 Desktop

The desktop edition is LXQt: Qt-based, lightweight, and a familiar desktop
metaphor. The display stack is Wayland-first where practical, with X11
compatibility where needed. The default browser is Helium, packaged under
stricter rules than ordinary packages because Chromium-derived browsers are
large and high-risk.

Default applications:

- terminal: QTerminal or foot
- file manager: PCManFM-Qt
- text editor: FeatherPad
- archive manager: LXQt Archiver
- image viewer: LXImage-Qt or qView
- media player: mpv
- document viewer: qpdfview
- network: NetworkManager
- audio: PipeWire
- Bluetooth: BlueZ with a minimal frontend
- login manager: SDDM or greetd

## 3. Component diagram

```
                         +---------------------------+
                         |  curated signed repo      |
                         |  repo/<arch>/index.toml   |
                         |  + index.toml.sig         |
                         |  packages/*.grain         |
                         +-------------+-------------+
                                       | sync / fetch (verify sig, then hash)
                                       v
+----------------+   CLI args   +---------------------------+
|  user / svc    +------------->+  salt (C++23 CLI)         |
+----------------+              +-------------+-------------+
                                              | calls
                                              v
                                +---------------------------+
                                |  halite (C library)       |
                                |  archive | toml | hash    |
                                |  sign    | zst  | tar     |
                                |  db      | txn  | trust   |
                                +----+--------------+-------+
                                     |              |
                       snapshot/restore        read/write
                                     v              v
              +-----------------------+   +-----------------------+
              |  Btrfs subvolumes     |   |  SQLite db.sqlite     |
              |  @ @home @var @log    |   |  packages /           |
              |  @snapshots           |   |  transactions /       |
              +-----------+-----------+   |  deployments          |
                          |               +-----------------------+
                          v
              +-----------------------+
              |  GRUB deployment      |
              |  boot entries         |
              +-----------------------+
```

The kernel, glibc, runit, and the LXQt desktop sit on top of the `@` subvolume
that `salt` manages; runit supervises services, and GRUB selects which
deployment of `@` to boot.

## 4. Source layout

```
src/halite/        C core library (halite), headers in include/salt/
src/salt/          C++23 CLI (salt)
recipes/<name>/    package recipes (recipe.toml [+ patches/, files/])
repo/<arch>/       built repository (index.toml, index.toml.sig, packages/)
os/runit/          runit service definitions and the svc wrapper
os/btrfs/          subvolume layout + snapshot/rollback helpers
os/installer/      Calamares branding, settings, and custom modules
os/iso/            ISO build system (rootfs bootstrap + image assembly)
os/bootstrap/      base-system build orchestration (toolchain -> base)
keys/              signing keys (public committed, secret git-ignored)
tests/             ctest suite
docs/              documentation
.github/workflows/ CI/CD
```

Build and quality gates: C11 and C++23, built with CMake + Ninja.

```sh
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## 5. Architecture support

saltOS targets `x86_64` and `aarch64`. Every recipe declares both in its `arch`
field unless a package is genuinely architecture-specific. The repository keeps
one tree per architecture (`repo/x86_64/`, `repo/aarch64/`), and CI builds a
matrix over both.

At runtime, `salt` detects the host architecture from the machine name reported
by `uname`, normalizing aliases:

```
arm64  -> aarch64
amd64  -> x86_64
```

This guarantees a package built and indexed for, say, `aarch64` is only fetched
and installed on an `aarch64` host.

## 6. State paths

All paths are relative to the operating root (`--root`, default `/`):

```
/var/lib/salt/db.sqlite     package database + transaction log + deployments
/var/lib/salt/state/        per-transaction saved file state (non-btrfs fallback)
/.snapshots or /@snapshots  btrfs snapshots
/etc/salt/repo.conf         repo source + trusted key
/etc/runit/sv/<name>        service definitions (run, optional finish/check)
/etc/runit/runsvdir/current symlinks for enabled services
```

`/etc/salt/repo.conf` names the repository source and the trusted public key;
`salt` consults it for `sync`, `install`, and `update`. The `--repo` and `--key`
global flags override the configured values, and `--root` lets `salt` operate on
an alternate root (used by the installer and by tests).

## 7. Control flow: installing a package

`salt install <pkg>` runs as a single transaction:

1. **Sync / trust.** If needed, `salt sync` refreshes the repository index from
   the source in `/etc/salt/repo.conf`. The signature `index.toml.sig` is
   verified against the trusted public key. A failed signature aborts before
   anything is fetched.
2. **Resolve.** Runtime dependencies declared in the index are resolved into an
   ordered install set, skipping packages already recorded as installed in the
   database.
3. **Fetch.** Each required `.grain` is fetched (from a URL or local path) by
   the filename recorded in the signed index.
4. **Verify hash.** Each fetched package's `sha256` is checked against the value
   in the signed index. A mismatch aborts the transaction. (Trust order:
   signature first, then per-package hash.)
5. **Open archive.** Each package is opened as a `salt_archive`, exposing its
   `metadata.toml`, `manifest.toml`, and the `files.tar.zst` payload.
6. **Begin transaction.** `salt_ctx_init` establishes the context; a new
   transaction id is allocated in the database (`salt_db_txn_new`). On Btrfs, `@`
   is snapshotted (`salt_snapshot_create`) and a deployment row is recorded. On
   non-Btrfs roots, prior file state is saved under `/var/lib/salt/state/`.
7. **Apply.** The payload is extracted into the target root
   (`salt_install_archive`), and the install — name, version, release, arch,
   file manifest with per-file hashes, repository source, and signature status —
   is recorded in the database (`salt_db_record_install`).
8. **Commit.** The SQL transaction commits and the transaction is finished as
   succeeded (`salt_db_txn_finish`).

If any step after the snapshot fails, the transaction is rolled back
automatically: the SQL changes are reverted and the pre-transaction snapshot is
restored, leaving the system exactly as it was. `salt remove` follows the same
transactional shape via `salt_remove_pkg`.

## 8. Control flow: update and rollback

`salt update` upgrades the installed set to the newest versions in the signed
index, as one transaction:

1. Refresh and verify the index (signature, then hashes), as in install.
2. Compute the upgrade set by comparing installed versions against the index.
3. Allocate a transaction, snapshot `@`, and record a new deployment row.
4. Apply the new package versions transactionally and record them in the
   database.
5. Commit on success; on any failure during the transaction, automatically
   restore the pre-update snapshot.

If an update commits but the resulting system misbehaves, the user rolls back
explicitly:

```sh
salt update      # snapshot @, record deployment, apply new versions
# a bad update happens
salt rollback    # restore the previous known-good deployment
reboot
```

`salt rollback` (`salt_rollback_last`) restores the previous deployment's root
state from its snapshot and makes it the active deployment. Because GRUB also
carries a boot entry per deployment, a machine that will not boot at all after an
update can still select the previous deployment from the boot menu. Throughout,
`@home` is never rolled back, so user data is preserved across both updates and
rollbacks.

Deployments are inspectable at any time:

```sh
salt deployments   # list deployments / rollback points
salt verify        # verify installed files against recorded hashes
```

See [rollback.md](rollback.md) for the full rollback model, command sessions,
and the non-Btrfs fallback.
