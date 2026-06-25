# Void-based daily-driver edition

`os/iso/void-build.sh` builds a daily-drivable saltOS ISO on a minimal,
independent, runit-native Void Linux base. It exists side by side with the
recipe-driven native base (`os/bootstrap` + `os/iso/build-iso.sh`) and the
Debian-seeded `os/iso/live-build.sh`. It is the pragmatic path to a usable
desktop: Void ships prebuilt binaries via `xbps`, so nothing is compiled from
source here, and Void is runit-native, so there is no systemd/elogind friction.

In this model saltOS remains the control plane and identity — `salt`, `halite`,
`.grain` for first-party packages, the stratum plane, `salt-setup`, and rollback
sit on top of the Void base. The big userland still arrives only when the user
picks additional strata.

## What it does

1. Downloads and sha256-verifies the official Void glibc rootfs tarball
   (`void-x86_64-ROOTFS-20250202.tar.xz`,
   `3f48e6673ac5907a897d913c97eb96edbfb230162731b4016562c51b3b8f1876`, from
   `https://repo-default.voidlinux.org/live/current`) and extracts it.
2. Chroots in and uses Void's own `xbps` to install the base, boot tooling
   (`dracut`, `grub`, `btrfs-progs`, `dosfstools`, `gptfdisk`, `parted`, `rsync`,
   `squashfs-tools`), networking (`NetworkManager`, `dbus`, `elogind`, `polkit`,
   `seatd`), firmware (`void-repo-nonfree` + `linux-firmware*`), and the LXQt
   desktop (`xorg-minimal`, `mesa-dri`, `xf86-input-libinput`, the `xf86-video-*`
   drivers, `lxqt`, `sddm`, plus apps).
3. Installs the saltOS layer: `salt` + `salt-setup` binaries, `/etc/salt/strata`,
   the `salt-shims.sh` profile hook, `repo.conf`/`salt.conf`, and a saltOS
   `os-release`.
4. Enables runit services the Void way (symlinks under
   `/etc/runit/runsvdir/default`), only for services that exist in the rootfs.
5. Builds the live initramfs with Void's own `dracut` (`dmsquash-live`) inside the
   chroot — avoiding any host-dracut/Void-kernel mismatch — then assembles the
   squashfs and a hybrid BIOS+EFI GRUB ISO.

## Editions

- `EDITION=desktop` — autologin into LXQt (the daily driver).
- `EDITION=installer` (default) — the LXQt desktop plus an autostarted
  "Install saltOS" launcher that runs `salt-setup`.

## Build

```
cmake --build build --target salt salt-setup
sudo env SALT_BIN=$PWD/build/src/salt/salt \
         SALTSETUP_BIN=$PWD/build/src/setup/salt-setup \
         REPO_DIR=$PWD EDITION=installer OUT=$PWD/out-iso \
         bash os/iso/void-build.sh x86_64
```

CI builds and QEMU boot-tests it via `.github/workflows/void-iso.yml`
(`SALTOS_BOOT_OK` serial marker). The rootfs tarball date is pinned and will need
bumping when Void rotates `live/current`.
