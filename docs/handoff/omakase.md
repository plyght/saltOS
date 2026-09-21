# Handoff: omakase track (opinionated Sway ISO)

Track: `os/omakase/`, `.github/workflows/omakase-iso.yml`, `docs/omakase.md`,
edition tables in `README.txt` / `DISTRO.md`. Product decisions and the full
user-facing description live in `docs/omakase.md`; this file is only the
resume-from-cold state for whoever continues the work.

## Done (on main)

| commit    | what                                                                                  |
|-----------|---------------------------------------------------------------------------------------|
| `350909a` | omakase tree, configurator, cidata loader, dashboard, orchestrator on `salt-setup --from`, package maps, dotfiles, themes, `saltos-theme`/`saltos-menu`/`saltos-update`, offline Arch mirror, ISO builder, workflow, docs |
| `d06226e` | workflow opens /dev/kvm to the runner (TCG timed out)                                  |
| `78eb685` | encrypt / alongside / interactive `OMAKASE_TEST_MODE`s in `test-vm.sh`, `install-paths` matrix in the workflow, real-user Sway launch (`saltos-session-launch`), os-prober `/run/udev` bind in `src/setup/main.cpp` |
| `feb6a39` | Unsplash wallpapers: manifests `os/omakase/wallpapers/*.toml`, build-time download + sha256 verify (`build/wallpapers.sh`), `CREDITS`, `saltos-wallpaper` |
| `15ddfc0` | saltos-site ASCII crystal (`os/omakase/live/crystal.txt`) above the greeter wordmark    |
| `0b91c0c` | gruvbox wallpaper swapped (owner disliked the orange salt crystals)                     |
| `5e2018c` | arch-aware `build/vendor.sh`: x86_64 tarballs vs aarch64 Vicinae AppImage (unsquashed at build, no FUSE) / Helium arm64 tar.xz verified with `gpgv` against `build/helium-signing-key.asc` / arm64 gum |
| `12feb43` | aarch64 edition: `build/arch-mirror.sh aarch64` (Arch Linux ARM rootfs + `core extra alarm aur`), `strata/arch-aarch64.toml`, arm64-efi `grub-mkrescue`, `serial_tty()` -> `ttyAMA0`, `stratum_stock_accounts` drops stock rootfs users (`alarm`, `debian`, ...) |
| `924e368` | `ARCH=aarch64 build/test-vm.sh` (qemu-system-aarch64, AAVMF, KVM if writable `/dev/kvm` else TCG + long timeouts), `build-aarch64` job on `ubuntu-24.04-arm`, docs |
| `615f83c` | `ipxe-qemu` on the arm64 runner (first `build-aarch64` run died on a missing `efi-virtio.rom`) |

Verified for each of the above before pushing: `cmake -G Ninja -B build
-DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build
--output-on-failure` (5/5), `shellcheck -S warning -x os/omakase/build/*.sh
os/omakase/live/saltos-* os/omakase/target/bin/* os/omakase/lib/omakase.sh
os/omakase/target/sv/*/run`, `bash os/omakase/build/verify-packages.sh`
(6 strata, 0 missing, on both x86_64 and aarch64 package maps).

QEMU/OVMF x86_64 (`OMAKASE_TEST_MODE=erase|encrypt|alongside|interactive`):
all `ALL OK` as of `feb6a39` (`SALTOS_SWAY_SESSION_OK`, LUKS unlock, `salt
rollback`, Windows-style ESP+NTFS untouched, `Windows Boot Manager` in GRUB,
gum configurator driven over serial, swaybg photo screendump).

QEMU/AAVMF aarch64 (TCG, ~45 min install + ~10 min boot, ISO built from
`924e368`): `ALL OK (erase)` -- `SALTOS_SWAY_SESSION_OK`, theme switch,
wallpaper next/credits, 82528-colour screendump, real-uid Sway, sudo, runit,
and `SALTOS_STRATUM_IDENTITY_OK` (the configured user's uid resolves to their
name inside the Arch Linux ARM stratum; the stock `alarm` account is gone).

