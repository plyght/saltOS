# saltOS Distro Design Document

## 1. Product Intent

saltOS is a personal-first, Unix-like Linux distribution focused on being fast, current, rollbackable, understandable, and resistant to common open-source supply-chain failure modes.

The project is not trying to be another general-purpose community distro on day one. It should start as a small, coherent operating system with its own package manager, its own package format, a curated package repository, and a simple full-system rollback model.

saltOS should feel like:

- a real Unix-like desktop OS
- current without being reckless
- reproducible without being hostile
- rollbackable without being complicated
- minimal without being useless
- opinionated without becoming a lifestyle cult

## 2. Non-Goals

saltOS will not initially attempt to:

- replace Debian, Arch, Fedora, or openSUSE
- maintain a massive package universe
- support every desktop environment
- support every architecture
- provide an AUR-like untrusted user repository
- use Nix-style package isolation as the primary model
- become source-only like Gentoo
- invent a custom kernel, libc, compiler, init system, or desktop shell
- require users to learn a strange declarative language before using the OS

## 3. Target Users

Primary target:

- the creator as the first daily-driver user

Secondary target:

- developers, power users, and technical desktop users who want a small, sharp, rollbackable Linux system without Arch/AUR supply-chain chaos or NixOS complexity

Tertiary target:

- people who care about explicit trust models, reproducible builds, and curated contribution pipelines

## 4. Core Principles

### 4.1 Current, but curated

saltOS should ship recent software, but updates should pass through a curated repository process instead of being pulled blindly from arbitrary user recipes.

### 4.2 Rollback is a first-class feature

Every system update should create a rollback point. If an upgrade breaks the machine, the user should be able to return to the previous working system with one simple command or boot menu selection.

### 4.3 Unix-like filesystem model

saltOS should use a traditional Unix-like filesystem layout. Packages install into normal system paths such as `/usr`, `/etc`, `/var`, and `/opt` where appropriate.

The system should not primarily use a Nix-style content-addressed store for normal packages.

### 4.4 Reproducible where it matters

The official repository should prefer reproducible package recipes, pinned source hashes, deterministic build environments, and signed repository metadata.

Reproducibility should be practical and enforced progressively rather than requiring perfection before the system can exist.

### 4.5 No AUR equivalent by default

saltOS should not ship with a default untrusted user package repository. The official package ecosystem should be curated, reviewed, signed, and intentionally small at first.

### 4.6 Simple tools over clever frameworks

The package manager, repository tools, installer, and system update mechanism should be understandable by one person.

## 5. System Overview

saltOS consists of:

- Linux kernel
- glibc or musl
- runit init system
- Btrfs root filesystem
- full-system rollback mechanism
- custom C/C++ package manager
- custom package format
- curated binary package repository
- TOML package recipes
- LXQt desktop edition
- Helium browser as the default browser
- minimal default application set

## 6. Base Technology Decisions

### 6.1 Kernel

Use the upstream Linux kernel with minimal patching.

Initial target:

- latest stable kernel series

Possible future tracks:

- `linux-stable`
- `linux-lts`
- `linux-hardened`

### 6.2 C Library

Initial recommendation:

- glibc

Reasoning:

- best binary compatibility
- easiest desktop support
- fewer surprises for browsers, GPU drivers, development tools, and third-party software

Future possibility:

- musl-based minimal/server edition

### 6.3 Init System

Use `runit`.

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
svc start helium-helper
svc status
```

### 6.4 Filesystem

Use Btrfs by default.

Required subvolumes:

```txt
@
@home
@var
@log
@snapshots
```

The exact layout can change, but rollback must not destroy user data by default.

## 7. Rollback Model

saltOS should provide full-system rollback in the simplest possible way.

Initial model:

- Btrfs snapshots before system transactions
- bootloader entries for previous deployments
- package transaction logs
- rollback command that restores the previous known-good root state

Core commands:

```sh
salt update
salt rollback
salt deployments
salt verify
```

Rollback requirements:

- every package upgrade creates a pre-transaction snapshot
- failed transactions automatically roll back
- bootloader can boot the previous deployment
- user home data is not rolled back by default
- rollback should be understandable and inspectable

Desired UX:

```sh
salt update
# bad update happens
salt rollback
reboot
```

## 8. Package Manager

The saltOS package manager should be implemented in C/C++.

Preferred architecture:

- C core libraries for archive, database, hashing, and low-level transaction handling
- C++23 CLI and higher-level orchestration

Working name:

```txt
salt
```

Potential subcommands:

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
salt build ./recipes/zlib
```

