# saltOS Distro Design Document

## 1. Product Intent

saltOS is an independent Linux distribution and multi-environment operating system.

It should be daily-drivable as its own OS, not a layer bootstrapped onto another distribution. It owns the boot process, base system, init, rollback model, installer, system identity, default experience, and native package repository.

At the same time, saltOS should not require its maintainer to repackage the entire Linux world. It should be able to use software from other Linux ecosystems through managed, isolated, rollbackable environments called strata.

The guiding idea:

```txt
saltOS is the host OS and control plane.
Native salt packages provide the curated base and first-party system.
Foreign distro strata provide package depth and interchangeable components.
salt decides what is integrated, exposed, adopted, or isolated.
```

saltOS should feel like:

- a real independent Unix-like desktop OS
- current without being reckless
- rollbackable without being complicated
- capable of using many Linux package ecosystems
- transparent when desired, explicit by default
- curated at the base, flexible at the edges
- understandable to one technical maintainer

## 2. Core Identity

saltOS is not just another package manager project and not just a Bedrock-style integration layer.

It is an independent operating system with two package planes:

1. **Native plane**
   - saltOS-owned packages
   - curated official repository
   - base system, kernel, init, bootloader integration, system tools, desktop defaults, and salt-specific integration packages

2. **Stratum plane**
   - managed foreign distro roots
   - package managers such as `apt`, `pacman`, `dnf`, `zypper`, `xbps`, `apk`, and Portage
   - isolated or selectively integrated software from other Linux ecosystems

The native plane makes saltOS independent.

The stratum plane makes saltOS practical.

The integration plane makes saltOS powerful: users should be able to mix components that normally belong to incompatible distributions while saltOS keeps ownership, rollback, routing, and conflict policy coherent.

Examples saltOS should eventually support:

```txt
Debian stable coreutils
Arch kernel
Void-style runit service stack
Gentoo-maintained patched application
Arch AUR font inside an explicitly lower-trust stratum
Games running against Ubuntu libraries
Business software running against Enterprise Linux libraries
Native saltOS rollback and command policy around all of it
```

This interchangeability should be designed from the start, not bolted on later.

## 3. Non-Goals

saltOS will not initially attempt to:

- repackage all of Debian, Arch, Fedora, Alpine, Void, Gentoo, or openSUSE
- become source-only like Gentoo
- become NixOS
- require users to learn a declarative language before using the OS
- let foreign package managers mutate the saltOS base system directly without saltOS adoption policy
- expose every foreign package globally by default
- use an AUR-like untrusted repository as the main software story
- invent a custom kernel, libc, compiler, desktop shell, or init system
- hide trust boundaries from the user
- pretend that packages from different distro ecosystems have equal security or compatibility guarantees

## 4. Target Users

Primary target:

- the creator as the first daily-driver user

Secondary target:

- developers, power users, and technical desktop users who want an independent Linux system with rollback, curated base packages, and access to broader Linux package ecosystems without reinstalling or distro-hopping

Tertiary target:

- users who like Bedrock Linux’s flexibility but want the host OS itself to be purpose-built for that model rather than retrofitted onto another distribution

## 5. Core Principles

### 5.1 Independent host, not bootstrap layer

saltOS must boot as saltOS.

It should have its own installer, base image, kernel policy, init setup, filesystem layout, rollback system, package repository, release channels, and support expectations.

Foreign distro roots are guests managed by saltOS. They are not the foundation of the machine.

However, saltOS should be able to deliberately adopt specific components from strata into the host role when the user chooses. The difference is ownership: a foreign package manager may provide the component, but saltOS records, exposes, snapshots, and can roll back the decision.

### 5.2 Native packages for the core

saltOS should keep its own package manager and package format for the parts of the system it must control tightly:

- base system
- kernel and firmware integration
- init and service supervision
- bootloader integration
- rollback tooling
- stratum manager
- desktop defaults
- system policy
- saltOS-specific glue packages

The native package repository should be curated, signed, and intentionally smaller than a general-purpose mega-distro repository.

### 5.3 Interchangeability from the start

saltOS should be designed around component interchangeability from v0.

The system should assume that different parts of the working environment may come from different providers:

- native saltOS package
- Debian stratum
- Arch stratum
- Fedora stratum
- Void stratum
- Alpine stratum
- Gentoo stratum
- enterprise compatibility stratum
- user-local language ecosystem

This must not mean uncontrolled filesystem merging. It means saltOS has a first-class model for choosing providers, resolving conflicts, exposing commands, launching apps with the right libraries, and rolling back integration decisions.

Possible provider choices:

