# Raspberry Pi 5

saltOS ships an aarch64 SD-card image for the Raspberry Pi 5 (BCM2712). Unlike the
generic `aarch64` live ISO (which targets UEFI virtual machines), the Pi image boots
through the Raspberry Pi firmware: a FAT `bootfs` partition holds `config.txt`,
`cmdline.txt`, the kernel and initramfs, and a Btrfs `rootfs` partition holds the
system with `@` and `@snapshots` subvolumes.

## Status

The image builder, OTA pipeline, and A/B tryboot logic are complete and build in CI,
but have **not yet been validated on physical Pi 5 hardware**. Treat the first flash
as a bring-up, not a daily driver, until it is confirmed to boot on a real device.

## Building

Requires an aarch64 Linux host (or `qemu-user-static` + binfmt) with `mmdebstrap`,
`gdisk`, `dosfstools`, `btrfs-progs`, and `zstd`.

```sh
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target salt

sudo env \
  SALT_BIN="$PWD/build/src/salt/salt" \
  REPO_DIR="$PWD" \
  EDITION=console \
  VERSION=0.1.0 \
  OTA_SOURCE="https://updates.example.com" \
  OTA_PUBKEY="$PWD/keys/ota.pub" \
  OUT="$PWD/out-pi" \
  bash os/build/pi5.sh
```

`EDITION` is `console` or `desktop`. The kernel (`linux-image-rpi-2712`) and firmware
(`raspi-firmware`) come from the Raspberry Pi archive; everything else from Debian.

Output: `out-pi/saltos-<version>-pi5-aarch64.img` (and a `.img.zst`).

## Flashing

```sh
zstd -d out-pi/saltos-0.1.0-pi5-aarch64.img.zst
sudo dd if=out-pi/saltos-0.1.0-pi5-aarch64.img of=/dev/sdX bs=4M conv=fsync status=progress
```

Default login is `salt` / `salt` (passwordless sudo). SSH is enabled on first boot.
The root partition is grown by Btrfs automatically when you `btrfs filesystem resize max /`
after first boot if you want the full card.

## Boot layout

- Partition 1 (`bootfs`, FAT32, label `saltos-boot`) → mounted at `/boot/firmware`
- Partition 2 (`rootfs`, Btrfs, label `saltos-root`) → `subvol=@` is `/`, snapshots in `@snapshots`

`cmdline.txt` points root at `LABEL=saltos-root` with `rootflags=subvol=@`. The A/B
updater rewrites `subvol=` to switch roots; see `docs/ota.md`.
