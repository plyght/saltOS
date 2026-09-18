# salt — Package Manager

`salt` is the saltOS package manager and package builder. It is implemented as a
native C core library (`halite`) with a C++23 command-line interface. There is
no daemon: every operation runs in the foreground of the invoking process and
exits when it is done.

`salt` is designed to be:

- **fast** — native code, no interpreter, no background service
- **transactional** — every mutating operation is a single recorded transaction
- **rollback-aware** — every transaction snapshots the system before mutating it
- **signature-checking** — repository metadata is verified before it is trusted
- **source-hash-aware** — package and source hashes are verified before use
- **hostile to arbitrary install-time execution** — maintainer scripts are
  optional, discouraged, and restricted
- **easy to audit** — small surface, plain TOML metadata, SQLite database
- **usable without a daemon**

The companion documents describe the formats and policies referenced here:
[recipes.md](recipes.md), [repository.md](repository.md),
[trust-model.md](trust-model.md), and [rollback.md](rollback.md).

## Package format: `.grain`

A `.grain` file is an **uncompressed POSIX ustar archive**. It contains the
following members, in this exact order:

```
metadata.toml      package identity + dependencies + reproducibility
manifest.toml      every installed file: path, mode, size, sha256, type, linkname
files.tar.zst      zstd-compressed ustar of the payload, paths relative to /
scripts/           optional, discouraged; post-install hooks
```

The outer archive is intentionally uncompressed so that the metadata and
manifest can be read and verified without decompressing the whole payload. Only
the payload (`files.tar.zst`) is compressed, with zstd.

Package files are named:

```
<name>-<version>-<release>-<arch>.grain
```

for example `zlib-1.3.1-1-x86_64.grain` or
`helium-0.13.4-1-aarch64.grain`.

### In-memory representation

The core library models an opened or freshly built package with `salt_archive`,
which bundles the parsed metadata, the parsed manifest, the (compressed) payload
buffer, and the list of script names:

```c
typedef struct {
  salt_pkg_meta meta;     /* parsed from metadata.toml */
  salt_manifest manifest; /* parsed from manifest.toml */
  salt_buf payload;       /* files.tar.zst bytes */
  salt_strlist scripts;   /* names under scripts/ */
} salt_archive;
```

`salt_archive_build_from_dir()` constructs a package from a staging directory
plus the package metadata; `salt_archive_write()` serializes it to a `.grain`
file; `salt_archive_open()` parses an existing one; and
`salt_archive_extract_payload()` decompresses and unpacks `files.tar.zst` into a
destination root, recording the installed paths.

### `metadata.toml`

`metadata.toml` carries the package identity, dependency list, and
reproducibility status. It maps directly onto `salt_pkg_meta`:

```c
typedef struct {
  char *name;
  char *version;
  int release;
  char *arch;
  char *summary;
  char *license;
  char *repro_status;   /* "verified" | "unverified" */
  char *repro_reason;   /* set when unverified */
  salt_strlist deps;    /* runtime dependencies */
  salt_strlist conflicts;
} salt_pkg_meta;
```

`salt_pkg_meta_to_toml()` / `salt_pkg_meta_from_toml()` serialize and parse this
structure, and `salt_pkg_filename()` derives the
`<name>-<version>-<release>-<arch>.grain` filename from it.

### `manifest.toml`

`manifest.toml` lists **every** installed file with the data needed to verify it
later. Each entry is a `salt_manifest_entry`:

```c
typedef struct {
  char *path;       /* installed path, relative to / */
  char typeflag;    /* ustar type: file, dir, symlink */
  unsigned mode;    /* permission bits */
  uint64_t size;
  char *sha256;     /* hex digest of regular-file contents */
  char *linkname;   /* target, for symlinks */
} salt_manifest_entry;
```

The manifest is what `salt verify`, `salt files`, and `salt owner` ultimately
operate on, and what is copied into the local database at install time so the
same checks work offline.

## Local database: SQLite

The package database lives at `/var/lib/salt/db.sqlite` (under the active
`--root`). It tracks installed packages, their file manifests and hashes, the
transaction log, and the deployment / rollback history. It is opened with
`salt_db_open()` and closed with `salt_db_close()`.

The schema below is an **illustrative sketch** of the concepts implied by the
core API; the exact column types are an implementation detail, but the shape is
stable.

