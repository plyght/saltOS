# saltOS Btrfs layer

The Btrfs subvolume layout and the snapshot/rollback helpers that back saltOS
full-system rollback. `salt` calls these as its Btrfs backend. Arch-neutral
(x86_64 and aarch64).

## Subvolume layout

```
@           ->  /            system root
@home       ->  /home        user data (never rolled back)
@var        ->  /var         variable state
@log        ->  /var/log     logs
@snapshots  ->  /.snapshots  read-only root snapshots
```

`/.snapshots` is the on-disk mount of the `@snapshots` subvolume. The snapshot
helper accepts either `/.snapshots` or `/@snapshots` via `SALT_SNAPSHOT_DIR`.

State paths used by `salt` (see docs/CONVENTIONS.md):

```
/var/lib/salt/db.sqlite     package database + transaction log + deployments
/var/lib/salt/state/        per-transaction saved file state (non-btrfs fallback)
/.snapshots                 btrfs snapshots of @
```

## layout.sh

Creates the subvolumes on a target device and emits fstab entries.

```sh
layout.sh -d /dev/nvme0n1p2 -m /mnt -f -o /mnt/etc/fstab
```

- `-d` target device (required)
- `-m` mountpoint used during setup (default `/mnt`)
- `-f` run `mkfs.btrfs` first (destroys data)
- `-c` compression (default `zstd:3`)
- `-o` also write the generated fstab to a file

It mounts subvolid 5, creates `@ @home @var @log @snapshots`, mounts them at the
final layout with `noatime,compress=zstd:3,space_cache=v2`, and prints fstab
lines keyed by filesystem UUID.

## snapshot.sh

The Btrfs backend for rollback. Snapshots name themselves by transaction id and
UTC timestamp: `root-<id>-<YYYYMMDDThhmmssZ>`.

```sh
snapshot.sh create 42        # pre-transaction snapshot, prints the snapshot name
snapshot.sh list             # newest first
snapshot.sh delete root-42-20260617T101500Z
snapshot.sh rollback root-41-20260616T090000Z
```

`create` is the pre-transaction hook: `salt` calls it before mutating `@` and
records the returned name against the deployment row in `db.sqlite`.

`rollback` snapshots the current root for safety, makes a writable copy of the
chosen snapshot, and sets it as the default subvolume with
`btrfs subvolume set-default`; the change takes effect on the next boot. `@home`
is a separate subvolume and is therefore never affected.

Override paths with `SALT_ROOT_SUBVOL`, `SALT_SNAPSHOT_DIR`, `SALT_STATE_DIR`.

## fstab.template

Placeholder fstab matching the layout. The installer substitutes
`@ROOT_UUID@` and `@BOOT_UUID@` with the real filesystem UUIDs.

## Install targets

| Source here       | Installs to                  | Mode |
|-------------------|------------------------------|------|
| `layout.sh`       | `/usr/lib/salt/btrfs/layout.sh`   | 0755 |
| `snapshot.sh`     | `/usr/lib/salt/btrfs/snapshot.sh` | 0755 |
| `fstab.template`  | `/usr/share/salt/fstab.template`  | 0644 |
