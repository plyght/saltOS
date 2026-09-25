# saltOS installers (stratum-base model)

## Goal

saltOS is its own system at the core (init, package manager, identity, boot
contract) but maximally interoperable: the bulk of userland comes from whichever
foreign distribution the user chooses *at install time*. The installer ships a
minimal base and lets the user pick the distro to source userland from, instead
of baking one distribution into the image.

Two installers are supported and both are unopinionated: they ask, they do not
assume. Both run the **same native implementation** (`salt-setup`), so a system
installed from the GUI and one installed from the console are identical.

| installer | media | how it runs |
|---|---|---|
| `salt-setup` (text) | console/base ISO (autostarts), desktop ISO ("Install saltOS (text installer)" launcher or any terminal), any SSH session | interactive prompts, or fully non-interactive with `--from system.lua` |
| Calamares (GUI) | desktop live ISO ("Install saltOS" launcher, `saltos-installer`) | Calamares collects the answers, partitions and mounts; its `saltos_setup` job writes `system.lua` and runs `salt-setup --from … --target … --yes` |

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
primary stratum` model. Neither installer deviates from it: Calamares never lays
down a foreign rootfs as `/`, and it never writes fstab, users, GRUB or services
itself — those steps are always `salt-setup`'s.

## Boot contract (kernel ownership)

salt is the **sole authority** over the boot path: `/boot`, the GRUB
configuration, and initramfs generation are always managed by salt. This is the
part that must stay centralized — if more than one component writes the boot
path, boot breaks.

The kernel that fills that contract is a *replaceable, declared input*:

```lua
  kernel = {
    source = "native",          -- default: the saltOS native kernel package
    -- source = "stratum:arch", -- advanced: take the kernel from a chosen stratum
    -- version = "6.12.30",     -- pin a specific native kernel
  },
