# saltOS Rollback Model

Rollback is a first-class feature of saltOS. Every system update creates a
rollback point, and a broken upgrade can be undone with a single command or a
boot-menu selection. This document describes the model, the commands, and the
UX.

For how rollback fits into the wider system, see
[architecture.md](architecture.md). For the package manager internals, see
[package-manager.md](package-manager.md).

## 1. The idea

The model is intentionally simple and inspectable:

- Btrfs snapshots are taken before system transactions.
- Each transaction records a **deployment** row in the local database.
- The bootloader carries an entry for previous deployments.
- A failed transaction rolls back automatically.
- `salt rollback` restores the previous known-good root state.
- User home data is **not** rolled back by default.

There is no clever framework here. Rollback is "snapshot the root, record what
happened, and be able to go back."

## 2. Subvolumes

saltOS uses a Btrfs root with these subvolumes:

```
@           system root        — snapshotted before every transaction
@home       user data          — never rolled back
@var        variable state
@log        logs
@snapshots  pre-transaction snapshots of @
```

`@home` is a separate subvolume on purpose. Rollback restores `@` (the system),
but it must never destroy the user's files. Because `@home` lives outside `@`,
returning the system to a previous deployment leaves documents, downloads, and
configuration in the user's home directory untouched. The exact layout can
evolve, but the invariant — rollback must not destroy user data by default —
does not.

## 3. Transaction lifecycle

Every system-mutating operation (`install`, `remove`, `update`) runs as one
transaction. The lifecycle is:

1. **Begin.** `salt` builds its operating context (`salt_ctx`) for the target
   root and opens the database. A new transaction id is allocated.
2. **Snapshot.** Before any file is touched, `@` is snapshotted into
   `@snapshots` (`salt_snapshot_create`). The snapshot is the rollback point.
3. **Record deployment.** A deployment row is written to the database recording
   the operation, its status, the time, and the snapshot that backs it.
4. **Apply.** Packages are extracted/removed and the package database is updated.
5. **Finish.** On success the transaction is marked succeeded and committed. On
   any failure, the transaction is rolled back **automatically**: the snapshot is
   restored (`salt_snapshot_restore`) and the database changes are reverted,
   leaving the system exactly as it was before the transaction began.

The automatic case means a transaction that dies partway — a bad package, an
interrupted extraction, a failed dependency step — never leaves a
half-installed system. The manual case (below) covers updates that *succeed*
mechanically but turn out to be bad in use.

### Core types

The transaction and rollback logic is exposed by `halite` (see
`include/salt/txn.h`):

- `salt_ctx` — the root, database path, state directory, snapshot directory, and
  whether Btrfs is available.
- `salt_deployment` / `salt_deployment_list` — a deployment's id, operation,
  status, timestamp, and backing snapshot; and a list of them.
- `salt_snapshot_create(ctx, txn_id, &snapshot)` — take the pre-transaction
  snapshot of `@`.
- `salt_snapshot_restore(ctx, snapshot)` — restore a given snapshot.
- `salt_deployments_list(ctx, db, &out)` — enumerate deployments / rollback
  points.
- `salt_rollback_last(ctx, db)` — restore the previous deployment.

## 4. Commands

```sh
salt update        # upgrade the system as one snapshotted transaction
salt rollback      # restore the previous known-good deployment
salt deployments   # list deployments / rollback points
salt verify        # verify installed files against recorded hashes
```

### salt update

```sh
$ salt update
==> refreshing repository index
==> verifying index signature ........ ok
==> 7 packages to upgrade
==> snapshot @ -> @snapshots/txn-42  (deployment #12)
==> applying transaction 42 ........... ok
==> deployment #12 is now active
```

The snapshot and deployment row are created *before* the new versions are
applied, so deployment #11 remains intact as the rollback target.

### salt deployments

```sh
$ salt deployments
  ID   OP        STATUS     WHEN                 SNAPSHOT
* 12   update    succeeded  2026-06-17 14:02     @snapshots/txn-42
  11   update    succeeded  2026-06-10 09:31     @snapshots/txn-38
  10   install   succeeded  2026-06-08 18:17     @snapshots/txn-35
   9   update    succeeded  2026-06-01 11:50     @snapshots/txn-31
```

The `*` marks the active deployment. Each row is backed by a snapshot that
rollback can restore.

### salt rollback

```sh
$ salt rollback
==> active deployment: #12 (update, 2026-06-17 14:02)
==> rolling back to deployment #11 (update, 2026-06-10 09:31)
==> restoring @ from @snapshots/txn-38 ... ok
==> deployment #11 is now active
==> reboot to run the restored system
```

`salt rollback` calls `salt_rollback_last`: it restores the previous
deployment's snapshot of `@` and makes that deployment active. `@home` is left
alone.

### salt verify

```sh
$ salt verify
==> verifying 412 installed files against recorded hashes
==> all files match the package database
```

`salt verify` re-hashes installed files and compares them against the per-file
`sha256` values recorded at install time, confirming the active deployment is
intact.

## 5. The desired UX

The whole point is that recovering from a bad update is trivial:

```sh
salt update
# bad update happens
salt rollback
reboot
```

If the bad update is severe enough that the system will not boot at all, the
user does not even need a running shell: GRUB carries a boot entry for the
previous deployment, so the prior known-good `@` can be selected straight from
the boot menu.

```
boot menu
  > saltOS (deployment #12)         <- broken update
    saltOS (deployment #11)         <- previous known-good, boots fine
    saltOS (deployment #10)
```

## 6. Timeline

A deployment history reads as a straight line of snapshotted root states. `@home`
runs alongside it, untouched by any rollback:

```
@home  ──────────────────────────────────────────────────────►  (never rolled back)

@      #9 ──► #10 ──► #11 ──────────► #12(bad)
                       ▲                  │
                       └── salt rollback ─┘
                           restore @snapshots/txn-38
                           #11 active again
```

After the rollback, deployment #11 is active again and #12's snapshot remains on
disk for inspection until it is pruned.

## 7. Non-Btrfs fallback

Btrfs snapshots are the primary mechanism, but `salt` does not assume Btrfs is
always present (for example, when operating on an alternate `--root`, or on a
non-Btrfs filesystem). The `salt_ctx` records whether Btrfs is in use.

When Btrfs is unavailable, the transaction engine falls back to per-transaction
saved file state under:

```
/var/lib/salt/state/
```

Before a transaction mutates files, the affected prior file state is saved
there; on failure or rollback, that saved state is restored. This is less
efficient than a copy-on-write snapshot, but it preserves the same guarantee:
a transaction can always be undone to its pre-transaction state.

## 8. Inspectability

Rollback in saltOS is meant to be understood, not trusted blindly:

- **Deployments are logged.** Every transaction writes a deployment row to
  `/var/lib/salt/db.sqlite`, viewable with `salt deployments`.
- **Snapshots are visible.** Pre-transaction snapshots live under
  `/@snapshots` (or `/.snapshots`) and can be listed with ordinary Btrfs tools.
- **Files are verifiable.** `salt verify` re-checks installed files against the
  recorded hashes, so the integrity of the active deployment can be confirmed at
  any time.

A user can always see which deployment is active, which snapshot backs it, and
what each transaction did — and can return to any previous known-good state.
