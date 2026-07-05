# saltOS installer (stratum-base model)

## Goal

saltOS is its own system at the core (init, package manager, identity, boot
contract) but maximally interoperable: the bulk of userland comes from whichever
foreign distribution the user chooses *at install time*. The installer ships a
minimal base and lets the user pick the distro to source userland from, instead
of baking one distribution into the image.

This replaces the Calamares/Debian-clone installer with a text-mode (TUI)
installer that runs in the bare boot environment and drives `salt`.

## What saltOS owns vs. what the chosen distro provides

```
/                native saltOS root: runit init, salt + halite,
                 the boot contract, btrfs layout, identity, cross-stratum glue
/strata/<distro> chosen distribution userland (apps, libraries, optional DE),
                 auto-exposed onto PATH via the shim layer
/strata/<other>  additional strata the user adds later
```

The native root is small and self-contained. The chosen distribution is
installed as the **primary stratum** under `/strata/<name>` and auto-exposed, so
its userland "just works" on `PATH`. This is the `Native root + distro as
primary stratum` model.

## Boot contract (kernel ownership)

salt is the **sole authority** over the boot path: `/boot`, the GRUB
configuration, and initramfs generation are always managed by salt. This is the
part that must stay centralized — if more than one component writes the boot
path, boot breaks.

The kernel that fills that contract is a *replaceable, declared input*:

```toml
[kernel]
source = "native"        # default: the saltOS native kernel package
# source = "stratum:arch" # advanced: take the kernel from a chosen stratum
# version = "6.12.30"     # pin a specific native kernel
```

Default is the native kernel. Advanced users may point the kernel slot at a
stratum or pin a version; salt still owns initramfs + GRUB generation and the
choice is pinned in the lockfile. This gives "saltOS owns boot by default" and
"the user can change the kernel" without per-distro boot integration.

## Install flow

The installer is `salt-setup`, a native C++23 program in `src/setup/` built on
the shared halite engine — a sibling of `salt`, not a shell script. It reuses
salt's stratum bootstrap and the reproducibility `config apply` code path, so
installing a system and reproducing one from a lockfile are the same code.
Build and CI scripts remain in shell; OS runtime logic does not.

`salt-setup` runs interactively (prompts on the console) or non-interactively
from a config: `salt-setup --from system.toml`.

1. Target disk — `lsblk` menu (the disk is erased).
2. Base distribution — radiolist built from the registered `strata/*.toml`
   (arch, debian, void, fedora, opensuse, alpine).
3. Identity — hostname, username, password, timezone, locale.
4. Kernel — native (default) or advanced override.
5. Confirm erase.
6. Partition: GPT bios-boot (ef02) + ESP (ef00) + root (8300).
7. Btrfs layout via `os/btrfs/layout.sh` (`@ @home @var @log @snapshots
   @strata`) and matching fstab.
8. Lay down the native base onto `@`.
9. `salt --root "$MNT" stratum add <distro>` bootstraps the chosen distribution
   into `/strata/<distro>` (rootfs / debootstrap / oci per its recipe) and
   auto-exposes its package manager and userland.
10. Install the kernel, regenerate initramfs, install + configure GRUB for the
    detected firmware (BIOS i386-pc and/or x86_64-efi / arm64-efi).
11. Write `/etc/salt/system.toml` (intent) and `/etc/salt/system.lock.toml`
    (fully pinned) so the install is reproducible. See `reproducibility.md`.
12. Enable runit services (NetworkManager, chronyd, dbus, seatd, getty; sddm
    only when a desktop is requested).

## Desktop

The base install is console + the chosen distribution's userland. A desktop is
an explicit follow-up driven through the chosen stratum, e.g.
`salt install <distro>/lxqt` plus exposing the display manager. The display
manager comes from the stratum, not the native base. This is phase 2.

## Reproducibility

The installer's output is captured as `system.toml` + `system.lock.toml`. On
another machine, `salt config apply system.lock.toml` reproduces the same base
distribution snapshot, the same exposed userland, and the same kernel/boot
contract. The native plane targets source-level reproducibility; the stratum
plane targets content-pinned reinstall (foreign package managers are not rebuilt
under our control). See `reproducibility.md` for the per-package-manager detail.

## Migration from Calamares

The Calamares GUI flow (`settings-live.conf`, `modules/`, `modules-live/`) and
the Debian-clone `saltos-install.sh` are superseded by `salt-setup`. The live
image gains a `base` edition (no desktop, no Calamares) that autostarts
`salt-setup` on the console. The Calamares assets remain in-tree until the
native path is validated on real hardware, then can be removed.

## Phasing

- Phase 1 (this change): `salt-setup` laying down the native base + bootstrapping
  the chosen stratum + boot contract; reproducible config/lock written; `base`
  live edition that autostarts it.
- Phase 2: desktop-from-stratum exposure and display-manager wiring.
- Phase 3: native base fully sourced from self-built `.grain` packages rather
  than a minimal seed.
