saltOS
======

saltOS is an experimental independent Linux distribution with its own boot,
base system, init, package manager, and rollback model. It can also run software
from major Linux ecosystems through managed, rollbackable environments called
strata.

Status: under active construction. The model is proven in CI, but saltOS is not
yet a polished daily-driver OS.

How it works
------------

saltOS owns the host system: kernel, libc, init, bootloader integration, system
tools, and native packages in the .grain format.

Strata provide package depth. A stratum is a managed foreign-distro root, such
as Arch, Debian, Void, Fedora, openSUSE, or Alpine. Each stratum keeps its own
package manager and filesystem isolated by default. Commands, desktop apps, and
services are exposed to the host only when selected.

The salt tool installs native packages, bootstraps strata, runs foreign software,
exposes commands or desktop apps, manages providers, and rolls back host or
stratum changes.

What works today
----------------

  Native package flow: build, lint, sign, publish, sync, install, verify, remove,
  and rollback.

  Strata flow across apk, xbps, pacman, apt, dnf, and zypper: bootstrap, run,
  install, snapshot, rollback, expose, and remove.

  QEMU-booted live ISO smoke tests with networking and a stratum package install.

  Self-hosted from-source ISO path for Linux, glibc, bash/coreutils, BusyBox,
  runit, and static salt.

Editions
--------

  base      console live ISO with salt-setup; installs the native core plus a
            chosen primary stratum (os/iso/live-build.sh, EDITION=base)
  desktop   unopinionated graphical live ISO (os/iso/live-build.sh,
            EDITION=desktop)
  omakase   opinionated Sway desktop: gum wizard on the ISO, offline Arch
            mirror, themes, saltos-theme/saltos-menu/saltos-update, curated
            apps from the chosen stratum (os/omakase/, docs/omakase.md)

Architecture
------------

  Kernel           upstream Linux
  C library        glibc
  Init             runit
  Filesystem       Btrfs subvolumes and snapshots
  Bootloader       GRUB
  Package manager  salt CLI + halite C core
  Package format   .grain
  Repository       signed per-architecture indexes
  Database         SQLite
  Architectures    x86_64 and aarch64

Build
-----

Requires Linux, CMake 3.20+, Ninja, pkg-config, a C/C++ toolchain, libzstd,
libsodium, and sqlite3.

  cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build
  ctest --test-dir build --output-on-failure

The main binaries are produced at build/src/salt/salt and
build/src/setup/salt-setup.

Quick commands
--------------

  salt sync                         refresh and verify repository index
  salt search <term>                search native package names and summaries
  salt install <pkg>                install a native package and its dependencies
  salt remove <pkg> [--cascade]     remove a package (and dependents with --cascade)
  salt update [--download-only]     snapshot and upgrade host
  salt rollback                     restore previous host deployment
  salt history                      list transactions and deployments
  salt info <pkg>                   show package details
  salt files <pkg>                  list files owned by a package
  salt owner <path>                 show which package owns a path
  salt list [--upgradable]          list installed or upgradable packages
  salt verify [pkg]                 compare installed files with their manifest
  salt lock                         pin every installed package to a lockfile
  salt lock apply [file]            reproduce a lockfile exactly
  salt lock diff                    compare the system with the lockfile
  salt clean                        remove downloaded package artifacts
  salt gc [--keep N] [--dry-run]    prune old generations and unreferenced artifacts
  salt-ota run                      snapshot, sync, update, rollback on failure
  salt-ota status                   show OTA and rollback state

  salt stratum add arch             bootstrap a stratum
  salt run arch firefox             run foreign software
  salt install arch/ripgrep         install from a stratum
  salt expose arch rg               expose a host shim
  salt pkg arch install firefox     use a stratum package manager
  salt stratum rollback arch        roll back one stratum

  salt build recipes/<name>         build a .grain package
  salt lint recipes/<name>          lint a recipe
  salt sign <pkg>                   sign a package or index
  salt repo publish <dir>           publish a signed repository index
  salt trust scan recipes/<name>    scan supply-chain risk

Repository layout
-----------------

  src/halite/   C core library for packages, repositories, transactions, trust,
                rollback, and strata
  src/salt/     C++23 salt CLI
  src/setup/    installer/setup binary
  recipes/      native package recipes
  repo/         package repository tree
  strata/       foreign-distro stratum definitions
  os/           OS integration, ISO, installer, Btrfs, and runit files
  tests/        unit and CLI smoke tests
  docs/         detailed design and contributor documentation

Documentation
-------------

  docs/architecture.md      system architecture
  docs/package-manager.md   native package manager
  docs/strata.md            stratum model
  docs/rollback.md          rollback design
  docs/recipes.md           package recipes
  docs/repository.md        repository format
  docs/reproducibility.md   lockfiles, config apply, generations and gc
  docs/trust-model.md       trust and supply-chain policy
  docs/installation.md      installation notes
  docs/omakase.md           opinionated Sway edition and unattended installs
  docs/raspberry-pi.md      Raspberry Pi 5 image
  docs/thinkpad.md          ThinkPad P40 Yoga image
  docs/ota.md               over-the-air update server and client
  docs/contributing.md      contribution guide
  docs/CONVENTIONS.md       build and repository conventions

Policy
------

Native packages must declare licenses and pin sources. saltOS avoids telemetry,
unclear licenses, unknown-provenance binaries, and install-time code downloads.
Foreign strata keep their upstream policy boundary visible instead of pretending
to be native packages.