```sql
CREATE TABLE packages (
  name        TEXT PRIMARY KEY,
  version     TEXT NOT NULL,
  release     INTEGER NOT NULL,
  arch        TEXT NOT NULL,
  repo        TEXT,          -- repository source the package came from
  sig_status  TEXT,          -- signature verification result at install time
  summary     TEXT,
  license     TEXT,
  filename    TEXT,          -- .grain artifact this row was installed from
  sha256      TEXT,          -- verified sha256 of that artifact
  install_time INTEGER NOT NULL,
  txn_id      INTEGER NOT NULL REFERENCES transactions(id)
);

CREATE TABLE deps (
  pkg_name TEXT NOT NULL REFERENCES packages(name),
  dep      TEXT NOT NULL
);

CREATE TABLE conflicts (
  pkg_name TEXT NOT NULL REFERENCES packages(name),
  conflict TEXT NOT NULL
);

CREATE TABLE files (
  pkg_name  TEXT NOT NULL REFERENCES packages(name),
  path      TEXT NOT NULL,
  typeflag  TEXT NOT NULL,   -- file / dir / symlink
  mode      INTEGER NOT NULL,
  size      INTEGER NOT NULL,
  sha256    TEXT,
  linkname  TEXT,
  PRIMARY KEY (pkg_name, path)
);

CREATE TABLE transactions (
  id     INTEGER PRIMARY KEY,
  op     TEXT NOT NULL,      -- install / remove / update / rollback ...
  status TEXT NOT NULL,      -- pending / committed / failed / rolled-back
  time   INTEGER NOT NULL
);

CREATE TABLE deployments (
  id       INTEGER PRIMARY KEY,
  op       TEXT NOT NULL,
  status   TEXT NOT NULL,
  time     INTEGER NOT NULL,
  snapshot TEXT              -- btrfs snapshot path for this deployment
);
```

A row in `packages` corresponds to `salt_db_pkg`:

```c
typedef struct {
  char *name;
  char *version;
  int release;
  char *arch;
  char *repo;
  char *sig_status;
  char *summary;
  char *license;
  char *filename;
  char *sha256;
  int64_t install_time;
  int64_t txn_id;
} salt_db_pkg;
```

and a row in `deployments` corresponds to `salt_deployment`:

```c
typedef struct {
  int64_t id;
  char *op;
  char *status;
  int64_t time;
  char *snapshot;
} salt_deployment;
```

The query side of the database is exposed through functions such as
`salt_db_get_pkg()`, `salt_db_is_installed()`, `salt_db_list_installed()`,
`salt_db_search()`, `salt_db_pkg_files()`, `salt_db_owner()`,
`salt_db_pkg_manifest()`, `salt_db_pkg_deps()`, `salt_db_revdeps()` (reverse
dependencies, used to keep removals safe), and `salt_db_conflicts_with()`
(installed packages that declare a conflict with a name, or that a name declares
a conflict with).

## Transactions

Every mutating operation is a transaction. There are two layers:

1. **SQLite atomicity.** `salt_db_sql_begin()`, `salt_db_sql_commit()`, and
   `salt_db_sql_rollback()` wrap the database writes so the on-disk database is
   never left half-updated.
2. **System transactions.** `salt_db_txn_new()` opens a logical transaction
   (recording the operation and returning a `txn_id`), and
   `salt_db_txn_finish()` closes it with a final status. Installs and removals
   are recorded against the `txn_id` via `salt_db_record_install()` and
   `salt_db_record_remove()`.

The higher-level orchestration in `txn.h` ties this to the filesystem. A
`salt_ctx` holds the active root and the relevant paths:

```c
typedef struct {
  char *root;
  char *db_path;       /* /var/lib/salt/db.sqlite */
  char *state_dir;     /* /var/lib/salt/state/ */
  char *snapshot_dir;  /* /.snapshots or /@snapshots */
  bool use_btrfs;
} salt_ctx;
```

The flow for a mutating operation is **snapshot-before-mutate**:

1. `salt_db_txn_new()` opens a transaction.
2. `salt_snapshot_create()` saves the database and snapshots the `@` subvolume
   on Btrfs, and records a deployment row. If the snapshot cannot be taken the
   transaction is refused before any file is touched. Per-file backups under
   `state_dir` are written as files are replaced, on every filesystem.
3. The payload is applied: `salt_install_archive()` extracts and records each
   package; `salt_remove_pkg()` removes one.
4. On success, the SQLite transaction commits and `salt_db_txn_finish()` marks
   it committed.
