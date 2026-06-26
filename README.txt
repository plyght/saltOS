saltOS
======

saltOS is an experimental independent Linux distribution with its own boot,
base system, init, package manager, and rollback model. It can also run software
from major Linux ecosystems through managed, rollbackable environments called
strata.

Status: under active construction. The model is proven in CI, but saltOS is not
yet a polished daily-driver OS.

Core model
----------

saltOS has two package planes:

- Native plane: saltOS-owned `.grain` packages built by `salt`, installed from a
  curated signed repository, and used for the base system.
- Stratum plane: managed Arch, Debian, Void, Fedora, openSUSE, Alpine, and other
  foreign-distro roots that keep their own package managers isolated until you
  explicitly expose software to the host.

`salt` is the control plane: it installs native packages, bootstraps strata,
runs foreign software, exposes commands or desktop apps, manages providers, and
rolls back host or stratum changes.

What works today
----------------

- Native package flow: build, lint, sign, publish, sync, install, verify, remove,
  and rollback.
- Strata flow across apk, xbps, pacman, apt, dnf, and zypper: bootstrap, run,
  install, snapshot, rollback, expose, and remove.
- QEMU-booted live ISO smoke tests with networking and a stratum package install.
- Self-hosted from-source ISO path for Linux, glibc, bash/coreutils, BusyBox,
  runit, and static `salt`.

Architecture
------------

- Kernel: upstream Linux
- C library: glibc
- Init: runit
- Filesystem: Btrfs subvolumes and snapshots
- Bootloader: GRUB
- Package manager: `salt` CLI + `halite` C core
- Package format: `.grain`
- Repository: signed per-architecture indexes
- Database: SQLite
- Architectures: x86_64 and aarch64

Build
-----

Requires Linux, CMake 3.20+, Ninja, pkg-config, a C/C++ toolchain, libzstd,
libsodium, and sqlite3.

```sh
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The main binaries are produced at `build/src/salt/salt` and
`build/src/setup/salt-setup`.

Quick commands
--------------

```sh
salt sync                         # refresh repository index
salt search <term>                # search native packages
salt install <pkg>                # install a native package
salt update                       # snapshot and upgrade host
salt rollback                     # restore previous host deployment

salt stratum add arch             # bootstrap a stratum
salt run arch firefox             # run foreign software
salt install arch/ripgrep         # install from a stratum
salt expose arch rg               # expose a host shim
salt pkg arch install firefox     # use a stratum package manager
salt stratum rollback arch        # roll back one stratum

salt build recipes/<name>         # build a .grain package
salt lint recipes/<name>          # lint a recipe
salt sign <pkg>                   # sign a package or index
salt repo publish <dir>           # publish a signed repository index
salt trust scan recipes/<name>    # scan supply-chain risk
```

Repository layout
-----------------

- `src/halite/` - C core library for packages, repositories, transactions, trust,
  rollback, and strata.
- `src/salt/` - C++23 `salt` CLI.
- `src/setup/` - installer/setup binary.
- `recipes/` - native package recipes.
- `repo/` - package repository tree.
- `strata/` - foreign-distro stratum definitions.
- `os/` - OS integration, ISO, installer, Btrfs, and runit files.
- `tests/` - unit and CLI smoke tests.
- `docs/` - detailed design and contributor documentation.

Documentation
-------------

- `docs/architecture.md` - system architecture
- `docs/package-manager.md` - native package manager
- `docs/strata.md` - stratum model
- `docs/rollback.md` - rollback design
- `docs/recipes.md` - package recipes
- `docs/repository.md` - repository format
- `docs/trust-model.md` - trust and supply-chain policy
- `docs/installation.md` - installation notes
- `docs/contributing.md` - contribution guide
- `docs/CONVENTIONS.md` - build and repository conventions

Policy
------

Native packages must declare licenses and pin sources. saltOS avoids telemetry,
unclear licenses, unknown-provenance binaries, and install-time code downloads.
Foreign strata keep their upstream policy boundary visible instead of pretending
to be native packages.
