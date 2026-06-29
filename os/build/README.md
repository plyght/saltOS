# saltOS build scripts

One script per target, all sharing [`common.sh`](common.sh) so the control
plane, strata config (expose-by-default), passwordless-sudo escalation rule, and
user setup stay identical across every image.

| Script         | Target                                   | Arch(es)         | Output                                  |
|----------------|------------------------------------------|------------------|-----------------------------------------|
| `vm-apple.sh`  | Apple Virtualization / UTM (Apple Silicon) | aarch64        | `saltos-<ver>-apple-aarch64.img`        |
| `vm-x86.sh`    | Generic UEFI VM (QEMU/KVM, VMware, VBox)  | x86_64           | `saltos-<ver>-vm-x86_64.img`            |
| `pi5.sh`       | Raspberry Pi 5 SD-card image              | aarch64          | `saltos-<ver>-pi5-aarch64.img`          |
| `iso.sh`       | Live / installer ISO (Void base)          | x86_64, aarch64  | `saltos-<ver>-<edition>-<arch>.iso`     |

All outputs follow `saltos-<version>-<target>-<arch>.<ext>` and gzip/zstd-compress
where it helps.

## Building

Builds need a Linux host with root (loop devices, `chroot`, `mkfs`). On a
non-Linux host, use a throwaway Linux VM (e.g. Lima). Build the `salt` binary
first, then point the script at it:

```sh
# salt binary (in a Linux env):
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target salt salt-setup

# then a target, e.g. the Apple Silicon image:
sudo env SALT_BIN="$PWD/build/src/salt/salt" \
         SALTSETUP_BIN="$PWD/build/src/setup/salt-setup" \
         REPO_DIR="$PWD" EDITION=desktop \
         bash os/build/vm-apple.sh

# x86_64 VM image:
sudo env SALT_BIN=... REPO_DIR="$PWD" bash os/build/vm-x86.sh

# Raspberry Pi 5:
sudo env SALT_BIN=... REPO_DIR="$PWD" EDITION=console bash os/build/pi5.sh

# Live/installer ISO (arch as $1):
sudo env SALT_BIN=... SALTSETUP_BIN=... REPO_DIR="$PWD" EDITION=installer \
         bash os/build/iso.sh aarch64
```

Common env knobs: `EDITION` (`console`|`desktop`|`installer`), `VERSION`,
`IMG_SIZE_MB`, `OUT`, `SALTOS_HOSTNAME`.

## Exposed commands work from any user, sudo-free

`salt.conf` enables `expose_all`, so installing a tool in any stratum makes it a
host command automatically. Running an exposed command (e.g. `nvim`) uses an
**unprivileged user namespace** — no sudo, no root — and runs as *you*, with your
real uid and files owned by you, exactly like a normal command.

Package-manager actions (`apk`, `pacman`, …) do need real root to write the
stratum, so they escalate via the `/etc/sudoers.d/salt-strata` passwordless rule.
That same rule is also the automatic fallback for `salt run` on kernels that
forbid unprivileged user namespaces (e.g. Ubuntu's AppArmor default); Void's
kernel allows them, so the images are genuinely sudo-free for running commands.

---

# Apple Virtualization / UTM (aarch64) — import notes

`vm-apple.sh` produces a persistent, EFI-bootable **raw aarch64 disk image**
tuned for Apple's Virtualization framework — the **"Apple Virtualization"**
backend in [UTM](https://mac.getutm.app/) on Apple Silicon Macs.

It is a real read-write install (not the live ISO), so it boots faster and runs
smoother in a VM. Everything is virtio: virtio-blk disk, virtio-net, virtio-gpu
graphics, and a virtio serial console (`hvc0`).

## Why this target is separate from the ISO

Apple's `Virtualization.framework` is not a generic PC:

| Concern  | Apple framework reality              | What the build does                                   |
|----------|--------------------------------------|-------------------------------------------------------|
| Console  | serial port is a **virtio console** `/dev/hvc0`, not `ttyS0`/`ttyAMA0` | kernel `console=hvc0` + an `agetty-hvc0` login service |
| Graphics | **virtio-gpu** only                  | `virtio_gpu` in the initramfs + Xorg `modesetting`     |
| Boot     | EFI loader reads `/EFI/BOOT/BOOTAA64.EFI` off the ESP | standalone `arm64-efi` GRUB installed there            |
| Disk/net | all **virtio** (`virtio_blk`, `virtio_net`, `virtio_pci`) | drivers forced into a non-host-only initramfs          |

## Import into UTM (Apple Virtualization)

1. Decompress if needed: `zstd -d saltos-*-apple-aarch64.img.zst`.
2. UTM → **+** → **Virtualize** → **Linux**.
3. **Uncheck** "Boot from kernel image" — this image is self-booting via EFI.
4. Don't attach a boot ISO. Finish the wizard with any placeholder disk.
5. VM settings → **Drives**: delete the placeholder → **Import Drive…** → select
   the `.img` (interface **VirtIO**). **Display**: keep the default (virtio-gpu).
   Memory ≥ 2 GB, CPU ≥ 2 cores.
6. Boot. You auto-login as **`salt`** (passwordless sudo; root password `root`).
   Desktop edition auto-starts LXQt; for the text console use UTM's **Serial**
   window (maps to `hvc0`).

If the GUI misbehaves, pick the **safe graphics (nomodeset)** GRUB entry.

## First steps inside the VM

```sh
salt --help                 # native package manager + strata
salt-setup                  # pick a base distro / install to a disk
sudo salt stratum add alpine    # bring up a foreign-distro environment (ARM-native)
sudo apk add neovim             # installs into the stratum (auto-exposed)
nvim somefile                   # runs as you, no sudo, from any user
```

On aarch64 `salt stratum add arch` automatically uses **Arch Linux ARM**.
