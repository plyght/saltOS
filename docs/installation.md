# Installing saltOS

This document covers how saltOS images are produced, how to boot the live ISO on
both supported architectures, and how to install the system with the Calamares
graphical installer.

saltOS targets two architectures: **`x86_64`** and **`aarch64`**. Everything
below applies to both unless stated otherwise.

See also: [rollback.md](rollback.md), [architecture.md](architecture.md).

## Where ISOs come from

The saltOS maintainer's machine is macOS, so saltOS is **built in CI**, not on
the maintainer's laptop. Live ISOs are produced by the `iso` GitHub Actions
workflow, which runs:

- on **`workflow_dispatch`** (manual runs), and
- on **tag releases**, where the resulting images are attached to the
  corresponding GitHub Release.

The workflow runs a matrix over both architectures and uploads one ISO artifact
per arch (`x86_64`, `aarch64`).

A full base-system build is large. On free CI runners some heavy steps are
**best-effort** and may be marked with `TODO` notes where a complete base build
exceeds the runner's limits; the pipeline is nevertheless structured correctly
end to end, so that a runner (or a self-hosted/maintainer machine) with enough
resources produces complete images without changes to the workflow.

## Build pipeline

The OS build is staged (see the `os/` layout in
[CONVENTIONS.md](CONVENTIONS.md)):

- **`os/bootstrap/`** — base-system build orchestration: it builds the toolchain
  and then the base system (toolchain → base).
- **`os/iso/build-iso.sh`** — the ISO build system: it performs the rootfs
  bootstrap and then assembles the live image.

Conceptually the flow is:

```
os/bootstrap (toolchain -> base)  ->  os/iso/build-iso.sh (rootfs -> live ISO)
```

The base is assembled from saltOS packages using `salt` itself, so the same
package manager, package format, and trust order that run on an installed system
are what produce the image.

## Booting the live ISO

### In QEMU

`x86_64`:

```sh
qemu-system-x86_64 \
  -machine q35 -accel kvm -m 4096 -smp 4 \
  -bios /usr/share/OVMF/OVMF_CODE.fd \
  -cdrom saltos-x86_64.iso \
  -boot d
```

`aarch64` (aarch64 has no legacy BIOS, so UEFI firmware is required):

```sh
qemu-system-aarch64 \
  -machine virt -cpu cortex-a72 -m 4096 -smp 4 \
  -bios /usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=none,file=saltos-aarch64.iso,format=raw,id=cd \
  -device virtio-scsi-pci -device scsi-cd,drive=cd
```

Notes:

- `x86_64` boots via UEFI here using OVMF; if you prefer legacy BIOS boot you can
  drop the `-bios` line on `q35`.
- `aarch64` **must** boot via UEFI; use AAVMF (the AArch64 build of OVMF). Adjust
  firmware paths to match your distribution's package locations.
- Drop `-accel kvm` (and use `-accel tcg`) when running an arch that does not
  match your host or when KVM is unavailable.

### On real hardware

Write the ISO to a USB stick and boot from it:

```sh
sudo dd if=saltos-x86_64.iso of=/dev/sdX bs=4M status=progress oflag=sync
```

Replace `/dev/sdX` with the **whole device** of your USB drive (not a partition),
and double-check the device name before running `dd`. Boot the target machine
from the USB device, selecting UEFI boot where available.

## Installing with Calamares

The live image ships the **Calamares** graphical installer (Qt). Its files live
under `os/installer/`:

- branding under `os/installer/branding/saltos/`,
- global settings in `os/installer/settings.conf`,
- custom modules under `os/installer/modules/`.

A custom saltOS module ties the installer into the saltOS model. During install
it:

- configures the Btrfs subvolume layout — `@`, `@home`, `@var`, `@log`,
  `@snapshots`;
- installs the base system via `salt`;
- writes the bootloader **deployment entries** so the system can boot the current
  deployment and roll back to previous ones;
- seeds the package database at `/var/lib/salt/db.sqlite`.

### What the installer supports

The installer supports the full v0 install flow (DISTRO §18):

- disk selection;
- Btrfs partitioning;
- an encryption option;
- bootloader install;
- user creation;
- network setup;
- base system install;
- a desktop install option (LXQt edition).

A text-first install is the v0 baseline; **Calamares is the graphical path** on
top of the same underlying steps. Both end with the same on-disk result: a Btrfs
root with the standard subvolumes, a `salt`-installed base, a bootloader wired
for deployments, and a seeded package database.

## After install

On first boot:

- **runit** brings up services (networking, seat/login management, the desktop
  session for the LXQt edition, and so on); manage them with the `svc` wrapper.
- the package database at `/var/lib/salt/db.sqlite` reflects the installed base.
- `salt update` performs transactional system updates, taking a pre-transaction
  Btrfs snapshot and recording a deployment each time.
- `salt rollback` restores the previous deployment if an update goes wrong;
  user data in `@home` is never rolled back. See [rollback.md](rollback.md) for
  the full rollback UX.

For how the installed pieces fit together, see
[architecture.md](architecture.md).
