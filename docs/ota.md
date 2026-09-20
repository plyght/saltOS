# OTA updates

saltOS updates are pull-based, signed and atomic. A client runs `salt sync` to
fetch a signed package index from a repository URL, verifies it against a
trusted ed25519 public key, then `salt update` runs one transaction: the root is
snapshotted, every grain is hash- and signature-checked, files are replaced, and
a failure at any point reverts the transaction so the system is unchanged. On
top of that `salt-ota` adds scheduling, locking, logging, a reboot policy and
the kernel trial/confirm cycle; the `salt-update` runit service drives it.

The whole flow — upgrade of `salt` and the kernel, trial boot, automatic
fallback of an unconfirmed kernel, confirmation, `salt rollback`, and refusal
of a bad-hash and a bad-signature repository — runs in QEMU in the `ota`
workflow (`.github/workflows/ota.yml`, script `os/ota/test-qemu.sh`).

## Quick publish (one command)

To build, sign, and serve a base-grain repo in one step from a salt binary
(and optional kernel):

```sh
SALT=build/src/salt/salt VERSION=0.1.2 sh os/ota/ship.sh ./ota-repo
# add KERNEL=out/Image to ship a kernel grain (image only), or
# KERNEL_TREE=/ KERNEL_RELEASE=$(uname -r) to package a complete kernel
# (vmlinuz + initramfs + modules) from an installed tree; PORT= to change the port
```

It creates a signing key on first run (reused after), builds the grains, signs
the index, prints the one-time client `repo.conf` to paste, and serves. The
client side is then just `salt update` (or `salt-ota run`). The manual steps
below explain what it does under the hood.

## Server side

The "server" is any static HTTPS host serving a published repository tree. The
layout `salt` expects is:

```
<base-url>/<arch>/index.toml
<base-url>/<arch>/index.toml.sig
<base-url>/<arch>/packages/<pkg>.grain
```

### 1. Create a signing key (once)

```sh
salt keygen ./keys ota          # writes keys/ota.pub and keys/ota.sec
```

Keep `ota.sec` secret. Bake the public key into images: `os/build/vm-x86.sh`
takes `OTA_SOURCE=<base-url>` and `OTA_KEY="$(cat keys/ota.pub)"` (written to
`/etc/salt/repo.conf`); `os/build/pi5.sh` takes `OTA_PUBKEY=keys/ota.pub`
(installed to `/etc/salt/keys/ota.pub`).

### 2. Build packages and publish a signed index

```sh
# build .grain packages into a repo tree (repo/<arch>/packages/*.grain)
OTA_SECRET_KEY=./keys/ota.sec bash os/ota/publish.sh ./repo
```

### 3. Host it

Any static host works (S3, GitHub Pages, nginx). For a quick self-hosted endpoint:

```sh
python3 -m http.server 8080 --directory ./repo
# or: OTA_ROOT=./repo OTA_PORT=8080 bun os/ota/server.ts
```

Then point clients at the base URL (the directory that contains `<arch>/`):

```sh
# /etc/salt/repo.conf on the device
repo = "current"
source = "https://updates.example.com"
key = "/etc/salt/keys/ota.pub"
```

## Client side

`salt-ota` reads `[ota]` and `[deploy]` from `/etc/salt/salt.conf`:

```toml
[ota]
enabled = true          # false makes every salt-ota command a no-op (exit 0)
interval = "86400"      # seconds between automatic runs
reboot_on_kernel = false # reboot by itself when a new kernel/root is armed
ab = false              # Pi: stage into the standby subvolume instead (see below)

[deploy]
keep = 5                # deployments (snapshots) to keep; pinned ones never count
```

### Commands

| command | what it does |
| --- | --- |
| `salt-ota check` | `salt sync` + `salt update --check`; prints the pending upgrades, applies nothing |
| `salt-ota run [--reboot\|--no-reboot]` | sync, recover a crashed transaction if any, apply all upgrades in one transaction, arm a trial boot when the kernel (or the A/B root) changed, reboot per policy |
| `salt-ota status` | config, last run + result, boot trial state (`salt boot status`), deployments |
| `salt-ota confirm [--fallback]` | run `/etc/salt/health.d/*`; if all pass, make the tried kernel/root the default. With `--fallback` an unhealthy trial boot reboots into the previous kernel/root instead |
| `salt-ota interval` | print the configured interval (used by the runit service) |

Exit codes (`salt-ota run` / `check` / `confirm`):

| code | meaning |
| --- | --- |
| 0 | nothing to do / already up to date / OTA disabled |
| 1 | error before anything changed (lock busy, sync or index verification failed, …); system unchanged |
| 2 | updated (or, for `check`: updates are available) |
| 3 | updated and a reboot is required (new kernel or root armed for one trial boot) |
| 4 | the transaction failed and was rolled back; system unchanged |
| 5 | health check failed (`confirm`) |

