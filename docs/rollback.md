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
@                        system root — snapshotted before every transaction
@home                    user data   — never rolled back
@snapshots/root-<N>      writable snapshot of @ taken before deployment N
```

`@home` is a separate subvolume on purpose. Rollback replaces `@` (the system),
but it must never destroy the user's files. Because `@home` lives outside `@`,
returning the system to a previous deployment leaves documents, downloads, and
configuration in the user's home directory untouched. The exact layout can
evolve, but the invariant — rollback must not destroy user data by default —
does not.

The subvolume names are configured in `/etc/salt/boot.lua` (`root_subvol`,
`snapshots_subvol`); the image builders write them together with the root
label and the kernel command line.

## 3. Transaction lifecycle

Every system-mutating operation (`install`, `remove`, `update`) runs as one
transaction. The lifecycle is:

1. **Begin.** `salt` builds its operating context (`salt_ctx`) for the target
   root and opens the database. A new transaction id is allocated.
2. **Snapshot.** Before any file is touched, `@` is snapshotted into
   `@snapshots/root-<id>` (`salt_snapshot_create`). The snapshot is writable so
   it can later become the root itself.
3. **Record deployment.** A deployment row is written to the database recording
   the operation, its status, the time, and the snapshot that backs it.
4. **Apply.** Packages are extracted/removed and the package database is updated.
   Each replaced file is written next to its destination and `rename()`d over
   it, so even the running `salt` binary can be upgraded mid-transaction.
5. **Finish.** On success the transaction is marked succeeded, the packages that
   changed (`txn_changes`) and the kernel the deployment carries (`txn_meta`)
   are recorded, the bootloader menu is regenerated, and snapshots beyond
   `deploy.keep` in `salt.lua` (default 5) are pruned — pinned deployments are never pruned.
   On any failure, the transaction is rolled back **automatically**: every file
   it touched is put back from the per-transaction backup
   (`salt_txn_revert_files`) and the database changes are reverted,
   leaving the system exactly as it was before the transaction began.

The automatic case means a transaction that dies partway — a bad package, an
interrupted extraction, a failed dependency step, a hash mismatch — never leaves
a half-installed system. The manual case (below) covers updates that *succeed*
mechanically but turn out to be bad in use.

### Core types

The transaction and rollback logic is exposed by `halite` (see
`include/salt/txn.h` and `include/salt/deploy.h`):

- `salt_ctx` — the root, database path, state directory, snapshot directory, and
  whether Btrfs is available.
- `salt_deployment` / `salt_deployment_list` — a deployment's id, operation,
  status, timestamp, and backing snapshot; and a list of them.
- `salt_snapshot_create(ctx, txn_id, &snapshot)` — take the pre-transaction
  snapshot of `@`.
- `salt_txn_revert_files(ctx, txn_id)` — put back every file a transaction
  touched from its saved state (used for the automatic rollback of a failed
  transaction and for rollback on non-Btrfs roots).
- `salt_deployments_list(ctx, db, &out)` — enumerate deployments / rollback
  points.
- `salt_deploy_record` / `salt_deploy_changes` / `salt_deploy_meta` — record and
  read the per-deployment package changes and kernel.
- `salt_deploy_pin`, `salt_deploy_prune` — pin a deployment; drop the oldest
  unpinned snapshots beyond `keep`.
- `salt_rollback_to(ctx, db, txn_id, ...)` — make deployment `txn_id` the root
  again (see below).

## 4. Commands

```sh
salt update          # upgrade the system as one snapshotted transaction
salt deployments     # list deployments: date, op, kernel, snapshot, packages changed
salt rollback [N]    # go back to the state before deployment N (default: the last one)
salt pin [--unpin] N # keep deployment N's snapshot forever (pruning skips it)
salt boot status     # bootloader state: default kernel, pending trial, armed flag
salt verify          # verify installed files against recorded hashes
```

### salt deployments

```
$ salt deployments
   ID    DATE              OP        STATUS  KERNEL             SNAPSHOT
*  2     2026-09-20 15:12  update    ok      6.18.52_1          root-2
       linux-saltos 6.12.110-1 -> 6.18.52-1
       salt 0.1.0-1 -> 0.1.1-1
   1     2026-09-20 15:08  install   ok      6.12.110_1         txn-1
       + linux-saltos 6.12.110-1
       + salt 0.1.0-1
