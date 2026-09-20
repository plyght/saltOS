# saltOS omakase edition

The omakase edition is the opinionated saltOS ISO: an Omarchy-style, fully
curated Sway desktop on top of saltOS's native core (runit, `salt`, `halite`,
Btrfs snapshots, GRUB) plus one **stratum** of your choice that supplies every
desktop-level package. saltOS owns boot, init, rollback, and system identity;
the stratum's package manager owns Sway, foot, Waybar, Neovim, fonts, and the
apps. Nothing desktop-level is packaged as a native `.grain`.

Everything lives in `os/omakase/`:

```txt
os/omakase/
  build/      arch-mirror.sh, vendor.sh, wallpapers.sh, iso.sh, test-vm.sh, verify-packages.sh
  lib/        omakase.sh (shared TOML/state helpers)
  live/       ISO-side: greeter, configurator, cidata loader, dashboard, installer
  packages/   packages.tsv, one column per stratum
  target/     installed-side: bin/saltos-*, config/, templates/
  themes/     <name>/theme.sh palettes
  wallpapers/ <name>.toml Unsplash manifests (photo id, url, photographer, sha256)
```

## Install flow

1. **Boot the ISO.** tty1 shows a centered logo on a Tokyo Night VT palette and
   `Press Return to Start Install`. Ctrl+C here offers *prepare this machine for
   another owner* (deferred provisioning: install now, ask identity, keyboard
   and password at first boot).
2. **Configurator** (`saltos-configurator`, a gum wizard; finishable in well
   under a minute):
   keyboard layout → username → password → full name (optional) → email
   (optional) → hostname → timezone → **base distribution** (arch, debian,
   fedora, void, alpine, opensuse; Arch is preselected) → confirmation table
   → disk → *full disk* or *free space (dual boot)* → **encryption toggle,
   default OFF** → final confirm.
   The wizard writes `/run/saltos-install/install.toml` and a `credentials`
   file (mode 0600).
3. **Install** (`saltos-install-dashboard` in the foreground, `saltos-install`
   underneath as a phased state machine writing `state.json`):
   preflight → base → mount → desktop → apps → host → user → finish.
   *base* turns the wizard answers into a `salt-setup` configuration
   (`install.mode = "erase"` or `"alongside"`, `install.encrypt`,
   `user.password`, `boot.cmdline`) and runs `salt-setup --from` on it, so
   partitioning, the `@ @home @var @log @snapshots @strata` Btrfs layout, LUKS2,
   the native core, the user account, the stratum bootstrap, kernel, initramfs
   and GRUB are the same implementation the text and Calamares installers use.
   *desktop* and *apps* install the curated set through the stratum's package
   manager and drop in the omakase configs, themes, and tools. Stratum shims
   that would shadow a native host command (`cat`, `bash`, `sudo`, ...) are
   unexposed again so the host userland stays authoritative; only stratum-only
   commands (`sway`, `foot`, `grim`, ...) remain on `PATH`.
   *host* enables `udevd`, `dbus`, `seatd`, `NetworkManager`, `bluetoothd` and
   the session service under `/etc/runit/runsvdir/current`, locks the `root`
   account (the user is in `sudo`/`wheel`), and removes the live installer.
4. **Reboot.** First boot autologs the user into Sway on tty1 through a runit
   service (`saltos-session`); no display manager. The session is started by
   `saltos-session-launch` (root via a passwordless sudoers entry): it enters
   the stratum with `salt run` and drops to the user with `setpriv`, keeping
   the real uid and the host's supplementary groups (`seat`, `audio`, `video`,
   `input`, ...). Because the uid is real rather than a user-namespace
   mapping, the stratum's setuid `sudo` works in the terminal, `sudo salt ...`
   escapes to the host as usual, and `saltos-host ota run` (used by
   `saltos-update`) runs `salt-ota` on the host from inside the stratum.

### Offline vs online

The ISO carries a full offline Arch mirror (`build/arch-mirror.sh` downloads
`base` plus every package in the `arch` column of `packages.tsv`, plus the
bootstrap tarball, and builds an `offline.db` pacman repository). An Arch
install therefore needs no network and takes a few minutes in a VM. The other
five strata are bootstrapped and populated online with their own package
managers; the wizard warns when it cannot see a default route.