```txt
coreutils provider: native or debian
kernel provider: native or arch
init/service style: native runit, optionally with imported service definitions
browser provider: native, arch, fedora, or debian
patched app provider: gentoo
legacy business app runtime: enterprise-linux stratum
game runtime: ubuntu stratum
font provider: arch or AUR-enabled arch stratum
```

The user should be able to mix these intentionally without turning the host into an untraceable pile of files.

### 5.4 Strata for package depth

saltOS should support managed distro strata for large package ecosystems.

Examples:

```txt
arch     -> pacman
debian   -> apt
ubuntu   -> apt
fedora   -> dnf
void     -> xbps
alpine   -> apk
gentoo   -> emerge
opensuse -> zypper
```

A stratum is a controlled foreign userspace rooted under saltOS management. It can provide apps, tools, libraries, and language ecosystems without owning the host OS.

### 5.5 Explicit by default, transparent by choice

Foreign software should not automatically pollute the global system namespace.

By default, users should run foreign software explicitly:

```sh
salt run arch firefox
salt run debian gcc
salt run void xbps-install ripgrep
```

Users may opt into exposing selected commands globally:

```sh
salt expose arch firefox
salt expose debian gcc as gcc-12-debian
salt expose arch code as code
```

This keeps conflicts understandable while still allowing a smooth daily-driver workflow.

### 5.6 Rollback everywhere

Rollback should apply to both the native host and strata.

saltOS should support:

- full-system deployments
- host-only rollback
- per-stratum rollback
- transaction logs across native and foreign package operations
- bootloader entries for known-good deployments

The user should be able to recover from a bad native update, a bad `pacman -Syu`, or a broken desktop package without reinstalling.

### 5.7 Trust boundaries must be visible

saltOS should clearly distinguish:

- official saltOS packages
- packages from a foreign distro’s official repository
- third-party repositories enabled inside a stratum
- user-installed language packages
- manually exposed commands

The system should not imply that an Arch AUR package, a Debian stable package, and a saltOS native package share the same review process.

### 5.8 Practical reproducibility

saltOS should pursue reproducible builds for its native packages and deterministic stratum bootstrap metadata where practical.

Reproducibility should improve over time without blocking a usable daily-driver OS from existing.

saltOS models the whole system as a declarative `system.toml` (intent) plus a fully pinned `system.lock.toml` (resolution), in the spirit of Nix and Cargo but across every plane. The native plane pins recipe name, version, and grain/source hashes; each stratum pins its bootstrap hash, a frozen repository snapshot, and per-package content hashes. `salt config apply system.lock.toml` reproduces the same system — the same base distribution snapshot, exposed userland, and kernel/boot contract — on another machine. The achievable guarantee differs per plane: source-level reproducibility for the native plane, content-pinned reinstall for foreign strata, whose package managers are not rebuilt under saltOS control. See `docs/reproducibility.md`.

## 6. System Overview

saltOS consists of:

- Linux kernel, normally native but replaceable through saltOS-managed provider policy
- glibc host base
- runit init system, normally native but compatible with imported service definitions
- Btrfs root filesystem
- full-system and per-stratum rollback
- native `salt` package manager
- native `.grain` package format for saltOS-owned packages
- curated native binary repository
- stratum manager for foreign distro roots
- command routing and optional command exposure
- signed metadata for native packages and salt-managed strata
- text-first installer
- daily-driver desktop edition

## 7. Base Technology Decisions

### 7.1 Kernel

Use the upstream Linux kernel with minimal patching.

Initial target:

- latest stable kernel series

Possible future tracks:

- `linux-stable`
- `linux-lts`
- `linux-hardened`

### 7.2 C Library

Initial host recommendation:

- glibc

Reasoning:

- best binary compatibility
- easiest desktop support
- fewer surprises for browsers, GPU drivers, development tools, and third-party software

Foreign strata may use their own libc where appropriate. For example, Alpine strata may use musl internally.

### 7.3 Init System

Use `runit` for the host system.

Goals:

- simple service supervision
- fast boot
- easy-to-understand service directories
- less complexity than systemd

saltOS should provide simple service management wrappers if raw runit UX is too sharp.

Possible command shape:

```sh
svc enable network
svc disable bluetooth
svc start bluetooth
svc status
```

Strata should not replace the host init system. If a stratum needs service support, saltOS should provide controlled integration instead of letting the stratum take over PID 1.

### 7.4 Filesystem

Use Btrfs by default.

Recommended subvolumes:

```txt
@
@home
@var
@log
@snapshots
@strata
```

Possible stratum layout:

```txt
/strata/arch
/strata/debian
/strata/fedora
/strata/void
/strata/alpine
```

Each stratum should be snapshot-capable independently.

User home data should not be rolled back by default.

## 8. Stratum Model