### 8.1 Package Manager Goals

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

### 8.2 Package Format

Initial package format:

```txt
.saltpkg
```

Internally:

```txt
metadata.toml
manifest.toml
files.tar.zst
scripts/
```

Scripts should be restricted and discouraged. The default official package policy should avoid arbitrary maintainer scripts where possible.

### 8.3 Local Package Database

The package database should track:

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

Database backend options:

- SQLite for v0
- custom simple database later only if SQLite becomes a problem

Recommendation:

- use SQLite initially because correctness matters more than aesthetic purity

## 9. Package Recipes

Use TOML for package metadata and build recipes.

TOML is not chosen because of Rust. It is chosen because it is smaller, clearer, less surprising, and easier to parse safely than YAML.

Example recipe:

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
```

Recipes should require:

- pinned source URLs
- cryptographic source hashes
- declared build dependencies
- declared runtime dependencies
- license field
- reproducibility status

Possible reproducibility status values:

```toml
[reproducibility]
status = "verified"
```

```toml
[reproducibility]
status = "unverified"
reason = "Chromium-derived browser build currently not bit-for-bit reproducible"
```

## 10. Repository Model

saltOS should use a curated official binary repository.

Repository layout:

```txt
repo/
  x86_64/
    index.toml
    index.toml.sig
    packages/
      zlib-1.3.1-1-x86_64.saltpkg
      helium-0.13.4-1-x86_64.saltpkg