5. On **any** failure — a file that cannot be backed up, a payload that fails
   to extract, or a database write that is rejected — the system automatically
   rolls back: `salt_txn_revert_files()` puts every touched file back from the
   per-transaction backup, the SQLite transaction is rolled back, and the
   transaction is finished with a failed status. A package whose extraction
   failed is never recorded as installed.

`salt rollback [N]` (`salt_rollback_to()`) returns to an earlier deployment on
demand. See [rollback.md](rollback.md) for the full model.

### Btrfs backend

`salt_ctx_init()` enables the Btrfs backend when the target root and its
`.snapshots` directory are both on Btrfs (a booted saltOS `@` with `@snapshots`
mounted at `/.snapshots`, or an installer target mounted the same way).
`SALT_BTRFS=1` / `SALT_BTRFS=0` force it on or off. With the backend on:

- every transaction first snapshots the root subvolume to
  `/.snapshots/root-<txn>`; the transaction is refused if that fails;
- `salt rollback [N]` re-snapshots `root-<N>` as the new `@` at the Btrfs top
  level, keeps the outgoing `@` as `root-<rollback txn>`, carries the newer
  transaction history into the new root's database and regenerates the GRUB
  menu; the swap takes effect at the next boot (exit status 3);
- `salt config gc` deletes the `root-<txn>` subvolumes of pruned generations
  with `btrfs subvolume delete`, never those of kept, pinned or booted ones.

On non-Btrfs roots (and under `--root <dir>` in tests) the same commands work
from the per-file backups under `state_dir` and take effect immediately.

`tests/btrfs_smoke.sh` runs the whole flow (install, update, rollback across a
remount, remove, lock, gc) on a loop-mounted Btrfs image in the `ci` workflow.

### State paths

Under the active `--root`:

```
/var/lib/salt/db.sqlite     package database + transaction log + deployments
/var/lib/salt/state/        per-transaction saved file state (non-btrfs fallback)
/var/lib/salt/cache/<arch>/ downloaded .grain artifacts
/var/lib/salt/repo/<arch>/  synced index.toml + index.toml.sig
/.snapshots or /@snapshots  btrfs snapshots
/etc/salt/repo.conf         repo source + trusted key
/etc/salt/salt.conf         gc.keep / gc.pinned retention policy
/etc/salt/system.toml       declarative config (see reproducibility.md)
/etc/salt/system.lock.toml  lockfile written by salt lock
```

## Verification and trust order

`salt` never trusts a downloaded package on its own. The order is:

1. Fetch the repository index and its signature (`index.toml` and
   `index.toml.sig`).
2. **Verify `index.toml.sig` against the trusted public key** before reading any
   package list. The trusted key comes from `--key`, or from
   `/etc/salt/repo.conf`.
3. **Reject the index** if any entry lacks a `sha256`, or carries a malformed
   or placeholder value (for example `TODO-sha256`); `salt sync` fails and the
   previous index stays in place.
4. For each package to be installed, **verify its `sha256` against the entry in
   the signed index** before unpacking it. A package whose index entry has no
   usable hash is refused unless `--allow-unverified` is passed explicitly, in
   which case a loud warning is printed and the install proceeds unverified.
   A cached or downloaded artifact whose content hash differs from the index is
   refused and deleted from the cache.

This means the signed index is the root of trust, and package integrity is
chained from it. The result of these checks is recorded as the `sig_status` of
the installed package in the database. Details of the repository format and the
signing model are in [repository.md](repository.md), and the contributor side of
trust is in [trust-model.md](trust-model.md).

## Global flags

These flags apply to all subcommands:

- `--root <dir>` — operate on an alternate root instead of `/`. All state paths
  (database, snapshots, `repo.conf`) are resolved under this root. Useful for
  installing into a target during system bootstrap or for testing in a fakeroot.
- `--repo <url-or-path>` — override the repository source (a URL or a local
  path) for this invocation.
- `--key <pubkey-hex-or-file>` — the trusted public key for verifying the
  repository index signature, either as hex on the command line or a file path.
- `--yes` — assume "yes" for confirmation prompts (non-interactive use).
- `--expose` / `--no-expose` — control whether foreign installs offer host shims.

Every command exits 0 on success, 1 on an operational failure (refused
transaction, verification failure, missing package), and 2 on a usage error.
Errors go to stderr; `salt <unknown>` exits 2.