A stratum is a managed foreign distro userspace.

It contains:

- a root filesystem
- its native package manager
- package database
- repository configuration
- integration metadata
- exposed command records
- rollback snapshots

A stratum does not directly own:

- the bootloader
- host kernel
- host init
- host `/usr`
- host `/etc`
- host package database
- host rollback policy

A stratum may provide a component that saltOS adopts into a host role. In that case, saltOS owns the adoption decision, integration metadata, rollback point, and conflict policy.

### 8.0 Runtime model: one system, many userlands

A stratum is not a container you live inside. It is a foreign **userland** grafted onto the one system. The dividing line is deliberate and fixed:

- **Isolated, per stratum — the distro's own program files:** `/usr`, `/lib`, `/lib64`, `/bin`, `/sbin`, and the distro-specific parts of `/etc`/`/var`. This is the entire reason strata exist — Arch is glibc, Alpine is musl; their `/usr` and `/lib` cannot be shared. So they are not.
- **Shared, one system — everything that is *yours*:**
  - **Your data:** `/home`, `/root`, `/srv`, `/opt`, `/mnt`, `/media`, `/tmp`.
  - **Your identity:** `/etc/passwd`, `/etc/group`, `/etc/shadow` — one user database.
  - **The machine:** the kernel, and the runtime trees `/dev`, `/proc`, `/sys`, `/run`.

The rule is a principle, not a hand-maintained allow-list: *your files and your identity belong to the system and are shared; the distro's binaries belong to the stratum and are isolated.* Consequences that follow directly and must hold:

- **Files are consistent everywhere.** A file any command creates in a shared data dir is the same on-disk file the host and every other stratum sees. `git clone` in one command and `ls` in the next agree, whichever stratum each came from.
- **A user is a user everywhere.** User accounts are system state, not stratum state. The human user database is one shared database, **merged into each stratum alongside — never over — that distro's own system users**, so the stratum's package manager still resolves its `alpm`/`http`/etc. Creating an account with the ordinary system tools, run anywhere, makes it exist host-wide and in every stratum; there is no `salt user` reinvention. (It is a merged/`extrausers`-style shared db with a writable target, not a raw single-file bind — a bind would shadow the distro's system users and break the atomic-rename that `useradd`/`passwd` use to write.)
- **Tools compose across strata.** Exposed commands resolve from any shell, so Alpine's `git` and Arch's `gcc` are usable together, operating on your shared files. A tool installed in one stratum need not be reinstalled in another.

**Namespace lifetime.** Each stratum has **one persistent mount namespace**, set up once (a detached holder pins it) and **joined** — not re-created — by every subsequent command. Per-command throwaway namespaces would split-brain the view (a directory one command made would be invisible to the next); a shared, persistent namespace is what makes the guarantees above true. `salt` itself is reachable from inside any stratum: invoked there, it re-enters the host mount namespace to do its work, so `salt …` and cross-stratum commands route cleanly with no nesting. Unprivileged callers fall back to a per-command unprivileged user namespace.

### 8.1 Stratum Commands

Possible command shape:

```sh
salt stratum list
salt stratum add arch
salt stratum add debian
salt stratum remove arch
salt stratum update arch
salt stratum shell arch
salt stratum rollback arch
salt stratum snapshot arch
salt stratum status arch
```

Software execution:

```sh
salt run arch firefox
salt run debian gcc --version
salt run fedora dnf search podman
```

Package manager passthrough:

```sh
salt pkg arch install firefox
salt pkg debian install build-essential
salt pkg fedora update
salt pkg void remove chromium
```

Direct package manager access can exist inside a stratum shell:

```sh
salt stratum shell arch
pacman -Syu
```

saltOS should still record these operations when possible.

### 8.2 Command Exposure

By default, commands from strata are not globally exposed.

Users can expose selected commands:

```sh
salt expose arch firefox
salt expose arch rg as rg
salt expose debian gcc as gcc-debian
salt unexpose firefox
salt exposed
```

Exposed commands should be implemented as shims owned by saltOS, not by the foreign package manager.

Possible shim location:

```txt
/usr/local/salt/shims
```

The shim records:

- source stratum
- target command
- chosen alias
- environment policy
- graphical/session permissions
- creation time

### 8.3 Conflict Policy

If multiple strata provide the same command, saltOS should not guess silently.

Example:

```txt
firefox provided by:
  arch:/usr/bin/firefox
  debian:/usr/bin/firefox
  native:/usr/bin/firefox
```

The user should choose what to expose.

Native saltOS packages win for host-owned paths. Foreign package managers must not overwrite native host files.

### 8.4 Component Providers and Adoption

saltOS should model important system components as provider-selectable where practical.

Provider-selectable components may include:

- shell utilities
- compiler toolchains
- kernels
- firmware packages
- graphical applications
- fonts
- language runtimes
- compatibility libraries
- game runtimes
- enterprise application runtimes
- service definitions

Possible command shape:

```sh
salt provider list coreutils
salt provider set coreutils debian
salt provider set kernel arch/linux
salt provider set font arch/ttf-jetbrains-mono
salt provider set pdf-reader gentoo/zathura-custom
salt provider status
salt provider rollback kernel
```

Provider adoption should be stricter than command exposure. Exposing `arch/firefox` as a desktop app is low-risk compared to adopting an Arch kernel or Debian coreutils into the host role.

Adoption requirements should include:

- explicit user action
- pre-adoption snapshot
- compatibility check
- file conflict check
- trust boundary warning
- rollback entry
- clear ownership record

The goal is Bedrock-like mix-and-match power with saltOS-native accountability.

### 8.5 Graphical Applications

Graphical apps from strata should be supported as first-class daily-driver use cases.

The integration layer should handle:

- Wayland sockets
- X11 fallback
- PulseAudio/PipeWire sockets
- D-Bus session access
- fonts
- icons
- desktop files
- portals where practical
- GPU device access

Desktop entry exposure should be explicit:

```sh
salt expose-desktop arch firefox
salt expose-desktop debian libreoffice
```

### 8.6 Service Integration

Host services are runit services.

Strata may provide daemons, but they should be integrated through saltOS-managed service wrappers.

Possible command shape:

```sh
salt service import arch docker
salt service enable arch docker
salt service start arch docker
```

This avoids letting a foreign init system control the host.

## 9. Native Package Manager

The `salt` tool should be both:

1. a native package manager for saltOS packages
2. a stratum manager for foreign package ecosystems

The native package manager should be implemented in C/C++.

Preferred architecture:

- `halite`: a C core library for archive, database, hashing, signatures, low-level transactions, stratum management, the native run engine, and rollback metadata
- C++23 CLI and higher-level orchestration

`halite` is the mineral form of rock salt (NaCl): the bedrock the `salt` tool is mined from.

Working name:

```txt
salt
```

Native package commands:

```sh
salt sync
salt search helium
salt install helium
salt remove helium
salt update
salt rollback
salt deployments
salt verify
salt query helium
salt files helium
salt owner /usr/bin/foo
```

Stratum and integration commands:

```sh
salt stratum add arch
salt stratum shell debian
salt run arch firefox
salt pkg fedora install podman
salt expose arch rg
salt provider list kernel
salt provider set kernel arch/linux
salt provider set coreutils debian
salt provider rollback kernel
```

System configuration and reproducibility commands:

```sh
salt config show
salt config apply system.lock.toml
salt config diff
salt config rollback
salt lock
```

These pin and reproduce the whole system across both planes; see `docs/reproducibility.md`. The installer (`salt-setup`) writes `system.toml`, and shares the `salt config apply` engine for non-interactive installs.

### 9.1 Native Package Manager Goals

The package manager should be:

- fast
- native
- transactional
- rollback-aware
- signature-checking
- source-hash-aware
- hostile to arbitrary install-time execution
- easy to audit
- usable without a daemon
- aware of stratum boundaries
- aware of component providers and adoption state

### 9.2 Native Package Format

Initial package format:

```txt
.grain
```

A package is a *grain* of salt: the smallest curated unit the native plane installs and rolls back.

Internally:

```txt
metadata.toml
manifest.toml
files.tar.zst
scripts/
```

`.grain` should be used for saltOS-owned packages, not as a requirement for all software installed on the machine.

Scripts should be restricted and discouraged. The default official package policy should avoid arbitrary maintainer scripts where possible.

### 9.3 Local Databases

saltOS should maintain separate but related databases:

1. native package database
2. transaction database
3. deployment database
4. stratum registry
5. exposed command registry
6. component provider registry

The native package database should track:

- installed package name
- version
- release
- architecture
- file manifest
- file hashes
- install time
- transaction ID
- repository source
- signature status

The stratum registry should track:

- stratum name
- distro family
- architecture
- root path
- bootstrap method
- package manager
- repository configuration hash
- snapshot history
- exposed commands
- integration permissions

The component provider registry should track:

- component class
- active provider
- candidate providers
- files or shims involved
- adoption transaction ID
- rollback target
- trust level
- compatibility notes

Database backend recommendation:

- SQLite for v0

Correctness matters more than aesthetic purity.

## 10. Native Package Recipes

Use TOML for native package metadata and build recipes.

TOML is chosen because it is small, clear, and easier to parse safely than YAML.

Example native recipe:

```toml
name = "zlib"
version = "1.3.1"
release = 1
summary = "Compression library"
license = "Zlib"
arch = ["x86_64"]

[source]
url = "https://zlib.net/zlib-1.3.1.tar.gz"
sha256 = "..."

[build]
system = "make"
deps = ["gcc", "make"]

[package]
deps = ["glibc"]

[reproducibility]
status = "verified"
```

Native recipes should require:

- pinned source URLs
- cryptographic source hashes
- declared build dependencies
- declared runtime dependencies
- license field
- reproducibility status

## 11. Stratum Recipes

saltOS should also support TOML definitions for managed strata.

Example:

```toml
name = "arch"
family = "arch"
arch = "x86_64"
package_manager = "pacman"
bootstrap = "pacstrap"
root = "/strata/arch"

[repositories]
core = "https://geo.mirror.pkgbuild.com/core/os/x86_64"
extra = "https://geo.mirror.pkgbuild.com/extra/os/x86_64"

[integration]
default_exposure = "explicit"
graphics = true
audio = true
dbus_session = true
system_services = "wrapped"

[rollback]
mode = "per-stratum"
pre_transaction_snapshots = true
```

Stratum recipes should declare:

- distro family
- bootstrap method
- package manager
- root location
- architecture
- repository sources
- host integration permissions
- snapshot policy
- trust level

saltOS should ship official stratum definitions for popular ecosystems instead of asking users to invent them from scratch.

## 12. Repository Model

saltOS should use a curated official binary repository for native packages.

Repository layout:

```txt
repo/
  x86_64/
    index.toml
    index.toml.sig
    packages/
      zlib-1.3.1-1-x86_64.grain
      salt-0.1.0-1-x86_64.grain
```

Repository metadata must be signed.

Packages should be hash-addressed by the index.

The package manager should trust signed metadata first, then verify package hashes from the signed index.

Strata use their own upstream repositories, but saltOS should record and display which repositories are enabled.

## 13. Trust and Contribution Model

saltOS should use an explicit trust system for native repository contribution and stratum definitions.

Contributor states:

- unknown
- vouched
- maintainer
- denounced

Unknown contributors may open issues or proposals depending on project policy, but native package recipe changes and official stratum definition changes should require review from trusted maintainers.

### 13.1 Native Package Admission Rules

A native package should not enter the official repository unless:

- source URL is pinned
- source hash is pinned
- license is declared
- build dependencies are declared
- runtime dependencies are declared
- build succeeds in a clean environment
- package contents are inspectable
- maintainer or trusted reviewer approves it

### 13.2 Stratum Definition Admission Rules

An official stratum definition should not enter the project unless:

- bootstrap method is documented
- repository sources are explicit
- GPG/signature policy is documented
- host integration permissions are declared
- rollback behavior is defined
- package manager operations are understood
- trust boundary is visible to users

### 13.3 Supply-Chain Risk Rules

The repository and stratum tooling should flag:

- maintainer changes
- abandoned packages
- source URL changes
- new network access during native builds
- new install scripts
- large unexpected file changes
- generated binary blobs
- crypto wallet addresses
- obfuscated scripts
- sudden package ownership changes
- suspicious contributor behavior
- newly enabled third-party stratum repositories
- exposed commands from lower-trust strata

### 13.4 No Blind Automation

Automated scoring can warn, block, or require extra review, but should not become the sole basis for trust.

The trust model should be explicit and boring:

- signed native releases
- reviewed native recipes
- reviewed official stratum definitions
- visible foreign repository configuration
- vouched contributors
- logged decisions
- no drive-by package ownership takeover

## 14. Security Policy

Initial security level:

- signed native repository metadata
- source hashes required for native packages
- build sandboxing required for official native builds
- no arbitrary native install scripts by default
- package contents manifest required
- transaction rollback required
- per-stratum snapshot support
- explicit command exposure
- visible stratum trust boundaries
- official repo curated by trusted maintainers

Foreign strata inherit the security model of their upstream distro repositories. saltOS should not hide that.

Future goals:

- reproducible builds for critical native packages
- multiple-builder verification
- binary transparency log
- package provenance records
- SBOM generation
- mandatory review for high-risk native packages
- automated policy engine for native repository and stratum changes
- optional sandbox profiles for high-risk foreign apps

## 15. Build System

saltOS needs three related tool groups:

1. native package manager for users
2. native package builder for maintainers
3. stratum bootstrap and integration tools

Possible commands:

```sh
salt build recipes/zlib
salt lint recipes/zlib
salt sign out/zlib-1.3.1-1-x86_64.grain
salt repo publish out/
salt stratum lint strata/arch.toml
salt stratum bootstrap arch
```

The native build system should:

- build in clean chroots or containers
- deny network access after source fetch
- verify source hashes before build
- record build logs
- record build environment
- generate package manifests
- generate reproducibility metadata

The stratum bootstrap system should:

- create the root filesystem
- initialize the foreign package manager
- verify upstream repository signatures where supported
- record repository configuration
- create an initial snapshot
- register integration permissions

## 16. Release Model

Use a rolling curated model with snapshot releases.

Branches:

```txt
unstable
current
stable-snapshot
```

Meaning:

- `unstable`: active native package updates and stratum integration changes; breakage allowed
- `current`: default user track after basic testing
- `stable-snapshot`: periodic known-good image/repository snapshot

The OS should feel current, but updates should be grouped into tested snapshots where possible.

Strata may follow their upstream release models. saltOS should make this visible.

Example:

```txt
host: saltOS current
arch stratum: rolling
debian stratum: stable
fedora stratum: versioned
```

## 17. Desktop Edition

saltOS should be a daily-drivable desktop OS, not just a package-manager experiment.

Default desktop environment:

```txt
LXQt
```

Reasons:

- Qt-based
- lightweight
- familiar desktop metaphor
- less massive than KDE Plasma
- more modern-feeling than XFCE
- suitable for a niche Unix-like desktop OS

### 17.1 Display Stack

Initial recommendation:

- Wayland-first where practical
- X11 compatibility where needed

Possible compositor/window manager:

- labwc for lightweight Wayland
- openbox if LXQt/X11 is simpler at first

The first desktop release may choose practicality over purity.

### 17.2 Default Applications

The default desktop should be useful out of the box without requiring immediate stratum setup.

Default browser options:

- native Helium if packaging is practical
- browser from an official desktop stratum if native packaging is too costly at first
- Firefox or ungoogled-chromium as temporary fallback

Default apps:

- terminal: QTerminal or foot
- file manager: PCManFM-Qt
- text editor: FeatherPad
- archive manager: LXQt Archiver
- image viewer: LXImage-Qt or qView
- media player: mpv
- document viewer: qpdfview or similar
- network: NetworkManager
- audio: PipeWire
- Bluetooth: BlueZ with minimal frontend
- login manager: SDDM or greetd

Design goal:

- usable out of the box
- not bloated
- no giant pile of duplicate apps
- native base where it matters
- stratum-backed apps where that is the practical path

## 18. Browser Policy

Helium should be the preferred browser if packaging is practical.

Because Chromium-derived browsers are large and high-risk packages, Helium should be treated as a special package with stricter rules:

- pinned upstream revision
- recorded Chromium base version
- recorded Helium revision
- build logs archived
- patches auditable
- update cadence monitored closely

If native browser packaging slows the OS down too much, the first daily-driver release may use a browser from an official stratum.

Fallback options:

- Firefox from a Debian, Fedora, or Arch stratum
- ungoogled-chromium from a stratum
- native Firefox only if maintainable

## 19. Developer Tooling

saltOS should support normal developer workflows.

It should not prevent users from running:

```sh
bun i -g
cargo install
pipx install
npm install -g
```

But the official system packages should remain separate from user-level language ecosystems.

Recommended policy:

- native `salt` owns the host system paths
- foreign package managers own their stratum roots
- user package managers own user-space paths
- development tools can be installed globally per user
- risky project dependencies should be isolated where practical

Developer tools may come from native packages or strata:

```sh
salt install git
salt pkg arch install rust go zig
salt pkg debian install build-essential
salt expose arch cargo
salt expose arch go
```

Default developer packages may include:

- git
- curl
- clang
- gcc
- make
- cmake
- ninja
- pkg-config
- bun
- node
- python
- rust
- go
- zig

## 20. Installer

The installer is text-first and native. It is `salt-setup`, a C++23 program in `src/setup/` built on the shared `halite` engine — a sibling of `salt`, not a shell script and not a desktop application. Installer logic is OS runtime code; only the image build (`os/iso/live-build.sh`) stays in shell.

The defining choice the installer presents is **which distribution provides userland**. saltOS installs its own minimal native layer as the root (init, `salt`, `halite`, the boot contract, the btrfs layout, and system identity) and then bootstraps the chosen distribution as the **primary stratum** under `/strata/<name>`, auto-exposed so its userland is on `PATH`. This is how saltOS stays its own OS while sourcing package depth from any ecosystem, without the maintainer repackaging the Linux world.

`salt-setup` runs interactively in the bare boot environment, or non-interactively from a config: `salt-setup --from system.toml`. The non-interactive path is intended to share the reproducibility `salt config apply` engine, so installing a system and reproducing one from a lockfile are the same code.

Installer must support:

- disk selection
- Btrfs partitioning (the canonical `@ @home @var @log @snapshots @strata` layout)
- encryption option
- bootloader install (the salt-owned boot contract)
- user creation
- network setup
- minimal native base install
- base distribution (primary stratum) selection
- optional desktop install from the chosen stratum
- default command exposure policy
- writing the reproducible `system.toml` (and, once the lock engine lands, `system.lock.toml`)

A graphical installer is not required. The previous Calamares GUI flow is retired (see 20.1).

Possible install profiles:

```txt
minimal native host
native host + arch userland
native host + debian userland
native host + void userland
native host + desktop (from chosen stratum)
native host + developer strata
```

The installer should not force users to think in distro theory before they can boot the machine: a sensible default base distribution is preselected.

See `docs/installer.md` for the full flow.

### 20.1 Native installer (`salt-setup`) and Calamares retirement

The installer is the native `salt-setup` program described above, autostarted on
the console by a `base` ISO edition (`EDITION=base` in `os/iso/live-build.sh`): a
minimal environment with no desktop and no Calamares, carrying `salt`,
`salt-setup`, the stratum bootstrap tools, firmware, and the `strata/` recipes.

The earlier graphical Calamares flow (`os/installer/settings-live.conf`, the
modules under `os/installer/modules/` and `os/installer/modules-live/`) and the
Debian-clone `os/installer/saltos-install.sh` are superseded by `salt-setup`.
Those assets remain in-tree until the native path is validated on real hardware,
then are removed.

The guided flow collects: target disk; the base distribution (primary stratum);
hostname, user account, timezone, and locale; kernel source (native by default);
and a confirmation before erasing. It then partitions the disk, lays down the
canonical `@ @home @var @log @snapshots @strata` btrfs layout, populates the
minimal native base, calls the `halite` stratum API directly to bootstrap and
auto-expose the chosen distribution, installs the kernel + initramfs + GRUB under
the salt boot contract, and writes `/etc/salt/system.toml`. The installed system
uses `salt` as its native package and stratum manager; the chosen distribution's
userland is available immediately.

## 21. Bootloader

Initial recommendation:

- GRUB if it simplifies Btrfs snapshot booting
- systemd-boot only if the project chooses not to depend on GRUB snapshot integration

Because rollback is central, bootloader integration matters more than aesthetic preference.

Boot entries should support known-good host deployments.

Per-stratum rollback can happen from the running system unless a stratum is critical to the graphical login path.

saltOS owns the **boot contract**: `salt` is the sole authority over `/boot`, the GRUB configuration, and initramfs generation. Centralizing this is what keeps boot recoverable and rollback-aware; nothing else writes the boot path.

The kernel that fills the contract is a declared, replaceable input rather than a hardcoded component:

```toml
[kernel]
source = "native"        # default: the saltOS native kernel package
# source = "stratum:arch" # take the kernel from a chosen stratum
# version = "6.12.30"     # pin a specific native kernel
```

Default is the native kernel; advanced users may point the kernel slot at a stratum or pin a version. `salt` still owns initramfs and GRUB generation regardless of the source, and the choice is pinned in the lockfile. This delivers "saltOS owns boot by default" together with "the user can change the kernel" without per-distro boot integration.

## 22. Architecture Support

Initial architecture:

```txt
x86_64
```

Future architecture:

```txt
aarch64
```

No other architectures should be supported until the host, native package manager, stratum manager, installer, rollback system, and desktop are stable.

Strata should only be supported where upstream bootstrap and package repositories are reliable for the target architecture.

## 23. Licensing Policy

saltOS should prefer permissive and copyleft open-source software.

Native package metadata must declare license.

The project should avoid:

- telemetry by default
- unclear licenses
- repackaged binaries with unknown provenance
- packages that download code at install time

Foreign strata may contain packages under their upstream policies. saltOS should display that boundary rather than pretending all software follows native saltOS policy.

## 24. Milestones

### 24.1 Prototype

Goal:

- `salt` runs on an existing Linux system
- can install native packages into a fake root
- native package format works
- repository index works
- signatures or hash verification work
- can create and enter a managed stratum root

### 24.2 Bootable Host Rootfs

Goal:

- build minimal saltOS root filesystem
- boot in QEMU
- runit starts services
- login shell works
- native package manager works inside the VM
- Btrfs layout exists

### 24.3 First Stratum Integration

Goal:

- bootstrap one foreign distro stratum
- run its package manager
- install a CLI package inside it
- run the installed command through `salt run`
- snapshot and roll back the stratum

Good first candidates:

- Arch for current desktop/developer packages
- Debian for stable compatibility
- Void for runit-aligned simplicity

The choice should be based on implementation practicality, not brand loyalty.

### 24.4 First Interchangeability Demo

Goal:

- install at least two strata
- expose one CLI app from one stratum
- expose one graphical app from another stratum
- select one provider-managed component from a non-native source
- verify conflicts are visible instead of silently merged
- roll back the provider choice

A convincing demo would look like:

```txt
native saltOS host
Debian-provided stable shell utilities or compatibility runtime
Arch-provided current desktop app or kernel candidate
Gentoo-provided patched application candidate
all tracked by saltOS
```

### 24.5 Transactional Updates

Goal:

- Btrfs host root
- pre-update native snapshots
- package transaction logs
- successful host rollback demo
- per-stratum snapshot demo
- failed stratum update rollback demo

### 24.6 Installable Daily-Driver System

Goal:

- installer creates disk layout
- installs bootloader
- installs base system
- installs desktop profile
- optionally bootstraps selected strata
- boots after install
- networking, audio, graphics, and login work
- rollback works after install

### 24.7 Desktop Preview

Goal:

- LXQt session
- browser available natively or through official stratum integration
- terminal, file manager, editor
- audio and networking work
- graphical stratum app launch works
- exposed desktop entries work

### 24.8 First Public Experimental Release

Goal:

- signed ISO
- signed native repository metadata
- documented package format
- documented stratum format
- documented trust policy
- documented rollback process
- explicit warning that the system is experimental
- at least one supported foreign stratum

## 25. Decisions and Open Questions

Decided:

- **Stratum isolation:** Bedrock-style. `salt run` enters the stratum root through a private Linux mount namespace with bind-mounted `/proc`, `/sys`, `/dev`, `/run`, `/tmp`, and `/home`, then `chroot` and drops to the invoking user. No daemon; fits runit. Wayland, X11, D-Bus, PipeWire/Pulse, and GPU (`/dev/dri`, `/dev/snd`) pass through so foreign apps feel first-party.
- **First strata:** Void (runit-native, cleanest fit), Arch (current depth), and Debian (stable compatibility) ship as official recipes under `strata/`.
- **Exposed-command path:** `/usr/local/salt/shims`, prepended to `PATH` by `os/profile.d/salt-shims.sh`. Shims are saltOS-owned `#!/bin/sh exec salt run …` wrappers.
- **Package format / core library:** `.grain` (a grain of salt) for native packages; the C core library is `halite`.
- **Service integration:** supported in v0 as runit `sv` wrappers generated by `salt service import` (a stratum never takes PID 1).
- **Foreign package manager use:** wrapped and recorded via `salt pkg`, with a pre-operation per-stratum snapshot; direct use inside `salt stratum shell` is allowed but unrecorded.
- **Installer:** native `salt-setup` (C++23 on `halite`, `src/setup/`), text-first; the Calamares GUI flow is retired. The base distribution is chosen at install time and bootstrapped as the auto-exposed primary stratum; saltOS's own minimal native layer is always the root.
- **Boot contract:** `salt` is the sole authority over `/boot`, GRUB, and initramfs generation. The kernel is a declared, replaceable input (`[kernel] source`), defaulting to the native kernel, with no per-distro boot integration required.
- **Reproducibility model:** declarative `system.toml` (intent) + fully pinned `system.lock.toml` (resolution); `salt config apply` reproduces a system. Source-level reproducibility for the native plane, content-pinned reinstall for foreign strata.

Still open:

- Should glibc remain the permanent host libc?
- Should the package database use SQLite permanently?
- Should native package scripts be banned entirely or allowed with strict declarations?
- Should host rollback use GRUB snapshot boot entries or a custom deployment selector?
- Should the native package manager and builder be one binary or separate tools?
- Should native package recipes allow shell build phases or use a restricted build DSL?
- Should the first desktop browser be native or stratum-backed?
- How much desktop integration should strata get by default beyond the current socket/GPU passthrough?

## 26. Definition of Success

saltOS is successful when it can:

- boot as its own independent OS
- install itself to disk
- update the host system
- roll back the host cleanly
- install native packages from a signed repo
- build native packages from pinned recipes
- create managed foreign distro strata
- install software through foreign package managers inside strata
- run stratum software through explicit commands
- expose selected stratum apps globally when the user chooses
- choose provider-backed components across native and foreign sources
- roll back broken provider choices
- roll back broken stratum updates
- run a lightweight desktop
- browse the web natively or through a supported stratum
- avoid making the maintainer package the whole Linux universe
- keep trust boundaries clear
- remain understandable to one technical maintainer

The first real version does not need thousands of native packages. It needs a coherent daily-driver host that proves native saltOS packages and managed foreign strata can work together cleanly.