```

Default is the native kernel. Advanced users may point the kernel slot at a
stratum or pin a version; salt still owns initramfs + GRUB generation and the
choice is pinned in the lockfile. This gives "saltOS owns boot by default" and
"the user can change the kernel" without per-distro boot integration.

## `salt-setup`

`salt-setup` is a native C++23 program in `src/setup/` built on the shared
halite engine — a sibling of `salt`, not a shell script. It reuses salt's
stratum bootstrap and the reproducibility `config apply` code path, so
installing a system and reproducing one from a lockfile are the same code.
Build and CI scripts remain in shell; OS runtime logic does not.

```
salt-setup                          # interactive: every option below is asked
salt-setup --from system.lua        # non-interactive: every answer comes from the file
salt-setup --profile base.lua       # preseed the prompt defaults; every question is still asked
salt-setup --set install.disk=/dev/nvme0n1 --set user.name=alice
salt-setup --from cfg.lua --target /mnt/root --yes    # already partitioned + mounted (Calamares path)
salt-setup --dump-config            # print the effective configuration and exit
```

The answer file is sandboxed Lua that returns a table, evaluated exactly like
recipes and `/etc/salt/system.lua` (no file, command or network access; see
[recipes.md](recipes.md#the-configuration-sandbox)); `salt eval system.lua`
prints what `salt-setup` will read. Option parsing, Lua config
loading/serialisation and validation live in
`src/setup/config.{hpp,cpp}` and are covered by `tests/setup_config_test.cpp`.
Every interactive question has a key in the file, so headless installs over SSH
(`docs/headless-vm-ssh.md`) and preseeded profiles (`--profile`, used by the
opinionated-ISO track) need no prompts.

### Configuration reference

```lua
return {
  system = {
    hostname = "saltos",
    locale   = "en_US.UTF-8",
    timezone = "Europe/Berlin",
    keymap   = "de",               -- console keymap; X/Wayland layout is derived
    -- xkb_layout  = "de",         -- override the derived X layout
    -- xkb_variant = "nodeadkeys",
  },
  install = {
    disk       = "/dev/nvme0n1",   -- target disk (erase / alongside)
    mode       = "erase",          -- erase | alongside | mounted
    filesystem = "btrfs",          -- btrfs (default, snapshots) | ext4
    encrypt    = false,            -- LUKS2 full-disk encryption of the root
    -- passphrase = "...",         -- or passphrase_file = "/run/secret"
    swap       = "none",           -- none | file | partition | zram
    swap_size  = "auto",           -- auto (≈RAM, capped) or e.g. "8G"
    -- swap_device = "/dev/sda3",  -- required for swap = "partition" with mode = "mounted"
    -- root_size = "120G",         -- alongside: size of the new root partition
    desktop    = "auto",           -- auto | keep (what the live media runs) | none
  },
  boot = {
    firmware  = "auto",            -- auto | bios | uefi | both  (x86_64: both installs i386-pc and x86_64-efi GRUB)
    os_prober = true,              -- keep other operating systems in the GRUB menu
    shim      = "auto",            -- auto | yes | no  — Secure Boot shim, see below
    -- cmdline = "console=ttyS0,115200",
  },
  user = {
    name      = "alice",
    password  = "...",             -- or password_hash = "$6$..."
    shell     = "/bin/bash",
    sudo      = true,              -- false requires root_password / root_password_hash
    autologin = false,
    create    = true,
    -- root_password = "...",      -- or root_password_hash
  },
  network = {
    mode = "dhcp",                 -- dhcp | wifi | none
    -- wifi_ssid = "home",         -- mode = "wifi": joins from the live session and persists it
    -- wifi_psk  = "...",
  },
  kernel = {
    source = "native",             -- native | stratum:<name>
  },
  stratum = {
    {
      name = "debian",             -- primary stratum: arch, debian, void, fedora, opensuse, alpine
      role = "primary",
    },
  },
}
```

### Install modes

- **erase** — wipes `install.disk`: GPT with bios-boot (ef02, x86_64) + ESP
  (ef00) + root (8300), optional swap partition, optional LUKS2 on the root.
- **alongside** — requires an existing GPT table and free space; creates the
  saltOS partitions in the unpartitioned area and **never touches existing
  partitions**. Existing installations stay bootable through `os-prober`
  entries in the GRUB menu (`boot.os_prober`).
- **mounted** — `--target <dir>`: the caller has already partitioned, formatted
  and mounted the root (and `/boot/efi`, `/boot`, swap). `salt-setup`
  discovers the layout from the mounts and installs into it. This is the mode
  Calamares uses.

### What happens after the questions

1. Partition/format per the mode above; Btrfs layout via `os/btrfs/layout.sh`
   (`@ @home @var @log @snapshots @strata`) or a plain ext4 root; matching
   fstab (and `crypttab` for LUKS).
2. Lay down the native base (init, `salt`, `halite`, `salt-setup`, stratum
   recipes, firmware) from the live squashfs onto the root.
3. `salt --root "$MNT" stratum add <distro>` bootstraps the chosen distribution
   into `/strata/<distro>` (rootfs / debootstrap / oci per its recipe) and
   auto-exposes its package manager and userland.
4. Kernel per `kernel`, initramfs, GRUB for the detected or requested
   firmware: `i386-pc` for BIOS, `x86_64-efi` / `arm64-efi` for UEFI (installed
   to the removable path `/EFI/BOOT` and, when NVRAM is writable, registered as
   `saltOS`). Serial consoles in `boot.cmdline` enable GRUB's serial terminal.
5. Hostname, locale (`locale.gen` + `locale.conf`), timezone, console keymap +
   X keyboard layout, users, passwords, sudo, autologin, swap (file / partition
   / zram runit service), network (NetworkManager DHCP, persisted Wi-Fi).
6. Write `/etc/salt/system.lua` (intent) and `/etc/salt/system.lock.toml`
   (fully pinned) so the install is reproducible. See `reproducibility.md`.
7. Enable runit services (NetworkManager, chronyd, dbus, seatd, getty; sddm
   only when a desktop is installed) and strip live-only pieces (live user,
   autologin, Calamares configuration, installer launchers).

## Calamares (GUI)

The desktop live ISO ships Calamares configured by
`os/installer/settings-live.conf` and `os/installer/modules-live/*.conf`. The
pages shown are welcome, locale, keyboard, partition (erase / alongside /
replace / manual, Btrfs or ext4, LUKS, swap), primary stratum, desktop, users
and summary. The exec sequence is `partition → mount → saltos_setup → umount`.

`saltos_setup` (`os/installer/modules/saltos_setup/main.py`) is a small Python
job that reads Calamares' global storage (locale, keyboard, users, partitions,
the chosen stratum and desktop, the detected firmware) and translates it into a
`system.lua` under `/run/saltos-installer/`. It then runs

```
salt-setup --from /run/saltos-installer/system.lua --target <rootMountPoint> --yes
```

streaming its output into the Calamares log and mapping each `==> ` step to the
progress bar. Anything Calamares does not ask (kernel source, extra cmdline) is
taken from `saltos_setup.conf`. Because the GUI only handles collection,
partitioning and mounting, every installation rule (native root, primary
stratum, boot contract, users, services) is implemented exactly once.

The live session also exposes `salt-setup` in a terminal (`Install saltOS
(text installer)` launcher, `saltos-setup-terminal`) for users who prefer the
text path on desktop media.

## Hardware and firmware coverage

| target | firmware | media / image | CI |
|---|---|---|---|
| x86_64 | legacy BIOS | installer/live ISO (isohybrid) | `installer-iso.yml` text install + reboot |
| x86_64 | UEFI | installer/live ISO, generic VM image | `installer-iso.yml` text + Calamares install + reboot, `vm-image-x86.yml` boot |
| aarch64 | UEFI | installer/live ISO, generic VM image (incl. Apple Silicon via `os/build/vm-apple.sh`) | `installer-iso-arm64.yml` text install + reboot, `live-iso-arm64.yml`, `vm-image-arm64.yml` |
| Raspberry Pi 5 | Pi firmware (`config.txt`) | `pi5-image.yml` | image built, boot partition mounted and inspected |
| ThinkPad (x86_64) | UEFI | `thinkpad-image.yml` | live boot, GPT install, installed UEFI boot, serial + screenshot |

Every QEMU gate asserts the runit markers on serial (`SALTOS_BOOT_OK`,
`SALTOS_STRATUM_OK`) and, for installs, that the installed disk boots after the
media is detached. The shared helpers are `os/iso/qemu-install-test.sh` and
`os/iso/qemu-boot-test.sh`; both pick KVM only when `/dev/kvm` is writable and
the guest matches the host and otherwise use multi-threaded TCG explicitly.

### Secure Boot

The ESP layout is shim-ready: GRUB is installed under `/boot/efi/EFI/BOOT` and
`/boot/efi/EFI/saltOS`, and when the target carries a distribution-signed shim
and signed GRUB (`/usr/lib/shim/shim*.efi.signed`, `grub/*-efi-signed`) they
are used (`boot.shim = "auto"|"yes"`). saltOS does **not** ship its own signed
shim and does not enroll keys; on a machine with Secure Boot enforced you must
either disable enforcement in firmware, enroll your own keys, or boot through a
signed shim from the primary stratum. `boot.shim = "yes"` fails the install if
no signed chain is present rather than pretending.

## Desktop

`install.desktop = "auto"` and `"keep"` carry over whatever the live medium
runs (LXQt + SDDM on the desktop ISO, nothing on the console ISO); `"none"`
installs a console system even from desktop media. Further desktops are added
afterwards through the primary stratum (`salt install <distro>/<desktop>`) and
their display manager is exposed from there, not from the native base.

## Reproducibility

The installer's output is captured as `system.lua` + `system.lock.toml`. On
another machine, `salt config apply system.lock.toml` reproduces the same base
distribution snapshot, the same exposed userland, and the same kernel/boot
contract. The native plane targets source-level reproducibility; the stratum
plane targets content-pinned reinstall (foreign package managers are not rebuilt
under our control). See `reproducibility.md` for the per-package-manager detail.