## CLI subcommands

### `salt sync`

Refresh the repository index from the configured source. This downloads
`index.toml` and `index.toml.sig`, verifies the signature against the trusted
key, and verifies every entry has a well-formed `sha256` before replacing the
previous index.

```sh
salt sync
salt sync --repo https://repo.saltos.example/current --key /etc/salt/repo.pub
```

### `salt search <term>`

Search the synced repository (and the local database) for packages whose name or
summary matches the term, case-insensitively. Installed packages are marked.

```sh
salt search helium
salt search "terminal emulator"
```

### `salt install <pkg>...`

Install one or more packages. Runtime dependencies are resolved from the signed
index and installed first, in dependency order; a missing dependency or a
conflict (declared by the new package, or by an installed package against it)
fails the command before anything is touched. This is a transaction: a snapshot
is taken, each package's `sha256` is verified against the signed index, the
payload is extracted, and the database is updated. On failure, the whole
transaction is rolled back.

Options:

- `--dry-run` — print the resolved plan and exit without changing anything.
- `--download-only` — fetch and verify the artifacts into the cache, install
  nothing.
- `--allow-unverified` — install a package whose index entry has no usable
  hash, with a loud warning. Never needed for a healthy repository.
- `--locked [--lockfile FILE]` — ignore the package arguments and converge the
  system to the lockfile instead (same as `salt lock apply`).
- `--nodeps` — install only the named packages without pulling in or checking
  their dependencies. Used by the bootstrap while the sysroot is being built
  up package by package; never needed on an installed system.

```sh
salt install helium
salt install mpv qterminal pcmanfm-qt --yes
salt install --locked
```

### `salt remove <pkg>...`

Remove one or more installed packages. Reverse dependencies are checked
(`salt_db_revdeps`): a package that other installed packages depend on is
refused unless `--cascade` is passed, in which case the dependents are removed
too and listed in the plan. Like install, it is a snapshotted,
auto-rolling-back transaction. `--dry-run` prints the plan only.

```sh
salt remove qview
salt remove zlib --cascade
```

### `salt update`

Upgrade the system to the current repository state. Upgrades are ordered so
that dependencies are replaced before their dependents. This is the canonical
rollback-protected operation: it snapshots `@`, records a deployment, applies all
package changes as one transaction, and rolls back automatically if anything
fails. `--download-only` fetches and verifies the new artifacts without
installing; `--dry-run` lists what would change; `--allow-unverified` behaves
as for `install`. Naming strata (`salt update alpine`) upgrades those strata
instead.

```sh
salt update
salt update --download-only
```

### `salt rollback`

Restore the previous deployment — the last known-good system state recorded
before the most recent transaction. After rollback, reboot to run the restored
deployment.

```sh
salt update
# a bad update happens
salt rollback
reboot
```

### `salt history`

List the recorded transactions and deployments (rollback points) with their id,
operation, status, time, and backing snapshot. `salt deployments` and
`salt config history` are aliases.

```sh
salt history
```

### `salt verify [pkg]`

Verify installed files against the hashes recorded in the database. This
re-hashes the on-disk regular files, checks symlink targets, and compares them
to the stored manifest values, reporting any drift or corruption. With a
package name only that package is checked. Exits 1 when any file differs.

```sh
salt verify
salt verify zlib
salt verify --root /mnt/target
```

### `salt info <pkg>`

Show details about a package: version, release, arch, summary, license,
repository source, signature status, artifact filename and hash, install time,
file count, dependencies and reverse dependencies. Packages that are not installed but available in the synced
index are shown from the index. `salt query` and `salt show` are aliases.

```sh
salt info helium
```

### `salt list`

List packages. `--installed` (default) lists the local database,
`--upgradable` lists installed packages the index offers a newer version of,
and `--available` lists every package in the index.

```sh
salt list
salt list --upgradable
```

### `salt files <pkg>`

List every file owned by an installed package, from its recorded manifest.

```sh
salt files zlib
```

### `salt owner <path>`

Show which installed package owns a given path on disk.

```sh
salt owner /usr/bin/mpv
```

### `salt clean`

Delete downloaded `.grain` artifacts from `var/lib/salt/cache/<arch>` for the
running architecture; `--all` sweeps every architecture's cache; `--dry-run`
lists what would be removed and frees nothing. Installed packages do not need
their artifact after installation, so this is always safe; `salt gc` is the
reference-aware alternative that keeps artifacts still pinned by a kept
generation or lockfile.