Every run appends to `/var/log/salt-ota.log` and records its outcome in
`/var/lib/salt/ota-state` (shown by `status`). Runs are serialised with an
`flock` on `/run/salt-ota.lock`; a second concurrent run exits 1 immediately.

### What happens on `salt-ota run`

1. `salt sync` fetches `index.toml` + `.sig` and verifies the signature against
   the key in `repo.conf`. An unknown key or tampered index is refused (exit 1).
2. `salt update --check` decides whether there is anything to do (exit 0 if not).
3. `salt update` runs one transaction: writable Btrfs snapshot of `@` into
   `@snapshots/root-<id>` (or a file backup on non-Btrfs), download + hash +
   signature check of every grain, extraction with rename-over so a running
   `salt` binary is replaced safely, DB update, deployment record
   (`salt deployments`). Any error reverts everything (exit 4).
4. If the transaction shipped a kernel, `salt boot update` regenerates
   `grub.cfg` with the new kernel first and the previous kernel(s) as fallback
   entries, keeps the running kernel's `vmlinuz`/`initramfs`/modules as a
   fallback even though the old kernel grain is gone, and `salt boot try` arms
   a one-shot trial through the GRUB environment block (`saltos_try=1`,
   `saltos_try_entry`, `saltos_pending`). Exit 3.
5. With `reboot_on_kernel = true` (or `--reboot`) the machine reboots now.

### Kernel trial and confirmation (x86_64, GRUB)

- GRUB boots the trial entry once and clears `saltos_try` before loading the
  kernel, so a hang or panic (`panic=30` on the cmdline) reboots into the
  previous default kernel — nothing is confirmed by the mere act of booting.
- After a successful boot the `salt-update` service waits
  `SALTOS_OTA_CONFIRM_DELAY` seconds (30 by default) and runs
  `salt-ota confirm --fallback`: the health checks in `/etc/salt/health.d/`
  run, and on success the tried kernel becomes `saltos_default`. On failure the
  pending kernel is dropped and the service reboots into the previous one.
- `salt boot status` shows `default`, `pending` and `try armed`. Old fallback
  kernels that are neither running, newest, default nor pending, and are not
  owned by an installed grain, are removed on the next `salt boot update`.

### The `salt-update` runit service

`os/runit/sv/salt-update/run` is systemd-free and enabled by the x86 and Pi
image builders. At boot it sleeps `SALTOS_OTA_CONFIRM_DELAY`, runs
`salt-ota confirm --fallback`, sleeps `SALTOS_OTA_STARTUP_DELAY` (120 s), then
loops `salt-ota run` every `interval` seconds. Both delays can be overridden in
`/etc/sv/salt-update/conf`.

### Self-update of `salt`

`salt` may upgrade itself in the same transaction as everything else. Grain
extraction writes each file next to its destination and `rename()`s it over,
so the running executable keeps its old inode and finishes the transaction with
the old code; the next invocation runs the new binary. Deployment tables are
created with `CREATE TABLE IF NOT EXISTS` on first use, so an old `salt`
reading a database written by a newer one keeps working.

## A/B root switch and kernel tryboot (Raspberry Pi 5)

The Pi image (`os/build/pi5.sh`) uses `loader = "tryboot"` in
`/etc/salt/boot.conf` and `os/pi/ab-update.sh` (installed as
`/usr/lib/saltos/ab-update.sh`):

- **Kernel updates** stage `vmlinuz_<slot>`, `initramfs_<slot>` and
  `cmdline_<slot>.txt` in the firmware partition and write a one-shot
  `tryboot.txt`; `salt boot try` reboots with `reboot "0 tryboot"`, the
  firmware boots `tryboot.txt` exactly once, and `salt-ota confirm` (running
  kernel == staged kernel) rewrites `config.txt` to make it permanent. An
  unconfirmed or failed boot returns to the committed `config.txt` on the next
  reboot.
- **A/B roots** (`ab = true` in `[ota]`): `prepare` snapshots the active
  subvolume (`@`) into the standby (`@b`), the update is applied with
  `salt --root`, `finalize` points `tryboot.txt` at the standby subvolume and
  `commit`/`abort` decide after the trial boot.

The script's slot/tryboot/commit/abort logic is exercised by the `pi_tryboot`
ctest against a simulated firmware directory. It has **not been run on Pi 5
hardware or under a Pi firmware emulator** yet; the snapshot + GRUB-style
rollback path is the one verified end-to-end in QEMU.