```

The `*` marks the active deployment, a `P` in the second column marks a pinned
one. The SNAPSHOT column is the subvolume under `@snapshots` that holds the
root *as it was before* that deployment ran (`-` once it has been pruned).

### salt rollback [N]

```
$ salt rollback
undid deployment 2 as deployment 3; reboot to activate
```

`salt rollback` (or `salt rollback N`) takes the snapshot that was made before
deployment N — i.e. the root exactly as it was when the previous deployment was
active — and makes it the root again, as a new deployment so history stays a
straight line:

1. the top level of the Btrfs filesystem is mounted;
2. `@snapshots/root-N` is snapshotted (writable) into a replacement root;
3. the package database in the replacement is checked against the recorded
   state and the rollback deployment is recorded in it;
4. the live `@` is renamed to `@snapshots/root-<rollback id>` (so the undone
   state itself stays inspectable and bootable), and the replacement is renamed
   to `@`;
5. the bootloader menu is regenerated from the new root and a reboot is
   requested. Kernel files that only exist in the undone root stay in its
   snapshot; the GRUB entry for that snapshot still boots them.

Nothing about the running system changes until the reboot; the next boot lands
in the restored root. `salt_rollback_to` is the halite API behind this; the
same steps are available from a rescue shell via `os/btrfs/snapshot.sh rollback`.

On a non-Btrfs root the per-transaction file backup under `/var/lib/salt/state`
is restored in place instead (see section 7).

### salt pin

```
$ salt pin 1
deployment 1 pinned (kept by pruning)
```

Pinned deployments are excluded from `keep`-based pruning and keep their GRUB
entry, so a known-good generation can be kept around indefinitely.

### salt verify

```sh
$ salt verify
==> verifying 412 installed files against recorded hashes
==> all files match the package database
```

`salt verify` re-hashes installed files and compares them against the per-file
`sha256` values recorded at install time, confirming the active deployment is
intact.

## 5. Boot menu: GRUB entries per deployment

DISTRO.md left open whether previous generations should be exposed through GRUB
entries or through a separate boot-environment tool. saltOS uses **GRUB
entries**: GRUB is already the bootloader on x86_64, it can read Btrfs
subvolumes directly, and the whole state — one `grub.cfg` plus a 1 KiB
environment block on the ESP — is inspectable with `cat`. `salt boot update`
regenerates `/boot/grub/grub.cfg` after every transaction and rollback:

```
saltOS 0.1.0 - kernel 6.18.52_1 (deployment 2)      <- newest kernel in @
saltOS 0.1.0 - kernel 6.12.110_1 (deployment 2)     <- previous kernel, still in @
Previous deployments
  saltOS 0.1.0 - deployment 2 (2026-09-20 15:12, kernel 6.12.110_1)   <- @snapshots/root-2
```

Each "previous deployment" entry boots the kernel and initramfs *inside* that
snapshot with `rootflags=subvol=@snapshots/root-N`, so it works even if the
kernel in `@` is broken. Deployments whose transaction failed are not listed;
entries are capped by `max_snapshots` in `boot.lua`.

Kernel upgrades are armed rather than switched: `salt boot try` writes
`saltos_try=1`, `saltos_try_entry` and `saltos_pending` to the GRUB environment
block, GRUB boots that entry once and clears the flag before loading the kernel,
and only `salt boot confirm` (run by `salt-ota confirm` after the health checks)
makes it `saltos_default`. If the trial kernel never reaches the confirmation,
the next boot is the previous default again. Kernel handling on the Pi
(firmware `tryboot.txt`) is described in [ota.md](ota.md).

The whole UX is therefore:

```sh
salt update
# bad update happens
salt rollback
reboot
```

and, if the bad update is severe enough that the system will not boot at all,
picking the previous deployment from the GRUB menu without a running shell.

## 6. Timeline

A deployment history reads as a straight line of snapshotted root states. `@home`
runs alongside it, untouched by any rollback:

```
@home  ──────────────────────────────────────────────────────►  (never rolled back)

@      #9 ──► #10 ──► #11 ──────────► #12(bad) ──► #13
                                          │          ▲
                                          └──────────┘
                                   salt rollback: @snapshots/root-12
                                   (the root before #12) becomes @ again
```

After the rollback, deployment #13 is the root as it was under #11, and #12's
root lives on as `@snapshots/root-13` for inspection until it is pruned.

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
  `@snapshots/root-<N>` and can be listed with `btrfs subvolume list /`.
- **Files are verifiable.** `salt verify` re-checks installed files against the
  recorded hashes, so the integrity of the active deployment can be confirmed at
  any time.

A user can always see which deployment is active, which snapshot backs it, and
what each transaction did — and can return to any previous known-good state.