```

Repository metadata must be signed.

Packages should be hash-addressed by the index.

The package manager should trust signed metadata first, then verify package hashes from the signed index.

## 11. Trust and Contribution Model

saltOS should use an explicit trust system for repository contribution.

The project should borrow ideas from trust-list systems like Vouch and policy engines like Tripwire without blindly copying either model.

### 11.1 Contributor Trust

Contributors can be:

- unknown
- vouched
- maintainer
- denounced

Unknown contributors may open issues or proposals depending on project policy, but package recipe changes should require review from trusted maintainers.

### 11.2 Package Admission Rules

A package should not enter the official repository unless:

- source URL is pinned
- source hash is pinned
- license is declared
- build dependencies are declared
- runtime dependencies are declared
- build succeeds in a clean environment
- package contents are inspectable
- maintainer or trusted reviewer approves it

### 11.3 Supply-Chain Risk Rules

The repository tooling should flag:

- maintainer changes
- abandoned packages
- source URL changes
- new network access during build
- new install scripts
- large unexpected file changes
- generated binary blobs
- crypto wallet addresses
- obfuscated scripts
- sudden package ownership changes
- suspicious contributor behavior

### 11.4 No Blind Automation

Automated scoring can warn, block, or require extra review, but should not become the sole basis for trust.

The trust model should be explicit and boring:

- signed releases
- reviewed recipes
- vouched contributors
- logged decisions
- no drive-by package ownership takeover

## 12. Security Policy

Initial security level:

- signed repository metadata
- source hashes required
- build sandboxing required for official builds
- no arbitrary install scripts by default
- package contents manifest required
- transaction rollback required
- official repo curated by trusted maintainers

Future goals:

- reproducible builds for critical packages
- multiple-builder verification
- binary transparency log
- package provenance records
- SBOM generation
- mandatory review for high-risk packages
- automated policy engine for repository changes

## 13. Build System

saltOS needs two related tools:

1. package manager for users
2. package builder for maintainers

Possible commands:

```sh
salt build recipes/zlib
salt lint recipes/zlib
salt sign out/zlib-1.3.1-1-x86_64.saltpkg
salt repo publish out/
```

The build system should:

- build in clean chroots or containers
- deny network access after source fetch
- verify source hashes before build
- record build logs
- record build environment
- generate package manifests
- generate reproducibility metadata

## 14. Release Model

Use a rolling curated model with snapshot releases.

Branches:

```txt
unstable
current
stable-snapshot
```

Meaning:

- `unstable`: active package updates and breakage allowed
- `current`: default user track after basic testing
- `stable-snapshot`: periodic known-good image/repository snapshot

The OS should feel current, but updates should be grouped into tested snapshots where possible.

## 15. Desktop Edition

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

### 15.1 Display Stack

Initial recommendation:

- Wayland-first where practical
- X11 compatibility where needed

Possible compositor/window manager:

- labwc for lightweight Wayland
- openbox if LXQt/X11 is simpler at first

The first desktop release may choose practicality over purity.

### 15.2 Default Applications

Default browser:

- Helium

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

## 16. Browser Policy

Helium should be the default browser if packaging is practical.

Because Chromium-derived browsers are large and high-risk packages, Helium should be treated as a special package with stricter rules:

- pinned upstream revision
- recorded Chromium base version
- recorded Helium revision
- build logs archived
- patches auditable
- update cadence monitored closely

Fallback browser if Helium packaging is not ready:

- Firefox or ungoogled-chromium as temporary bootstrap browser

## 17. Developer Tooling

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

- system package manager owns `/usr`
- user package managers own user-space paths
- development tools can be installed globally per user
- risky project dependencies should be isolated where practical

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

## 18. Installer

The installer should be simple and text-first initially.

Installer must support:

- disk selection
- Btrfs partitioning
- encryption option
- bootloader install
- user creation
- network setup
- base system install
- desktop install option

A graphical installer is not required for v0.

## 19. Bootloader

Initial recommendation:

- GRUB if it simplifies Btrfs snapshot booting
- systemd-boot only if the project chooses not to depend on GRUB snapshot integration

Because rollback is central, bootloader integration matters more than aesthetic preference.

## 20. Architecture Support

Initial architecture:

```txt
x86_64
```

Future architecture:

```txt
aarch64
```

No other architectures should be supported until the package manager, repository, installer, rollback system, and desktop are stable.

## 21. Licensing Policy

saltOS should prefer permissive and copyleft open-source software.

Package metadata must declare license.

The project should avoid:

- telemetry by default
- unclear licenses
- repackaged binaries with unknown provenance
- packages that download code at install time

## 22. Milestones

### 22.1 Prototype

Goal:

- package manager runs on an existing Linux system
- can install packages into a fake root
- package format works
- repository index works
- signatures or hash verification work

### 22.2 Bootable Rootfs

Goal:

- build minimal root filesystem
- boot in QEMU
- runit starts services
- login shell works
- package manager works inside the VM

### 22.3 Transactional Updates

Goal:

- Btrfs root
- pre-update snapshots
- package transaction logs
- successful rollback demo

### 22.4 Installable System

Goal:

- installer creates disk layout
- installs bootloader
- installs base system
- boots after install
- rollback works after install

### 22.5 Desktop Preview

Goal:

- LXQt session
- Helium or fallback browser
- terminal, file manager, editor
- audio and networking work

### 22.6 First Public Experimental Release

Goal:

- signed ISO
- signed repository metadata
- documented package format
- documented trust policy
- documented rollback process
- explicit warning that the system is experimental

## 23. Open Questions

- Should the C library be glibc permanently, or should musl be explored later?
- Should the package database use SQLite permanently?
- Should package scripts be banned entirely or allowed with strict declarations?
- Should rollback use GRUB snapshot boot entries or a custom deployment selector?
- Should the package manager and builder be one binary or separate tools?
- Should package recipes allow shell build phases or use a restricted build DSL?
- Should official builds require reproducibility immediately, or should reproducibility status be tracked and improved over time?
- Should Helium be packaged from source immediately, or should the first desktop image use a temporary browser?

## 24. Definition of Success

saltOS is successful when it can:

- boot in a VM
- install itself to disk
- update itself
- roll back cleanly
- install packages from a signed repo
- build packages from pinned recipes
- run a lightweight desktop
- browse the web with Helium or a temporary browser
- avoid untrusted package submission models
- remain understandable to one technical maintainer

The first real version does not need thousands of packages. It needs a coherent base system that proves the model works.
