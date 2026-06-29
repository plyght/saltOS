# saltOS on Apple Virtualization / UTM (aarch64)

`build-arm64-vm.sh` produces a persistent, EFI-bootable **raw aarch64 disk image**
tuned for Apple's Virtualization framework — the **"Apple Virtualization"**
backend in [UTM](https://mac.getutm.app/) on Apple Silicon Macs.

It is a real read-write install (not the live ISO), so it boots faster and runs
smoother in a VM. Everything is virtio: virtio-blk disk, virtio-net, virtio-gpu
graphics, and a virtio serial console.

## Why this target exists

Apple's `Virtualization.framework` is not a generic PC. Three things differ from
the QEMU/SeaBIOS world the ISO targets, and all three are handled here:

| Concern  | Apple framework reality              | What the build does                                   |
|----------|--------------------------------------|-------------------------------------------------------|
| Console  | serial port is a **virtio console** `/dev/hvc0`, not `ttyS0`/`ttyAMA0` | kernel `console=hvc0` + an `agetty-hvc0` login service |
| Graphics | **virtio-gpu** only                  | `virtio_gpu` in the initramfs + Xorg `modesetting`     |
| Boot     | EFI loader reads `/EFI/BOOT/BOOTAA64.EFI` off the ESP | standalone `arm64-efi` GRUB installed there            |
| Disk/net | all **virtio** (`virtio_blk`, `virtio_net`, `virtio_pci`) | drivers forced into a non-host-only initramfs          |

If you boot the plain live ISO under the Apple backend you typically get a dead
serial terminal (wrong console) and no graphics — that's what this fixes.

## Build (on an aarch64 Linux host or with binfmt/qemu-user-static)

```sh
# build the salt binaries first (see top-level README), then:
EDITION=desktop ./os/vm/build-arm64-vm.sh      # LXQt desktop (default)
EDITION=console ./os/vm/build-arm64-vm.sh      # headless / text only
```

Output: `out-vm/saltos-<version>-vm-aarch64.img` (and a `.img.zst`).

Useful knobs: `IMG_SIZE_MB`, `ESP_SIZE_MB`, `SALTOS_HOSTNAME`, `VERSION`.

## Import into UTM (Apple Virtualization)

1. Decompress if needed: `zstd -d saltos-*-vm-aarch64.img.zst`.
2. UTM → **+** → **Virtualize** → **Linux**.
3. **Uncheck** "Boot from kernel image" — this image is self-booting via EFI.
4. Don't attach a boot ISO. Finish the wizard with any placeholder disk.
5. Open the new VM's settings:
   - **Drives**: delete the placeholder disk, **Import Drive…**, and select the
     `.img`. Interface: **VirtIO**.
   - **Display**: keep the default (virtio-gpu).
   - Memory ≥ 2 GB, CPU ≥ 2 cores for a comfortable desktop.
6. Boot. You auto-login as **`salt`** (passwordless sudo; root password `root`).
   - Desktop edition auto-starts the LXQt session via SDDM.
   - For the text console, use UTM's **Serial** window (it maps to `hvc0`).

## First steps inside the VM

```sh
salt --help                 # native package manager + strata
salt-setup                  # pick a base distro / install to a disk
salt stratum add alpine     # bring up a foreign-distro environment
```

Networking comes up automatically via NetworkManager over virtio-net (DHCP).
