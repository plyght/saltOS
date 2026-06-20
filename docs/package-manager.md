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
  install_time INTEGER NOT NULL,
  txn_id      INTEGER NOT NULL REFERENCES transactions(id)
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
`salt_db_pkg_manifest()`, and `salt_db_revdeps()` (reverse dependencies, used to
keep removals safe).

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
2. `salt_snapshot_create()` snapshots the `@` subvolume (or saves per-file state
   under `state_dir` on non-Btrfs fallback) and records a deployment row.
3. The payload is applied: `salt_install_archive()` extracts and records each
   package; `salt_remove_pkg()` removes one.
4. On success, the SQLite transaction commits and `salt_db_txn_finish()` marks
   it committed.
5. On **any** failure, the system automatically rolls back:
   `salt_snapshot_restore()` restores the pre-transaction state, the SQLite
   transaction is rolled back, and the transaction is finished with a failed
   status.

`salt rollback` reuses this machinery via `salt_rollback_last()` to return to the
previous deployment on demand. See [rollback.md](rollback.md) for the full model.

### State paths

Under the active `--root`:

```
/var/lib/salt/db.sqlite     package database + transaction log + deployments
/var/lib/salt/state/        per-transaction saved file state (non-btrfs fallback)
/.snapshots or /@snapshots  btrfs snapshots
/etc/salt/repo.conf         repo source + trusted key
```

## Verification and trust order

`salt` never trusts a downloaded package on its own. The order is:

1. Fetch the repository index and its signature (`index.toml` and
   `index.toml.sig`).
2. **Verify `index.toml.sig` against the trusted public key** before reading any
   package list. The trusted key comes from `--key`, or from
   `/etc/salt/repo.conf`.
3. For each package to be installed, **verify its `sha256` against the entry in
   the signed index** before unpacking it.

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

## CLI subcommands

### `salt sync`

Refresh the repository index from the configured source. This downloads
`index.toml` and `index.toml.sig` and verifies the signature against the trusted
key.

```sh
salt sync
salt sync --repo https://repo.saltos.example/current --key /etc/salt/repo.pub
```

### `salt search <term>`

Search the synced repository (and the local database) for packages whose name or
summary matches the term.

```sh
salt search helium
```

### `salt install <pkg>...`

Install one or more packages. This is a transaction: a snapshot is taken, each
package's `sha256` is verified against the signed index, the payload is
extracted, and the database is updated. On failure, the whole transaction is
rolled back.

```sh
salt install helium
salt install mpv qterminal pcmanfm-qt --yes
```

### `salt remove <pkg>...`

Remove one or more installed packages. Reverse dependencies are checked
(`salt_db_revdeps`) so a removal does not silently break dependents. Like
install, it is a snapshotted, auto-rolling-back transaction.

```sh
salt remove qview
```

### `salt update`

Upgrade the system to the current repository state. This is the canonical
rollback-protected operation: it snapshots `@`, records a deployment, applies all
package changes as one transaction, and rolls back automatically if anything
fails.

```sh
salt update
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

### `salt deployments`

List the recorded deployments (rollback points) with their id, operation,
status, time, and backing snapshot.

```sh
salt deployments
```

### `salt verify`

Verify installed files against the hashes recorded in the database. This
re-hashes the on-disk files and compares them to the stored `sha256` manifest
values, reporting any drift or corruption.

```sh
salt verify
salt verify --root /mnt/target
```

### `salt query <pkg>`

Show details about a package: version, release, arch, repository source,
signature status, and install time.

```sh
salt query helium
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