Vicinae, Helium, and gum are not packaged by every stratum; `build/vendor.sh`
pins their upstream releases with SHA-256 checksums and the installer places
them under `/opt/saltos/vendor` with wrappers on `PATH`, running against the
stratum's runtime libraries (`helium-runtime` / `vicinae-runtime` rows).
Helium's tarball is additionally checked against its detached `.asc`
signature with the pinned upstream signing key
(`build/helium-signing-key.asc`); Vicinae's AppImage is unpacked at build
time with `unsquashfs`, so nothing needs FUSE at runtime.

### aarch64 edition

`iso.sh aarch64` produces `saltos-omakase-aarch64.iso`, a UEFI-only image for
64-bit ARM machines and VMs (`qemu-system-aarch64 -machine virt` under
AAVMF/`QEMU_EFI`). It is the same live system and installer built from the
arm64 Debian base; the differences are all in what gets staged:

- `build/vendor.sh aarch64` pins `Vicinae-aarch64.AppImage`,
  `helium-<ver>-arm64_linux.tar.xz` and `gum_<ver>_Linux_arm64.tar.gz`.
- The default stratum is **Arch Linux ARM**: `strata/arch-aarch64.toml`
  (picked over `arch.toml` whenever an `<name>-<arch>.toml` recipe exists)
  bootstraps from `ArchLinuxARM-aarch64-latest.tar.gz` and pulls from the
  `core`/`extra`/`alarm`/`aur` ALARM repositories. `build/arch-mirror.sh
  aarch64` imports that rootfs as a Docker image and runs its own `pacman` to
  build the offline mirror, so the ISO installs Arch fully offline on ARM too.
- The serial console is `ttyAMA0` (installer, dashboard markers and the Sway
  session marker all follow `uname -m`).

Stratum support on aarch64, checked with `VERIFY_ARCH=aarch64
build/verify-packages.sh` against each distro's arm64 repositories:

| Stratum  | aarch64 | Notes                                                        |
|----------|---------|--------------------------------------------------------------|
| arch     | yes     | Arch Linux ARM, offline by default; every curated package (incl. `bluetui`, `lazygit`, `satty`, `cliphist`) resolves |
| debian   | yes     | same package map as x86_64                                   |
| fedora   | yes     | same package map as x86_64                                   |
| alpine   | yes     | same package map as x86_64                                   |
| opensuse | yes     | same package map as x86_64                                   |
| void     | yes     | same package map as x86_64                                   |

