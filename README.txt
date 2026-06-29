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

  salt sync                         refresh repository index
  salt search <term>                search native packages
  salt install <pkg>                install a native package
  salt update                       snapshot and upgrade host
  salt rollback                     restore previous host deployment
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
  docs/trust-model.md       trust and supply-chain policy
  docs/installation.md      installation notes
  docs/raspberry-pi.md      Raspberry Pi 5 image
  docs/ota.md               over-the-air update server and client
  docs/contributing.md      contribution guide
  docs/CONVENTIONS.md       build and repository conventions

Policy
------

Native packages must declare licenses and pin sources. saltOS avoids telemetry,
unclear licenses, unknown-provenance binaries, and install-time code downloads.
Foreign strata keep their upstream policy boundary visible instead of pretending
to be native packages.
