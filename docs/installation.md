# Installing saltOS

This document covers how saltOS images are produced, how to boot the live ISO on
both supported architectures, and how to install the system with either of the
two supported installers: the text installer `salt-setup` and the Calamares
graphical installer. Both produce the same system: a native saltOS root with the
distribution you choose as the primary stratum. `docs/installer.md` has the full
option reference; this page is the practical walkthrough.

saltOS targets two architectures: **`x86_64`** (legacy BIOS and UEFI) and
**`aarch64`** (UEFI). Everything below applies to both unless stated otherwise.

See also: [rollback.md](rollback.md), [architecture.md](architecture.md),
[headless-vm-ssh.md](headless-vm-ssh.md).

## Where images come from

saltOS is **built in CI**. Every image workflow builds its image, boots it in
QEMU, asserts the runit markers on the serial console and uploads the image plus
the serial log (and a screenshot where a display is involved) as artifacts.

| workflow | output | boot gate |
|---|---|---|
| `installer-iso.yml` | `saltos-<ver>-installer-x86_64.iso` (LXQt + Calamares + `salt-setup`) | text install on BIOS and UEFI, Calamares install on UEFI; each reboots into the installed disk |
| `installer-iso-arm64.yml` | `saltos-<ver>-installer-aarch64.iso` | live boot and text install + reboot under AAVMF |
| `live-iso.yml` | `saltos-<ver>-{console,desktop}-x86_64.iso` | boot, stratum plane, foreign-stratum E2E, LXQt panel |
| `live-iso-arm64.yml` | `saltos-<ver>-console-aarch64.iso` | boot + stratum plane under AAVMF |
| `void-iso.yml` | Void-based live ISO (`os/build/iso.sh`) | boot + native package E2E |
| `vm-image-x86.yml` | `saltos-<ver>-vm-x86_64.img` generic UEFI disk image | boots to the autologin console under OVMF |
| `vm-image-arm64.yml` | `saltos-<ver>-vm-aarch64.img` (QEMU, UTM, Apple Virtualization) | boots to the autologin console under AAVMF |
| `thinkpad-image.yml` | ThinkPad live/installer ISO with the self-hosted kernel | live boot, GPT install, installed UEFI boot, screenshot |
| `pi5-image.yml` | `saltos-<ver>-pi5-aarch64.img` SD-card image | boot partition mounted and inspected (firmware, kernel, initramfs, `config.txt`, `cmdline.txt`), root inspected (runit, `salt`, `salt-setup`) |
| `native-base.yml` | ISO assembled from self-built `.grain` packages via `os/iso/build-iso.sh` | boot marker |

The Debian-based live ISOs are built by `os/iso/live-build.sh` with
`EDITION=console|base|desktop|installer`:

- **console** — minimal live shell, no installer.
- **base** — console plus `salt-setup`, which autostarts on tty1.
- **desktop** — LXQt live session with `salt-setup` reachable from a terminal
  ("Install saltOS (text installer)" launcher).
- **installer** — desktop plus Calamares ("Install saltOS" launcher). This is
  the image most users want.

## Booting the live ISO

### In QEMU

Use the shared helpers, which pick KVM when it is usable and multi-threaded TCG
otherwise:

```sh
# boot the ISO and wait for the runit marker
bash os/iso/qemu-boot-test.sh --iso saltos-0.1.0-installer-x86_64.iso --arch x86_64 --firmware uefi
bash os/iso/qemu-boot-test.sh --iso saltos-0.1.0-installer-x86_64.iso --arch x86_64 --firmware bios
bash os/iso/qemu-boot-test.sh --iso saltos-0.1.0-installer-aarch64.iso --arch aarch64 --firmware uefi

# fully automated install into a fresh virtual disk, then reboot from that disk
bash os/iso/qemu-install-test.sh --iso saltos-0.1.0-installer-x86_64.iso \
  --config os/iso/tests/text-erase-btrfs.lua --mode text --firmware bios --out t-bios
bash os/iso/qemu-install-test.sh --iso saltos-0.1.0-installer-x86_64.iso \
  --config os/iso/tests/calamares-erase-btrfs.lua --mode calamares --firmware uefi --out t-cala
```

Or by hand — `x86_64`:

```sh
qemu-system-x86_64 \
  -machine q35 -accel kvm -m 4096 -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -cdrom saltos-0.1.0-installer-x86_64.iso -boot d
```

`aarch64` (no legacy BIOS exists, so UEFI firmware is required):

```sh
qemu-system-aarch64 \
  -machine virt -accel tcg,thread=multi -cpu max -m 4096 -smp 4 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=none,file=saltos-0.1.0-installer-aarch64.iso,format=raw,id=cd \
  -device virtio-scsi-pci -device scsi-cd,drive=cd \
  -device virtio-net-pci,netdev=n0,romfile= -netdev user,id=n0
```

Notes:

- Drop the pflash line on `q35` for a legacy BIOS boot of the x86_64 ISO.
- Use `-accel kvm` only when `/dev/kvm` is writable and the guest architecture
  matches the host; otherwise say `-accel tcg,thread=multi` explicitly. Do not
  rely on `accel=kvm:tcg` fallback — it hides the failure.
- `romfile=` on the virtio NIC avoids the `efi-virtio.rom` lookup failure on
  hosts without the QEMU option ROMs.

### On real hardware

Write the ISO to a USB stick and boot from it:

```sh
sudo dd if=saltos-0.1.0-installer-x86_64.iso of=/dev/sdX bs=4M status=progress oflag=sync
```

Replace `/dev/sdX` with the **whole device** of your USB drive (not a partition),
and double-check the device name before running `dd`. The x86_64 ISO is a
hybrid image and boots from USB on both legacy BIOS and UEFI machines; on
aarch64 select UEFI boot. The live media carries the common Wi-Fi, Bluetooth,
Intel/AMD/NVIDIA GPU firmware and NetworkManager, so wired networks come up over
DHCP and Wi-Fi can be joined from the live session.

If Secure Boot is enforced, disable it in firmware for the live medium; see
"Secure Boot" in `docs/installer.md` for what the installed system supports.

## Installing with `salt-setup` (text)

On the base ISO `salt-setup` starts on tty1. On the desktop/installer ISO open a
terminal and run `sudo salt-setup`, or use the "Install saltOS (text installer)"
launcher. It asks, in order:

1. **Disk and mode** — erase the whole disk, or install alongside the existing
   operating systems in free GPT space (existing partitions are never touched;
   you choose the size of the new root). `--target` installs into a root you
   partitioned and mounted yourself.
2. **Firmware** (x86_64) — auto-detected from how the live medium booted; UEFI
   only, legacy BIOS only, or both. aarch64 is always UEFI.
3. **Filesystem** — Btrfs (default; snapshots and rollback) or ext4.
4. **Encryption** — LUKS2 full-disk encryption with a passphrase.
5. **Swap** — none, swap file, swap partition or zram, with a size.
6. **Primary stratum** — the distribution that provides userland (`arch`,
   `debian`, `void`, `fedora`, `opensuse`, `alpine`).
7. **Identity and user** — hostname, username, password, sudo, optional root
   password, autologin, timezone, locale, console keymap (the X keyboard layout
   is derived).
8. **Network** — wired DHCP (default), Wi-Fi (SSID/PSK, joined immediately and
   persisted), or none.
9. **Desktop** — keep what the live medium runs or install a console system.
10. **Boot** — keep other operating systems in the GRUB menu (os-prober), extra
    kernel command line, kernel source (native or from a stratum).

Then it shows a summary and asks for confirmation before touching the disk.

Non-interactive: put the answers in a Lua file that returns a table (see the
reference in `docs/installer.md` or the fixtures in `os/iso/tests/*.lua`;
`salt eval system.lua` shows what it evaluates to) and run

```sh
sudo salt-setup --from system.lua
```

Disk, user and primary stratum are required; the other keys fall back to the
documented defaults and the whole file is validated before anything is touched.
Use `--profile` to preseed the defaults offered by the interactive prompts
(the prompts still appear; press Enter to accept), and `--set
section.key=value` for one-off overrides. `--dump-config` prints the effective configuration without
installing.

## Installing with Calamares (GUI)

On the installer ISO double-click "Install saltOS". Calamares walks through
welcome, language, keyboard, partitions (erase / alongside / replace / manual,
Btrfs or ext4, encryption, swap), **primary stratum**, **desktop**, users and a
summary, then partitions and mounts the target. The final step, the
`saltos_setup` job, writes those answers to `/run/saltos-installer/system.lua`
and runs

```sh
salt-setup --from /run/saltos-installer/system.lua --target <root> --yes
```

so the GUI and the text installer share one implementation and one result. The
installer log (`sudo calamares -D6` output, `/root/.cache/calamares/session.log`)
contains `salt-setup`'s full output if something fails.

## After install

On first boot:

- **runit** brings up services (NetworkManager, chronyd, dbus, seatd, getty and
  — when a desktop was installed — sddm); manage them with the `svc` wrapper.
- `/etc/salt/system.lua` and `/etc/salt/system.lock.toml` describe the
  installed system; `salt config apply system.lock.toml` reproduces it.
- the package database at `/var/lib/salt/db.sqlite` reflects the installed base.
- `salt update` performs transactional system updates, taking a pre-transaction
  Btrfs snapshot and recording a deployment each time (Btrfs installs only).
- `salt rollback` restores the previous deployment if an update goes wrong;
  user data in `@home` is never rolled back. See [rollback.md](rollback.md) for
  the full rollback UX.

For how the installed pieces fit together, see
[architecture.md](architecture.md).