```sh
salt clean
salt clean --all --dry-run
```

### `salt gc`

Prune old generations and the artifacts they referenced. Keeps the `N` most
recent generations (`--keep N`, default `gc.keep` from `etc/salt/salt.conf`,
default 3) and never removes the current, booted, or pinned generation
(`--pin ID` or `gc.pinned` in `salt.conf`). `--dry-run` reports without
deleting. `salt config gc` is an alias. See
[reproducibility.md](reproducibility.md).

```sh
salt gc --keep 5 --dry-run
salt config gc
```

### `salt lock`, `salt lock apply`, `salt lock diff`

Write, apply and compare `etc/salt/system.lock.toml`, which pins every installed
native package by name, version, release, arch, sha256 and repository, and
every package installed in each stratum by name and the foreign manager's exact
version. `lock apply` fails closed on any hash mismatch and on any foreign
version the stratum's manager cannot provide (rolling the stratum back to its
pre-apply snapshot). `salt config apply`,
`salt config diff` and `salt config rollback` operate on the default lock path.
See [reproducibility.md](reproducibility.md) for the format and the exact
semantics.

```sh
salt lock
salt lock diff
salt lock apply /srv/locks/lab.lock.toml --dry-run
```

### `salt config <subcommand>`

Declarative system management driven by `etc/salt/system.toml`. `show` prints
the config, `check` validates it (schema, keys, types) without touching the
system, and `apply` converges the machine to it: with an up-to-date lock it is
`salt lock apply`; with `--relock` (or no lock yet) it resolves `[native]` and
`[native.pin]` against the repository index into one native transaction that
installs the declared closure and removes everything outside it, bootstraps
missing `[[strata]]` and installs their declared packages, reconciles `[expose]`
shims, enforces `[policy]`, and writes a fresh lock. `diff`, `history`,
`rollback` and `gc` are the lock/generation commands described above. `apply`
accepts `--dry-run`, `--download-only` and `--allow-unverified`.

```sh
salt config check
salt config apply --relock --dry-run
salt config apply --relock
```

### `salt build <recipe-dir>`

Build a `.grain` from a recipe directory. The source is fetched and its hash
verified, the build runs in a clean environment with network access denied after
the fetch, and the resulting tree is packaged with a generated manifest and
reproducibility metadata. See [recipes.md](recipes.md) for the recipe format and
build environment variables.

```sh
salt build recipes/zlib
salt build recipes/glibc
```

### `salt lint <recipe-dir>`

Lint a recipe for correctness and policy: pinned source URL and hash, declared
license, declared build and runtime dependencies, valid build system, and
present reproducibility status. See [trust-model.md](trust-model.md) for the
admission rules these checks enforce.

```sh
salt lint recipes/zlib
```

### `salt sign <pkg>`

Sign a built `.grain` (or a repository index) with the maintainer's secret
key, producing the ed25519 signature used in the trust chain.

```sh
salt sign out/zlib-1.3.1-1-x86_64.grain --key /path/to/secret.key
```

### `salt repo publish <out-dir>`

Build and sign a repository index from a directory of packages, producing
`index.toml` and `index.toml.sig`. See [repository.md](repository.md) for the
resulting layout and trust order.

```sh
salt repo publish out/
```

### `salt trust <subcommand>`

Manage the contributor trust list and run supply-chain scans. Contributors can
be `unknown`, `vouched`, `maintainer`, or `denounced`, and recipes are scanned
for supply-chain risks (maintainer changes, source URL changes, new install
scripts, obfuscated scripts, embedded crypto wallet addresses, and so on). The
full model, including admission rules and the scan severities
(`info` / `warn` / `block`), lives in [trust-model.md](trust-model.md).

```sh
salt trust scan recipes/helium
salt trust vouch alice
salt trust set bob denounced --reason "drive-by ownership takeover attempt"
salt trust lookup alice
```

## Summary

`salt` is a single, daemonless, native tool that builds, signs, installs, and
verifies packages, all through transactions that snapshot the system first and
roll back automatically on failure. The signed repository index is the root of
trust, package hashes chain from it, and the SQLite database keeps a complete,
auditable record of what is installed and how the system reached its current
state. For the formats and policies it depends on, continue with
[recipes.md](recipes.md), [repository.md](repository.md),
[trust-model.md](trust-model.md), and [rollback.md](rollback.md).