omakase-iso run 35549759545 (`615f83c`) is fully green: x86_64 build + erase,
encrypt, alongside, interactive legs and `build-aarch64` (TCG on the GitHub
ARM runner, no `/dev/kvm`; ISO build ~3 min with native arm64 mmdebstrap,
QEMU step ~2 h). That covers `stratum_stock_accounts()` and
`SALTOS_STRATUM_IDENTITY_OK` on both architectures.

## In progress (exact state)

Nothing in flight; the working tree is clean.

## Remaining, in priority order

1. Keep the `omakase-iso` workflow on main green (`gh run list --repo
   plyght/saltOS --workflow omakase-iso.yml`); `build-aarch64` runs ~2 h under
   TCG and has a 240 min job timeout.
2. If `SALTOS_STRATUM_IDENTITY_OK` fails: check `install-serial.log` for
   "removing stock account" / "could not remove"; `userdel -r` runs through
   `in_stratum` (chroot with `/proc` bound) and `deluser` is the Debian
   fallback.
3. aarch64 encrypt / alongside / interactive modes were never run
   (`docs/omakase.md` says so). `alongside` needs an ARM fixture with an ESP but
   no Windows boot manager exists for ARM, so only "existing partitions
   preserved" can be asserted there.
4. Open product questions for the owner are listed at the end of
   `docs/omakase.md` (contested apps, per-distro gaps).

## How to build and test

x86_64 (in `debian:bookworm`, see the `build` job in the workflow for the exact
apt list):

    bash os/omakase/build/vendor.sh x86_64 out/vendor
    bash os/omakase/build/wallpapers.sh out/wallpapers
    bash os/omakase/build/arch-mirror.sh x86_64 out/mirror/arch   # needs docker
    WORK=out/work OUT=out MIRROR_DIR=out/mirror/arch VENDOR_DIR=out/vendor \
      WALLPAPER_DIR=out/wallpapers REPO_DIR=$PWD \
      SALT_BIN=build/src/salt/salt SALTSETUP_BIN=build/src/setup/salt-setup \
      bash os/omakase/build/iso.sh x86_64
    OMAKASE_TEST_MODE=erase bash os/omakase/build/test-vm.sh out/saltos-omakase-x86_64.iso out/test

aarch64: same with `aarch64`; the ISO build needs an arm64 Debian rootfs
(`mmdebstrap --arch=arm64` through `qemu-aarch64-static`, ~40 min emulated;
bind-mount `/usr/bin/qemu-aarch64-static` into the build container) and
`grub-efi-arm64-bin` (or the modules in the built rootfs, `iso.sh` falls back
to them). `ARCH=aarch64 bash os/omakase/build/test-vm.sh <iso> <dir>` picks
AAVMF, `-machine virt`, `ttyAMA0`; without `/dev/kvm` set
`INSTALL_TIMEOUT=3600 BOOT_TIMEOUT=900 WALLPAPER_TIMEOUT=600`.

`OMAKASE_REUSE_ROOTFS=1` skips the base rootfs rebuild only if
`$WORK/rootfs/usr/bin/salt` already exists.

## Known risks / failures

- `ubuntu-24.04-arm` has no writable `/dev/kvm`, so the `build-aarch64` QEMU
  step runs under TCG (~2 h) with a 240 min job timeout; locally the same
  install took ~55 min under TCG on 8 cores.
- `ArchLinuxARM-aarch64-latest.tar.gz` is unpinned upstream; the mirror
  builder records its sha256 in `bootstrap.sha256` and `iso.sh` pins the
  recipe on the ISO to that hash, so a rebuild always matches its own ISO.
- Legacy BIOS is not supported by the aarch64 edition (UEFI only).
- Session-VM artifacts (ISO, serial logs, screendumps) live under
  `/home/ubuntu/omk/` and are not in the repo.