All six resolve with 0 missing packages (the per-distro gaps listed under
[Package maps](#package-maps) are the same on both architectures). The
unattended `erase` install has been run end to end on aarch64
(`ARCH=aarch64 build/test-vm.sh` under AAVMF, TCG on an x86_64 host: ~42 min
install, then `SALTOS_SWAY_SESSION_OK`, theme switch, wallpaper cycle).

Not supported / not tested on aarch64: legacy BIOS boot (the ISO is
UEFI-only); the `encrypt`, `alongside` and `interactive` harness modes only
run in the x86_64 CI matrix (the installer code path is identical, but there
is no Windows boot manager to preserve on the ARM VMs and the TCG runtime of
three more installs is prohibitive without KVM).

## Unattended installs (`cidata`)

Attach a second block device with filesystem label `cidata` (or `CIDATA`)
containing:

- `install.toml` — the same file the configurator writes. Minimal example:

  ```toml
  [system]
  hostname = "saltos-omakase"
  locale = "en_US.UTF-8"
  timezone = "UTC"
  keymap = "us"

  [kernel]
  source = "native"

  [[stratum]]
  name = "arch"
  role = "primary"
  expose = true

  [user]
  username = "salt"
  full_name = "saltOS Tester"
  email = ""
  deferred = false

  [install]
  profile = "omakase"
  disk = "/dev/vda"
  mode = "disk"          # or "free" for dual boot into unallocated space
  encrypt = false
  serial_console = true  # optional: autologin getty on ttyS0 (ttyAMA0 on aarch64) + GRUB serial
  ```

- `credentials` — `password=<plain text>` (mode 0600 on the target; omit when
  `deferred = true`). With `encrypt = true` an optional `passphrase=<text>`
  line sets the LUKS passphrase; it defaults to the account password.

`saltos-cidata-load` mounts it read-only, validates disk/stratum/credentials,
applies the keymap, and the greeter skips straight to the dashboard. On
success the ISO prints `SALTOS_OMAKASE_INSTALL_OK` on the serial console and
reboots; on failure `SALTOS_OMAKASE_INSTALL_FAIL <reason>`.

`build/test-vm.sh <iso>` does exactly this under QEMU/OVMF: creates the target
disk and `cidata` image, installs, boots the installed disk, waits for
`SALTOS_SWAY_SESSION_OK`, runs `saltos-theme set gruvbox` over serial, checks
that `swaybg` is showing the theme's Unsplash photo and that `saltos-wallpaper
next` moves it to the second one (the `wallpaper.png` screendump must contain
thousands of distinct colours, not a solid fill), lists runit services, and
grabs a `screendump`. `OMAKASE_TEST_MODE` selects the path: `erase` (default),
`encrypt` (cidata with `encrypt = true`; answers the LUKS prompt at boot, then
installs a throwaway grain, runs `salt rollback` and reboots to prove the
rolled-back root unlocks and boots), `alongside`
(free-space install next to a fake Windows ESP + NTFS layout; asserts the
existing partitions are byte-identical afterwards and GRUB lists `Windows Boot
Manager`), and `interactive` (no cidata; drives the gum configurator over the
serial console, toggling encryption on). `.github/workflows/omakase-iso.yml`
runs `erase` in the build job and the other three as a matrix on the built ISO
on every push touching `os/omakase/`. A separate `build-aarch64` job on an
`ubuntu-24.04-arm` runner builds the ARM ISO and runs the `erase` path under
`qemu-system-aarch64` (KVM when `/dev/kvm` is writable, otherwise TCG with
`INSTALL_TIMEOUT`/`BOOT_TIMEOUT` raised accordingly; `ARCH=aarch64
build/test-vm.sh` picks AAVMF, `virtio-gpu-pci` and a `virtio-scsi` CD-ROM).

## Desktop

Sway with Omarchy-style bindings (`$mod` = Super). Waybar, mako, swaylock,
swayidle, swaybg, xdg-desktop-portal-wlr/gtk, mate-polkit, PipeWire +
WirePlumber.

| Keys                    | Action                                   |
|-------------------------|------------------------------------------|
| Super+Return            | terminal (foot)                          |
| Super+Space             | launcher (vicinae)                       |
| Super+B                 | browser (Helium)                         |
| Super+F                 | file manager (Thunar)                    |
| Super+N                 | Neovim                                   |
| Super+T                 | btop                                     |
| Super+E                 | `saltos-menu`                            |
| Super+Escape            | power menu (`saltos-power`)              |
| Super+Ctrl+Escape       | lock                                     |
| Super+Ctrl+Space        | next theme                               |
| Super+Ctrl+Shift+Space  | next wallpaper                           |
| Super+V                 | clipboard history (cliphist)             |
| Super+Shift+N / B       | Wi-Fi (nmtui) / Bluetooth (bluetui)      |
| Print / Shift / Ctrl    | screenshot region / output / window      |
| Super+W                 | close window                             |
| Super+H/J/K/L, arrows   | focus; +Shift moves                      |
| Super+1..0, +Shift      | workspace; move to workspace             |
| Super+Tab / Shift+Tab   | next / previous workspace                |
| Super+Shift+V / S       | split vertical / horizontal              |
| Super+Shift+F           | fullscreen                               |
| Super+Shift+Space       | float                                    |
| Super+Shift+T           | toggle tabbed                            |
| Super+Minus, +Shift     | scratchpad show / move                   |
| Super+R                 | resize mode                              |
| Super+Shift+C           | reload Sway                              |
| XF86 media/brightness   | pamixer, brightnessctl, playerctl        |

Curated apps: foot, Helium, vicinae, Neovim (lazy.nvim, LSP, treesitter,
telescope, fzf), Thunar, imv, mpv, zathura, grim/slurp/satty, wl-clipboard,
cliphist, brightnessctl, playerctl, pamixer, pavucontrol, btop, fastfetch,
NetworkManager (`nmtui`), bluetui/bluez. No web-app or PWA shortcuts.
Vicinae's system-info telemetry is switched off in the shipped
`~/.config/vicinae/settings.json`.

## Themes

`tokyo-night` (default), `catppuccin`, `gruvbox`, `nord`, `everforest`,
`kanagawa`, `rose-pine`. Each is a `themes/<name>/theme.sh` palette plus 2-3
colour-matched wallpapers; `saltos-theme` renders the templates under
`target/templates/` into `~/.config/saltos/theme/` for Sway, foot, Waybar,
mako, swaylock, Neovim, GTK 3/4 (`gsettings` + `gtk.css`), the cursor theme,
and `swaybg`.

```sh
saltos-theme list
saltos-theme current
saltos-theme set gruvbox      # works headlessly; live-reloads when Sway is up
saltos-theme next
saltos-wallpaper list         # the active theme's photos
saltos-wallpaper next         # cycle swaybg through them (Super+Ctrl+Shift+Space)
saltos-wallpaper credits
```

### Wallpapers

The wallpapers are real photos from Unsplash -- salt flats, salt and mineral
crystals, salt lakes, ice, nebulae, Hokusai -- picked to match each palette.
They are **not** committed to the repository: `os/omakase/wallpapers/<theme>.toml`
lists, per photo, the Unsplash id, title, photographer and profile URL, the
photo page, the download URL and its SHA-256. `build/wallpapers.sh` downloads
them at ISO build time (cached under `$WALLPAPER_CACHE`), verifies every hash
and fails the build on any mismatch, then writes a `CREDITS` file. The
installer ships the set to `/usr/share/saltos/wallpapers/<theme>/NN-<id>.jpg`
(a symlink to `/opt/saltos/share/wallpapers` so the same path resolves on the
host and inside the stratum) together with `CREDITS`. All photos are under
the Unsplash License (no Unsplash+ images); `saltos-wallpaper credits` prints
the attributions.

`saltos-theme` picks the theme's first photo unless `saltos-wallpaper set|next`
recorded a choice in `~/.config/saltos/wallpaper/<theme>`; the choice is
remembered per theme. A theme directory containing `background.png|jpg`
still overrides the Unsplash set, and a theme with neither falls back to a
solid `$BG` fill.

## Tools

- `saltos-menu` — gum TUI: Theme, Wallpaper, Update, Install (search/install/remove
  stratum packages, add another stratum), Setup (Wi-Fi, Bluetooth, audio,
  keyboard, password), System (about, stratum snapshots, keybindings, power).
- `saltos-update [all|host|stratum|neovim]` — `salt-ota run` (falls back to
  `salt sync && salt update`), then the stratum's package manager upgrade, then
  Neovim plugins.
- `saltos-power`, `saltos-screenshot`, `saltos-firstboot` (deferred
  provisioning), `saltos-provision-home`.

## Package maps

`packages/packages.tsv` has one row per role and one column per stratum; `-`
marks a gap. `build/verify-packages.sh [tsv] [distro]` resolves every name
against the distro's official repositories in Docker and prints a per-distro
`ok`/`missing` list. Known gaps at the time of writing:

| Role            | Missing in                    | Consequence / workaround                              |
|-----------------|-------------------------------|-------------------------------------------------------|
| `bluetui`       | debian, fedora, void, alpine, opensuse | `bluetoothctl` from bluez; Super+Shift+B opens it |
| `satty`         | debian, fedora, alpine, opensuse | screenshots are saved without the annotation step    |
| `cliphist`      | fedora, alpine, opensuse      | Super+V is disabled; `wl-clipboard` still works       |
| `lazygit`       | all but arch                  | omitted                                               |
| `vicinae`, `helium`, `gum` | all                | vendored upstream releases (`build/vendor.sh`)        |

Arch is the complete column and the default selection.

## How it differs from Omarchy

- **Sway** (wlroots) instead of Hyprland; no Quickshell; Waybar + mako.
- **Stratum choice**: the wizard's *base distribution* step decides which
  package manager provides the desktop. Omarchy is Arch-only.
- **Encryption is off by default** and toggled explicitly in the wizard
  instead of Ctrl+C on the format confirmation.
- **Native core**: runit, Btrfs snapshots + rollback, `salt`/`halite`, GRUB;
  `saltos-update` wraps `salt-ota` before the stratum's upgrade.
- **No web-app shortcuts**; Helium instead of Chromium; vicinae instead of
  Walker; foot instead of Alacritty.
- Same greeter → gum wizard → dashboard → reboot flow, same deferred
  provisioning and `cidata` unattended path, same offline default install.
